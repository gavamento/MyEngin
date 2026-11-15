# review-1: m104-ai-change-tracking

- 対象コミット範囲: `ca6a3991..c62d2f2d` (28 本。最後の 1 本は台帳の記入だけ)
- 日付: 2026-11-16
- reviewer: harness-reviewer (round 1)

```
REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — 境界の入力 (Unicode のパス、CRLF、末尾に改行の無いファイル、リネーム、9 MiB のバイナリ、壊れたフック入力、空の入力) は実サービスで全部正しく扱えた。復元の安全の作り (書き込み口 1 か所、軌跡に合うファイルだけを戻す、BeginBatch / EndBatch の対、git を起動しない) もコードと試験で確かめた。ただし帰属に穴がある: フックの pre だけが届いて post が来ない操作 (利用者が道具の実行を拒否した・道具が失敗した) の窓が 10 分開いたままで、その後の**人の編集が ai / claimed** になる。セッションを終えた後でも同じ (指摘 1)。目的の「誤って AI のせいにしない」に当たる、よくある流れ。
  機能性: 4 — 自動の検証は全部通った: Release / Debug の --selftest-full (各 125 スイート 0 失敗。Debug は physics を含む)、cargo test (track の既定 + 負荷で変わる 4 件、mcp)、track_verify 29 シナリオ、check_rules 0 error、check_rules_selftest、replay_verify (MYE_REPLAY_JOBS=3) PASS、warn-as-error の両構成のビルド。ABI・SimSnapshot・TypeId の不変と src\Engine の差分の範囲も確かめた。手動の確認 (実 Editor の操作、本物の Claude Code) は未確認のまま。
  ビジュアルデザイン: 4 — coder のスクショを見た (下の「見た画像」)。文字の欠けや崩れは無い。ゴーストのラベルが重なる (既知の nit) のを 1 枚で確認した。自分では撮り直していない (撮るには一時プローブをソースに足す必要があり、reviewer はソースを変えない)。m104s / m104t の一部は最終の版より前のビルドで撮られている (sub-18 / sub-19 の実装メモ)。
  コード品質: 4 — 規約の静的検査 0 error。書き込みの API が RestoreWriteIo.cpp に集まっていることを diff の追加行の grep で確かめた。Tr() を printf の書式に渡す箇所なし、rand / random_device なし、TODO などの仮実装の残りなし。minor が 3 件 (指摘 2〜4) と、既知の nit (指摘 5)。
指摘:
  1. [major] 宛先: planner — フックの pre (op_begin) の後に post (op_end) が来ない操作の窓が、打ち切り (10 分) まで開いたままになり、その間に人がそのファイルを初めて変えると、宣言と一致しない内容でも ai / claimed になって AI の操作とセッションに結び付く。セッションを終えた (session_end) 後でも窓は閉じない。Claude Code では、利用者が道具の実行を拒否したときや道具が失敗したときに PostToolUse が来ない (PreToolUse は許可の確認より前に走る) ので、「AI の提案を拒否して自分で直す」というよくある流れがこれに当たる。要求 §4.3 の Claimed は「MCP・CLI が変更元を申告した」で、この変更は申告されていない (道具は実行されていない)。窓の中の変更なので AI の一覧 (「AI の変更 (検証済み / 申告あり)」) に出て、`restorable: true` (利用者が自分の編集を「AI の変更」として戻せてしまう)。実装は設計どおり: attribution-rules.md:45 (「op_end が来ていない操作は、打ち切り (既定 10 分) まで開いている」)、ipc-contract.md:316 (「その間に窓へ入った Revision の根拠は残る」)、attribution-rules.md §3 の行 6 (op_window + before_match → claimed)、tools\track\src\attribution.rs:616-622 (finished_at IS NULL の操作を窓に入れる)、:635-650、:205-213。どの文書も「pre の後に post が来ない」場合を扱っていない (hooks-setup.md §3、attribution-rules.md §6.1) — 根拠 (実サービスで再現): 一時プロジェクトで `tools\track\target\release\mye_track.exe serve --project <tmp>` を起動し、(a) `hook pre --project <tmp>` に Edit (h.txt、old_string "two" → "TWO"、session_id s-e1) の入力を渡す → post は送らない → 4 秒後に h.txt を宣言と違う内容 ("one\ntwo\nhuman edit after the AI tool was rejected\n") に書く → `list` の h.txt の最初の Revision が `"actor":"ai","attribution":"claimed","operationId":"01d3be61-…"`、`describe` の evidence は observed + op_window + before_match だけ、`restorable: true`。(b) 同じく h.txt の pre だけ → 同じセッションで k.txt の pre + post (成功) → `hook session-end` (sessions の status = finished) → 3 秒後に h.txt を初めて変える → やはり `ai / claimed`、終わったセッションの `changeCount` が 2 (k.txt と、この人の編集)。比較: post が届いた k.txt のあとの人の編集は `unknown / unknown` (窓が閉じていれば正しく動く) — 期待: 仕様で「op_end が来ない操作の窓をいつ閉じるか」と「閉じていない窓だけを根拠にした帰属をどこまで上げるか」を決める。候補 (選ぶのは planner): session_end / change_session_end でそのセッションの開いた操作を閉じる、同じセッションの次の op_begin / op_end で前の開いた操作を閉じる、op_end の無い窓 + before_match は claimed にしない (inferred 止まり、または unknown)、Claude Code が拒否・失敗を知らせるイベントを持つならそれもフックに登録する (公式文書で確かめる。私は確かめていない)。どれを採っても、上の (a)(b) を試験 (cargo test か track_verify のシナリオ) にする。attribution-rules.md §2.1 / §3、ipc-contract.md §7 の表、hooks-setup.md を合わせて直す。
  2. [minor] 宛先: coder — `mye_track.exe serve --project <存在しないパス>` が、そのディレクトリと `.mye\ai_changes\` (DB、journal、service.json) を作って動き続ける。フックの `--project` やMCP の `--project` の綴りを間違えると、知らない場所にディレクトリとサービスができる (フックの pre / session-start はサービスを起動するので、設定の誤りがそのまま形になる) — 根拠: `%TEMP%` の下の存在しないパスで serve を起動 → 8 秒後も動いていて (自分で止めた)、`<パス>\.mye\ai_changes\{blobs,journal,service.json,service.lock,tracking.db}` ができていた。標準エラーは `StartupReport { state: Some(Recording) … }` — 期待: ルートが無い・ディレクトリでないときは何も作らずに 0 以外で終わる (フックの終了コードは今までどおり 0)。ipc-contract.md の起動の作法に一言足す。
  3. [minor] 宛先: coder — M104n で、変更と関係の無い行の空白が消えた: `rebuildDockLayout_ = !std::filesystem::exists(...)` が `rebuildDockLayout_ =!std::filesystem::exists(...)` になった (挙動は同じ) — 根拠: src\Editor\App\EditorApp.cpp:153、`git log -S"rebuildDockLayout_ =!std"` が 42a1b552 (M104n) — 期待: 元の書き方に戻す (規約 §5 の既存のスタイル)。
  4. [minor] 宛先: coder — 新しく足した文書に、機械に固有の絶対パスがあり、規則 17 の WARN が 1 件増えた — 根拠: `tools\check_rules.ps1` の出力 `WARN [rule 17] C:\HAL\MyEngin\docs\ai-change-tracking\performance.md:111` (`robocopy C:\HAL\MyEngin %TEMP%\mye_vscan …`)。他の rule 17 の WARN は M104 より前のもの (git blame で確認) — 期待: リポジトリからの相対 (`<リポジトリ>` など) に書き換える。
  5. [minor] 宛先: coder — 既知の nit の持ち越し (planner の重点の「拾ってよい」分のうち、私が根拠を見たもの): (a) ゴーストのラベルが重なる (`Deleted Crate (lost mesh)` と `Deleted Crate (mesh)`) — 根拠: C:\HAL\MyEngin\cache\m104r_shots\1_scene_ja.png。(b) 大きいファイル: src\Editor\Windows\Project\AiChangesWindow.cpp 2021 行、src\Editor\ChangeTracking\RestoreRunner.cpp 1208 行、SceneRestorePlan.cpp 984 行 (`wc -l`) — 期待: 台帳の申し送りへ。今回の FAIL の理由ではない。
検証した手段:
  - ビルド: MSBuild `MyEngine.sln` Release|x64 と Debug|x64 を `/p:MyeWarnAsError=true` で (どちらも exit 0、警告 0)。
  - 自己テスト (作業ディレクトリはリポジトリ): Release `Editor.exe --selftest-full` → 125 スイート 0 失敗 (249 秒)。Debug `Editor.exe --selftest-full` → 125 スイート 0 失敗 (829 秒、physics を含む。D38 / D43 の解消を確認)。
  - 実サービスとの結合 (配布物に近い置き方): bin\x64\Release の exe / dll / json を make_dist.ps1 と同じ形で `%TEMP%` の下へ写し、`MYE_TRACK_EXE` を外して `MYE_TRACK_PROBE=1 MYE_SELFTEST_ONLY=changetracking` で写した先の `Editor.exe --selftest` → 隣の mye_track.exe を起動して Ready、記録・通知・describe / hunks・承認・Hunk の復元 (restore_expect で actor = restore)・editor_saved・シーンの overlay・shutdown まで全 PASS、mye_track.exe の残りなし。A21 の「展開先の Editor が隣の mye_track.exe を起動して接続できる」を、selftest の経路 (ChangeTrackingSession::Start、実 pipe、実ファイル) で確かめた (対話起動の窓は未確認のまま)。
  - Rust: `cargo test --manifest-path tools\track\Cargo.toml` (全部 ok、56 秒)、`--test watch -- --ignored` (負荷で変わる 4 件 ok)、`cargo test --manifest-path tools\mcp\Cargo.toml` (58 ok。debug の出力だけで、使用中の release の mye_mcp.exe は触っていない)。
  - `tools\track_verify.bat` → 29 シナリオ全部 PASS。
  - `tools\check_rules.ps1` → 0 error / 66 warning (M104 で増えたのは指摘 4 の 1 件)。`tools\check_rules_selftest.ps1` → exit 0 (規則 19 の違反の検出を含む)。
  - `tools\replay_verify.bat` (MYE_REPLAY_JOBS=3、MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true) → `[PASS] replay consistency (Debug/Release, 17 scenes …)`、24 ジョブ全部 PASS。
  - 決定性: `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空。`src\Engine` の差分は LocalizationTable.inl と HotReload\ReloadHub.{h,cpp} と ReloadHubSelfTest.cpp だけ。src\Runtime / Server / GameLogic の差分なし。非対話起動の判定 (src\Editor\App\EditorMain.cpp:302-308、:590 `allowChangeTracking = !automation`) を読んだ。
  - 実サービスでの境界の入力 (一時プロジェクト、自作の PowerShell の駆動): Unicode のパス (`日本語/ファイル.txt`)、CRLF、末尾に改行の無い行の変更、リネーム (`ren_src.txt` → `ren_dst.txt` が renamed 1 件)、9 MiB のバイナリ (diff_mode binary)、フックへの壊れた JSON と空の入力 (終了コード 0、標準出力 0 バイト、20〜80 ms)、存在しないプロジェクト (指摘 2)、post の来ない操作 (指摘 1)。作った一時ディレクトリと起動したサービスは全部片付けた (自分が起動した pid だけを止めた)。
  - 読んだ範囲 (全文): RestoreWriteIo.{h,cpp}、RestoreRunner.cpp、RestoreJournal.cpp、RestoreGate.{h,cpp}、PathGuard.cpp、HunkRestore.cpp、SceneRestoreRunner.cpp、TrackLaunch.cpp、ReloadHub の diff、tools\track\src\attribution.rs (120-775)、hook.rs の入口、main.rs、EditorApp.cpp の追加分と BuildGateInputs、ChangeTrackingSession.cpp の保存の知らせ。部分: watch.rs (監視の分け方)、session.rs (打ち切り)、server.rs (hello)、protocol.rs (役割の表)、ScenePreviewSession / ScenePreviewContent の読み込み、diff の周辺ファイル (ci.yml、release.yml、make_dist.ps1、.gitignore、AssetOps など scmhint の追加、check_rules.ps1、ADR-032、mcp-server.md)。
  - 書き込み口の確認: `git diff ca6a3991..HEAD -- src/Editor src/Engine` の追加行で WriteFileReplacing / ofstream / DeleteFile / MoveFile / CopyFile / filesystem::remove 等 / CreateProcess / ShellExecute を grep → 製品のコードでは RestoreWriteIo.cpp、TrackLaunch.cpp (サービスの起動、bInheritHandles = FALSE)、explorer の表示だけ。残りは試験。
  - 見た画像: cache\m104k_shots\1_running_inline.png、cache\m104t_shots\1_propagation_ja.png、cache\m104p2_shots\recovery_ja.png、cache\m104r_shots\1_scene_ja.png、cache\m104s_shots\4_side_ja.png、cache\m104q_shots\preview_ja.png、cache\m104m2_shots\1_ja.png、cache\m104n_shots\1_modal_ja.png、cache\m104o_shots\3_list_ja.png。
  - 実行しなかったもの (理由): `tools\mcp_verify.bat` (`cargo build --release` が使用中の tools\mcp\target\release\mye_mcp.exe を書き換えうる。MCP の追跡の道具は track_verify の `# mcp` シナリオと mcp の cargo test で代わりに見た)、`tools\make_dist.ps1 -SkipBuild` と `Editor.exe --package` (dist_engine\ と配布物の出力を作り直すため。sub-20 の coder の結果のまま)、対話起動の Editor と新しいスクショ (ソースに一時プローブが要る)、CI と release.yml の実走 (push しない)、Rust 1.89 でのビルド、本物の Claude Code。
前回指摘の消込: なし (round 1)
```

## 補足 (指摘 1 の再現手順の詳細)

自作の駆動 (PowerShell) で行った。手順を書いておく (駆動のスクリプトはリポジトリに置いていない)。

1. `%TEMP%\<任意>\` に `h.txt` = `one\ntwo\n`、`k.txt` = `alpha\nbeta\n` を作る。
2. `tools\track\target\release\mye_track.exe serve --project <そのディレクトリ>` を起動し、`client` の `status` が `phase = watching` になるまで待つ。
3. `mye_track.exe hook pre --project <dir>` の標準入力へ
   `{"session_id":"s-e1","cwd":"<dir>","hook_event_name":"PreToolUse","tool_name":"Edit","tool_use_id":"t-e1","tool_input":{"file_path":"<dir>\\h.txt","old_string":"two","new_string":"TWO","replace_all":false}}`
   を渡す。post は送らない (利用者が拒否した、の模擬)。
4. 4 秒待って、`h.txt` を `one\ntwo\nhuman edit after the AI tool was rejected\n` に書く (宣言の結果 `one\nTWO\n` とは違う)。
5. 3 秒待って、`client` で `hello` (role editor) → `list {"path":"h.txt"}` → `describe {"revisionId":<最初の Revision>}`。
6. 結果: `actor = ai`、`attribution = claimed`、`operationId` が t-e1 の操作、evidence = observed / op_window / before_match、`restorable = true`。
7. 変形: 3 の後に同じ `session_id` で別ファイルの pre + post (成功) と `hook session-end` を送ってから 4 を行っても、同じ結果 (セッションは finished、`changeCount` に人の編集が数えられる)。
