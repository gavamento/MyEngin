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
| sub-03 | OK | 1 | (M66c、下の記入待ち) | M66c: stage / unstage / commit / History / diff / identity。依存: sub-02 |
| sub-04 | 実装中 | 0 | | M66d: ReloadHub Begin/EndBatch + ゲート + 4 窓の HasUnsavedChanges + トランザクション + revert。依存: sub-03 |
| sub-05 | 未着手 | 0 | | M66e: Branches — 一覧 / 作成 / checkout + 段階の事前判定。依存: sub-04 |
| sub-06 | 未着手 | 0 | | M66f: fetch / pull / push + 定期 fetch + 通知 + EditorSettings。依存: sub-04 (sub-05 と独立。並列なら SourceControlWindow.cpp / ops.rs のマージ順を司会が決める) |
| sub-07 | 未着手 | 0 | | M66g: 競合 — abort / ours / theirs / continue。依存: sub-06 |
| sub-08 | 未着手 | 0 | | M66h: 衛生 — .gitignore テンプレ 4 行 + project_settings.json の個人設定分離。依存: sub-03 (sub-05〜07 と独立) |
| sub-09 | 未着手 | 0 | | M66i: Content Browser に Git バッジ + 保存直後のヒント。依存: sub-02 (sub-03〜08 と独立) |
| sub-10 | 未着手 | 0 | | M66j: 仕上げ — engine_spec §14 Source control / README / CLAUDE.md / 規則の記載。依存: sub-01〜sub-09 |

## 未決事項 (planner PLAN_RESULT より。coder が「不安・質問」で拾う)
- S5 の新規アセット登録方式 (増分登録 vs `ScanAndSync` 再実行。`AssetDatabase.cpp:250-278` は 3 表を clear するので安全性未確認) — sub-04 で比較して選ぶ
- B 段階の `.controller.json` / `.terrain.json`: ライブラリのキャッシュ無効化 API の有無 (無ければ C に格上げ) — sub-04
- `.terrain.edit` に `.meta` が付くか (`ClassifyPath`) — sub-03 は「存在するサイドカーだけ束ねる」実装で依存しない
- GUI 無しの孫プロセス git から GCM のダイアログが出るか — sub-06 の冒頭確認 (出なければ案内文を変える)
- ~~`--porcelain=v2` の下限 git 2.11 は記憶ベース~~ → **閉じた (sub-01)**: 2.11.0 で正しいことを一次情報で確認、出典 URL は `tools\collab\src\git.rs` の定数コメント
- `StartGameLogicBuild` の全呼び出し元 (同期経路の有無) — sub-04 のゲート `ScriptBuildRunning`
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
- planner のエージェント ID はセッション限り。セッションを跨いだら `MODE: VERDICT` 用の planner を新規起動し、文脈は spec.md / sub-NN.md / この台帳で渡す。
