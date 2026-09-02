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
| sub-01 | OK | 1 | (M66a、下の記入待ち) | M66a: Rust cdylib + CLI + fixture + collab_verify + CI + 規則 12 + DLL 往復の縦切り。依存: なし |
| sub-02 | 未着手 | 0 | | M66b: CollabClient 完成 + SourceControlState + Source Control 窓 (読み取り専用) + canonicalRoot。依存: sub-01 |
| sub-03 | 未着手 | 0 | | M66c: stage / unstage / commit / History / diff / identity。依存: sub-02 |
| sub-04 | 未着手 | 0 | | M66d: ReloadHub Begin/EndBatch + ゲート + 4 窓の HasUnsavedChanges + トランザクション + revert。依存: sub-03 |
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
- `--porcelain=v2` の下限 git 2.11 は記憶ベース — sub-01 の冒頭確認で公式を見る
- `StartGameLogicBuild` の全呼び出し元 (同期経路の有無) — sub-04 のゲート `ScriptBuildRunning`
- `kCollabMaxBatchApply = 200` / `kReloadRetryMax = 60` は初期値。実機で不便なら仕様変更として spec §8 に積む
- Rust cdylib が実際に Editor.exe から LoadLibrary で往復するかは策定時点で**未検証** — sub-01 の縦切り (受け入れ条件 4) で最初に潰す

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
    `tools\build_collab.bat` は PATH の cargo → 無ければ `%USERPROFILE%\.cargo\bin\cargo.exe` の順に解決し、どちらも無ければ機能 OFF の WARN で 0 終了 (MyeScripting 不在時と同じ縮退) にするのが筋。
  - **罠 2**: `cargo new` の既定 edition は **2024** — `#[no_mangle]` は `#[unsafe(no_mangle)]` と書かないとエラー (unsafe attribute)。
  - **罠 3**: MSVC リンカが「ライブラリ ... .dll.lib とオブジェクト ... .dll.exp を作成中」を stdout に出し、rustc が `linker_messages` lint の **warning** として拾う (無害)。
    CI で `RUSTFLAGS=-D warnings` 相当を立てるとこれがエラーになるので、`#![allow(linker_messages)]` か lint 個別指定で逃がすこと。
- planner のエージェント ID はセッション限り。セッションを跨いだら `MODE: VERDICT` 用の planner を新規起動し、文脈は spec.md / sub-NN.md / この台帳で渡す。
