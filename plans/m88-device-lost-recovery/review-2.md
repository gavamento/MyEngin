# review-2 — m88-device-lost-recovery

- 対象: `218297cfbd462f278fd6beeeb003c07de6ecd7a3..HEAD`。今回の修正は 4a05f7b (M88f: review-1 #1・#2) と 7131e8c (M88g: sub-06 = review-1 #3 → ユーザー判断 U5)
- 日付: 2026-10-07
- bin は、今回の replay_verify.bat が HEAD から Debug / Release を再ビルドしたもの

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 5 — round 1 で穴だった「復旧を試みた後の致命停止」を実ループで通せるようになった (--simulate-device-lost-stale)。Editor の Debug / Release とも exit 6 で、退避ファイルがあり、クラッシュも assert も無い。専用サーバのセッション中の消失も、復旧が 10.9 s かかってもサーバに切られず、.rep がサーバと一致した。ネット無しの実行では pump が起動しないことも確認した
  機能性: 5 — 受け入れ 12 (実ループ) と 16 (server_verify のケース E の 4 点) を実走で満たした。回帰の確認として、Editor.exe --selftest (両構成)、check_rules、replay_verify (フラグ無し)、server_verify ABCDE が全部通った
  ビジュアルデザイン: 5 — 今回の修正は見た目に関わらない。round 1 で確認した復旧後の画面 (Runtime の 3 シーンが tol=0 一致、Editor は文字だけ違う) から変わる経路は無い (変更は ImGui の終了処理、致命経路、ネットのポンプ、CLI だけ)
  コード品質: 5 — ImGuiRenderer は 2 つのバックエンドの生死を別々に持ち、すべての遷移で更新している。RecoveryNetPump は RAII で join し、排他をロックではなく所有権の受け渡しで行い、その不変条件をコメントに書いている。受信は ClientReceive に切り出して ClientFrame と共有しており、複製していない。MYE_LOG はロック付きのリングバッファなので (Log.cpp:79,123)、ポンプのスレッドからも安全に書ける。新しい CLI 3 本には SelfTest があり、不正値の扱いも既存と揃っている。check_rules は 0 error
指摘: なし
検証した手段:
  - 読んだ範囲: `git diff 30347ad..HEAD` (src、docs、tools\server_verify.bat)、sub-06.md、spec の受け入れ 16 / R6 / 変更履歴、Log.cpp のロック
  - `tools\replay_verify.bat` (フラグ無し。chcp 65001 → cmd /c、終了後に chcp を戻した) → exit 0、Debug / Release のビルド成功、`[parallel] all 17 jobs passed`、check_rules は `0 error(s), 50 warning(s)` (既知の rule 7)
  - 受け入れ 12 (実ループ): 一時の空プロジェクト (scratchpad\tmpproj。外部プロジェクトには書いていない) で `Editor.exe --frames 120 --simulate-device-lost 30 --simulate-device-lost-stale` を実行。Release / Debug ともに `old device external references: 1 (allowed 0)` → `cannot continue` → 退避 `tmpproj\crash\device_lost_*\empty.scene.json` → exit 6。stderr と crash\ 配下に、assert やクラッシュバンドルは無い。Debug ではデバッグレイヤが、残った `ID3D11Buffer` を名指しした。修正前 (30347ad) で落ちることの再現は行っていない (この入口は修正前のコードには無い)
  - `tools\server_verify.bat` (ABCDE) → exit 0 (15 分)、`[PASS] server_verify (cases ABCDE ...)`。ケース E の結果: client 2 で `client network pump started` → `device recovered in 10873.2 ms ... unchanged=1`。3 プロセスとも exit 0、`server dropped no peer by timeout: ok`、サーバのログで peer 2 の切断理由は `bye`。`server == c1` は 3000 tick、`server == c2` は 1200 tick の重なりが一致。Debug / Release の Server と Runtime による再生検証は 4 本とも PASS。止めていないレーン 0 の late-subst は 0.90%、止めたレーン 1 は 60.87% で判定から除外。許容した行は止めたレーンの `cannot keep up` (平均 4498 ms 遅れ) の 1 行だけで、「許容:」として表示されている
  - Editor.exe --selftest: Release は exit 0 (1 分)、Debug は exit 0 (6.6 分)。どちらも `  FAIL:` 行は 0、DeviceLostRescue / DeviceRecovery は ALL PASS、新しい CLI のテスト (stale / after-join / delay-ms) も PASS
  - ネット無しの退行の確認: Release Runtime `--simulate-device-lost 30,60` → 2 回とも参照 0 で復旧、exit 0、pump のログは出ない
  - 未検証 (round 1 から変わらず、ユーザーの手動確認待ち): 本物の TDR、メニュー偽装の実操作、ImGui の別窓を出した状態での復旧、test_checklists M88 (a)〜(c) の目視項目。復旧が約 17 s (履歴 1024 tick) を超えたときに resync へ入る経路は試していない (sub-06 で「既存の仕組みどおりなら許容」とされている範囲)
前回指摘の消込:
  1. [major] 致命停止の経路で ImGui の Win32 を二重に Shutdown する — 解消。`win32Bound_` で生きている方だけを畳む (ImGuiRenderer.cpp の Shutdown / ReleaseDevice / RecreateDevice)。実ループの Debug / Release で exit 6、クラッシュも assert も無いことを確かめた。test_checklists の M88 (c) にも載った
  2. [minor] ADR-026 の実体欄のファイル名 — 解消。RecycleDevice と DeviceLostLimiter は DeviceRecovery.{h,cpp}、DeviceFatalInfo は EngineLoop.h、参照数の測定は GraphicsDevice、になっている
  3. [minor] 専用サーバのセッション中の復旧停止がタイムアウトを超える — 解消 (ユーザー判断 U5 で sub-06 として対応)。ケース E で、対策ありのとき切断されないことを確かめた。対策無しで落ちることは coder の記録 (sub-06 の実装メモ) によるもので、私は再現していない
```

副産物: `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\b62c015d-a484-41dc-a394-ac954c4e3198\scratchpad\tmpproj\` (一時プロジェクトとその crash\ 出力)。リポジトリと外部プロジェクトには何も作っていない。`cache\sv_*` は server_verify が毎回作り直すもの。
