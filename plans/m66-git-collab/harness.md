# harness 台帳: m66-git-collab

- 依頼原文: "C:\HAL\MyEngin\plans\quiet-merging-harbor.md"を読んで計画に穴が無いかを確認したのち実装をしたい
- 開始: 2026-09-02 / 基点コミット: 02bf3c924a6b5036255f5e31a694b4bd8c054055
- フェーズ: 実装
- 元計画: `plans/quiet-merging-harbor.md` (M66: エンジン内 Git 連携 v1 — 別プロセス Rust サービス MyeCollab + Editor 層クライアント。ユーザー作成、327 行)
- 仕様書: `spec.md` 確定 (2026-09-02)。穴 13 件 (S1〜S13) を §2 に記録、受け入れ条件 17 件 (§5)、サブ 10 本 (§6、元計画 M66a〜M66j に 1:1)
- 策定の往復: R1 (4 問: Rust 前提 / 段階 C 判定 / 未コミット変更 / 未保存ガード 4 窓) → R2 (3 問: DLL 解釈 / 仮置き 6 点 / 確定)
- ユーザー判断で押し切られた点: 実装形態を「別プロセス exe」(元計画 決定 1) から「in-process Rust cdylib」へ反転。planner の留保 = メモリ隔離が消える (Rust の未定義動作はエディタごと落ちる)。spec §4.0 に「safe Rust 限定 / unsafe は FFI 境界だけ / panic は catch_unwind で service_error 化」と記録し、ADR-015 に経緯を書く (sub-01)。以降は蒸し返さない。

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 4716d6a | M66a: Rust cdylib + CLI + fixture + collab_verify + CI + 規則 12 + DLL 往復の縦切り。依存: なし。VERDICT round 1 OK (should 1 → sub-02、nit 3 → sub-02 / sub-10) |
| sub-02 | OK | 1 | 165cfc6 | M66b: CollabClient 完成 + SourceControlState + Source Control 窓 (読み取り専用) + canonicalRoot。依存: sub-01 |
| sub-03 | OK | 1 | 83d0b4f | M66c: stage / unstage / commit / History / diff / identity。依存: sub-02 |
| sub-04 | OK | 2 | 205e90d | M66d: ReloadHub Begin/EndBatch + ゲート + 4 窓の HasUnsavedChanges + トランザクション + revert。依存: sub-03。round 1 REWORK (must 1 = 段階 C モーダルの「あとで」撤去。should 2 件 → sub-05、nit 1 件 → sub-10) |
| sub-05 | OK | 1 | c3be569 | M66e: Branches — 一覧 / 作成 / checkout + 段階の事前判定。依存: sub-04 |
| sub-06 | OK | 1 | 1e6b8f8 | M66f: fetch / pull / push + 定期 fetch + 通知 + EditorSettings。依存: sub-04 (sub-05 と独立。並列なら SourceControlWindow.cpp / ops.rs のマージ順を司会が決める) |
| sub-07 | OK | 1 | 3c0bf56 | M66g: 競合 — abort / ours / theirs / continue。依存: sub-06 |
| sub-08 | OK | 1 | (M66h、下の記入待ち) | M66h: 衛生 — .gitignore テンプレ 4 行 + project_settings.json の個人設定分離。依存: sub-03 (sub-05〜07 と独立) |
| sub-09 | 実装中 | 0 | | M66i: Content Browser に Git バッジ + 保存直後のヒント。依存: sub-02 (sub-03〜08 と独立) |
| sub-10 | 未着手 | 0 | | M66j: 仕上げ — engine_spec §14 Source control / README / CLAUDE.md / 規則の記載。依存: sub-01〜sub-09 |

## 未決事項 (planner PLAN_RESULT より。coder が「不安・質問」で拾う)
- ~~S5 の新規アセット登録方式~~ → **閉じた (sub-04)**: 増分登録 (`AssetDatabase::GuidForPath(abs, createIfMissing=true)`)。`ScanAndSync` は 3 表を clear するので解決先が一瞬空になる窓が開く。増分なら `.meta` 同梱の guid をそのまま採る
- ~~B 段階の `.controller.json` / `.terrain.json` のキャッシュ無効化 API~~ → **閉じた (sub-04)**: 両方ある (`ControllerLibrary::LoadFromFile` の差し替え / `RenderSystem::InvalidateTerrain()`)。C への格上げ不要。無効化の置き場は `GitTransaction::ApplyStageB` の 1 箇所
- `.terrain.edit` に `.meta` が付くか (`ClassifyPath`) — sub-03 は「存在するサイドカーだけ束ねる」実装で依存しない
- ~~GUI 無しの孫プロセス git から GCM のダイアログが出るか~~ → **閉じた (sub-06、実測)**: `GIT_TERMINAL_PROMPT=0` **だけでは GCM の GUI が出る** (GCM は端末プロンプトの可否としか読まない)、`GCM_INTERACTIVE=never` でのみ止まる。仮置き 3 はそのまま成立 → 背景 fetch から外さない / ユーザー操作側に付けない (付けると初回認証ができない)。手順は `git.rs` の `run_background` のコメント
- ~~`--porcelain=v2` の下限 git 2.11 は記憶ベース~~ → **閉じた (sub-01)**: 2.11.0 で正しいことを一次情報で確認、出典 URL は `tools\collab\src\git.rs` の定数コメント
- ~~`StartGameLogicBuild` の全呼び出し元~~ → **閉じた (sub-04)**: `BuildSettingsWindow::AdvancePipeline` の 1 箇所のみ、同期経路なし。**ただし Asset Browser の [Rebuild Scripts] (`AssetOps::RebuildGameLogic`) は `ShellExecuteW` の fire-and-forget でハンドルを持たず、ゲートから観測できない** → **閉じた (sub-05)**: `RebuildGameLogic` を削除し `StartGameLogicBuild` 1 本に寄せて EditorApp がハンドルを保持、`GateInputs.scriptBuildRunning` に OR。退行 (可視 cmd 窓でコンパイルエラーが読めた) は sub-08 の should で Console へ error 行を流して埋める
- `kCollabMaxBatchApply = 200` / `kReloadRetryMax = 60` は初期値。実機で不便なら仕様変更として spec §8 に積む
- ~~Rust cdylib が実際に Editor.exe から LoadLibrary で往復するかは策定時点で未検証~~ → **閉じた (sub-01)**: Debug / Release とも Editor.exe が MyeCollab.dll をロードして JSON が往復し git 版が返る (selftest で恒常検査)
- ci.yml の GitHub 上の実走は**未検証** (push 後の最初の run で見る。割れやすいのは `dtolnay/rust-toolchain` の所要時間と cmd の `&&` 連鎖の終了コード伝播)

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- **rustup (stable) はユーザー側の前提** (2026-09-02 R2 で確定)。この機体には cargo / rustup が無い (策定時に `where` で確認)。
  coder は環境を変えない — sub-01 着手時に `cargo --version` が通らなければ実装せず「不安・質問」で司会へ返す。
- 実装形態は元計画の「別プロセス exe」ではなく **in-process Rust cdylib `MyeCollab.dll`** (spec §4.0、ユーザー判断 R1/R2)。
  元計画 `plans/quiet-merging-harbor.md` の M66a / M66b の spawn / Job Object / stdio の記述は読まない (spec と sub-01/02 が正)。
- 実機目視はエンジンリポの裸起動では**できない** (Collab は `--project` 起動のみ)。`tools\collab_fixture.ps1` (sub-01) で作る一時プロジェクトを使う。
- `replay_verify.bat` は全サブで無変更緑が必須 (sim 不変の証明)。割れたら「記録中に窓を触った」を先に除外 (memory: replay-verify-triage)。
- **rustup 導入済み (2026-09-02、司会が winget でユーザー承認のうえ実行)**: rustup 1.29.1 / stable-x86_64-pc-windows-msvc / cargo・rustc 1.98.0。
  司会が scratchpad で cdylib のプローブ (`#[unsafe(no_mangle)] extern "C"`) を `cargo build --release` → DLL 生成 + dumpbin でエクスポート確認済み = MSVC リンカまで通る。
  - **罠 1**: `~/.cargo/bin` はユーザー PATH に入ったが、Claude Code のツールシェル (PowerShell / Bash) は起動時の環境を引き継ぐので **`cargo` が PATH に無い**。
    コマンドの先頭で `$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"` (bash なら `export PATH="$USERPROFILE/.cargo/bin:$PATH"`) を付けること。
    `tools\build_collab.bat` は PATH の cargo → 無ければ `%USERPROFILE%\.cargo\bin\cargo.exe` の順に解決する (sub-01 で実装済み)。
    **どちらも無ければ exit 1** — 司会が当初「WARN + exit 0 の縮退」と書いたが planner が退けた (`build_managed.bat` も dotnet 不在で exit 1、この bat は sln ビルドから呼ばれないので未導入者を巻き込まない、CI は失敗にしたい。spec §8 に記録)。
  - **罠 2**: `cargo new` の既定 edition は **2024** — `#[no_mangle]` は `#[unsafe(no_mangle)]` と書かないとエラー (unsafe attribute)。
  - **罠 3**: MSVC リンカが「ライブラリ ... .dll.lib とオブジェクト ... .dll.exp を作成中」を stdout に出し、rustc が `linker_messages` lint の **warning** として拾う (無害)。
    CI で `RUSTFLAGS=-D warnings` 相当を立てるとこれがエラーになるので、`#![allow(linker_messages)]` か lint 個別指定で逃がすこと。
- **sub-01 (M66a) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 入口: `CollabClient::Load(GetExecutableDir())` → `Create(WideToUtf8(projectRoot))` → 毎フレーム `Poll()`。**まだどこからも呼ばれていない** (配線は sub-02)。`Unavailable` 列挙は `CollabProtocol.h` に spec §4.3 の 8 値ぶん。
  - op を足す = `tools\collab\src\ops.rs::dispatch` の match に 1 行 + `CollabProtocol.h` の `collabop` に定数 1 行。**C ABI は増やさない** (増やすなら PROTO_VERSION と kCollabProtoVersion を同時に上げる = 規則 9 が止める)。
  - collab_verify のシナリオ追加 = `tests\collab\NN_*.ndjson` を置いて `--update` で期待を撮る。ファイル操作は `# write` / `# delete` ディレクティブ。**期待ファイルは必ず中身を読んでからコミット** (--update は今の出力を正にする)。期待 NDJSON のキー順は serde_json 既定の辞書順 (`preserve_order` を有効にすると全部ずれる)。
  - **CLI (script モード) は event 行を出さず watcher・定期 fetch を起動しない** (planner が spec §4.4 に追記。sub-02 で通知が入ると CLI 出力が非決定になるのを先に封じた)。
  - 実機目視: `pwsh -File tools\collab_fixture.ps1 cache\collab_fixture` → `bin\x64\Debug\Editor.exe --project cache\collab_fixture` (fixture は既存ディレクトリを拒否する)。
  - `MyeCollab.dll` はエディタ起動中に上書きできない (build_collab.bat がコピー失敗で exit 1 + "is the editor running?")。
  - edition は **2021** を採用 (2024 だと rustc 1.85 未満で `#[unsafe(no_mangle)]` が通らない)。`Cargo.lock` は版管理対象で確定。
  - planner の should (sub-02 へ): porcelain の fixture に非 ASCII パス (`core.quotepath=false` で生 UTF-8) が 1 本も無い。collab_verify は cmd 経由で ASCII 限定なので、UTF-8 パスが `entries.path` にそのまま載る検査は cargo test で。sub-02 で watch.rs を足すとき 1 本追加 (sub-02.md の「planner 追記」節)。
  - planner の nit: `CollabClient::AddPendingForTest` (テスト専用公開 API) は sub-02 で `DispatchLine` の検査経路が増えるなら friend か「Request の DLL 無し分岐」に寄せられないか検討 / ci.yml の `dtolnay/rust-toolchain@stable` ステップに `name:` が無い → sub-10 / linker stdout の warning は触らない。
  - Bash ツールのヒアドキュメント経由で PowerShell / 正規表現を patch すると**バックスラッシュが 1 段潰れる**。patch スクリプトは scratchpad にファイルとして書いてから実行する。
- **sub-02 (M66b) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 仕様確定 (planner が spec / sub-03 / sub-09 を修正済み): `.terrain.edit` の対は **`x.terrain.json` ⇄ `x.terrain.edit`** (元 sub の「`X` + `X.terrain.edit`」は planner の誤り。`TerrainEdit.cpp EditPathFor` が根拠)。束ねは `SourceControlState.cpp` の `PrimaryPathFor` を再利用し二重実装しない。フォルダ集約は `CombineState` = 最重 (`{D,?}` → D)。**PROTO_VERSION の bump 規則** (spec §4.2): C ABI の増減・改変 / フィールドの削除・改名・意味変更 / error.code・event 名の削除・改名で bump、**追加は bump しない**。`hint_changed` は応答に status を載せる (通知にしない = CLI の決定論)。
  - planner の should (sub-03 で 1 行): Source Control 窓の既定表示をプロジェクト起動で `open = true` (裸起動は false)。以後は `.mye\imgui.ini` が保持。
  - planner の nit: `SourceControlState.h` のモデル / セッション同居は sub-04 で `GitTransaction` が Session を掴むときに分割余地を見る / 監視除外集合 (`.mye / cache / dist / target`) は sub-08 の `.gitignore` 4 行と揃えるか検討。
  - `ServiceDied` の実機表示は未検証 → sub-04 で `GateBlocker::ServiceUnavailable` を検査するとき、cargo test の panic 注入経路 (service.rs) を DLL 経由でも 1 回通す。`ProtoMismatch` / `GitTooOld` / タイムアウト実発火も未検証 (文言経路は NoService と同一)。
  - op を足す = `ops.rs::dispatch` に 1 行 + `CollabProtocol.h` の `collabop` に 1 行 + **読み取り系なら `CollabOpKindOf` の表にも 1 行** (書き込み系は既定 = 無期限)。書き込み系 op は `CollabClient::OpInFlight()` が自動でゲートを閉じる (sub-04 はこれを読むだけ)。
  - `SourceControlSession::Busy()` / `Client()` / `HintChanged()` はまだ未使用 (sub-03 / sub-04 / sub-09 用の口)。
  - **ImGui の罠**: `###` の右辺は DockBuilderDockWindow / layouts の文字列と 1 バイト一致させる (`ImHashStr` は `###` でシードに戻すので ID は右辺だけ。不一致だと既定ドックが**黙って無視**される)。`ImGui::SameLine(x)` の x は窓ローカル絶対座標で `TreePush` のインデントを含まない → 行頭の `GetCursorPosX()` 基準で列を作る。新しい窓 / タブを足すサブは必ず実機で 1 枚撮る。
  - **トーストは実時間 4 秒で消えるので `--screenshot` では撮れない** → 発火の検証は INFO ログを併置する。
  - Bash ヒアドキュメントの `\\` 潰れ: `LocalizationTable.inl` の `tools\\build_collab.bat` が `\b` になり UI に `tools?uild_collab.bat` と出た。パスを含む文字列は `cat -A` か実機で確認。
  - collab_verify は fixture を**シナリオごとに作り直す** (共用だと N 本目が 1..N-1 の結果に依存)。notify は `default-features = false` (Cargo.lock +24 crate、初回ビルド 12 秒)。
  - fixture の残骸 (`cache\fixture_proj`、`cache\fixture_proj.gitconfig`、`%TEMP%\mye_notrepo_proj`) は生成物・gitignore 済み。
- **sub-03 (M66c) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 仕様確定 (planner が spec §4.1 / §4.3 / §4.4 と sub-04 / sub-05 を修正済み): 「保存してコミット」= **保存 → 保存した文書を対で stage → commit** (「保存 → commit」は保存前の index をコミットする = 元 sub の誤り)。unstage は `git reset -q --` (restore --staged は未出生ブランチで落ちる)。diff は 256 KB 打ち切り + `truncated`。書き込み系 op の応答型 `{"status": ...}` + `last_head` 更新を §4.1 で固定 (sub-04 の revert / sub-05 の checkout / sub-06 の pull・push は同じ型を使う。**`last_head` を更新し忘れると自分の操作で「外部で HEAD が移動」トーストが出る**)。spec §4.3 の「imgui.ini が選択を保持」は planner の誤り → 「プロジェクト起動では毎回開く」。新規ファイルの 5 行ヘッダは**付けない**で確定。
  - planner の should (sub-04 で): identity 未設定のときコミットボタンを**無効化** (`SourceControlWindow.cpp` の分岐に `BeginDisabled`。設定 UI は作らない)。根拠: `user.name` 未設定でも git は OS アカウント名 + 機体名で補完してコミットに成功する = 共有履歴が汚れる。
  - planner の should (sub-05 で): 既定ドックを下段帯 (Assets 束) から**左列 (Hierarchy 束) のタブ**へ移し、差分は inline ペインではなく**別の dockable 窓「Diff」**にする (高さ 200 px の帯では一覧が 2 行で切れる)。既定レイアウトのスクショを 1 枚残す。
  - planner の nit: 「窓のボタン → Session → DLL」の結線に永続テストが無い → `MYE_COLLAB_PROBE` のレシピを sub-04 の受け入れ検証で再利用。
  - **一時プローブのレシピ**: `SourceControlSelfTest.cpp` に env ガード付きブロックを足し、`MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` で Session を実 DLL 経由で駆動する (UI を触らずに窓のボタンと同じ経路を実走)。スクショ側は `DockBuilderDockWindow` を `left` に変える + `SetNextWindowFocus()` + `ImGuiTabItemFlags_SetSelected` の 3 点でクリックなしに任意のタブを撮れる。
  - `git commit` は失敗理由を **stdout** に書く (`no changes added to commit`)。`classify_error` は stderr しか見ないので stdout も読む op はその場で分類する。
  - collab_verify の期待ファイルに **git の案内文 (`use "git restore <file>..."` 等、版で変わる) と機体名 (identity 未設定の fatal に入る) を入れない**。`# git <args>` ディレクティブは fixture の前提条件専用。ディレクティブ判定は `'#' + 半角空白 1 個 + 動詞` (字下げした説明行は誤爆しない)。
  - `SourceControlSession::Diff()` / `History()` / `IdentityOk()` / `WriteInFlight()` が使える口。`WriteInFlight()` は `CollabClient::OpInFlight()` の別名で、ゲート (sub-04) はこれとは別に `GateBlocker` を組む想定。
  - Bash ツール経由の heredoc / python は quoted でも `\\` を 1 段潰す (`'\0'` が実 NUL バイトになった)。C++ / Rust のエスケープを含む編集は Edit ツールか `bytes([92])`。
  - 未検証: マウス実押下 (プローブで代替)。fixture の残骸 `cache\collab_fixture` / `cache\probe_repo` / `cache\*.gitconfig` / `cache\scm_*.png` は生成物。
- **sub-04 (M66d) からの申し送り** (coder SELF_EVAL round 1/2 + planner VERDICT round 1/2):
  - 仕様確定 (planner が spec §4.1 / §7 / sub-05 を修正済み): `src/GameLogic/Scripts/` の `.h .hpp .inl` も段階 B / B で `EndBatch` に渡すのは A 集合のみ / `.meta` の guid 変化は実行前後の比較で検出 (事前予測は楽観側 = A/B に倒れる) / 未追跡の削除は `git clean -q -f` (`-d` 無し) / C の前に `EndBatch({})` / **段階 C のモーダルは『再起動』のみ** (「あとで」「キャンセル」を置かない = 食い違ったまま保存できる経路を作らない) / 再起動失敗 (`RelaunchSelfWithProject` = ShellExecuteW ≤ 32) はモーダルを閉じず赤字で案内、**`GitTransaction::Hooks::relaunch` は bool を返す契約** (握り潰すと「あとで」と同じ状態になる)。DLL 内 panic の隠し op は作らない (§7 に残存リスク)。v1 は「B なら必ず開き直す」(`.cs` / `.cpp` だけの最適化は後回し)。
  - planner の should (sub-05 で): (1) Asset Browser の [Rebuild Scripts] (`AssetOps::RebuildGameLogic`、`ShellExecuteW` fire-and-forget) をゲートから観測できるようにする — ハンドルを返す `StartGameLogicBuild` に寄せて EditorApp が保持、`GateInputs.scriptBuildRunning` に OR。(2) fixture (`tools/collab_fixture.ps1`) にテクスチャを参照するエンティティを 1 個置く (A 段階の画素証拠と sub-05 受け入れ条件 3 のため)。(3) 既定ドックを左列 (Hierarchy 束) + Diff 別窓 (sub-03 からの持ち越し)。
  - planner の nit: 段階 C モーダルが画面中央でなく上寄り (y ≒ 137 px) に出る — 破棄の確認モーダルが open stack に残ったまま次の `OpenPopup` をしている疑い。sub-05 で Branches / Diff のモーダルを足すときに一緒に見る (`CloseCurrentPopup()` を明示)。`DocumentDirty.h` の改名 (`DiskCompare.h` 等) は sub-10。
  - **sub-05 / sub-06 が書き込み系 op を足す形**: `GitTransaction` に op を 1 種足す (今は revert 専用の `RequestRevert`)。`BeginOp` / `ApplyResult` / `BuildChangeSet` は op 非依存なので「変更集合をどう決めるか」だけ差し替える。checkout / pull は `diff_names(HEAD, target)` を事前予測へ、実行後は `diff_names(before, after)` を `BuildChangeSet` の代わりに使う。段階 B のライブラリ無効化を足す場所は `GitTransaction::ApplyStageB` の 1 箇所。
  - `GateBlocker` を足したら `SourceControlSelfTest.cpp` の `kRows` 表にも足す (`static_assert(std::size(kRows) == GateBlocker::Count)` が止める)。
  - `ImGui::IsItemHovered` は `BeginDisabled` の内側では効かない → `EndDisabled` の後で `AllowWhenDisabled` 付きで呼ぶ。既定ドック幅 (285 px) にボタンは 3 個まで。
  - collab_verify の `# git <args>` は `-c` 付きも通る。`commit.gpgsign=false` を明示しないと開発者設定で固まる。
  - 未検証: 「Restart now」でプロセスが実際に入れ替わる経路 (押すと撮影プロセスが終了する) → sub-05 の受け入れ条件 3 (C: schemas が違うブランチへ切替) で実走。テクスチャ画素の A 段階証拠 → fixture 拡張後に撮る。`kReloadRetryMax` の WARN 発火は許容。
  - 生成物: `cache\probe_repo` (schemas を追加コミット済み)、`cache\collab_verify\`、`cache\scm_m66d_*.png`、`cache\*.log`。
- **sub-05 (M66e) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 仕様確定 (planner が spec §4.0 / §4.1 / §7 と sub-06 / sub-08 を修正済み): **`CollabClient::Shutdown` で `FreeLibrary` を呼ばない** (notify 8.2.0 の `ReadDirectoryChangesWatcher::drop` は内部スレッドを join しない → アンマップ後にコードが走り 0xC0000005、実測 1〜2/6 → 0/12)。**Rust 側に join できないスレッドを増やさない — 定期 fetch は worker のタイマーで** (spec §4.0)。§4.1「ブランチ周り」: **実行後の変更集合は op の応答に載せる** (`checkout` = `{branch, head, names, status}`。2 往復目を挟むと監視の status が割り込む。pull / continue / abort も同型)、未出生ブランチは `ls-tree` で全部 A、予測に無かった `.meta` は C。`branch_create` はゲート外 (working tree を触らない = commit / stage と同じ)。`local_changes_overwritten` の `detail` は固定文、`paths[]` が正。
  - planner の should (sub-06 で): リモート追跡だけのブランチへの `-t` 付き checkout を bare origin ができたら collab_verify に 1 本足す (sub-06.md に転記済み)。
  - planner の should (sub-08 で): スクリプトビルド失敗時の `error` 行を Console へ流す (`PollScriptBuild` の失敗検知で log の `path(line): error Cxxxx:` 行を `MYE_LOG_ERROR` へ、Console の double-click ジャンプが拾える形。旧 `RebuildGameLogic` の可視 cmd 窓 + `pause` の代替。sub-08.md に転記済み)。
  - planner の nit: `Scm_ComingSoon` が未使用 (sub-07 で使わないなら sub-10 で消す) / `ResolveMetaGuidChanges` の線形検索 (触らない)。
  - **書き込み系 op を足す形 (sub-06/07)**: `GitTransaction` に `OpKind` を 1 つ足し、(a) 予測の作り方 (`SendPredict`) と (b) 実行の呼び出し (`BeginOp` の分岐) の 2 箇所だけ差し替える。`ApplyResult` 以降 (段階分類 → `RegisterAdded` → `EndBatch(A集合)` → `ApplyStageB` → 段階 C モーダル) は共通。`SourceControlSession` の口: `Branches()` / `RequestBranches()` / `CreateBranch()` / `Checkout()` / `RequestDiffNames()` / `TakeMergeWarning()`。`ChangesFromNames()` は pull / merge の `names` にそのまま使える。
  - **ImGui の罠 (実測)**: `SetNextWindowFocus()` はドックのタブを直接選ばない (次フレームの `DockNodeUpdateTabBar` が `g.NavWindow` から拾う → 毎フレーム他窓に focus を与えていると永久に効かない) / 束の既定タブは `node->SelectedTabId = ImHashStr("#TAB", 0, 窓 ID)` / `AlwaysAutoResize` の modal を pivot 中央に置くなら `ImGuiCond_Always`。
  - fixture は「テクスチャを貼った床 + 立方体 + guid 固定の `.meta` 3 枚」。**エディタに `.meta` を作らせない** (作らせると未追跡が増えて checkout が拒否される)。ブランチ検証は fixture を作り直してから。collab_verify の次の番号は **07**。
  - 未検証: `-t` 付き checkout、detached HEAD 表示、`shot_verify.bat` (golden は Runtime.exe が撮るのでエディタの既定レイアウトは入らない。sub-08 で回る)。
  - 生成物: `cache\m66e_fx` (+ `.gitconfig`、3 ブランチ入り)、`cache\fx_test`、`cache\scm_m66e_*.png`、`cache\*.log`。
- **sub-06 (M66f) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 仕様確定 (planner が spec §4.1「リモート周り」/ §7 と sub-07 を修正済み): Pull は `behind > 0` のときだけ有効 (未 fetch の予測 `HEAD..@{u}` は必ず空。「Pull が内部で fetch する」は v1.5)。`push{setUpstream}` は省略可・既定 false・UI に口を作らない (**引数は削らない** = フィールド削除は bump 対象)。背景 fetch の失敗トーストは `error.code` のみ。`non_fast_forward` の detail は固定文。fetch / pull / push の応答に `remote`、`remote_state` に `hasRemote`。`CheckoutResult` → `TreeOpResult` に改名。
  - **定期処理は worker の `handle_tick` に相乗りさせ、スレッドを増やさない。`handle_tick` は `recv_timeout` の前に毎周回呼ぶ** (timeout 枝だけだと 1 秒未満の間隔でメッセージが届く間タイマーが飢える。実機で発見、回帰テストあり)。
  - **sub-07 の入口**: `pull{allowMerge:true}` の競合が `error.code=conflict` + 固定文を返し、リポジトリはマージ途中で止まる。`continue` を pull と同型の応答 (`{head, names, status}`) にすれば `GitTransaction` は `OpKind` を 1 つ足すだけ (`TreeOpResult` / `ApplyResult` を共有)。**競合ファイル一覧を git の案内文から解析しない** (`CONFLICT (modify/delete)` など別形がある。porcelain v2 の `u` レコードが正)。競合は git が stdout に書く (`classify_pull_failure`)。
  - planner の nit (sub-07 で): pull の段階 B / C の実機は未検証 → sub-07 の競合シナリオ (同じシーンを両側で変更 → pull) が B 経路を通るので、そこでシーン開き直しまで観測する。左列の Changes タブが縦に混んできた → 競合中は一覧を競合モードに切り替える (spec §4.1 決定 9)、UI を足さないこと。
  - planner の nit (sub-10 で): `auth_failed` の実 https 経路は fixture で作れない → README に「初回認証はターミナルで一度 `git push`」。
  - **`.txt` のような未知拡張子にもエディタが `.meta` を作る** — 実機 fixture にテキストを置くと未追跡が増え checkout / pull が弾かれる。
  - collab_verify の `# write` / `# git` は `..` を受け付ける (07 は兄弟に origin と workB を作成)。**bare origin は必ず `init --bare -b main`** (省くと HEAD が master になり clone がすれ違い「分岐しないはずのテスト」が緑になる)。次の番号は **08**。
  - 生成物: `cache\m66f_fx` (+`.gitconfig`)、`cache\m66f_origin.git`、`cache\m66f_peer`、`cache\scm_m66f_*.png`、`cache\m66f_*.log`。
- **sub-07 (M66g) からの申し送り** (coder SELF_EVAL + planner VERDICT round 1):
  - 仕様確定 (planner が spec §4.1「競合周り」と sub-08 / sub-10 を修正済み): `resolve{paths[], side}` (単数 path は仕様の穴 = 本体と `.meta` を 1 往復で)。ours / theirs の可否は porcelain `u` の**モード (m2/m3)** で決める (XY の文字では `AU` を誤る)。`status` / `repo_check` の両方に `mergeInProgress` / `rebaseInProgress` (status に無いと pull 競合直後のゲートが開いたまま)。競合種別は git-status.txt の 7 組 + `unmerged`。`conflicts` op が `merged[]` と ours / theirs を返す。`merge_abort` / `continue` は pull と同型 `{head, names, status, remote}`。`continue` は merge のみ (rebase の続行は `bad_request` = v1 外)。
  - **競合の入口は 2 つ**: pull の応答 (`Phase::ConflictScan`) と、起動時に外の git で競合していた場合 (status の mergeInProgress → `ApplyStatusResult` が自動で conflicts を投げる)。**競合したシーンは JSON として壊れて空で開くので `SaveCurrentScene` 先頭の `IsConflictedPath` が唯一の砦**。`resolve` はトランザクションを通さない (監視 → ReloadHub の通常経路で拾う)。`conflicts.merged[]` に pull 前の未コミット変更が混ざるのは v1 許容 (余分に読み直すだけ)。
  - planner の nit (sub-08 で): マージ中に pull を投げたときの `git_failed` + 生文を `merge_in_progress` へ分類 (1 行) / `Scm_ComingSoon` の削除。
  - collab_verify の次の番号は **10**。期待ファイルにマージコミットの件名を載せない (`Merge branch 'main' of <URL>` にリモート URL が入る)。
  - 未検証: UI クリック自体 (プローブ代替)、`git mergetool` の実起動 (`cmd /k` の配線のみ)。sub-06 の未検証「pull の段階 B」は競合シナリオで消込 (シーン開き直りまで観測)。
  - 生成物: `cache\m66g_fx` (+`.gitconfig`)、`cache\m66g_origin.git`、`cache\m66g_peer`、`cache\scm_m66g_*.png`、`cache\m66g_*.log`。
- planner のエージェント ID はセッション限り。セッションを跨いだら `MODE: VERDICT` 用の planner を新規起動し、文脈は spec.md / sub-NN.md / この台帳で渡す。
