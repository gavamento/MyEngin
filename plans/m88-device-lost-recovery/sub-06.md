# sub-06: 復旧中もクライアントのネット送受信を回し、専用サーバから切断されないようにする

- 依存: sub-05 (EngineLoop の RecoverDevice と、その失敗経路の修正が入った後に着手する)
- 状態: 未着手
- 往復: 0

## やること
spec 7. R6 の裁定どおりに実装する。

1. EngineLoop の Recovering (RecoverDevice の呼び出しを囲む位置) で、クライアント構成かつセッションが生きているとき (`clientEnabled && !clientDone && !clientDropped && !clientLeft`。Connecting / Downloading / Running のどれでもよい) だけ、ネット送受信専用のスレッドを起動する。復旧が終わったら (成功でも Fatal でも) 必ず join してから先へ進む。RAII のスコープで、例外や早期 return でも join する。
   - スレッドがすること: `clientSocket.Recv` → 送信元が `clientServerAddr` なら `clientSession.OnPacket` → `clientSession.Poll(ClientNowMs())`。これを約 10 ms 間隔で、停止要求まで回す。受信ループは既存の `ClientFrame` (`EngineLoop.cpp` の「1 フレームに 1 回: 受信 → セッション駆動 → tick」) の受信部分と同じ処理を共有する (関数に切り出す。複製しない)。
   - **tick は回さない** (`clientRunner.Update` を呼ばない)。sim・recorder・ECS に触れない。
   - スレッドが動いている間、メインスレッドは `clientSocket` / `clientSession` / `clientRecvBuf` に触れない (所有の受け渡しで排他する。ロックで共有しない)。この不変条件をコメントに書く。
   - 実装前に確認すること: `OnPacket` / `Poll` が、復旧中のメインスレッドと共有する状態 (ログ、recorder、ECS、入力レーンなど) に書き込まないこと。書き込む場合は planner へ戻す。MYE_LOG がスレッド安全かも確認する。
2. 復旧から戻った後の最初の `ClientFrame` が、溜まった Confirmed を既存の追い付き経路で処理できること。追い付き経路そのものは変えない。追い付けずに resync (Downloading) へ入るのは、既存の仕組みどおりなら許容する。
3. 検証専用の入口を 2 つ足す。
   - **復旧の所要を人為的に延ばす** (例: `--simulate-device-recovery-delay-ms <ms>`)。Release の WARP では復旧が 3 s 未満で終わることがあるので、タイムアウトを確実に超えさせるため。RecoverDevice の中、デバイス再作成の前で眠る。描画専用の実時間なので sim には影響しない。
   - **セッション参加後に確実に発火する疑似消失** (例: クライアントの sim tick が T に達したら発火)。server_verify は参加まで最大 240 s 待つので、フレーム番号指定だと参加前に発火し得るため。
   - 既存の `--simulate-device-lost` の流儀 (EngineCli、不正値は無視 + エラー、CLI の SelfTest) に揃える。
4. `tools\server_verify.bat` にケース E を足す。内容は、Release の Server.exe と Release の Runtime.exe クライアント 2 本 (`--warp`) で、片方だけ参加後に疑似消失を起こし、復旧の遅延を 5000 ms にするもの。判定は spec 受け入れ 16 の 4 点 (timeout による切断なし / クライアントが正常終了 / サーバとクライアントの .rep の tick 列が一致 / サーバ .rep の再生検証が一致)。既存の late-subst の上限チェックは、ケース E の停止したレーンにだけ適用しない (止まった約 5 s 分の代入が出るのは想定どおり)。適用しない理由を bat のコメントに書く。サーバの「cannot keep up」警告がケース E で出るなら、それが一時的な停止によるものかを調べ、緩めるかどうかを SELF_EVAL の「不安・質問」で planner に判断を仰ぐ。黙って緩めない。最後の PASS 行の要約にケース E を足す。
5. 対策無しで同じケースが落ちることを、一度だけ確認する (スレッドを起動しない一時的な変更で、サーバのログに timeout が出ることを見る)。確認したら戻す。結果は実装メモに書く。

## やらないこと (このサブでは)
- P2P (`NetSession`) への同じ対策 (stallTimeoutMs = 20 s に収まる)。
- プロトコル・Server.exe・タイムアウト値の変更。
- 復旧中の tick 進行。

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp`: `ClientFrame` (L1646 付近) の受信部分の切り出し、Recovering の呼び出し箇所
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\DeviceRecovery.{h,cpp}`: 遅延の入口を置くならここ
- `C:\HAL\MyEngin\src\Engine\Engine\Net\ClientSession.{h,cpp}`: 読むだけ (L518 `Poll`、L550 timeout、L556 keep-alive)。変更は原則しない
- `C:\HAL\MyEngin\src\Engine\Engine\App\EngineCli.cpp` / `EngineCliSelfTest.cpp`
- `C:\HAL\MyEngin\tools\server_verify.bat`
- `C:\HAL\MyEngin\docs\adr\ADR-026-device-lost-recovery.md`: 「ネットセッション中の復旧」節を足す (方式と、却下した案 a〜c)
- `C:\HAL\MyEngin\docs\test_checklists.md`: M88 節にケース E を 1 行

## 受け入れ条件 (このサブ)
1. (spec 16) `tools\server_verify.bat` が、ケース E を含めて PASS する。
2. 対策無しでケース E が切断で落ちることを、一度確認した記録が実装メモにある (サーバのログの timeout 行の引用)。
3. 疑似消失を使わない既存のケース A〜D の結果が変わらない (同じ実行で PASS)。
4. (spec 14) `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat` が通る。
5. ADR-026 と test_checklists.md に追記がある。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `tools\server_verify.bat` (UDP と複数プロセスを使うので CI では回らない。ローカルで実行し、ログ `cache\sv_E_*.log` の要点を実装メモに引用する)
- `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
