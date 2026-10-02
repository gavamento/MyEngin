# sub-11: クライアント記録の再生・時刻同期・運用の修正 (review-1 #3 #5 #6 #7 #8 #10)

- 依存: sub-10 (同じファイルを触るので直列にする)
- 状態: 未着手
- 往復: 0

## やること
review-1 の残り。spec 4.1.6 / 4.1.7 / 4.1.7b / R5 と V6〜V11。

1. **#3 (V6、spec 4.1.7 の追記)**: 開始 tick ≠ 0 の .rep を再生できるようにする。
   - `ReplayPlayer` (Replay.h:161-170 の InputForTick / HasTick / ExpectedHash / SystemInputForTick) が、`startMeta.tick` (v9 ヘッダ) を基点に引くようにする。開始 tick = 0 の .rep (既存の全 .rep) は 1 バイトも挙動を変えない。v8 は基点 0。
   - EngineLoop と HeadlessSim の verify ループは、復元後の `ctx.tickIndex` から回る。
   - 対象はクライアントの .rep、`.rsN.rep`、desync バンドルの `local.rep`、CrashRing の crash.rep (開始 tick ≠ 0 になりうるもの)。どれも再生できること。
   - `--rep-diff` / `--rep-diff-overlap` の既存の挙動 (セッション tick で名指し) は保つ。
   - クライアントの desync.txt の手順を書き換える: 相手のバンドルではなく**サーバ .rep** を同じ tick まで再生して `--hash-dump` し、`local.rep` の dump と `--hash-diff` で突き合わせる。サーバ .rep の置き場所が分からない (GameLift 等) ことも想定して、文面は「サーバの --replay-record で指定した .rep」とする。
   - server_verify に検査を足す。
     - ケース C: クライアントのバンドルの local.rep が単独で再生でき (desync tick より前は一致)、desync tick の dump が出る。サーバ .rep の同じ tick の dump との `--hash-diff` が、壊したフィールドを名指しする。
     - ケース A: クライアント .rep の `--replay-verify` が 0 でない tick 数で PASS する。
2. **#5 (V7、spec 4.1.6 の追記)**: 時刻同期を収束させる。
   - 追いつき (ClientSimRunner.cpp:310-319 の CatchUpTicks) と速度係数 (ClientSession.cpp:548-566) を、同じ基準 (サーバの予定時刻に対する自分の位置 = 到着余裕) から導く。サーバの確定フロンティア (ServerSession.cpp:608 で前倒しされうる) は基準にしない。
   - selftest: 偽トランスポート (遅延一定・ロス 0) で、参加から 10 秒後以降の到着余裕の移動平均が「目標 ± 1 tick」に入ることを assert する。遅延が途中で変わるケース (例: 25ms → 75ms) でも、変化から 10 秒後に同じ範囲へ戻ること。
   - 実プロセス: Release の Server + Release のクライアント 1 台 (ロス 0) を 2 分回し、定常の到着余裕と catch-up の割合をログで示す。review-1 の実測 (92.9ms、catch-up 2.0%) と並べる。
   - 目標値 (1 tick) は変えない。変えたくなったら SELF_EVAL で planner に聞く。
3. **#6 (V8 / R5)**: server_verify のログに、クライアントごとの late-subst の割合 (生きていたレーン tick のうち代替入力になった割合) と強制再同期の回数を出す。ケース A は「late-subst 5% 以下かつ強制再同期 0」を合格条件にする。ケース D は数値を出すだけで合否には使わない。bat のヘッダコメントと docs\test_checklists.md に「ケース D は 1 台の PC で WARP の Runtime 4 台を回す負荷試験で、R3 の tick 時間は計測環境の制約を受ける」と書く。engine_spec §11.5 の R3 の記述も同じ趣旨に直す。
4. **#7 (V9、spec 4.1.7b)**: エディタがクライアントとして接続している間は、Pause / Step を無効にする (無効表示 + ツールチップに理由、文字列は Tr() の en/ja)。Stop は Bye でセッションを抜けて Play を終える。sim は常に simulateScripts = true で回す。
   - 確認方法: Release のエディタをクライアントにして接続し、Pause / Step が無効になったツールバーのスクショ 1 枚を `plans\m81-dedicated-server\shots\` に置く。Stop で Bye が出てサーバ側が Leave を記録することをログで示す。
   - 一時プローブで Pause を強制した場合に desync が繰り返されることを先に確かめておくと、修正の根拠になる (任意)。
5. **#8 (V10)**: ServerLoop の PeerTable (ServerLoop.cpp:35, 52-79, 297-300) で、Gone になった peer の宛先キーを回収する。上限 (1024) に達したら、以後の Hello を無視していることを ERROR ログに出す。回収を selftest で固定する (大量の再接続を偽装して、上限を超えても新規の Hello を受け付ける)。
6. **#10 (V11)**: 再接続の Hello で、旧所有者 (Gone 済み) の Leave が積まれ済み (leaveQueued) なら、2 本目の Leave を積まない (ServerSession.cpp:207-212, 359-367)。同じ受信周に「旧 peer の Gone」と「再接続の Hello」が届く場合の selftest を足す。確定入力に無効イベントが載らないこと、ERROR ログが出ないことを検査する。

7. (sub-10 VERDICT から、V12) **システム入力を持つ .rep (SessionConfig.role が Server / Client) をオフラインで再生するとき**、verify 用の sim (EngineLoop の verify と HeadlessSim の VerifyReplay) の NetRuntimeInfo を、D14 と同じ値で立てる。値は connected = 1、playerCount = .rep の SessionConfig.playerCount。今は既定値 (0 / 1) のままなので、ゲームが tick 中に NetIsConnected / NetPlayerCount を読むと、ライブとオフライン再生で割れる。
   - これを入れても、v13 の契約 (表示専用) は弱めない。目的は、サーバ構成の .rep の再生がライブと同じ値を見ることだけ。
   - 機種依存のまま残す値は NetLocalPlayer / NetPingMs / NetRollbackCount。再生では既定値。
   - P2P の .rep (システム入力なし) は今までどおり。
   - selftest: v13 の Net* を毎 tick 読んで sim 状態へ書くプローブを、サーバ構成で回す。サーバ .rep のオフライン再生 (HeadlessSim の VerifyReplay) が全 tick 一致することを固定する。プローブは検証専用で、どのシーンにも自動では付けない。

## やらないこと (このサブでは)
- sub-10 の範囲 (境界・D14・0 tick 判定・規則 13)
- AcceptPlayerSession の非同期化 (R-11)
- 目標到着余裕の変更

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Replay\Replay.h/.cpp`、`CrashRing.cpp` (開始 tick の扱い)、`src\Engine\Engine\Net\NetRollback.cpp` (desync.txt の文面)
- `src\Engine\Engine\Net\ClientSession.cpp`、`ClientSimRunner.cpp`、`ServerSession.cpp`、`ServerNetSelfTest.cpp`
- `src\Server\ServerLoop.cpp`
- `src\Editor\` の PlayModeController / ツールバー、LocalizationTable.inl
- `tools\server_verify.bat`、`docs\test_checklists.md`、`engine_spec.md` §11.5

## 受け入れ条件 (このサブ)
spec 5. の **V6, V7, V8 (= R5), V9, V10, V11, V12** と **C1〜C7**。

## 検証コマンド
- `tools\server_verify.bat` (ABCD。追加した検査行と late-subst の数値を貼る)
- `tools\replay_verify.bat`、`tools\net_verify.bat`、`pwsh -File tools\check_rules.ps1`
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (直列)、`Server.exe --selftest`
- V7 の 2 分実走ログ、V9 のスクショ

## 実装メモ (coder が追記)

## フィードバック履歴
