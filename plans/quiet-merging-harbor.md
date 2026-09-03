# M66: エンジン内 Git 連携 v1 — 別プロセス Rust サービス (MyeCollab) + Editor 層クライアント

**★実装形態の正本は `plans\m66-git-collab\spec.md` §4.0** — 決定 1 はユーザー判断 (2026-09-02) で
反転し、**別プロセス exe から in-process の Rust cdylib (`MyeCollab.dll`) へ**なった。
本ファイルの spawn / Job Object / stdio に関する記述は読まない (経緯は ADR-015、
穴 14 件 S1〜S14 と確定仕様は spec、サブごとの判定は `plans\m66-git-collab\harness.md`)。

**再開手順**: `git log --oneline -5` で最後に完了したサブを確認 → 本ファイルの進捗表と
突き合わせ → 次のサブの節を読む → 着手前にそのサブの「冒頭確認」があれば先に潰す。
1 サブ = 1 コミット (`M66a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルの進捗表には**計画外の事実・罠・申し送りのみ**書く。

**元文書**: `Git管理.md` (ユーザー作成)。本計画は同文書の §3〜§6 を v1 として実装し、
§7〜§8 (Sparse / Remote 取得) と §17 (LFS) を**落とし**、§9〜§12 (PR / Review) を v2 へ送る。
評価の経緯は 2026-09-02 のセッション (自動メモリ `git-collab-assessment`) を参照。

**スコープ (ユーザー決定 2026-09-02)**:
1. v1 = ローカル Git + リモート監視 (§3〜6)。PR / Review (§9〜11) は v2。
2. サブアセット ID の絶対パス依存は**チーム規則「全員同じパスに clone」で回避**し、
   `assetsRoot` 相対化は独立マイルストーンに切る (本計画に混ぜない)。
3. サービスが管理するのは **`config.projectRoot` のプロジェクトリポだけ**。
   `projectRoot == git toplevel` を必須にする。エンジンリポの改修は通常の git。
4. チーム運用は **feature ブランチ + PR**。v1 の UI はブランチ一覧・作成・切替まで。
5. 競合は **(a) merge を abort して一覧を出す + (b) ファイル単位 ours / theirs**。
   fileId 単位の 3-way マージ (c) は独立の大物として別計画。
6. `project_settings.json` の個人設定分離を**含める**。

## 出口の姿

エディタの Source Control 窓 (Changes / Branches / History) で、変更一覧 → stage →
commit → push、fetch → pull、ブランチ作成 → 切替、競合の abort / ours / theirs が
**エディタを閉じずに**通る。起動時と一定間隔で fetch し、upstream に新しいコミットが
あればトーストで知らせる。書き込み系操作 (pull / checkout / revert / merge ...) は
「**全文書が保存済み かつ 何も実行中でない**」ときだけ押せて、実行中はモーダルで
編集側を止め、完了後にシーンを開き直す。Git 操作の最中にホットリロードが暴れない。
**sim には 1 バイトも触れない** = `replay_verify.bat` の 7 ペアと golden 19 枚が無変更で緑。

## 調査済みの土台 (2026-09-02 調査。計画の前提)

**追い風 (そのまま載る)**:
- モジュール分離の前例が 2 本ある: `GameLogic.dll` (C ABI 関数表) と `MyeScripting.dll`
  (sln 外で `tools\build_managed.bat`、不在なら `ManagedHost.cpp:99` で WARN → 機能 OFF)。
  MyeCollab は 3 本目。ただし置き場所は **Editor 層のみ** (ManagedHost は Engine 層で
  Runtime も読むが、Collab はエディタ専用)。
- 子プロセスの起動 + 毎フレームポーリングの前例: `BuildSettingsWindow.cpp:36-70`
  (`StartChildProcess` = stdout リダイレクト + stdin NUL + `CREATE_NO_WINDOW`、
  `PollProcess` = `WaitForSingleObject(0)`)。MyeCollab はこれを長寿命 + 双方向パイプ化。
- 通知: `ToastCenter::Notify` (`EditorApp::PollReloadToasts` が使い方の実例)。
- 未保存ガード: `EditorApp::RequestGuardedAction` / `PendingAction` / `DrawSaveConfirmModal`。
- 永続 fileId (`SceneSerializer.h`)、`.meta` GUID (guid / type / version のみで**絶対パスを
  含まない** → コミットして安全。`AssetDatabase::WriteMeta`)。
- `git` は `build/Common.props:75` が既に Exec で呼び、不在なら "unknown" に落とす。
- 起動時 `--project DIR` → `config.projectRoot`、個人ファイルは `<root>\.mye\`
  (`kProjectLocalDir`、`Project.h:13`)。`EditorSettings` は `.mye\editor_settings.json`
  (`EditorSettings.cpp:14`) = **個人設定の置き場が既にある**。
- ビルド元コミットは `MYE_GIT_HASH` として埋め込み済み (`obj\generated\<Config>\MyeBuildInfo.h`、
  読み手は `CrashHandler.cpp` のみ)。
- `EngineContext::net` (NetRuntimeInfo POD) を Editor 層が読む境界が `NetWindow.h:12` に
  ある = ネット中の判定に使える。
- `GpuResources::WaitForAsyncLoads` (`GpuResources.h:134`) = スクショ前 drain と同じ口。

**逆風 (新設 or 直す)**:
- **HTTP クライアント無し** (winhttp / wininet / curl とも 0 件) — v1 は不要 (v2 の GitHub API で Rust 側に持つ)。
- `ReloadHub::Update` は drain した変更を**同一フレームで全件** `HandleChange` する
  (`ReloadHub.cpp:82-84`)。バッチ化 API (`BeginBatch` / `EndBatch`) は無い。
- `FileWatcher` は 64 KB バッファ (`FileWatcher.cpp:14`)、溢れると WARN 1 行で変更を失う
  (`:87`)。デバウンス 150 ms。checkout の一斉書換には信用できない。
- `ApplyDiff` は「JSON に無い fileId を破棄」し (`SceneSerializer.cpp` `ApplyDiff` 手順 3)、
  新規オブジェクトは作成時点で fileId を持つ (`CreateMenu.cpp:161`) → 未保存保護は効かない。
- `ReadEntityComponents` は未知コンポーネントを `skipped` WARN で**捨てる**
  (`SceneSerializer.cpp:177` 以降) → ブランチ側の schema / C# / C++ コンポーネントが
  消えた状態で保存すると相手のデータが消える。
- ホットリロードが追従しないもの: `.controller.json` / `.terrain.json` / `input/actions.json` /
  `project_settings.json` / `assets/schemas/*` / `.cs` / `assets/scripts/*.cpp` / `.meta`。
  `AssetDatabase::ScanAndSync` は起動時 1 回 (`EngineLoop.cpp:396`)。
- 削除イベントは `retryLater` で**無限リトライ** (`ReloadHub.cpp:89-95`、上限なし)。
- `MusicStream` は `ifstream` を開いたまま (`MusicStream.cpp:49,89`) → Windows では
  git の上書きが失敗しうる (仮定。M66d 冒頭確認で実験)。
- `project_settings.json` を書く箇所: `ParticleSystem.cpp:144-147` (particleBackend /
  particleCompareMode / particleCompareOffsetX / particleCpuSimd)、`PartTagNames.cpp:74`、
  `PhysicsLayerNames.cpp:56`、`ProjectSettingsWindow.cpp:62,83,110,383` (EditorSettings /
  レイヤ名 / 部位タグ / 入力アクション)、`InspectorWindow.cpp:1050` (部位タグ選択 = 読み)。
  ★`ParticleSystem.cpp:28` 自身が「スクショ 1 枚のために開発者の project_settings.json が
  書き換わる」と嘆いている = 既知のノイズ。ADR-008:17-18 で SIMD on/off はビット一致。
- テンプレの `.gitignore` は `/.mye/ /cache/ /dist/` の 3 行 (`ProjectTemplates.cpp:118`)。
  `assets/scripts/Generated/` (`SchemaCodegen.cpp:392`)、`<root>\crash\`、`*.log` が漏れる。
- 未保存文書は 5 種: メインシーン (`IsSceneDirty`)、ミニシーン編集 (`InActorEdit`)、
  アニメ (`AnimationWindow.cpp:228,256` `SaveToFile`)、コントローラ
  (`AnimatorControllerWindow.cpp:116`)、ミキサー (`AudioMixerWindow.cpp:173`)、
  Project Settings 各セクション。後ろ 4 つに dirty アクセサが**無い**。
  地形ブラシは `.terrain.edit` へストロークごとに書く (`TerrainEdit.h:20,79`) ので保存済み扱い。
- 実行中の判定に要るが無いもの: `BuildSettingsWindow` の「実行中」アクセサ
  (`PipelineFinished` しか無い)、`StartGameLogicBuild` のハンドル保持場所。

## アーキテクチャ

```
Editor.exe (Editor 層のみ。Engine.lib は Collab を知らない)
  EditorApp
   ├ CollabClient        … MyeCollab.exe を Job Object 付きで spawn、stdio NDJSON、
   │                        読み取りスレッド → キュー → OnImGui で drain (FileWatcher と同型)
   ├ SourceControlState  … status キャッシュ (対 = asset+.meta+.terrain.edit を 1 行に束ねる)、
   │                        ahead/behind、ブランチ一覧
   ├ GitTransaction      … ゲート (CanRunGitWriteOp → 阻害要因リスト) / モーダル /
   │                        ReloadHub::BeginBatch..EndBatch / 段階 A・B・C の後処理
   └ SourceControlWindow … Changes / Branches / History タブ、コミット欄、競合一覧
          │ stdio (NDJSON, id 付き要求/応答 + event 通知, hello で proto 版照合)
          ▼
MyeCollab.exe (Rust, tools\collab\)   … git CLI ラッパ + porcelain v2 解析 + リポ監視 (notify)
          │                              + 定期 fetch + ahead/behind。UI 文字列は返さない
          ▼
   git.exe (2.11+) → working tree / .git / origin
```

- **Rust はコードと構造化データだけ返す** (規則 10 は Rust を走査できない)。表示は C++ が `Tr()`。
- **書き込み系は 1 トランザクション**: 前提チェック → モーダル → `ReloadHub::BeginBatch`
  → Rust 実行 → `git diff --name-status <before>..<after>` で変更集合 → 段階分類 →
  `EndBatch(changes)` (watcher に頼らず種別順に適用) → シーン開き直し / ビルド案内 / 再起動。
- **ゲートは双方向**: 条件を満たさない間はボタン無効 + 理由ツールチップ。押した瞬間に
  モーダル → 編集側 (Play / Save / Build / AssetBrowser 操作 / 各窓 Save / D&D) を止める。
- **不在なら機能 OFF**: `MyeCollab.exe` が無い / git が無い / toplevel 不一致 / proto 不一致 →
  Source Control 窓は「利用不可 + 理由」だけ出す。ADR-006 の「clone → .sln → F5」は不変。

## 決定台帳

| # | 決定 (2026-09-02) | 根拠 / 帰結 |
|---|---|---|
| 1 | 別プロセス + IPC (在プロセス cdylib ではない) | Rust の panic をエディタのクラッシュ経路 (.rep 付きバンドル) に混ぜない / エンジンは main-thread 中心で、別プロセスなら隔離が構造になる / `cargo test` が D3D 無しで回る |
| 2 | IPC = stdio NDJSON、要求 `{id,op,args}` / 応答 `{id,ok,result\|error}` / 通知 `{event,...}`。hello で `proto` 版を交換し不一致は機能 OFF | `Interop.cs` の「位置ベース・実行時検証なし」を繰り返さない。nlohmann は既にある |
| 3 | crate は `tools\collab\` (= `src\` の外)、出力 `bin\x64\<Config>\MyeCollab.exe`、`tools\build_collab.bat <Config>`、CI に rust-toolchain 1 ステップ | `gen_project_files.ps1` の対象外に置く。`build_managed.bat` と同型 |
| 4 | status の更新契機 = Rust 自前の監視 (notify) + エディタ保存直後のヒント + 定期再取得 | エンジンの `FileWatcher` は `assets\` 限定。Rust 側なら `src\` も含めて 1 本 |
| 5 | fetch = 起動時 + 一定間隔 (既定 5 分)。間隔と on/off は `EditorSettings` (個人) | 従量課金や機内での無駄打ちを止められる |
| 6 | 認証・identity は git global 設定 + Credential Manager に任せる。未設定の検出と案内のみ | v1 に認証 UI を作らない |
| 7 | 対の規則: `.meta` / `.terrain.edit` は本体と一体で stage / revert。`.meta` 無し資産はコミットする人の環境で `EnsureMeta` してから対で stage | 「最初にコミットした人が GUID を決める」= 現行の path-hash 継承設計と整合 |
| 8 | `project_settings.json` から particleCompareMode / OffsetX / CpuSimd を `EditorSettings` へ移す。`particleBackend` は残すが**書き戻しを Project Settings の Apply だけに限定** (Particle Settings 窓のトグルはセッション上書き = CLI 固定と同じ扱い) | backend は replay_verify が記録する内容を決めるプロジェクト設定。SIMD はビット一致 (ADR-008) なので個人設定でよい |
| 9 | 競合中 (`MERGE_HEAD` / `rebase-merge` あり) は他の書き込み系操作もゲートで止める。解決は abort か ours/theirs のみ | 競合マーカー入り JSON は `ReloadHub` が parse 失敗 WARN を出すだけで、開けない |
| 10 | 段階分類: A = その場で再読込 (hlsl / 画像 / 音声 / モデル / mat / anim / sound / mixer / physmat / actor / 非アクティブ scene)、B = シーン開き直し + ビルド案内 (アクティブ scene / .cs / scripts/*.cpp / .controller / .terrain / actions / project_settings)、C = 再起動 (schemas / .meta のリネーム / 件数超過 / **エンジンソースが build hash と差分**) | 未知コンポーネント読み飛ばしを「保存前に必ず止める」ため。C の判定は `git diff --quiet <MYE_GIT_HASH> HEAD -- src build external` |
| 11 | LFS / Sparse / ContentManager は v1 から削除。LFS の引き金は「単一ファイル 100 MB (GitHub の push 拒否) / リポ 1 GB 級 / 動画・PSD・長尺 wav」。到達したら `.gitattributes` を足すだけ (履歴書き換えはしない) | エンジンリポ assets 9.5 MB。学生規模ならこの範囲 |
| 12 | チーム規則「同じパスに clone」の補助: `project.mye.json` に `canonicalRoot` を置き、違えば起動時に WARN トースト | 規則が破られたことに気付ける最小の仕掛け。強制はしない |
| 13 | サービス死亡 (パイプ EOF) → 「SCM 利用不可」状態。次回起動時に `MERGE_HEAD` / `rebase-merge` 残骸を検査して警告 | checkout 途中で死ぬと working tree が半端 |
| 14 | `--package` は `MyeCollab.exe` と `.git` を含めない | Runtime に Collab は無い |

## サブ分割 (10 サブ、a → j)

### M66a: Rust サービス骨格 + 検証ハーネス + CI
- **成果物**: `tools\collab\` (Cargo.toml / src: main.rs = stdio ループ、protocol.rs = serde 型、
  git.rs = `git` 子プロセス実行 (`-c core.quotepath=false`、`-z`)、porcelain.rs = `status --porcelain=v2 -z` 解析、
  watch.rs = notify + デバウンス)。op: `hello` / `repo_check` (toplevel・git 版・MERGE_HEAD) / `status`。
  `--script <ndjson>` モード (要求を流して応答を stdout へ = ハーネス用)。
- `tools\build_collab.bat <Config>` (`where cargo` 不在で exit 1。`cargo build --release` は
  両 Config に同じ exe をコピー。Rust 側に Debug/Release の区別を持ち込まない)。
- `tools\collab_verify.bat`: 一時ディレクトリに `git init` → fixture コピー → 変更 → `--script` で
  status を取り → 期待 NDJSON と比較。**エディタ無し**で回る。
- `.gitignore` に `tools/collab/target/`。`ci.yml` に `dtolnay/rust-toolchain@stable` +
  `cargo test` + `build_collab.bat` + `collab_verify.bat` の 1 ステップ (縮退: 失敗しても
  後段を止めない `continue-on-error` は**付けない** — 検証の一部)。
- `tools\check_rules.ps1` に規則 12: `src\Engine` / `src\Runtime` / `src\GameLogic` /
  `src\Shared` から `Editor/SourceControl/` を include しない (Collab の Editor 層封じ込め)。
- ADR-015 (別プロセス Rust サービス。決定台帳 1〜3 を移す)。
- **検証**: `cargo test` (porcelain fixture 5 種: M / A / D / R / ? / 競合 u) 緑、
  `collab_verify.bat` 緑、`check_rules.ps1` 緑、CI 緑。
- 冒頭確認: `git --version` の下限 2.11 が `--porcelain=v2` の要件として正しいか公式で再確認。

### M66b: C++ クライアント + Source Control 窓 (読み取り専用)
- `src/Editor/SourceControl/CollabClient.h/.cpp`: spawn (Job Object `KILL_ON_JOB_CLOSE`)、
  stdin/stdout パイプ、読み取りスレッド → mutex キュー → `Poll()` (main thread) で応答/通知を配る、
  id 付き要求、タイムアウト、EOF → Unavailable。hello で proto 照合。
- `SourceControlState.h/.cpp`: status モデル (対の束ね、フォルダ集約)、`repo_check` 結果。
- `Windows/SourceControlWindow.h/.cpp`: Changes タブ (一覧 + 選択 + 状態バッジ)、利用不可時の理由表示。
  `EditorApp` にメンバ・Window メニュー・dock 位置・`.mye` の `canonicalRoot` 警告 (決定 12)。
- `LocalizationTable.inl` に en/ja (`###SourceControl` の ID 一意)。
- `SourceControlSelfTest.h/.cpp`: **偽トランスクリプト** (NDJSON 文字列) を CollabClient の
  解析経路へ流し、status モデル (対の束ね / フォルダ集約 / 競合) を検証。プロセスは起動しない。
  `EditorMain.cpp` の連鎖へ追加。
- **検証**: selftest 緑、`check_rules` 緑、実機で窓に status が出る (目視)、
  `MyeCollab.exe` を消して起動 → 「利用不可」表示 + 他機能無傷 (目視)。

### M66c: stage / unstage / commit / History / diff
- op: `stage(paths)` / `unstage` / `commit(msg)` / `log(n)` / `diff(path, staged?)` / `identity_check`。
- 対の規則 (決定 7): stage は本体 + `.meta` + `.terrain.edit` を束ねて送る。`.meta` 無し資産は
  C++ 側が `AssetDatabase::EnsureMeta` してから stage。
- Changes タブにコミット欄。dirty なら「未保存の変更は含まれません」+「保存してコミット」。
  identity 未設定なら案内 (設定 UI は作らない)。History タブ (log)。diff は `git diff` の
  テキストをそのまま読み取り専用の子窓へ (シーン JSON の意味付けは v1.5)。
- **検証**: `collab_verify.bat` に stage → commit → log のシナリオ追加、selftest に対の束ね
  (`.meta` 欠落 → EnsureMeta 呼び出しの判定) を追加。実機で commit が `git log` に載る。

### M66d: 書き込みトランザクション核 + 最初の書き込み系 = revert
- `ReloadHub::BeginBatch()` / `EndBatch(const std::vector<Change>&)`: batch 中は drain を溜めるだけ。
  `EndBatch` は渡された変更集合を **種別順 (texture → mat → model → actor → scene) で固定**して
  `HandleChange`、`D` は `HandleChange` に渡さずアンロード/無視の別経路 (無限リトライ封じ)。
  `EndBatch` 後に溜めていた watcher 分は破棄 (同じ変更を二重適用しない)。
- ゲート `GitTransaction::CanRunGitWriteOp(std::vector<StrId>& blockers)`:
  `IsSceneDirty` / `InActorEdit` / 各窓 `HasUnsavedChanges()` (**M66d で追加**: Animation /
  AnimatorController / AudioMixer / ProjectSettings) / `PlayState != Editing` /
  `ctx.net` セッション中 / `BuildSettingsWindow::IsRunning()` (**追加**) / スクリプトビルド子プロセス /
  Collab 側の op 実行中 / `MERGE_HEAD` あり。
- 実行前処理: `WaitForAsyncLoads`、BGM 停止 (冒頭確認: `AudioSystem` の音楽停止 API の有無)、
  `BeginBatch`、モーダル表示 (他窓の入力遮断、`ProcessPendingFileDrops` 保留)。
- 段階分類 (決定 10) と後処理: B は `LoadSceneFromPath` + `ClearAll` + serial リセット
  (`EditorApp.cpp:1253-1264` の経路) + `CompileScripts` 自動 + GameLogic はトースト。
  C は `Platform` に `BuildGitHash()` を新設 (`MyeBuildInfo.h` の唯一の読み手を 2 つ目に) →
  Rust `diff_quiet(from,to,paths)` → 「ビルドが必要」+ `RelaunchSelfWithProject`。
- 最初の書き込み系として `revert(paths)` (対で `git checkout -- `) と「すべて破棄」。
- **検証**: selftest = ゲートの阻害要因列挙 (各条件を 1 つずつ立てて期待リスト)、
  `EndBatch` の適用順 (一時ディレクトリに種別混在の変更を作り呼び出し順を記録)、
  `D` が retries_ に入らないこと。実機で「Play 中は押せない + 理由」(目視)、
  revert 後にアクティブシーンが開き直り dirty でない (目視)。
  **`replay_verify.bat` 無変更で緑** (sim に触れていない証明。以降の全サブで必須)。

### M66e: Branches — 一覧 / 作成 / 切替 (checkout)
- op: `branches` / `branch_create(name, from)` / `checkout(name)` / `diff_names(from,to)`。
- 切替はトランザクション経由。実行前に `diff_names(HEAD, target)` で段階を**先に**判定し、
  C なら「再起動します」を確認してから実行 (再起動 = 不可逆ではないが待ち時間がある)。
- feature ブランチ運用 (決定 4): 作成 UI は「現在のブランチから」既定、upstream 未設定は push 時に `-u`。
- 起動時の `repo_check` で `MERGE_HEAD` / `rebase-merge` 残骸 → 警告 (決定 13)。
- **検証**: `collab_verify.bat` にブランチ作成 → 変更 → 切替 → `diff_names` の期待値。
  実機で A 段階 (テクスチャだけ違うブランチ) は再起動なしで絵が変わる、
  B 段階 (scene が違う) は開き直し、C 段階 (`src/` が違う) は再起動案内 (目視 3 種)。

### M66f: fetch / pull / push + RemoteMonitor + 通知
- op: `fetch` / `pull` (`--ff-only` 既定、非 ff は merge へ = M66g) / `push` / `remote_state`
  (ahead / behind / upstream 有無)。Rust 側で定期 fetch (間隔は hello の引数で受ける)。
  通知 `remote_changed{ahead,behind,commits[]}`。
- 起動時 fetch (§6): 結果は ToastCenter + Changes タブ上部の帯。**自動 pull はしない**。
- `EditorSettings` に `scmFetchIntervalMin` / `scmAutoFetch` (個人)。
- push の非 ff 拒否 → 「先に pull」案内。認証失敗 → Credential Manager の案内 (決定 6)。
- **検証**: `collab_verify.bat` に bare リポを origin にした 2 クローンシナリオ
  (A が push → B で `remote_state` behind=1 → pull → 一致)。実機でトーストが出る (目視)。

### M66g: 競合 — abort + ours / theirs
- op: `merge_abort` / `conflicts` / `resolve(path, ours|theirs)` / `continue`。
- 競合状態では Changes タブが競合一覧に切り替わり、他の書き込み系は無効 (決定 9)。
  解決後の `continue` はトランザクションの後処理 (段階分類) を通す。
- 外部マージツールは `git mergetool` を `externalEditorCmd` と同じ `ShellExecuteW` 流儀で起動する
  ボタンのみ (結果は Rust の監視で拾う)。
- **検証**: `collab_verify.bat` に競合シナリオ (同一ファイルを両側で変更 → pull → conflicts →
  ours → continue)。selftest に競合 (u) 行の解析。実機で競合中のボタン無効 (目視)。

### M66h: 衛生 — .gitignore テンプレ + project_settings 分離 + 対の追加
- `ProjectTemplates.cpp:118` の `.gitignore` に `/crash/` `/assets/scripts/Generated/` `*.log`。
  Source Control 窓に「推奨 .gitignore を適用」(不足行を追記、既存行は触らない)。
- 決定 8: `ParticleSystem` の compare/simd 3 キーの永続化を撤去 → `EditorSettings` に移し、
  Editor 側が起動時と変更時に `ParticleSystem` の setter へ流す (Engine 層は EditorSettings を
  知らない)。`particleBackend` の書き戻しは Project Settings の Apply だけに。
  ★旧 `project_settings.json` に残る 3 キーは読み飛ばし (前方互換)。`--particle-backend` CLI の
  「書き戻さない」挙動は不変 (`ParticleSystem.cpp:28` の嘆きが消える)。
- `.terrain.edit` を対に追加 (M66c の束ねに拡張子 1 つ足すだけ)。
- **検証**: `ParticleSelfTest` に「compare/simd を変えても project_settings.json が変わらない」、
  `AssetOpsSelfTest` (または新設) にテンプレ `.gitignore` の行検査。
  **`shot_verify.bat` 無変更で緑** (粒子設定の読み経路を触るため)。

### M66i: Content Browser バッジ + 保存ヒント
- `AssetBrowserWindow` のグリッド/ツリーに M / A / D / R / ? / 競合のバッジ (フォルダは集約)。
  色は `themeColor::*` のトークン (リテラル禁止、`ImGuiTheme.h` の 5 箇条)。
- `EditorApp::SaveCurrentScene` 等の保存直後に `CollabClient` へ `hint_changed(paths)` (決定 4)。
- **検証**: selftest = フォルダ集約 (子 M → 親 M、子 ? のみ → 親 ?)。実機 (目視)。

### M66j: 仕上げ — spec / README / CLAUDE.md / 規則
- `engine_spec.md` §9 の表に Source Control 行、新節「§14 Source control (M66)」に
  トランザクション / 段階 / ゲート / 対の規則。README の機能概要とビルド手順に
  `build_collab.bat`。`CLAUDE.md` のビルド節と検証表に `collab_verify.bat`。
- `check_rules.ps1` の規則 12 を spec 11.2 に記載。
- **検証**: 全検証 (selftest / replay_verify / shot_verify / check_rules / collab_verify / cargo test) 緑。

## 壊れるもの台帳 (触るが、壊してはいけないもの)

| 触る場所 | 壊さない約束 | 検証 |
|---|---|---|
| `ReloadHub` (BeginBatch/EndBatch 追加) | batch 外の従来挙動はビット単位で不変 | 既存の hot-reload 経路を実機で 1 回ずつ (hlsl / png / scene 外部編集) |
| `ParticleSystem` の設定永続化 | backend の既定と CLI 上書きは不変。golden 16/17 枚目 | `shot_verify.bat` |
| `EditorSettings` (キー追加) | 旧 JSON は `value(key, default)` で読める | `EditorSettings` の読み直しを selftest に |
| `ProjectTemplates` (.gitignore 行追加) | 既存プロジェクトは触らない (適用ボタンは追記のみ) | selftest |
| `Platform` に `BuildGitHash()` | `CrashHandler` の出力は不変 | `crash_verify.bat` (CI 対象外、ローカルで 1 回) |
| `EditorMain.cpp` selftest 連鎖 | 連鎖の短絡順序を崩さない (末尾 append) | `--selftest` |
| `ci.yml` | 既存 6 ステップの順序と環境変数は不変。追加は 1 ステップ | CI 緑 |
| sim 全般 | **1 バイトも触らない**。TypeId / ABI / ハッシュ / .rep 版 / snapshot 版すべて不変 | `replay_verify.bat` 全サブで無変更緑 |

## 対象外 (明示)

- PR / Review / CI 状態 / レビューコメントの表示 (§9〜11) → v2 (GitHub API は GraphQL の
  `resolveReviewThread` が要る = REST では Resolve 不可。v2 計画時に再調査)。
- Sparse Checkout / Remote-Local 表示 / ContentManager (§7〜8)、LFS (§17) → 決定 11。
- fileId 単位のシーン 3-way マージ (競合 (c))。
- シーン JSON の意味付き diff (HEAD との差分を Hierarchy で強調) → v1.5 として本計画完了後。
- サブアセット ID の `assetsRoot` 相対化 → 独立マイルストーン (Replay / Scene / cooked cache の版 bump)。
- エンジンリポ自体の管理、エンジン内の認証 UI、コードエディタ、`git lfs lock`。
- 未知コンポーネントの「保持して書き戻す」化 (段階 B/C で保存を止めることで v1 では不要)。

## リスクと縮退ライン

- **Rust toolchain が CI で入らない / 遅い** → 縮退: rust ステップを `workflow_dispatch` 時のみに
  落とす。ローカルでは `collab_verify.bat` を必須のまま。
- **stdio の詰まり** (巨大 status / log 出力で子が stdout 書き込み待ち、親が stdin 書き込み待ち) →
  読み取りは専用スレッドで常時 drain (M66b の設計そのもの)。`log` は `n` 上限必須。
- **パスのエンコード**: `-z` + `core.quotepath=false` で UTF-8 固定。C++ 側は `Utf8ToWide` →
  `NormalizePathKey` で対のキーを作る。長いパス (260 超) は v1 では非対応と明記。
- **段階 A の適用順が足りない** (scene ↔ actor、mat ↔ 新規 texture) → 縮退: 変更集合に
  actor と scene が同居したら B に落とす (開き直しは常に正しい)。
- **`FileWatcher` 溢れ** → batch 中は watcher の結果を捨て、Rust の `diff_names` を正とする
  (M66d の設計そのもの)。
- **git の上書き失敗 (開いたままのハンドル)** → 実行前に BGM 停止 + `WaitForAsyncLoads`。
  それでも失敗したら Rust が `error.code = "locked_file"` + パスを返し、モーダルに表示して abort。
- **サービスが checkout 途中で死ぬ** → 決定 13。復旧は手動 (`git status` を案内)。
- **チームが別パスに clone** → 決定 12 の WARN のみ。根治は独立マイルストーン。
- **2 回連続で検証失敗** → SOP の停止規則に従い、仮説・変更・結果を列挙してユーザー判断。

## 検証の柱 (全サブ共通)

1. `cargo test` (tools\collab) — porcelain / diff_names / protocol の解析。
2. `tools\collab_verify.bat` — 一時リポで `--script` シナリオ (エディタ不要、CI で回る)。
3. `bin\x64\Debug\Editor.exe --selftest` — `RunSourceControlSelfTest` (偽トランスクリプト / ゲート / 対 / 集約)。
4. `pwsh -File tools\check_rules.ps1` — 規則 10 (新規文字列) + 規則 12 (Editor 層封じ込め)。
5. `tools\replay_verify.bat` — **全サブで無変更緑** (sim 不変の証明)。
6. `tools\shot_verify.bat` — M66h (粒子設定) と M66i (バッジは撮影対象外だが念のため) で。
7. 実機の目視 — 各サブの「(目視)」項目。判定者がユーザーの主観になる UI の手触り
  (バッジの色・トーストの頻度) は「小さく変えて試してもらう」ループに切り替える。

## 進捗 (完了時に更新。計画外の事実・罠・申し送りだけ書く)

| サブ | 状態 | コミット | 申し送り |
|---|---|---|---|
| M66a | 完了 | 4716d6a | 実装形態が in-process cdylib になり spawn / Job Object / stdio が丸ごと不要に。crate の edition は **2021** 固定 (2024 は `#[unsafe(no_mangle)]` が要る)。cargo 不在は exit 1 (縮退しない) |
| M66b | 完了 | 165cfc6 | 対は **`x.terrain.json` ⇄ `x.terrain.edit`** (計画の「`X` + `X.terrain.edit`」は誤り)。フォルダ集約は「最も重い状態」。PROTO_VERSION は**フィールド追加では bump しない** |
| M66c | 完了 | 83d0b4f | 「保存してコミット」= 保存 → **保存した文書を対で stage** → commit (「保存 → commit」は保存前の index を撮る)。identity 未設定はボタン無効 — git は機体名で補完して**成功してしまう** |
| M66d | 完了 | 205e90d | 段階 C のモーダルは**『再起動』のみ** (「あとで」を置くとスキーマ不一致のまま保存できる = C の存在理由が消える)。新規アセットは増分登録 (`ScanAndSync` は 3 表を clear する) |
| M66e | 完了 | c3be569 | **`FreeLibrary` を呼ばない** — notify の `Drop` が内部スレッドを join せず終了時に 0xC0000005 (実測 1〜2/6 → 0/12)。Asset Browser の [Rebuild Scripts] は fire-and-forget でゲートから見えなかったので `StartGameLogicBuild` へ一本化 |
| M66f | 完了 | 1e6b8f8 | `GIT_TERMINAL_PROMPT=0` **だけでは GCM の GUI が出る** (`GCM_INTERACTIVE=never` が要る) → 背景 fetch にだけ付ける。定期 fetch は worker のタイマーに相乗り、`handle_tick` は `recv_timeout` の**前**に毎周回呼ぶ (でないと飢える) |
| M66g | 完了 | 3c0bf56 | ours / theirs の可否は porcelain `u` の**モード (m2/m3)** で決める (XY の文字だと `AU` を誤る)。`resolve` は `paths[]` (本体と `.meta` を 1 往復で)。競合したシーンは空で開くので保存ガードが唯一の砦 |
| M66h | 完了 | 69e767e | 旧 `project_settings.json` の個人 3 キーは**移行せず・消さず**読み飛ばす。計画外: `shot_verify` の acoustic 2 枚 (golden 18/19) に **M66 以前からの run-to-run フレーク**を発見 → ユーザー判断「何もしない」(2026-09-03)、根治は M66 の外 |
| M66i | 完了 | 048ec64 | バッジに **`Accent` 系を使わない** (選択行の面と同色で読めない)。保存ヒントは `AssetOps` の choke point 3 箇所に置く (公開関数の末尾に散らすと取りこぼす) |
| M66j | 完了 | (本コミット) | ドキュメントのみ。`engine_spec.md` に **§14 Source control** を新設し §11.2 に規則 11 (S12 の抜け) と規則 12 を追加。仕様の正本は `plans\m66-git-collab\spec.md` |
