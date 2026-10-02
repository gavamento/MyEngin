# sub-10: 決定論の境界の修正 (review-1 #1 #2 #4 #9)

- 依存: sub-09 (コミット済み d16d77b)
- 状態: OK (commit c6ac1aa)
- 往復: 1

## やること
review-1 (`plans\m81-dedicated-server\review-1.md`) のうち、**サーバ .rep の中身と決定論の境界に関わるもの**。sub-08 (実疎通) より先に入れる。spec 5. の V1〜V5。

1. **#1 (blocker)**: ライブサーバの HeadlessSim は `TickServices::netLockstep` を立てていない (HeadlessSim.cpp:162-213 の BuildTickServices)。そのため、スクリプトの LoadGame / LoadPersist がサーバ上でだけディスクのセーブファイルを読み、sim に入る (TickRunner.cpp:729-748, 755-771)。
   - サーバ/クライアント構成のサーバでも、ネット中と同じ境界で回す。
   - EngineLoop (EngineLoop.cpp:900 の `netLockstep = netEnabled || clientEnabled`) と HeadlessSim が、**同じ関数**からこのフラグを導くようにする。例: SessionConfig / 役割から「ネット中の決定論境界か」を返す関数を Session/ か Loop/ に置く。
   - HeadlessSim の verify 経路 (`--replay-verify`) は今まで Verifying() で同じ境界になっていた。ライブとオフライン再生が同じ境界になることを確認する。
   - selftest (ServerNetSelfTest か新規): サーバ/クライアント構成で、tick 中に LoadPersist / LoadGame を積むプローブを用意する (既存の検証用スクリプトの流儀。デモへは自動で付けない)。サーバ側の save ディレクトリに実在のセーブファイルを置いた状態で回し、次の 2 つを固定する。
     - サーバとクライアントのハッシュ列が一致する。
     - サーバの確定ログの再生 (CheckReplayOfLog 相当) が一致する。
   - 負の対照: 修正前の境界 (netLockstep = false) では割れること。一時的に確認するだけでよく、方法は SELF_EVAL に書く。
2. **#1 の根本 (V2)**: EngineLoop と HeadlessSim の TickServices の組み立てで、ゲート系のフラグ (netLockstep / resim / app / recorder / player / prevWorld など、TickRunner.h:52-56 と :132-145 の一覧) の対応を、機械的に照合できるようにする。案は 2 つ。
   - ゲート系だけを決める小さな共通関数を作り、両者が呼ぶ。
   - selftest で HeadlessSim の TickServices を検査する。
   どちらを選んだかと理由を書く。TickServices の組み立て全体の統合 (sub-01 で許容した二重) までは求めない。
3. **#2 (blocker、spec D14 の未実装)**: サーバの NetRuntimeInfo を D14 の値で埋める。値は active = 1、connected = 1、playerCount = SessionConfig.playerCount、localPlayer = 0、role = Server。書く場所は ServerLoop か HeadlessSim の 1 か所。
   - サーバ構成で v13 の Net* (NetIsConnected / NetPlayerCount / NetLocalPlayer) が、この値を返す selftest を足す。
   - クライアント側の値 (EngineLoop.cpp:2176-2178) とサーバの値で、**ゲームが sim の中で使いうる値** (NetIsConnected / NetPlayerCount) が同じになることも検査する。
4. **#4 (major)**: `--replay-verify` は、照合した tick が 0 本、または復元後の tick が .rep の範囲外なら FAIL (exit 1) にし、理由をログに出す。Editor / Runtime (EngineLoop.cpp:2566) と Server (HeadlessSim.cpp:425) の両方で直す。
   - selftest で固定する。
   - review-1 #3 の 2 本 (`bin\x64\Debug\crash\desync_6768_p1\local.rep`、`cache\sv_A_c2.rep`。無ければ server_verify を回して作り直す) が、**このサブの時点では FAIL になること**を実測で示す。sub-11 で再生できるようになると PASS に変わる。
5. **#9 (minor)**: check_rules 規則 13-a で、HeadlessSim.cpp の許可を `OnlyInclude` 方式に絞り、NetRuntime.h と NetSession.h の include だけを許す。意図的な違反 (一時的に NetProtocol.h を include する) で exit 1 になることを確認する。

## やらないこと (このサブでは)
- クライアント .rep の再生、時刻同期、server_verify の合否基準、エディタの Pause、PeerTable、二重 Leave (sub-11)
- TickServices の組み立て全体の統合

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Loop\HeadlessSim.cpp` (BuildTickServices、VerifyReplay の判定 :425)
- `src\Engine\Engine\Loop\EngineLoop.cpp` (:900 の netLockstep、:2566 の判定)
- `src\Server\ServerLoop.cpp` (NetRuntimeInfo の書き込み)
- `src\Engine\Engine\Loop\TickRunner.h` (共通関数を置くなら。RunOneTick の契約は変えない)
- `src\Engine\Engine\Net\ServerNetSelfTest.cpp`、`src\Editor\SelfTest\PartSelfTest.cpp` (v13 の値の検査)
- `src\GameLogic\Scripts\` (プローブスクリプト)
- `tools\check_rules.ps1`

## 受け入れ条件 (このサブ)
spec 5. の **V1, V2, V3, V4, V5** と **C1〜C7** (C5 は review-1 で補強した観点で、ゲート系フラグを経由する外部 I/O を含めて点検し、SELF_EVAL に一覧を書く)。

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (直列)、`bin\x64\{Debug,Release}\Server.exe --selftest`
- `tools\replay_verify.bat`、`tools\net_verify.bat`、`tools\server_verify.bat`
- `pwsh -File tools\check_rules.ps1` (+ 13-a の意図的違反)
- #4: 0 tick の .rep 2 本を `Server.exe --replay-verify` と `Runtime.exe --replay-verify` にかけて exit 1

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-10 (round 1)
実装:
  - #1 TickRunner.h: NetLockstepBoundary(int netRole) を新設。EngineLoop.cpp は NetLockstepBoundary(netWanted ? config.netRole : 0)、HeadlessSim.cpp は BuildTickServices(netRole) へ NetRole::Server (systemInput = true のとき) を渡して同じ関数から netLockstep を導く。ライブサーバ / サーバ構成の sim でも LoadGame / LoadPersist が no-op になる。
  - #1 selftest (ServerNetSelfTest.cpp TestSaveLoadBoundary): 新規 probe スクリプト GameLogic\Scripts\SaveLoadProbe.cpp (指定 tick に LoadPersist / LoadGame を要求し PersistStore の値を sim 状態へ写す) を全 sim に付け、実在のセーブ (slot 7) を exe 隣の save_server に置いて、サーバ 1 + クライアント 3 で回す。固定した内容: (a) 全クライアントのハッシュ列 == サーバ (b) CheckReplayOfLog (c) サーバ・クライアントの sim にセーブが入っていない (requests == 2 かつ persistValue == -1) (d) サーバ確定ログを .rep に書き、Server.exe --replay-verify と同じ経路 (systemInput = false の HeadlessSim の VerifyReplay、Verifying 境界) で全 543 tick 一致。CheckReplayOfLog は probe 設定を含む開始 blob を使うよう startBlob を足した。
  - #1 根本 (V2): 案 2 つの両方を採用。(i) 共通関数 NetLockstepBoundary (EngineLoop / HeadlessSim が同じ関数)。(ii) TickGates / GatesOf / IsNetSessionGates (TickRunner.h) とアクセサ HeadlessSim::Gates()。ServerNetSelfTest がサーバ・クライアント sim 全部を IsNetSessionGates で検査し、再生専用 sim (systemInput = false) が session ゲートでないことを負の対照にする。EngineLoop のクライアント構成は起動時に同じ関数で検査してログ (不一致は ERROR)。理由: 共通関数だけでは recorder / resim / player など他のフラグの食い違いを見張れない。逆に selftest だけでは EngineLoop 側を検査できない。
  - #2 HeadlessSim.cpp Init: systemInput = true のとき NetRuntimeInfo を active = 1 / connected = 1 / role = Server / localPlayer = 0 / playerCount = ctx.playerCount (= SessionConfig.playerCount、ServerLoop が一致を検査済み) で埋める。書く場所はここ 1 か所。ServerNetSelfTest の CheckSessionNetInfo が ABI の NetIsConnected / NetPlayerCount / NetLocalPlayer を検査し、さらにクライアントの ClientSession (Running() / PlayerCount()、EngineLoop が netInfo.connected / playerCount へ写す値) とサーバの値が一致することも検査。アクセサ HeadlessSim::NetInfo() を追加。
  - #4 Replay.h/.cpp: JudgeReplayVerification(player, startTick, reason) を新設。不一致 / tick 0 本 / 開始 tick が範囲外 / 照合 0 本 のとき FAIL。EngineLoop.cpp (verifyStartTick を復元直後に記録、不合格は ERROR + exitCode = 1) と HeadlessSim::VerifyReplay (HeadlessVerifyResult::failReason を追加) の両方で使う。ServerMain.cpp の FAIL 行は failReason を出す。selftest: tick 0 本の .rep と開始 tick が範囲外の .rep (参加途中のクライアントの形) がどちらも FAIL (VerifyReplay を実際に走らせる)、判定関数の単体 (記録なし / 不一致)。
  - #9 tools\check_rules.ps1: OnlyInclude を配列対応にし、13-a の HeadlessSim.cpp を NetRuntime.h / NetSession.h の 2 つの include だけに限定。
仕様との差分:
  - [追加] HeadlessSim の NetRuntimeInfo は setup.systemInput = true の全 sim (selftest のクライアント sim も含む) へ role = Server で書く。クライアント sim は ABI を読まないので無害だが、role の値はサーバのものになる。
  - [追加] 新規ファイル src\GameLogic\Scripts\SaveLoadProbe.cpp (検証専用の恒久 probe。NetEventProbe と同じ流儀、どのシーンにも自動では付かない)。gen_project_files.ps1 で build\GameLogic.vcxproj / .filters が更新される。
  - [追加] HeadlessSim に Gates() / SaveDir() / NetInfo() の const アクセサ、HeadlessSim.h が TickRunner.h を include (selftest 用)。
  - [追加] #4 の失敗理由は reason 文字列で、「0 tick」だけでなく「照合が 0 本 (期待ハッシュ無しだけで走った)」も FAIL にした (spec 4.1.7 の趣旨: 何も比べていない PASS を作らない)。クラッシュ .rep でも replay_verify (crash_verify 経路) は PASS のまま。
  - [未実装] なし。
検証:
  - MSBuild MyEngine.sln Debug|x64 / Release|x64 → どちらも exit 0 (tmp\m81j_build_*.log)
  - Editor.exe --selftest Debug (CWD = リポジトリ直下) → exit 1、FAIL は基点から既知の Source control 2 項目だけ。Server/client net self test ALL PASS (新 V1-V4 試験は Debug 17.8 s)。Release も同じ (70 s)。再実行 (最終コード) でも同じ (tmp\m81j_selftest_*.log / .err)
  - Server.exe --selftest Debug / Release → exit 0
  - 負の対照 (V1): HeadlessSim の BuildTickServices(0) へ一時的に戻して Debug ビルド + Editor --selftest → 9 FAILED: 全 sim の gates 検査、サーバ・クライアント sim に persistValue 1234 が入った (セーブを読んだ)、サーバ .rep のオフライン再生が「hash mismatch at tick 180」(= LoadPersist の tick) で割れた。戻して再ビルド後に ALL PASS。注: selftest のクライアントも HeadlessSim なので、この一時変更では全 sim が同時に境界を外し、サーバ対クライアントのチェーン一致は割れない (実機で割れる = EngineLoop 側だけ境界が立っている構成は、サーバ sim の境界を外してオフライン再生 = Verifying と比べる (d) で代わりに示している)
  - pwsh -File tools\check_rules.ps1 → 0 error(s) / 0 warning(s)。13-a の意図的違反 (HeadlessSim.cpp に一時的に NetProtocol.h を include) → 規則 13-a のエラー 1 件、exit 1。戻して 0 error
  - #4 実測: bin\x64\Debug\crash\desync_6768_p1\local.rep (開始 6664、108 tick) と cache\sv_A_c2.rep (開始 6665、600 tick) を Release の Server.exe と Runtime.exe の --replay-verify にかけた 4 通りすべて exit 1、「VERIFY FAIL: the restored start tick N is outside the .rep's tick range」。sub-11 で再生可能にすると PASS に変わる
  - tools\replay_verify.bat → exit 0 (14 ジョブ PASS、9 シーン + タイムトラベル + What-if + 規則)
  - tools\net_verify.bat → exit 0 [PASS] (net_verify の --replay-verify は P2P の開始 tick 0 の desync バンドルで、FAIL に変わらないことを確認)
  - tools\server_verify.bat (ABCD) → exit 0、759 s、[PASS]。server_verify がクライアント .rep を --replay-verify にかけていないことを確認 (check_replays は server .rep だけ。クライアントは --rep-diff)
自己採点 (1-5):
  仕様適合: 4 — V1〜V5 を実装し、V4 は実 .rep 2 本で FAIL を実測。ただし NetLocalPlayer / NetPingMs などクライアントと値が違う v13 スロットは契約どおり (機種依存) のまま
  正しさ: 4 — 全検証コマンドが期待どおり。負の対照で境界を外すと検出できることを確認。EngineLoop 側のゲート検査は起動ログ止まりで、実プロセスの Editor クライアント経由は未実走
  コード品質: 4 — 共通関数 + ゲート表の 2 段。HeadlessSim の role がクライアント sim にも Server になる点は割り切り
  テスト: 4 — サーバ/クライアント構成の LoadPersist / LoadGame、オフライン再生、D14、0 tick の FAIL を固定。負の対照は一時変更の手動確認で、恒久テストにはしていない
不安・質問:
  - オフライン検証 (Server.exe / Runtime --replay-verify、EngineLoop の verify) の sim は systemInput = false で NetRuntimeInfo が既定 (NetIsConnected = 0 / NetPlayerCount = 1) のため、ゲートが tick 中に Net* v13 を sim へ使うとライブのセッション (1 / N) と .rep 再生で割れる。P2P の既存契約 (表示専用、書き戻し禁止) と同じ扱いで D14 の範囲外としたが、v13 の値を sim に使うゲームを禁止する契約を強めるか (再生側も .rep の SessionConfig から D14 の値を立てるか) は planner の判断
  - Editor.exe (クライアント) を実プロセスで起動して gates のログ行を見る確認は未実施 (sub-08 / sub-11 の実走に任せる)
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\TickRunner.h
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\HeadlessSim.h
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\HeadlessSim.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Net\NetRuntime.h
  - C:\HAL\MyEngin\src\Engine\Engine\Net\ServerNetSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Replay\Replay.h
  - C:\HAL\MyEngin\src\Engine\Engine\Replay\Replay.cpp
  - C:\HAL\MyEngin\src\Server\ServerMain.cpp
  - C:\HAL\MyEngin\src\GameLogic\Scripts\SaveLoadProbe.cpp (新規)
  - C:\HAL\MyEngin\tools\check_rules.ps1
  - C:\HAL\MyEngin\build\GameLogic.vcxproj / GameLogic.vcxproj.filters (gen_project_files.ps1 の出力。SaveLoadProbe.cpp を足したため必要)
申し送り:
  - C5 自己点検 (sim へ値が入る経路と TickServices のゲート系外部 I/O)。確定入力経由: ApplyConfirmedInputs (レーン入力 + SystemInputTick)、参加・再同期のスナップショット復元、SessionConfig (いずれも .rep に載る)。ゲート系フラグを経由する外部 I/O: LoadGame / LoadPersist のディスク読み (Recording || Verifying || Networked で no-op。ライブサーバ / クライアント / オフライン再生が今回すべて同じ境界になった)、LoadScene (assets のファイルを読む。全経路で許可、内容は provenance の contentHash で照合)、C# レーン (Recording / Verifying / Networked / resim で停止。サーバは ManagedHost 未初期化)、SaveGame 書き出し (出力レーンで sim を読み書きしない。resim でだけ抑止。サーバは save_server に書く)、オーディオ drain (出力レーン、resim で抑止、サーバは device 無し)、prevWorld 採取 (描画補間用、サーバは null)、config.netPokeTick / hashDump (検証用、sim の外)。TickServices のうち ctx.simulateScripts は HeadlessSim の OnTick が常に true。EngineLoop のクライアントの Pause 時の false は review-1 #7 (sub-11)
  - D14 の v13 値: NetIsConnected / NetPlayerCount はクライアントと同じ。NetLocalPlayer (サーバ 0 / クライアントはレーン)、NetPingMs、NetRollbackCount は spec D14 どおり機種依存のまま
  - ABI は変更していない
  - 検証ログは tmp\m81j_*。selftest が exe 隣の save_server\slot7.json を一時的に作って消す (save_server は HeadlessSim の置き場で Editor / Runtime の save とは別)
```

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。V1 は共通関数と SaveLoadProbe で確認した。サーバ/クライアントのハッシュ一致、ログ再生、Verifying 境界でのオフライン再生 543 tick、負の対照 (境界を外すと tick 180 で割れる) が揃っている。V2 は共通関数とゲート表の検査の 2 段で確認。V3 は D14 の値と、クライアントとの一致で確認。V4 は判定関数を共通化し、実 .rep 2 本 × 2 exe で FAIL になることで確認。V5 は OnlyInclude の配列化で確認。C1〜C7 も PASS。不安 1 (オフライン再生の NetRuntimeInfo) は sub-11 の V12 へ回した。負の対照の残骸 (bin 配下の .mismatch.txt / .actual.dump) の削除はユーザーの許可を取ってから。
