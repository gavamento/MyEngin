# sub-01: Server.exe: マネージド EC2 の起動・停止要求の一本化・ログの向け先

- 依存: なし
- 状態: OK (commit 45131630)
- 往復: 0

## やること

spec 4.1.1 の表をすべて。

1. `--hosting gamelift-managed`: 引数なし `Aws::GameLift::Server::InitSDK()` を呼ぶ経路。既存の 30 秒上限 (`kInitTimeout`) と
   切り離しスレッドの扱いは Anywhere と同じ。Anywhere 用の引数 (`--gamelift-ws-url` / `-fleet-id` / `-host-id` / `-process-id`)
   との同時指定は InitSDK 前に 1 行の理由を出して exit 1。トークン (環境変数) は要求しない。
2. 停止要求の一本化: コンソールの Ctrl+C / Ctrl+Break / 閉じる と、新設 `--stop-event <名前>` (既存の名前付きイベントを開く。
   開けなければ exit 1) を、**ホスティングの種類によらず** Terminate として ServerLoop の既存経路へ入れる
   (D8 のバグ修正: 今は `LocalHosting` だけがコンソールハンドラを持つ)。GameLift なら .rep を閉じる → ProcessEnding → Destroy → exit 0。
3. `--log-dir <dir>`: ディレクトリを作り、`<dir>\server-<pid>.log` へ stdout / stderr を向ける。そのパスを ProcessReady の
   logPaths に足す (`.rep` があればそれも残す)。
4. stdout / stderr がファイル・パイプのときもログが 1 行ごとに届く (D10)。
5. `PrintUsage` の更新。

## やらないこと (このサブでは)

- エディタ側の変更 (窓・CLI 層・サーバ版の組み立て)
- 文書 (`docs/gamelift-anywhere.md` 8 章の修正は sub-06)
- 版の変更

## 触る場所 (planner の見立て)

- `src/Server/ServerMain.cpp`: `ServerOptions` (hosting 値・stopEvent・logDir)、引数の解析 (`wmain` の if 連鎖)、`MakeHosting`
  (gamelift-managed の分岐と矛盾検査)、`PrintUsage`。ログの向け先は sim 起動前 (`RunLiveServer` の先頭か `wmain`) で。
- `src/Server/Hosting/GameLiftSdk.h`: InitSDK の 2 形を表す窓口 (例: `GameLiftConnection` に方式の enum を持たせる、または
  `InitSdkManaged()` を足す)。**SDK の型をここへ漏らさない** (規則 13-c)。
- `src/Server/Hosting/GameLiftSdkAws.cpp`: 引数なし `InitSDK()` の呼び出し (`GameLiftServerAPI.h:53`)。
- `src/Server/Hosting/GameLiftHosting.{h,cpp}`: Init のログ文言 (managed のときは fleet/host が空)。
- `src/Server/Hosting/LocalHosting.cpp`: コンソールハンドラ (`g_terminateRequested`) を共通の受け口へ移す。
  置き場所の案: `src/Server/` に小さな停止要求の型 (コンソールハンドラ + 名前付きイベントを持ち、`Requested()` を返す) を作り、
  ServerLoop が tick 境界で見て Terminate と同じ扱いにする。または各ホスティングの Poll が見る。どちらでも、
  「停止要求 → Terminate」の経路が 1 本になっていること。
- `src/Server/ServerLoop.{h,cpp}`: 停止要求を見る位置 (既存の `HostingEventKind::Terminate` 処理 `ServerLoop.cpp:114` に合流)。
  `NotifyReady` に渡す logPaths (`ServerLoop.cpp:158`)。
- `src/Server/ServerSelfTest.cpp`: 偽 SDK (`InitSdk` の呼ばれ方を記録)、停止イベントの実プロセス試験
  (既存の `TestRealConsoleTerminate` と同じ作り。こちらはコンソール不要なので SKIP しない)、`--log-dir` と行単位出力の試験。
- 注意: `ServerMain.cpp:217-223` は hosting.Init 失敗時に TerminateProcess で落とす。managed でも同じでよい。

## 受け入れ条件 (このサブ)

spec 5. の 1〜5。

1. 偽 SDK で `--hosting gamelift-managed` 相当の構成 → 引数なし InitSDK が 1 回、ProcessReady 以降は既存どおり。併記は exit 1 (実プロセスで確認してよい)。
2. stop イベントのシグナル → local / gamelift の両方で Terminate。GameLift は ProcessEnding / Destroy が 1 回ずつ。実プロセス (local、`--replay-record` 付き) で exit 0 かつ .rep が正しく閉じる (`!RecoveredUnfinished()`)。存在しない名前は exit 1。
3. GameLift ホスティングでもコンソールの停止が Terminate になる (偽 SDK で ProcessEnding)。コンソールが無い環境は既存どおり SKIP してよい。
4. `--log-dir` でファイルができ、logPaths に入る。stdout をファイルへ向けた実プロセスで、終了前に `ready on UDP port` の行が読める。
5. 既存の Server / net の自己テストが通る。ABI / snapshot / proto / replay の版が変わっていない (`git diff` で該当定数に変更が無い)。

## 検証コマンド

- `bin\x64\Debug\Server.exe --selftest` (と Release)
- `MYE_SELFTEST_ONLY=servernet,session` (名前は `Editor.exe --selftest-list` で確かめる) で `bin\x64\Debug\Editor.exe --selftest`
- `tools\check_rules.ps1`
- 手で: `Server.exe --hosting gamelift-managed --gamelift-fleet-id x` → exit 1 と理由 1 行

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
- 停止要求の受け口は ServerLoop 案 (新設 `StopRequest` を `ServerLoopConfig::stop` で渡し、Poll と同じ位置で Terminate に合流)。
- `--hosting gamelift-managed` は `IGameLiftSdk::InitSdkManaged()` を追加、`GameLiftHostingOptions::managedEc2` で選ぶ。
- ログ: wmain 先頭で stdout / stderr を `_IONBF`、`--log-dir` は CreateFileW (共有許可) + `_dup2` で fd 1/2 を差し替え。
- 検証: Server.exe --selftest (Debug / Release) exit 0、check_rules 0 error、Editor `servernet,session` exit 0、手で managed 併記 exit 1。

## フィードバック履歴

- round 1: VERDICT OK (planner, 2026-11-27)。planner が `bin\x64\Debug\Server.exe --selftest` を再実行して exit 0・M107 の 25 項目 PASS を確認 (81 秒)。指摘は nit 2 件 (申し送り): StopRequest.h の `<Windows.h>` を `void*` にしてヘッダの汚染を避ける余地 / `--log-dir` 時は MakeHosting の矛盾エラーもログファイルへ行く点を sub-06 の文書に書く。
