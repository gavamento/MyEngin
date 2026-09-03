# M66: エンジン内 Git 連携 v1 — 仕様書

- slug: m66-git-collab
- 状態: 確定 (2026-09-02)。R1 = 4 問 (S1 / S2 / S7 / S6)、R2 = 3 問 (S1 の解釈確認 / 仮置き 6 点 / 確定確認)、いずれも回答済み
- 依頼原文: "C:\HAL\MyEngin\plans\quiet-merging-harbor.md"を読んで計画に穴が無いかを確認したのち実装をしたい
- 正本: `plans/quiet-merging-harbor.md` (ユーザー作成 2026-09-02、以下「元計画」)。本書は元計画を
  取り込み、**コードと突き合わせて見つかった穴と、その埋め方だけ**を追記する。元計画の
  スコープ決定 6 点・決定台帳 1〜14 は再議論しない (穴の修正で決定の文言が変わる場合のみ明記。
  **決定 1 は R1 Q1 のユーザー回答で反転** — §4.0)。

## 1. 目的 (なぜ作るか)

学生規模のチームが feature ブランチ + PR でプロジェクトリポを共同編集するとき、**エディタを
閉じずに** 変更確認 → stage → commit → push / fetch → pull / ブランチ切替 / 競合の abort・
ours・theirs が通り、**pull や checkout の一斉書き換えでエディタが壊れない** (未保存の巻き添え、
ホットリロードの取りこぼし、未知コンポーネントの読み飛ばし保存) 状態にする。
sim には 1 バイトも触れない (`replay_verify.bat` 7 ペア + golden 19 枚が無変更で緑)。

## 2. 疑った点と結論

元計画の「調査済みの土台」24 項目をコードで再検証した (2026-09-02、基点 02bf3c9)。
18 項目は正確 (行番号ずれのみ)。**実害のある食い違いは以下**。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| S1 | **Rust ツールチェーンが無い** | `where cargo` / `where rustup` とも不在 (この機体)。git は 2.48.1。計画は cargo の存在を一度も確認していない。`bin/` は gitignore なのでチーム全員が cargo を持たないと機能 OFF になる | R1: 「コンパイルして DLL としてのせる」(a/b/c のどれでもない) → R2: a. 解釈を確認 | **Rust cdylib `MyeCollab.dll` を Editor.exe が in-process ロード** (§4.0)。決定台帳 1 は反転 (ADR-015 に記録)。rustup (stable) はユーザーが着手前に入れる |
| S2 | **段階 C の「エンジンソースが build hash と差分」はプロジェクトリポで成立しない** | プロジェクト = `assets\ + project.mye.json + .mye\ + .gitignore` (`ProjectTemplates.cpp:65-124`)。`src\ build\ external\` を持たない。`MYE_GIT_HASH` は Engine プロジェクトのビルドが**エンジンリポ**で `git describe` した値 (`Common.props:73-88`) = プロジェクトリポには存在しないオブジェクト。エンジンリポ自身はマニフェストが無く `--project` で開けない (`EditorMain.cpp:619`) | R1: a. 削除 | 判定を削除。`Platform::BuildGitHash()` も作らない (crash_verify の再確認は不要に) |
| S3 | プロジェクトの C++ スクリプトの置き場が計画と違う | `assets/scripts/*.cpp` ではなく **`<root>\src\GameLogic\Scripts\*.cpp`** (`AssetOps.cpp:457, 1347`)。`assets\scripts\` は `.cs` | 事実 | 段階 B の対象パスを修正 |
| S4 | **削除イベントは ReloadHub に届かない** (計画は「retryLater で無限リトライ」) | `FileWatcher.cpp:96-97` は MODIFIED / ADDED / RENAMED_NEW_NAME しか enqueue しない。`ReloadHub.cpp:89-95` は共有違反のリトライ列で、`Retry::attempts` は増えも比較もされない (`ReloadHub.h:58`, `:109`) = **共有違反が永続するファイルだけ**無限リトライ | 事実 | `EndBatch` の D は「watcher 経由で来ない前提で明示処理」。リトライ上限 (`kReloadRetryMax` 仮 60 回) は別件の衛生として M66d に含める |
| S5 | **他人が足した新規アセットは再起動まで見えない** | 新規ファイルは watcher が拾うが `HandleChange` は登録済みアセット以外を no-op にする。`AssetDatabase::ScanAndSync` は起動時 1 回 (`EngineLoop.cpp:396`)、実行時登録はエディタ操作経由のみ (`AssetOps.cpp:199-206`) | 事実 | 段階 A に「`A` (新規) の AssetDatabase 登録」を含める (方式は §7) |
| S6 | 未保存ガードの 4 窓に dirty 追跡が無い | Animation / AnimatorController / AudioMixer は編集のたびにメモリ上の資産を直接書き換え、Save ボタンでだけ書き出す (`AnimationWindow.h:34-48` 等に dirty メンバ無し)。ProjectSettings は EditBuffer をその場で書き換え、render-path ラジオは永続化されない (`ProjectSettingsWindow.cpp:40-46`) | R1: a. 直列化してディスクと比較 | 各窓に `HasUnsavedChanges()` (§4.1) |
| S7 | checkout / pull 時のローカル未コミット変更の扱いが未定義 | ゲートは「文書が保存済み」までしか要求しない。保存済み = working tree に未コミットの差分がある状態 | R1: a. git に任せる | 持ち越し。git が拒否したら `error.code=local_changes_overwritten` + 対象ファイル一覧をモーダルに (§4.1) |
| S8 | `MusicStream` の開きっぱなしは WAV だけ | `MusicStream.cpp:49,79` (WAV は file_ を保持) / `:89` は OGG で全読みして閉じる。停止 API は `AudioSystem::StopMusic(float)` (`AudioSystem.h:192`) | 事実 | 実行前処理は `StopMusic(0)`。「冒頭確認」は解消 |
| S9 | 実機目視の場が無い | replay / shot / CI は全部裸起動 (`--project` は bat / yml に 0 件) = Collab OFF。エンジンリポにマニフェストは無い | R2: a. 採用 | `tools\collab_fixture.ps1 <dir>` で git 初期化済みの一時プロジェクトを作り、`collab_verify.bat` と目視で共用 |
| S10 | テンプレ `.gitignore` の漏れは 4 行 | 計画の 3 行 (`/crash/` `/assets/scripts/Generated/` `*.log`) に加え `/save/` (`EngineLoop.cpp:422`) | 事実 | M66h に追加 |
| S11 | 計画が参照する自動メモリ `git-collab-assessment` が存在しない | `~/.claude/projects/C--HAL-MyEngin/memory/` に無い | 事実 | 評価の根拠は決定台帳のみ。本書 §2 が代替の記録 |
| S12 | `engine_spec.md` 11.2 の表に規則 11 が無い | 表は 1〜10 のみ、`check_rules.ps1:531` は規則 11 を実装済み | 事実 | M66j で 11 と 12 を同時に足す |
| S13 | M66c と M66h で `.terrain.edit` の対が二重に書かれている | 元計画 M66c「本体 + `.meta` + `.terrain.edit`」と M66h「`.terrain.edit` を対に追加」 | 事実 | M66c で確定。`.terrain.edit` はソース資産 (`TerrainEdit.h:20-24`、gitignore 対象外) |

再検証で**そのまま使える**と確認したもの: `StartChildProcess` / `PollProcess` (`BuildSettingsWindow.cpp:38-82`)、`ToastCenter::Notify` (同文は重複排除、`ToastCenter.cpp:40-54`)、`RequestGuardedAction` (条件は `IsSceneDirty()` のみ、`EditorApp.cpp:1238`)、`RelaunchSelfWithProject` (`ProjectManager.cpp:456-469`、`ShellExecuteW`)、`AssetDatabase::EnsureMeta` / `WriteMeta` (`.meta` = guid / type / version [+ tex])、`EditorSettings` の `value(key, default)` 読み、`NetRuntimeInfo::active` (`NetRuntime.h:22`)、`TextureLibrary::WaitForAsyncLoads`、`themeColor::*` の 5 箇条、nlohmann は Editor 層で 12 か所使用中、`--package` は明示コピー (`BuildSettingsWindow.cpp:163-184`) なので決定 14 は無作業で成立、`$constGroups` は `.rs` ⇄ `.h` の整数比較にそのまま使える、ADR-015 は空き番、`DllReloader` (`LoadLibrary` + `GetProcAddress` の前例、Engine 層)、`gen_project_files.ps1` は `src\Editor` を再帰で拾う (新ディレクトリは実行すれば載る)。

## 3. スコープ

- やる: 元計画 M66a〜M66j (§6 の表)。加えて本書で足した穴埋め: S9 の fixture、S5 の新規登録、
  S4 のリトライ上限、S10 の `/save/`、S12 の規則 11 表記、proto 版の機械照合 (§4.4)、
  M66a に「DLL を Editor からロードして hello が往復する」薄い縦切りを含める (最大の未知を最初に潰す)。
- やらない: 元計画「対象外」の全項目 (PR / Review / Sparse / LFS / 3-way / 意味付き diff /
  `assetsRoot` 相対化 / エンジンリポ管理 / 認証 UI / 未知コンポーネント保持)。
  **`git init` (リポジトリ作成) は v1 に含めない** — 「利用不可: リポジトリではありません」+ 案内のみ (R2 で確定)。
  同一リポを 2 つのエディタで同時に開く運用は非対応 (index.lock 競合は git のエラーをそのまま表示)。
  `MyeCollab.dll` のホットリロードはしない (エディタ実行中は `build_collab.bat` が上書きできない = `MyeScripting.dll` と同じ)。
- 後回し: 外部 (ターミナル) で git を叩いたときの HEAD 移動検知は**トーストのみ** (v1)。
  再適用のトランザクション化は v1.5。段階 B のうち `.cs` / `.cpp` だけの変更でシーン開き直しを省く最適化も v1.5
  (v1 は「B なら必ず開き直す」。ゲートが保存済みを保証しているので失うものは undo 履歴だけ)。

## 4. 仕様 (元計画との差分のみ。無印は元計画どおり)

### 4.0 実装形態 — Rust cdylib を in-process ロード (決定 1 の反転。R2 でユーザー確認済み)

```
Editor.exe (Editor 層のみ)
  EditorApp
   ├ CollabClient        … LoadLibraryW(<exeDir>\MyeCollab.dll) + GetProcAddress ×6、毎フレーム
   │                        mye_collab_poll を NULL まで drain して応答 (id) / 通知 (event) を配る。
   │                        C++ 側にスレッドは無い
   ├ SourceControlState / GitTransaction / SourceControlWindow … 元計画どおり
          │ C ABI (UTF-8 JSON 文字列を渡すだけ。op ごとのスロットは作らない)
          ▼
MyeCollab.dll (Rust cdylib, tools\collab\) … worker スレッド 1 本 (git 直列実行 + 定期 fetch)
          │                                     + notify 監視スレッド。UI 文字列は返さない
          ▼
   git.exe (2.11+、CREATE_NO_WINDOW、stdin NUL)
```

- **C ABI (6 関数、これ以外は増やさない。増やすなら proto 版を bump)**:
  ```
  uint32_t mye_collab_proto_version(void);
  void*    mye_collab_create(const char* rootUtf8);        // NULL = 失敗 (メモリ)
  void     mye_collab_request(void* h, const char* json);  // 非同期。応答は poll で届く
  char*    mye_collab_poll(void* h);                       // 応答 or 通知 1 件、無ければ NULL
  void     mye_collab_free(char* s);
  void     mye_collab_destroy(void* h);
  ```
  メッセージ形は元計画 決定 2 のまま (要求 `{id,op,args}` / 応答 `{id,ok,result|error}` / 通知 `{event,...}`)。
  `hello` も普通の要求 (proto は `mye_collab_proto_version` で先に照合、`hello` は git 版 + 設定の受け渡し)。
- **panic の隔離**: 全 export は `catch_unwind` で囲み、worker / 監視スレッドの panic は
  `{event:"service_error", code:"internal_panic"}` を 1 回出してハンドルを dead 化 (以後の要求は
  `error.code=service_dead`)。Cargo profile は `panic = "unwind"` のまま (abort にすると隔離が消える)。
  C++ は `service_error` で `ServiceDied` へ遷移 (決定 13 の「サービス死亡」と同じ扱い)。
- **決定 1 の根拠のうち生き残るもの**: `cargo test` は D3D 無しで回る / CLI (`MyeCollabCli.exe`、
  同じ crate の `[[bin]]`、NDJSON を stdin → stdout) でエディタ無しの `collab_verify.bat` が回る。
  **失うもの**: メモリ隔離 (Rust の未定義動作はエディタごと落ちる。safe Rust に限定し `unsafe` は FFI 境界だけ)。
- 出力: `cargo build --release` → `bin\x64\Debug\` と `bin\x64\Release\` の両方へ `MyeCollab.dll` +
  `MyeCollabCli.exe` をコピー (`build_collab.bat <Config>` は引数を受けるが両方に置く。Rust 側に構成の区別を持ち込まない)。
- **Shutdown は `destroy` のみで `FreeLibrary` は呼ばない** (sub-05 round 1 で確定)。notify 8.2.0 の `ReadDirectoryChangesWatcher::drop` (`src/windows.rs:590-596`) は Stop を送るだけで内部スレッドを join しない → アンマップ後にそのスレッドが DLL のコードを実行して 0xC0000005 (実測 1〜2/6 → 撤去後 0/12)。Shutdown はプロセス終了直前にしか呼ばれないので失うものは無い。**Rust 側に join できないスレッドを増やさない** (定期 fetch は worker のタイマーで)。
- 不在時: `LoadLibraryW` 失敗 → `NoService`。`mye_collab_proto_version() != kCollabProtoVersion` → `ProtoMismatch`。
- **前提**: rustup (stable) をユーザーが着手前にインストールする。coder は環境を変えない。
  README / CLAUDE.md に前提として明記 (M66j)。CI は `dtolnay/rust-toolchain@stable`。

### 4.1 振る舞い

**段階分類 (決定 10 の確定版)** — 変更集合 (`git diff --name-status -z <before>..<after>`、revert は
対象パス集合) を以下で分類し、**最も重い段階を採る**。

| 段階 | 対象 | 後処理 |
|---|---|---|
| A (その場) | ReloadHub が扱う拡張子 `.hlsl .hlsli .png .tga .jpg .jpeg .dds .wav .ogg .glb .gltf .fbx .mat.json .anim.json .sound.json .mixer.json .physmat.json .actor.json .prefab.json` の M / R。非アクティブ `.scene.json`、`project.mye.json`、未知拡張子、`assets\` と `src\GameLogic\Scripts\` の外 = no-op。**`A` (新規) は AssetDatabase へ登録** (S5) | `EndBatch` が種別順 (texture → mat → model → actor → scene) に `HandleChange` |
| B (開き直し) | アクティブ `.scene.json` / `src\GameLogic\Scripts\` 配下の `.cpp .h .hpp .inl` (S3。ヘッダだけの変更でも DLL は古い) / `.cs` / `.controller.json` / `.terrain.json` / `.terrain.edit` / `assets\input\actions.json` / `assets\project_settings.json` / **A 対象の `D`** / A 対象に actor と scene が同居 | `LoadSceneFromPath` 経路 (`EditorApp.cpp:1218-1234`) + `.cs` は `CompileScripts` 自動 + `.cpp` は「Rebuild Scripts」トースト。開き直し前に `.controller` / `.terrain` のライブラリキャッシュを無効化 (API が無ければその種別は C へ格上げ) |
| C (再起動) | `assets\schemas\*` / `.meta` の guid 変更・リネーム (実行前に guid を控え、実行後に読み直して比較。事前予測では見えないので楽観側に倒れる) / 変更件数 > `kCollabMaxBatchApply` (= 200、R2 で確定) | `EndBatch({})` → モーダル「再起動します」は**『再起動』ボタンのみ** (「あとで」「キャンセル」を置かない) → `RelaunchSelfWithProject` → 自プロセス終了。ゲートが「全文書保存済み」を保証しているので即時再起動で失うものは無い。**「あとで」を許すと、メモリ上のスキーマとディスクのスキーマが食い違ったまま保存できる = 未知コンポーネント読み飛ばしで相手のデータが消える** (C を設けた理由そのもの)。再起動に失敗したら (`RelaunchSelfWithProject` = `ShellExecuteW` の戻り ≤ 32) **モーダルを閉じない** — 赤字で「手動で起動し直してください」を出し、ボタンは押し直せる。閉じると「あとで」と同じ状態になる (sub-04 round 2 で確定) |

- S2: 「エンジンソースが build hash と差分」判定は**無し**。
- 段階は**実行前**に `diff_names(HEAD, target)` で予測して確認ダイアログに出し、実行後は実際の変更集合で再分類する (予測 < 実際なら実際に従う)。

**checkout / pull とローカル未コミット変更** (S7 = a): 変更は持ち越す。git が拒否
(`error: Your local changes ... would be overwritten`) → `error.code=local_changes_overwritten` +
`paths[]` → モーダルに一覧表示 + 「対象を revert してから再実行」の案内。トランザクションは何も変えずに閉じる。

**未保存ガードの 4 窓** (S6 = a): `AnimationWindow` / `AnimatorControllerWindow` / `AudioMixerWindow` /
`ProjectSettingsWindow` に `bool HasUnsavedChanges() const` を追加。判定 = 「その窓が Save で書き出す
JSON をメモリから直列化した文字列」と「ディスクの現物」の比較 (ファイルが無ければ「メモリに何かあれば dirty」)。
評価は `GitTransaction::CanRunGitWriteOp` からのみ呼び、結果は 500 ms キャッシュ (毎フレーム直列化しない)。
ProjectSettings の render-path ラジオ (永続化されない) は対象外。

**対の規則 (決定 7 の確定版、sub-02 round 1 で修正)**: 本体 `X` に `X.meta` を束ねる。地形は
**`x.terrain.json` ⇄ `x.terrain.edit`** (`TerrainEdit.cpp:469 EditPathFor` = `.json` を `.edit` に差し替える。
「`X` + `X.terrain.edit`」ではない)。束ねの主 (primary) は本体、サイドカーは「存在するものだけ」。
状態の合成とフォルダ集約はどちらも **`CombineState` = 最も重いもの** (競合 > D > R > A > M > ?)。
`{D, ?}` の親は D (削除が折り畳みに隠れない)。

**commit 周り (sub-03 round 1 で確定)**:
- 「保存してコミット」= **保存 → 保存した文書を対の規則で stage → commit** の 3 手。「保存 → commit」だと保存前の index が
  コミットされる (押した人の意図と逆)。stage するのは保存した文書 (+ サイドカー) だけで、他の未 stage 変更には触れない。
- identity (`user.name` / `user.email`) が未設定なら**コミットボタンを無効化** + 案内。git は未設定でも OS アカウント名と
  機体名 (`akita@DESKTOP-....(none)`) で補完してコミットに成功してしまい、共有履歴が汚れる。設定 UI は作らない (決定 6)。
- unstage は `git reset -q -- <paths>` (`git restore --staged` は未出生ブランチで `could not resolve HEAD` になる。実測)。
- `diff` は 256 KB で打ち切り `truncated: true` を返す (巨大差分で 1 フレーム固まるのを防ぐ)。
- 書き込み系 op の応答は `{"status": <実行後 status>}` を載せ、Rust 側で `last_status` / `last_head` も更新する
  (自分の commit が「外部で HEAD が移動」トーストを誘発しない)。revert / checkout / pull も同じ型。

**ブランチ周り (sub-05 round 1 で確定)**:
- **実行後の変更集合は op の応答に載せる** (`checkout` は `{branch, head, names, status}`)。C++ から 2 往復目の `diff_names` を投げる形にしない — 間に監視由来の status が割り込み「切り替わったが何が変わったか分からない」状態が生まれる。pull / continue / abort も同じ型。
- 未出生ブランチから乗り換えたときは `ls-tree` で「全部 A」を返す (diff が使えない。空を返すと段階 A = 何もしない と読まれる)。
- `branch_create` はゲートを通さない (working tree を触らない = commit / stage と同じ扱い。塞ぐのは `WriteInFlight` だけ)。
- `local_changes_overwritten` の `detail` はサービス側の固定文 (git の原文は版で変わる)。対象は `paths[]` が正。
- リモート追跡だけのブランチは `-t` 付きで checkout (sub-06 の bare origin で 1 回実走する)。
- `.meta` の guid 変化: 予測に無かった `.meta` は C へ倒す (実行後の比較)。
- Asset Browser の [Rebuild Scripts] は `StartGameLogicBuild` 経由に一本化し `EditorApp` が HANDLE を保持 → ゲートの `ScriptBuildRunning` に載る。旧 `AssetOps::RebuildGameLogic` (可視 cmd 窓 + 失敗時 `pause`) は削除。出力は `<project>/cache/build_scripts.log` + トースト。**失敗時のエラー行を Console へ流して旧 UX (その場で読める) と同等にする** — sub-08 で。

**競合周り (sub-07 round 1 で確定)**:
- `resolve{paths[], side}` (単数ではない): 本体と `.meta` を 1 往復で解決しないと「本体は theirs、.meta は競合のまま」が 1 往復ぶん存在する。ours / theirs を採れるかは porcelain `u` の **モード (m2 / m3) で決める** (XY の文字だと `AU` のように相手の版が無いのに Y='U' な組があり `checkout --theirs` が落ちる。実測)。版が無い側を選ぶ = `git rm`。
- `conflicts` op が `{files[{path, kind, ours, theirs}], merged[{path,status}], mergeInProgress, rebaseInProgress}` を返す。種別は git-status.txt の 7 組 (`both_modified` / `deleted_by_us` / `deleted_by_them` / `added_by_both` / `both_deleted` / `added_by_us` / `added_by_them`) + `unmerged`。競合ファイル一覧を git の案内文から解析しない。
- `status` / `repo_check` の両方に `mergeInProgress` / `rebaseInProgress` (status に無いと pull が競合した直後のゲートが開いたまま = 決定 9 が成立しない)。
- 競合した pull は **`EndBatch({})` せず** `conflicts` を 1 往復聞いて `merged[]` の A 段階だけ適用してから競合を報告する (`Phase::ConflictScan`)。`merged[]` は `diff --name-status HEAD` 由来なので pull 前からの未コミット変更も混ざる (実害 = 余分に読み直すだけ)。
- `merge_abort` / `continue` の応答は pull と同型 `{head, names, status, remote}`。abort の names は実行前後のディスクの在り方から A/D/M (HEAD が動かないので diff は必ず空)。`continue` は未解決があれば `merge_in_progress` + 残りのパス、全件解決なら `commit --no-edit`。
- `continue` は **merge のみ**。外で始まった rebase の続行は `bad_request` (GIT_EDITOR が要る経路)。中止は rebase も受ける。
- `resolve` はトランザクションを通さない (競合ファイルだけを書き換える = 監視 → ReloadHub の通常経路で拾える)。
- **競合したシーンは JSON として壊れているので起動時に読めず、エディタは空シーンで開く**。`SaveCurrentScene` 先頭の `IsConflictedPath` (actor edit のパスも対象) が「空シーンを競合ファイルへ上書き」を止める唯一の砦。
- 競合の入口は 2 つ: pull の応答 (ConflictScan) と、起動時に外の git で競合していた場合 (status の `mergeInProgress` → 自動で `conflicts`)。

**ゲートの阻害要因 (列挙型 `GateBlocker`)**: `SceneDirty` / `ActorEdit` / `AnimationDirty` /
`ControllerDirty` / `MixerDirty` / `ProjectSettingsDirty` / `Playing` / `NetActive` / `BuildRunning` /
`ScriptBuildRunning` / `OpInFlight` / `MergeInProgress` / `ServiceUnavailable`。全件を列挙して返す (最初の 1 件で止めない)。

**アクティブシーンがブランチ側で削除された**: 空シーン (`NewScene` 経路) + トースト。`lastScenePath` は消す。

**外部 git 操作の検知** (後回し扱いの最小): 監視は `.git\HEAD` `.git\index` `.git\refs\**`
`.git\MERGE_HEAD` `.git\rebase-merge\` を含め、変化 → `status` 再取得 + `repo_changed{head}` 通知。
HEAD が変わっていたらトースト「外部で HEAD が移動しました。再起動を推奨」。

**背景 fetch と認証** (R2 で確定): 定期 / 起動時 fetch は `GIT_TERMINAL_PROMPT=0` + `GCM_INTERACTIVE=never`
で実行し、失敗は**同じ `error.code` が続く間は 1 回だけ**通知。ユーザーが押した push / pull / fetch は
GCM の GUI を許す (`GIT_TERMINAL_PROMPT=0` のみ)。全 git 呼び出しは `LC_ALL=C`
(エラー文の照合を安定させる) + `-c core.quotepath=false -c color.ui=false --no-pager`。
★実測 (sub-06、GCM 2.x / git 2.48.1、CreateNoWindow + stdio リダイレクトの孫プロセス): `GIT_TERMINAL_PROMPT=0` **だけでは GCM の GUI が出る** (GCM は端末プロンプトの可否としか読まない)。`GCM_INTERACTIVE=never` を足すと `fatal: Cannot prompt because user interactivity has been disabled.` で止まる。= 背景 fetch から `GCM_INTERACTIVE=never` を外さない / ユーザー操作側には付けない (付けると初回認証が一切できない)。

**リモート周り (sub-06 round 1 で確定)**:
- `remote_state` は `{hasRemote, upstream, ahead, behind, commits[]}`。`hasRemote=false` なら Push を塞ぐ (他に判断材料が無い)。
- `fetch` / `pull` / `push` の応答に `remote` (実行後の remote_state) を載せる。pull は checkout と同型 `{head, names, status, remote}`。
- **Pull ボタンは `behind > 0` のときだけ有効** (ツールチップ「先に Fetch」)。未 fetch の pull は予測 `HEAD..@{u}` が必ず空で確認モーダルの意味が消える。起動時と定期 fetch があるので実用上は困らない。「Pull が内部で fetch する」は v1.5。
- `push{setUpstream}` は省略可 (既定 false)。upstream が無ければサービスが自動で `-u origin <branch>` を張るので UI に口は作らない。引数は削らない (フィールド削除 = bump 対象)。
- `non_fast_forward` の `detail` はサービス側の固定文 (git 原文は短縮 SHA と版依存の hint を含む)。競合は git が **stdout** に書くので pull 専用の分類 (`classify_pull_failure`) を通す。
- 定期 fetch は worker のタイマー (`recv_timeout(1 s)` + `handle_tick`) でスレッドを増やさない。**`handle_tick` は待つ前に毎周回呼ぶ** (timeout の枝だけだと 1 s より短い間隔でメッセージが届き続けるとタイマーが飢える。実機で 2 分半 fetch が走らなかった)。
- 背景 fetch の失敗トーストは `error.code` のみ (detail はログ)。同じ code が続く間は 1 回。

**op 一覧 (v1 で凍結)**: `hello` / `repo_check` / `status` / `stage` / `unstage` / `commit` / `log` /
`diff` / `identity_check` / `revert` / `diff_names` / `branches` / `branch_create` / `checkout` /
`fetch` / `pull` / `push` / `remote_state` / `merge_abort` / `conflicts` / `resolve` / `continue` /
`hint_changed`。通知: `status_changed` / `remote_changed` / `repo_changed` / `service_error`。
**`error.code` 一覧 (v1)**: `not_repo` / `toplevel_mismatch` / `git_missing` / `git_too_old` /
`identity_missing` / `local_changes_overwritten` / `locked_index` / `locked_file` / `auth_failed` /
`non_fast_forward` / `conflict` / `merge_in_progress` / `nothing_to_commit` / `network` /
`internal_panic` / `service_dead` / `bad_request` / `git_failed` (未分類、`detail` に stderr)。

### 4.2 データ・保存形式・互換性

- `project.mye.json` に `canonicalRoot` (文字列、任意) を追加。`ProjectManifest` 構造体にも持たせ、
  `SaveProjectManifest` (ProjectManager のリネームが呼ぶ) で**失われない**こと。書き手は
  `CreateProject` (作成時のパス) と Source Control 窓の「このパスを正にする」。
  比較は `NormalizePathKey` 同士。`formatVersion` は現状どおり 1。
- `EditorSettings` に `scmAutoFetch` (bool, true) / `scmFetchIntervalMin` (int, 5) /
  `particleCompareMode` / `particleCompareOffsetX` / `particleCpuSimd` (M66h)。旧 JSON は `value(key, default)` で読める。
- `.rep` / snapshot / SceneSerializer / TypeId / ABI (`EngineAPI.h`): **一切触らない**。
- JSON: UTF-8、パスは toplevel 相対 `/` 区切り。C++ 側で `Utf8ToWide` → 絶対化 → `NormalizePathKey`。
  260 文字超のパスは v1 非対応 (エラーはそのまま表示)。
- proto 版: Rust `pub const PROTO_VERSION: u32 = N;` (`tools\collab\src\protocol.rs`) と C++
  `constexpr int kCollabProtoVersion = N;` (`src\Editor\SourceControl\CollabProtocol.h`) を
  `check_rules.ps1` の `$constGroups` で機械照合。
  **bump の規則** (sub-02 round 1 で確定): C ABI の関数を増減・改変したとき / JSON のフィールドを**削除・改名・意味変更**したとき /
  `error.code` や event 名を削除・改名したとき。**フィールドの追加は bump しない** (C++ は `value(key, default)` 読みで前方互換)。
  op の追加も bump しない (未知 op は `bad_request` で返る = 実行時に分かる)。

### 4.3 UI / ビジュアル

元計画どおり (Changes / Branches / History タブ、コミット欄、競合一覧、利用不可時の理由)。
色は `themeColor::*` のみ。文言は `LocalizationTable.inl` en/ja、`###SourceControl*` の ID 一意。
利用不可の理由は列挙型 `Unavailable::{NoProject, NoService, ProtoMismatch, NoGit, GitTooOld, NotRepo,
ToplevelMismatch, ServiceDied}` → `Tr()`。裸起動 (プロジェクト無し) は `NoProject`。
`error.code` → `Tr()` キーの表を C++ が持ち、未知 code は生文字列を `Text("%s")` で出す。
**窓の既定表示** (sub-02 round 1 で確定、sub-03 round 1 で訂正): プロジェクト起動では**毎回開く**、裸起動では閉じる。
ImGui の ini は `p_open` を保存しない (保持するのは名前付きレイアウトの `panels.json` だけ) ので「選択を保持する」は誤りだった。
**既定ドック** (sub-03 round 1 で変更): 下段帯 (Assets 束、高さ ≒ 200 px) では Changes 一覧が 2 行で切れコミット欄が見えない
(`cache\scm_m66c3.png` で実測) → **左列 (Hierarchy 束) のタブ**に移す。差分は inline ペインではなく**別の dockable 窓「Diff」**
(選択時に開く、読み取り専用) — 元計画 M66c の「読み取り専用の子窓」に戻す。実装は sub-05。ユーザーの手触りで再調整可。

### 4.4 非機能

- **決定論**: Editor 層のみ。`replay_verify.bat` は全サブで無変更緑。`src\Engine` / `src\Runtime` /
  `src\GameLogic` / `src\Shared` から `Editor/SourceControl/` を include しないことを規則 12 で機械検査。
- **collab_verify の決定論**: 一時リポは `git init -b main`、`-c user.name=mye -c user.email=mye@example.com`、
  `GIT_CONFIG_GLOBAL=<空ファイル>` + `GIT_CONFIG_NOSYSTEM=1` (開発者の global 設定・hook・gpgsign を遮断)、
  `core.autocrlf=false`。期待 NDJSON との比較は SHA / 日時 / author を `<sha>` `<time>` `<author>` に正規化してから。
- **CI**: ci.yml に 1 ステップ追加 (rust-toolchain + `cargo test` + `build_collab.bat` + `collab_verify.bat`)、
  managed host の後・replay の前。環境変数 `MYE_COLLAB_REQUIRED=1` を CI env に追加 (selftest の DLL 往復を
  SKIP ではなく失敗にする。ローカルで DLL 不在なら SKIP + ログ 1 行)。既存 6 ステップと環境変数 4 種は不変。
- **タイムアウト**: `hello` 5 s、読み取り系 30 s、書き込み系は無期限 (キャンセル無し)。
- **collab_verify のシナリオ形式** (sub-01 で確定): `tests\collab\NN_*.ndjson`。行頭 `#` はコメントまたはディレクティブ
  (`# write <relpath> <text>` / `# delete <relpath>`)。ディレクティブの位置で CLI は終了して次行から起動し直す
  (走行中プロセスへ変更を伝える経路を作らない)。`--update` は期待ファイルを今の出力で上書きするので、**中身を読んでからコミット**。
  `# git <args>` (fixture の前提条件専用。identity 未設定など) も使える。ディレクティブ判定は `'#' + 半角空白 1 個 + 動詞`。
  期待ファイルに git の案内文 (`use "git restore ..."`) や機体名を含む fatal を入れない (版・機体依存)。
  期待ファイルは serde_json 既定のキー順 (辞書順)。fixture のパスと中身は ASCII に限る (cmd 経由の stdout はコンソール CP で復号される)。
- **CLI (script モード) は決定的でなければならない**: 応答は要求順、`event` 行は**出さない** (watcher と定期 fetch を起動しない。
  `create` 経由 = DLL では起動する)。通知の検証は cargo test の単体側で行う (sub-02 以降)。
  `hint_changed` は通知ではなく**応答に status を載せて返す** (同じ理由。C++ は監視経路と同じ `ApplyStatusResult` へ流す)。
- **fixture はシナリオごとに作り直す** (sub-02 で確定): 共用すると N 本目の期待が 1..N-1 本目の実行結果に依存する。+1 s/シナリオは許容。
- `Cargo.lock` は版管理対象 (cdylib / bin を配る crate。再現ビルドのため)。
- `status` は `--untracked-files=all` (バッジがファイル単位。既定の normal だと未追跡ディレクトリが畳まれる)。
- **CLAUDE.md**: 「43 スイート」→ 44、検証表に `collab_verify.bat`、ビルド節に `build_collab.bat` + rustup 前提 (M66j)。

## 5. 受け入れ条件

| # | 条件 | 検証手段 |
|---|---|---|
| 1 | `cargo test` 緑: porcelain v2 fixture (M / A / D / R / ? / 競合 u) の解析、`diff_names` 解析、要求→応答の往復、`error.code` 分類 | `cd tools\collab && cargo test` |
| 2 | `collab_verify.bat` 緑: fixture リポで status → stage → commit → log → branch_create → 変更 → checkout → diff_names → bare origin 2 clone (push → 相手 behind=1 → pull) → 競合 → conflicts → ours → continue の期待 NDJSON (正規化後) 一致。**エディタ不要** | `tools\collab_verify.bat` |
| 3 | `check_rules.ps1` 緑 + 規則 12 (Editor 層封じ込め) + proto 版 `$constGroups` が食い違いで赤くなる | `pwsh -File tools\check_rules.ps1` (故意に版をずらして赤を 1 回観測) |
| 4 | `--selftest` 緑 + `RunSourceControlSelfTest`: (a) 偽トランスクリプトの解析 (b) DLL 往復 hello (DLL 不在は SKIP、`MYE_COLLAB_REQUIRED=1` なら失敗) (c) 対の束ね (`.meta` 欠落 → EnsureMeta 判定) (d) フォルダ集約 (e) ゲート阻害要因の全列挙 (f) `EndBatch` の適用順と D の非リトライ (g) `.gitignore` テンプレ行 (h) `EditorSettings` 新キーの往復 (i) `canonicalRoot` の保存往復 | `bin\x64\Debug\Editor.exe --selftest` (両構成) |
| 5 | `replay_verify.bat` **無変更緑** (全サブ) | `tools\replay_verify.bat` |
| 6 | `shot_verify.bat` 無変更緑 (M66h / M66i) | `tools\shot_verify.bat` |
| 7 | 実機 (fixture): 窓に status が出る / `MyeCollab.dll` を消して起動 → `NoService` 表示 + 他機能無傷 / 裸起動 → `NoProject` | 目視 (fixture は `tools\collab_fixture.ps1`) |
| 8 | 実機: commit が `git log` に載る / dirty なら「保存してコミット」誘導 / identity 未設定なら案内 | 目視 + `git log` |
| 9 | 実機: Play 中 / dirty / Animation 窓未保存で書き込み系ボタン無効 + 理由ツールチップ / revert 後にシーン開き直し & 非 dirty | 目視 |
| 10 | 実機: ブランチ切替 A (テクスチャのみ違う → 再起動なしで絵が変わる) / B (scene が違う → 開き直し) / C (schemas が違う → 再起動案内) / ローカル変更と衝突 → `local_changes_overwritten` 一覧 | 目視 3+1 種 |
| 11 | 実機: 起動時 fetch のトースト / push → 相手クローンで behind=1 / 非 ff push → 「先に pull」 | 目視 |
| 12 | 実機: 競合中は他の書き込み系無効 / abort / ours / theirs → continue で解消 | 目視 |
| 13 | `ParticleSelfTest`: compare / simd を変えても `project_settings.json` 不変。`--particle-backend` の非書き戻しは不変 | `--selftest` + `shot_verify.bat` |
| 14 | AssetBrowser バッジ (M / A / D / R / ? / 競合、フォルダ集約) + 保存直後の `hint_changed` | 目視 + selftest 4(d) |
| 15 | ドキュメント: `engine_spec.md` §9 行 + 新節「§14 Source control」+ 11.2 に規則 11 / 12、README、CLAUDE.md、ADR-015 | 目視 (reviewer が読む) |
| 16 | `--package` 出力に `MyeCollab.dll` / `MyeCollabCli.exe` / `.git` が無い | ci.yml の package contents ステップに否定検査 1 行 |
| 17 | エンジンリポの既存ホットリロード (hlsl / png / 外部編集 scene) が batch 外で従来どおり動く | 目視 3 種 (M66d) |

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | M66a: Rust cdylib + CLI + fixture + collab_verify + CI + 規則 12 + **DLL 往復の縦切り** | なし | 1, 2 (status のみ), 3, 4(a)(b), 16 | `M66a: MyeCollab.dll (Rust cdylib) と検証ハーネス — hello が Editor から往復する` |
| sub-02 | M66b: CollabClient 完成 + SourceControlState + Source Control 窓 (読み取り専用) + canonicalRoot | sub-01 | 4(a)(d)(i), 7 | `M66b: Source Control 窓 (読み取り専用) — status が対で束なって見える` |
| sub-03 | M66c: stage / unstage / commit / History / diff / identity | sub-02 | 2 (commit/log), 4(c), 8 | `M66c: stage → commit → History — .meta と .terrain.edit は本体と一体` |
| sub-04 | M66d: ReloadHub Begin/EndBatch + ゲート + 4 窓の HasUnsavedChanges + トランザクション + revert | sub-03 | 4(e)(f)(h), 5, 9, 17 | `M66d: 書き込みトランザクション — 全文書保存済みかつ何も実行中でないときだけ revert が通る` |
| sub-05 | M66e: Branches — 一覧 / 作成 / checkout + 段階の事前判定 | sub-04 | 2 (branch/checkout/diff_names), 10 | `M66e: ブランチ切替 — 段階 A/B/C を先に判定してから checkout` |
| sub-06 | M66f: fetch / pull / push + 定期 fetch + 通知 + EditorSettings | sub-04 | 2 (2 clone), 11 | `M66f: fetch / pull / push — 起動時と 5 分ごとに fetch してトースト` |
| sub-07 | M66g: 競合 — abort / ours / theirs / continue | sub-06 | 2 (競合), 12 | `M66g: 競合は abort か ours/theirs — 競合中は他の書き込み系を止める` |
| sub-08 | M66h: .gitignore テンプレ 4 行 + project_settings の個人設定分離 | sub-03 | 4(g), 6, 13 | `M66h: 衛生 — .gitignore テンプレと project_settings.json の個人設定分離` |
| sub-09 | M66i: AssetBrowser バッジ + 保存ヒント | sub-02 | 14 | `M66i: Content Browser に Git バッジ — 保存直後にヒントを送る` |
| sub-10 | M66j: spec / README / CLAUDE.md / 規則の記載 | sub-01〜09 | 15 + 全検証緑 | `M66j: 仕上げ — engine_spec §14 Source control / README / CLAUDE.md` |

依存が一方向であることの注記: sub-05 と sub-06 は互いに独立 (両方 sub-04 待ち) だが
`SourceControlWindow.cpp` と Rust の `ops.rs` を両方触るので、並列にするなら司会がマージ順を決める。
sub-08 / sub-09 は sub-05〜07 と独立。

## 7. 未決事項・リスク

- S5 の登録方式: `ScanAndSync` は 3 表を `clear()` してから再走査する (`AssetDatabase.cpp:250-278`) ので
  再実行の安全性が不明。coder が sub-04 で「増分登録 (`.meta` を読んで GUID 表へ追加)」と
  「`ScanAndSync` 再実行」を比較し、選んだ方を実装メモに残す。
- B 段階の `.controller.json` / `.terrain.json`: ライブラリ側にキャッシュ無効化 API があるか未確認 (coder が sub-04 で確認。無ければ C へ格上げ)。
- `.terrain.edit` に `.meta` が付くか (`ClassifyPath` の分類) は未確認 → 対の束ねは「存在するサイドカーだけ」を集める実装にして依存しない。
- ~~Windows の GCM が GUI 無しプロセスの子孫から GUI を出せるか~~ → **閉じた** (sub-06: 出る。`GCM_INTERACTIVE=never` でのみ止まる。§4.1 参照)。
- ~~`--porcelain=v2` の下限 git 2.11 は記憶ベース~~ → **閉じた** (sub-01 round 1: v2.11.0 の `Documentation/git-status.txt` に "Porcelain Format Version 2" と `# branch.ab`、v2.10.0 には無い。出典 URL は `tools\collab\src\git.rs` の定数コメント)。
- Rust cdylib と MSVC CRT: Rust は自前のアロケータで文字列を返すので `mye_collab_free` 以外で解放しない (境界の規則、sub-01 のヘッダコメントに書く)。
- ~~S5 の登録方式~~ → **閉じた** (sub-04: 増分登録 `AssetDatabase::GuidForPath(abs, createIfMissing=true)`。`ScanAndSync` は 3 表を clear するので解決先が一瞬空になる窓が開く)。
- ~~B 段階のキャッシュ無効化 API~~ → **閉じた** (sub-04: `ControllerLibrary::LoadFromFile` は同 hash で差し替え、`RenderSystem::InvalidateTerrain()` が public、`InputActions::Load(root, true)` / `PhysicsLayerNames`・`PartTagNames::Load(root, true)`。C への格上げ不要)。
- ~~`StartGameLogicBuild` の呼び出し元~~ → **閉じた**が穴が 1 本: 呼び出し元は `BuildSettingsWindow::AdvancePipeline` のみ。ただし Asset Browser の [Rebuild Scripts] = `AssetOps::RebuildGameLogic` (`AssetOps.cpp:1447`) は `ShellExecuteW` の fire-and-forget で**ハンドルを持たない** → その間 `ScriptBuildRunning` が立たない。→ **sub-05 で塞いだ** (`EditorApp::PollScriptBuild`、`RebuildGameLogic` は削除)。
- DLL 内で本当に panic させる経路は作らない (sub-04 の質問 1 = (a))。`catch_unwind` → `service_error` → `ServiceDied` → ゲート閉鎖の C++ 側は通知行の注入で実走済み、Rust 側は cargo test (service.rs) の panic 注入で実走済み。残るのは C ABI ラッパ 1 段だけで、隠し op を足すほどの価値は無い。
- `kReloadRetryMax = 60` の WARN は実機で発火させていない (共有違反を 60 フレーム続ける再現手段が無い)。カウントの配線はコードレビューで確認 (`ReloadHub.cpp` の `retryAttempt_` 引き継ぎ)。
- `kCollabMaxBatchApply = 200` と `kReloadRetryMax = 60` は R2 で確定した初期値。実機で不便なら定数だけ変える (仕様変更として §8 に積む)。
- 競合中のアクティブシーン: 競合マーカー入り JSON は開けない (決定 9)。競合中はアクティブシーンが競合一覧にある間 **Save を止める** (保存 = マーカーを潰して「ours」を黙って選ぶのと同じ)。詳細は sub-07。
- `pull` の非 ff: 既定 `--ff-only` で `non_fast_forward` を返し、UI が「マージして pull」を提示 → `pull{allowMerge:true}` (`--no-rebase`)。競合は §4.1「競合周り」。
- マージ中に pull を投げると `git_failed` + 生文 ("Pulling is not possible because you have unmerged files.")。UI ではゲートで到達しないが `merge_in_progress` へ分類する (sub-08 の衛生)。

## 8. 変更履歴

(確定後の変更のみ。出所と理由を書く)

- 2026-09-02 (sub-01 round 1、coder SELF_EVAL): §4.4 に collab_verify のシナリオ形式 (`# write` / `# delete` / `--update`) を追記 — 仕様が「ファイル変更 → status M」を要求しながら変更手段を定義していなかった (仕様の欠落)。
- 同上: §4.4 に「CLI (script モード) は event 行を出さない / watcher・定期 fetch を起動しない」を追記 — coder の CLI は「EOF 後 200 ms drain」で終了するため、sub-02 で通知が入ると出力が非決定になる。先に仕様で封じる。
- 同上: §4.4 に `Cargo.lock` 版管理対象、`status --untracked-files=all` を追記 (coder の追加を仕様として採用)。
- 同上: §7 の `--porcelain=v2` 下限の未決を閉じた (一次情報で確認済み)。
- 同上: `build_collab.bat` の cargo 不在時は **exit 1 のまま** (sub-01 受け入れ条件 2)。司会の申し送り「WARN + exit 0」は採らない — `tools\build_managed.bat:10` も dotnet 不在で `exit /b 1` であり、この bat は sln ビルドから呼ばれない (明示的に叩いたときだけ走る) ので、rustup 未導入の開発者が巻き込まれる経路は無い。CI は失敗にしたい。
- 2026-09-03 (sub-02 round 1、coder SELF_EVAL): §4.1 に対の規則の確定版 — `.terrain.edit` は `x.terrain.json` の `.json` を `.edit` に差し替えた名前 (`TerrainEdit.cpp:469`)。sub-02 / sub-03 の「`X` + `X.terrain.edit`」は planner の誤り。フォルダ集約は「最も重いもの」に一般化 (`{D, ?}` → D。削除が折り畳みに隠れない)。
- 同上: §4.2 に PROTO_VERSION の bump 規則 (フィールド追加は bump しない)。status 結果への `head` 追加は据え置きで正しい。
- 同上: §4.3 に窓の既定表示 (プロジェクト起動で開く / 裸起動で閉じる) — coder の質問 6 への裁定。実装は sub-03 (should)。
- 同上: §4.4 に `hint_changed` は応答で status を返す (CLI の決定論を優先)、fixture はシナリオごとに作り直す。
- 2026-09-03 (sub-03 round 1、coder SELF_EVAL): §4.1 に「commit 周り」を新設 (保存してコミット = 保存 → 対で stage → commit / identity 未設定はコミットボタン無効 / unstage は `reset -q` / diff 256 KB 打ち切り / 書き込み系の応答に status)。「保存 → commit」は保存前の index をコミットする = 仕様の誤り。identity は「git が機体名で補完して成功する」実測から塞ぐ側に裁定。
- 同上: §4.3 の「imgui.ini が選択を保持」は誤り (p_open は保存されない) → 「毎回開く」に訂正。既定ドックを下段帯から左列 (Hierarchy 束) へ、差分を別窓「Diff」へ (sub-05 で実装)。根拠 = `cache\scm_m66c3.png` で一覧が 2 行で切れる。
- 同上: §4.4 に `# git <args>` ディレクティブと期待ファイルの禁則 (案内文・機体名)。
- 2026-09-03 (sub-04 round 1、coder SELF_EVAL): §4.1 B 行に `src/GameLogic/Scripts/` 配下の `.h .hpp .inl` を追加 (ヘッダだけの変更で DLL が古いまま進むのを防ぐ。coder の追加を採用)。C 行に guid 変化の検出方式 (実行前後の比較) を明記。
- 同上: §4.1 C 行を「モーダルは『再起動』のみ」に確定 — coder が置いた「あとで」は、スキーマ不一致のまま保存できる経路を開き、C を設けた理由 (未知コンポーネント読み飛ばしを保存前に止める) を無効化する。must で差し戻し。
- 同上: §3 後回しに「.cs / .cpp だけの B で開き直しを省く」を v1.5 として記録 (質問 3)。§7 の未決 3 件 (S5 / B の無効化 API / StartGameLogicBuild) を閉じ、Rebuild Scripts の fire-and-forget をゲートの穴として sub-05 に割り当て。panic 注入の隠し op は作らない (質問 1 = a)。
- 同上: revert の未追跡削除は `git clean -q -f` (`-d` 無し = ディレクトリごと消さない)、段階 B で EndBatch に渡すのは A 集合のみ、段階 C の再起動前に `EndBatch({})` — いずれも coder の実装を仕様として採用。
- 2026-09-03 (sub-04 round 2、coder SELF_EVAL): §4.1 C 行に再起動失敗時の振る舞い (モーダルを閉じず赤字案内、`Hooks::relaunch` は bool を返す契約) を追記。coder の追加を採用 — 3 択 (閉じる / 黙って exit / 閉じずに知らせる) のうち唯一「食い違ったまま編集できる状態」を作らない。
- 2026-09-03 (sub-05 round 1、coder SELF_EVAL): §4.0 に「Shutdown は destroy のみ、FreeLibrary は呼ばない」— notify の Drop が join しない一次情報 (windows.rs:590-596) と終了時クラッシュの実測。coder の判断を採用。
- 同上: §4.1 に「ブランチ周り」を新設 (応答に names / 未出生は全部 A / branch_create はゲート外 / detail 固定文 / -t / guid / Rebuild Scripts の一本化)。旧 RebuildGameLogic の可視 cmd 窓 (失敗時 pause で読めた) が消える代わりに、失敗時のエラー行を Console へ流す should を sub-08 に割り当て。
- 同上: §4.3 の既定ドック (左列) と Diff 別窓は sub-05 で実装済み (`cache/scm_m66e_layout.png`)。§7 の Rebuild Scripts の穴を閉じた。
- 2026-09-03 (sub-06 round 1、coder SELF_EVAL): §4.1 に GCM の実測 (GIT_TERMINAL_PROMPT=0 だけでは GUI が出る) と「リモート周り」を新設 (hasRemote / 応答に remote / Pull は behind > 0 / setUpstream 省略可 / detail 固定文 / 競合は stdout / タイマーは毎周回 / 失敗トーストは code のみ)。質問 1〜3 はすべて coder の判断を採用。§7 の GCM 未決を閉じた。
- 2026-09-03 (sub-07 round 1、coder SELF_EVAL): §4.1 に「競合周り」を新設 (resolve は paths[] / モードで側を決める / conflicts が merged[] と 7 種別を返す / status に merge フラグ / 競合 pull は ConflictScan / abort・continue は pull と同型 / continue は merge のみ / resolve はトランザクション外 / 競合シーンは空で開くので保存ガードが唯一の砦)。元の「pull の応答に mergedPaths」は conflicts op へ移した (coder の判断を採用)。既存の期待 NDJSON 7 本の撮り直しは 2 キー追加だけであることを diff で機械確認。質問 1 (merge_in_progress 分類) は sub-08 へ、質問 2 (Scm_ComingSoon) も sub-08 へ。
