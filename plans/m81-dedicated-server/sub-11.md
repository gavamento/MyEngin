# sub-11: クライアント記録の再生・時刻同期・運用の修正 (review-1 #3 #5 #6 #7 #8 #10)

- 依存: sub-10 (同じファイルを触るので直列にする)
- 状態: OK (コミット待ち)
- 往復: 1

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

```
SELF_EVAL: sub-11 (round 1)
実装:
  - Replay.h/.cpp: ReplayPlayer — StartTick() (埋め込みスナップショットあり = startMeta.tick、無し = 0) を基点に InputForTick / SystemInputForTick / ExpectedHash / HasTick を引く (V6)。記録の通し番号で引く InputAt / SystemInputAt / HashAt を追加し、--rep-diff の DiffTickRange はそちらへ (セッション tick で名指しする挙動は不変)。JudgeReplayVerification の理由文を更新
  - NetRollback.cpp: WriteNetDesyncBundle — role == 4 (クライアント) の desync.txt を「サーバの --replay-record の .rep を検出 tick まで再生して dump → local.dump と --hash-diff」へ (checkpoint tick で両方を再生する手順も併記)
  - ServerSession.cpp/.h (V7, V8, V11): 到着余裕を tick ごとに 1 標本にして平均を Confirmed に載せる (NoteMargin、標本なしの送信は flags 無効)。遅れて着いた入力の本当の遅れを、基準 = min(締め切り, 実際の確定時刻) で標本にする (HandleClientInput。待たれていないレーンは確定時刻基準。lateSampledTick で冗長送信の同じ tick を二度数えない)。確定時の代替入力では余裕を上書きしない。confirmMs_ (tick -> 確定時刻)。ServerStats に laneWaitedTicks[] / laneLateSubst[] / forcedResyncs。HasPeer()。再接続 Hello は pending_ の Leave を見て 2 本目を積まない
  - ClientSession.cpp/.h, ClientSimRunner.cpp (V7): 速度係数と追いつきを同じ誤差 MarginErrorTicks() = (目標 - 到着余裕) / tick から導く。CatchUpTicks(clientTick) (確定フロンティア基準) を廃止し、PlanCatchUp() が誤差 > 2 tick のとき floor(誤差) を 1 回だけ決める (CatchUpPending / OnCatchUpTickRan)。追いつき後は誤差を捨て RTT + 100ms 標本を捨てる
  - Server/ServerLoop.h/.cpp (V10): PeerTable をヘッダへ出し、キーは単調増加 (再利用しない) の (宛先, キー) 表に。Sweep(keep) で session.HasPeer(key) が偽の宛先を毎周回収。満杯 (1024) の Hello は ERROR ログ 1 回 + HellosIgnoredWhileFull() で数える。終了時にレーンごとの "lane N: waited on W tick(s), late-subst S (P%)" と "forced resyncs: N" を出す
  - HeadlessSim.cpp VerifyReplay / EngineLoop.cpp verify (V12): システム入力を持つ .rep の再生で netInfo.connected = true、playerCount = SessionConfig.playerCount。EngineLoop.cpp: クライアント 10 秒ごとの "[client] time sync: ..." ログ、ctx.netLeaveRequested で Bye を送って tick を止める (clientLeft)
  - EngineLoop.h: EngineContext::netLeaveRequested、EngineConfig::kNetRoleClient (= 4)
  - Editor (V9): PlayModeController::SetNetClientLocked (Pause / Step が no-op)、EditorApp::serverClient (OnTick は simulateScripts を常に true、EditorMain が --net-connect で立てる)、EditorToolbar (Pause / Step / Play を無効表示 + ツールチップ、接続中の Stop は ctx.netLeaveRequested)、LocalizationTable.inl に Tool_TipPauseLockedNet / Tool_TipStopNet / Tool_TipPlayLockedNet (en / ja)
  - GameLogic/Scripts/NetInfoProbe.cpp (新規、V12 の検証専用 probe。どのシーンにも自動では付かない。build\GameLogic.vcxproj(.filters) は gen_project_files.ps1 の生成物)
  - ServerNetSelfTest.cpp: TestTimeSync (V7: 遅延一定 / 25→75ms / 90ms 片道で半秒遅れて参加 / 3 台ジッタ。1 秒窓の平均が全部 目標 ±1 tick。窓平均をログに出す)、TestReconnectLeavesOnce (V11、ERROR ログ件数も検査)、TestSaveLoadBoundary に V6 (クライアント形 .rep の単独再生 + サーバ .rep の同じ tick のダンプと DiffHashDumps が一致)、V4 (startMeta.tick がスナップショットとずれた .rep は範囲外で FAIL)、V12 (NetInfoProbe の読み値)
  - ServerSelfTest.cpp: TestPeerTable (V10: 満杯で無視 / Sweep で回収 / キー再利用なし / 上限の 5 倍の再接続)
  - SessionSelfTest.cpp / CrashRingSelfTest.cpp: 記録の通し番号で引いていた箇所を InputAt / HashAt / SystemInputAt へ。V6 の tick 引きの検査を 1 つ追加
  - tools/server_verify.bat: クライアント .rep の単独再生 (A)、late-subst と強制再同期 (A は合否、B / D は表示のみ)、診断バンドルの local.rep 単独再生 + local.dump とサーバ .rep のダンプの --hash-diff (C)。ヘッダに D の負荷試験の注記
  - docs/test_checklists.md、engine_spec.md §11.5: R3 / D の計測環境の制約、時刻同期の説明、クライアント .rep の扱い、Editor の Pause / Step 無効、v13 Net* の再生時の値
仕様との差分:
  - [追加] ReplayPlayer::StartTick() は「スナップショットを埋めた記録だけ startMeta.tick、無い記録は 0」。スナップショット無しの記録は復元で tick を戻せず 0 から走るので、startMeta.tick だけ非 0 のファイルを読んでも従来どおり 0 から引く (v8 と開始 tick = 0 の既存 .rep は 1 バイトも変わらない)。--rep-diff-overlap は従来どおりヘッダの startMeta.tick を直接使う
  - [追加] 追いつきの実装方式: 仕様どおり確定フロンティアは基準にしていないが、到着余裕だけでは「サーバが待っていない/遅れて着いた入力の遅れ」が見えない (確定時の代替入力は余裕 ≒ 0 としか言えない) ので、サーバ側の標本の取り方を変えた (遅れて着いた入力の実際の遅れを負の余裕として報告、tick ごとに 1 標本の平均で送る)。Confirmed のペイロード形式は不変 (marginMs は int32 のまま、意味だけ「前回送信からの標本の平均」)。kNetProtoVersion は上げていない (旧クライアントは値の意味が違うだけで動く)
  - [追加] ClientSession: 追いつきの後 RTT + 100ms (kCatchUpHoldExtraMs) は余裕の標本を捨てる。捨てないと追いつき前に送った入力の報告が往復して戻り、同じ遅れを二度数えて行き過ぎる
  - [追加] V11 は「旧所有者の leaveQueued」ではなく pending_ に同じ playerId の Leave が積まれ済みかで判定した (FindOwnerOfLane が Gone を返さないので peer の旗は見えない。結果は同じ)
  - [追加] V12 の NetInfoProbe は新しい検証専用 probe (NetEventProbe に足さず別ファイル)。InitSim が全 sim に付けるので、既存シナリオのハッシュ列にも載る (全 sim 共通なので一致は崩れない)
  - [追加] server_verify のケース A のクライアント (c1, c2) だけ --warp を外した (実 GPU)、c1 の記録を 5x → 8x tick に。理由は不安・質問 1 のとおり。他のケースは --warp のまま
  - [追加] Editor: 接続中は Play ボタンも無効 (Stop 後に Play しても、抜けたセッションの上で世界が再始動して意味が無い)。再接続は Network 窓から起動し直す
  - [追加] ServerStats に forcedResyncs / laneWaitedTicks / laneLateSubst を足した。late-subst の「生きていたレーン tick」の分母は WaitsOnLane (最初の有効な入力以降) の tick
検証:
  - Release/Debug build (MyEngine.sln) → 0 error / 0 warning
  - bin\x64\Debug\Editor.exe --selftest / Release (直列、CWD = リポジトリ直下) → どちらも exit 1。FAIL は基点から既知の Source control 2 項目のみ。Session self test ALL PASS、Server/client net self test ALL PASS (tmp\m81k_selftest_Debug3.log / m81k_selftest_Release3.log。Debug は約 6 分)
  - Debug / Release Server.exe --selftest → exit 0 ALL PASS (V10 の PeerTable 6 項目を含む)
  - tools\replay_verify.bat → exit 0 [PASS] (14 ジョブ、tmp\m81k_replay_verify3.log)
  - tools\net_verify.bat → exit 0 [PASS] (tmp\m81k_net_verify3.log)
  - pwsh -File tools\check_rules.ps1 → 0 error(s), 0 warning(s)
  - tools\server_verify.bat (ABCD) → exit 0 [PASS] (tmp\m81k_server_verify3.log)。追加検査の行: A: "client .rep c1 replays alone: PASS verified 4800 ticks" / "c2 ... verified 600 ticks"、"lane 0: waited on 4790 tick(s), late-subst 1 (0.02%)" / "lane 1: waited on 591 tick(s), late-subst 7 (1.18%)" / "late-subst within 5 % on every lane: ok" / "forced resyncs: 0 ok"。C: "bundle: ...\desync_<tick>_p1" / "bundle local.rep replays alone, matches before tick 6600 and differs there: PASS (verified 100 ticks - VERIFY FAIL: hash mismatch at tick 6600)" / "--hash-diff names the corrupted field: Main Camera LocalTransform.position"。B (WARP・ロス 20%): lane 0/1/2 の late-subst 51.8% / 61.0% / 80.9%、D (WARP x4): 92.5% / 94.7% (lane 2, 3 は最後まで待たれず)。B と D は表示のみ
  - sub-10 で FAIL (exit 1) になっていた形の再検証: cache\sv_A_c2.rep → Release Server.exe --replay-verify "verified 600 ticks - VERIFY PASS" exit 0 (sub-10: FAIL)。review-1 の bin\x64\Debug\crash\desync_6768_p1\local.rep → "verified 100 ticks - VERIFY FAIL: hash mismatch at tick 6764" (6764 は注入した壊し tick。以前は 0 tick で PASS)
  - V7 実プロセス (Release Server + Release Runtime 実 GPU、ロス 0、2 分 = 7200 tick、cache\m81k_v7c_* は WARP / cache\m81k_v7d_* は実 GPU の 1 分): 実 GPU 1 分 (3600 tick): 到着余裕 16.1 / 15.9 / 16.9 / 15.9 / 16.2 / 16.1 ms (10 秒ごと、目標 16)、catch-up 0 / 3600 tick (0.0%)、stall 0、サーバの late-subst 0 (0.00%)、強制再同期 0。WARP の Runtime 2 分 (7201 tick): 到着余裕 15.0 〜 18.3 ms で定常、catch-up 0。review-1 の実測 (Editor・実 GPU・2 分: 127 → 94.7 → 92.9 ms、catch-up 136 / 6770 = 2.0%) とは、約 77 ms 上に張り付く状態が解消し catch-up の釣り合いも無い
  - V7 selftest (Release / Debug とも同じ値): 25ms 一定 → 75ms: [10 s, 20 s) と [30 s, 40 s) の 1 秒窓の平均が全部 16 ms。3 台 30ms ± 10ms ジッタ: 窓平均 15 〜 17 ms。90ms 片道 (半秒遅れて参加): 16 ms
  - V9 実走 (Release Server + Release Editor、実 GPU、CWD = scratchpad): plans\m81-dedicated-server\shots\net_client_pause_disabled.png (一時停止ボタンが無効表示、Network 窓は arrival margin 17.0 ms)。Stop をクリック (Win32 マウス入力) → Editor ログ "[play] stopped (scene restored)" / "[client] left the session (Stop in the editor): Bye sent, the sim stops ticking"、サーバログ "peer 1 (player 1, lane 0) dropped: bye" / "tick 3095: player 1 left lane 0 (reserved)"、session summary "leaves 1"。Editor クライアントの EngineLoop ゲート検査ログ行: "[net] tick gates: netLockstep=1 resim=0 recorder=0 player=1" (IsNetSessionGates 通過、ERROR なし)。リポジトリ直下の imgui.ini の SHA256 は実走の前後で不変 (BDA64133...DAC4)
  - 負の対照 (直して戻した): V11 の修正を外すと "leaves 2, rejoins 1" と ERROR 2 件で selftest が FAIL。V12 の修正を外すとオフライン再生が tick 0 で "hash mismatch" (verified 0 of 539)、NetInfoProbe は connected ticks 0 / player count 1
  - 未実行 / 限界: Pause を強制した場合に desync が繰り返されることの一時プローブ (任意の項目) は撮っていない。ツールチップの見た目 (ホバー表示) は目視していない。--lang ja の見た目は未確認。Editor の Stop 後のスクショ (抜けた後の絵) は撮っていない。GameLift 実接続は sub-08 (ユーザー作業待ち)
自己採点 (1-5):
  仕様適合: 4 — V6〜V12 を全て実装し検証したが、R5 のケース A の合否は「A だけ実 GPU のクライアントにする」環境変更を伴う (質問 1)。それ以外の差分は上記 [追加] に全て出した
  正しさ: 4 — selftest (Debug/Release 同値) と実プロセス (server_verify ABCD、2 分の実走、Editor 実走) で確認。負の対照も取った。ジッタが大きいクライアントでは目標 1 tick が代替入力の割合を押し上げる (質問 1)
  コード品質: 4 — 速度係数と追いつきを 1 つの誤差に集約、再生側の tick 引きと通し番号引きを型の違う名前で分離、PeerTable をテスト可能にした。ServerSession の標本の取り方 (3 経路) は説明コメントが必要で、やや複雑
  テスト: 4 — V6 / V7 / V10 / V11 / V12 に自動検証と負の対照。Editor の Pause / Step 無効は実走のスクショとログのみで自動テストは無い (ImGui の操作)
不安・質問:
  1. [planner に判断を求める — 目標 1 tick が実環境で厳しい] 到着余裕の平均を目標 16ms (1 tick) に収束させると、到着のジッタ (σ) が大きいクライアントでは締め切りを越える tick が多くなる。実測: Release Runtime + WARP (低フレームレートで tick がまとめて走る) = late-subst 26% (cache\m81k_v7c_server.log、余裕の平均は 15〜18 ms で目標どおり)、同じ構成を実 GPU にすると 0.00%、Release Editor (実 GPU、UI の描画で数十 ms の引っかかりがある) = 約 14% (cache\m81k_v9_server.log)。server_verify のケース B (WARP + ロス 20%) は 52〜81%、D (WARP x4) は 90% 超。review-1 では約 90ms のまま張り付いていたことが、ジッタに対するクッション (約 80ms) の役目を偶然果たしていた。V7 を満たすほど late-subst が悪化する。案: (a) 目標を「1 tick + ジッタの 2σ」のように、サーバが測る標本の分散で適応させる (ジッタが 0 の selftest では今と同じ 16ms に収束、実環境では余裕が自動で増える)、(b) 目標を固定のまま 3 tick 程度に上げる、(c) 今のまま (遅い/ジッタの大きい端末は代替入力が増えるが、操作の遅延は最小)。私は目標値を変えていない。ケース A は暫定で「実 GPU のクライアント」にして R5 の合否 (5% 以下) を通している (WARP のままだと lane 0 が 73%、lane 1 が 24%: 標本の平均化を入れる前の版での server_verify A の実測。平均化後の WARP 単独 1 台は 26%)
  2. [planner] R5 の「強制再同期 0」は、サーバの "fell N ticks behind - forcing a resync" の回数 (ServerStats::forcedResyncs) で判定している。クライアント自身が要求した再同期 (desync / EventGap / BadSnapshot) は含めていない (ケース A のクライアントのログには resync request(s) として出る)。含めるべきなら指示がほしい
  3. [参考] 往復 300ms (片道 150ms) のリンクでは、クライアントは予測上限 12 tick のため永遠に「最初の有効な入力」に届かず、サーバに待たれないまま (laneWaited 0、late-drop が増え続ける)。これは sub-04 の N4 の「RTT 300ms は stall する」と同根で、今回の変更前からの性質。ケース D (WARP x4、1 台の PC) でもクライアント 3, 4 がこの状態になっている。直すなら再同期の仕様 (遅れすぎのクライアントを再スナップショット) の話になる
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Replay\Replay.h、Replay.cpp、CrashRingSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Session\SessionSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Net\NetRollback.cpp、ServerSession.cpp / .h、ClientSession.cpp / .h、ClientSimRunner.cpp、ServerNetSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp / .h、HeadlessSim.cpp
  - C:\HAL\MyEngin\src\Server\ServerLoop.cpp / .h、ServerSelfTest.cpp
  - C:\HAL\MyEngin\src\Editor\App\EditorApp.cpp / .h、EditorMain.cpp、Editor\Scene\PlayModeController.cpp / .h、Editor\Widgets\EditorToolbar.cpp
  - C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl
  - C:\HAL\MyEngin\src\GameLogic\Scripts\NetInfoProbe.cpp (新規)
  - C:\HAL\MyEngin\tools\server_verify.bat、docs\test_checklists.md、engine_spec.md
  - C:\HAL\MyEngin\plans\m81-dedicated-server\shots\net_client_pause_disabled.png (新規)
  - (生成物だが追跡対象) C:\HAL\MyEngin\build\GameLogic.vcxproj、GameLogic.vcxproj.filters (NetInfoProbe.cpp の追加)
申し送り:
  - CrashRingSelfTest / SessionSelfTest は ReplayPlayer の通し番号引きに InputAt / HashAt / SystemInputAt を使うようになった。tick で引く旧名 (InputForTick など) は sim の ctx.tickIndex を渡す口で、開始 tick ≠ 0 の記録に通し番号を渡すと範囲外参照になる (Debug の selftest が CRT の assert ダイアログで止まった。直した)
  - Editor の Debug --selftest は 6 分、Release は 70 秒。Debug Editor.exe を Start-Process -NoNewWindow で回すときは、ダイアログで止まっていないか CPU 時間で見ること
  - 時刻同期のログ: クライアントは 10 秒ごとに "[client] time sync: arrival margin ..., catch-up N" を出す。サーバの終了時ログに "lane N: ... late-subst P%" と "forced resyncs"
  - 一時的な検証用の変更 (環境変数での selftest 分岐、サーバの標本ヒストグラム) は全部外した (grep TMPHACK / TMPDEBUG / MYE_TMP は 0 件)
```

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。確認した受け入れ条件は次のとおりで、すべて PASS。V6 (sv_A_c2 は 600 tick PASS、review-1 のバンドルは壊した tick で FAIL)、V7 (selftest で窓平均 16ms、実 GPU で 16ms に収束、catch-up 0)、V8/R5 (ケース A は実 GPU で 0.02% / 1.18%)、V9 (スクショ、Stop → Bye → Leave)、V10、V11、V12 (いずれも負の対照あり)、C1〜C7。差分の承認: StartTick の定義、サーバの余裕の標本の取り方 (proto は据え置き)、V11 の判定方法、Play も無効にしたこと。目標値 (不安 1) は spec D18 で裁定した (適応目標、[ユーザーに聞ける])。実装は sub-12。ケース A を実 GPU にした暫定措置は sub-12 で WARP に戻す。不安 2 (クライアント要求の再同期) と 3 (RTT 300ms) も sub-12。
