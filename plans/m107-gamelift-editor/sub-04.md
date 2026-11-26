# sub-04: GameLift 窓: 共通部と Anywhere の手順

- 依存: sub-01 (`--stop-event` / 行単位ログ)、sub-03 (CLI 層)
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.4 (共通) と 4.1.5 (Anywhere)、4.3 のうち Anywhere タブと確認ダイアログ。

1. **窓** `GameLiftWindow` を Window メニューの Network の隣に足す (`EditorApp.cpp:1977` 付近)。既定サイズ 640x560。
   上段の状態 (CLI の場所と版 / アカウント / リージョン / 再確認 / aws login を開く)、タブ (Anywhere / EC2 = EC2 は「sub-05 で実装」の空タブでよい)、
   下段 (実行中の CLI と取消、直近のエラー)。
2. **手順の状態機械** (UI から切り離した型。偽 CLI を差して自己テストで回せること):
   準備 / サーバ起動 / セッションを作って参加 / サーバ停止 / 片付け。各段の CLI の順序・冪等性・再試行・待ちの上限は spec 4.1.5 の表のとおり。
   1 窓で同時に走る CLI は 1 本。
3. **確認ダイアログ** (モーダル、既定フォーカスは「やめる」): 準備 (作成)・片付け (削除)。文言に資源名とリージョン。
4. **Server.exe の起動と監視**: トークンは `get-compute-auth-token` (秘密フラグ) で取り、子の環境だけへ。名前付き stop イベントを作って
   `--stop-event` で渡す。kill-on-close のジョブに入れる (`HostServerProcess.cpp:85` と同じ考え方)。stdout/stderr は
   `<project>\cache\gamelift\server-<日時>.log` へ (sub-01 で行単位に届く)。窓はログを読んで `ProcessReady sent` で「待機中」、
   プロセス終了で exit code とログ末尾 8 行を出す。停止 = イベント → 10 秒待つ → 終わらなければ TerminateProcess + 警告。
   エディタ終了時も同じ停止手順。
   - (VERDICT sub-03 round 1 の回答) Server.exe の起動には `CliProcess` を**使わない**。`CliProcess` は stdout を全部メモリへ溜めるので、短い CLI 呼び出し専用とする。
     Server.exe は stdout / stderr をファイルへ向ける専用の起動にする。環境ブロックは sub-03 の `BuildEnvironmentBlock` を使い、
     継承ハンドルは `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` で絞り、Job (kill-on-close) に入れる (作りは `CliProcess::Start` と同じ)。
     既存の `ChildProcess.{h,cpp}` の振る舞いは変えない。
5. **クライアントの起動**: `<exeDir>\Runtime.exe --project <proj> --net-connect <ip>:<port> --player-session-id <id>`
   (player session を作った同じ流れで直ちに。作業ディレクトリ = プロジェクト)。
6. **設定の保存**: `<project>\.mye\editor_settings.json` の `gameLift` (D17)。トークンは書かない。
7. **aws login を開く**: 新しいコンソール窓で `aws login` (対話はユーザーがそちら)。完了待ちはしない。
8. **UI 文字列**: `LocalizationTable.inl` に両言語。識別子の接頭辞は既存と衝突しない名前 (例 `GameLift_`)。
9. **(VERDICT sub-03 round 1 #1 からの持ち越し)** sub-03 の `AwsCliCommands` の入力検査を項目ごとにする (spec D14 の改訂)。
   コンピュート名とロケーション接尾辞は英数と `-` だけ (`_` `.` は拒否)、接尾辞は `custom-` を足して 64 文字以内。
   コンピュート名の既定は PC 名を英数と `-` へ整えたもの (`_` `.` → `-`、空なら `mye-dev-pc`)。
   `gameliftcli` スイートに `my_pc` / `a.b` / 長すぎる接尾辞を拒否する項目を足す。
10. 自己テスト (偽 CLI = プロセス内の差し替え。必要なら `cmd.exe`) と、一時プローブ + `--screenshot` の撮影 (プローブはコミットしない)。

## やらないこと (このサブでは)

- EC2 タブの中身 (sub-05)、起動時の課金チェック (sub-05)
- Network 窓 (`NetWindow.cpp`) の変更
- 実 AWS への呼び出しを自己テストで行うこと

## 触る場所 (planner の見立て)

- 新規 `src/Editor/Windows/Project/GameLiftWindow.{h,cpp}`
- 新規 `src/Editor/Tools/GameLift/` に状態機械 (例 `GameLiftAnywhereFlow.{h,cpp}`)、Server 子プロセスの管理、設定の読み書き
- `src/Editor/App/EditorApp.{h,cpp}` (窓の所有・メニュー・終了時の停止)
- `src/Editor/App/EditorSettings.{h,cpp}` (`gameLift` の往復。既存キーを変えない)
- `src/Engine/Core/Localization/LocalizationTable.inl`
- `src/Editor/SelfTest/SelfTestSuites.cpp`
- 参考: `NetWindow.cpp` の `themeColor` の使い方、`BuildSettingsWindow` の非同期段の作り
- ★Editor 層から `Engine/Engine/Online/HostServerProcess` を流用しない (Engine 側の型で、ロビー用。考え方だけ参考にする)

## 受け入れ条件 (このサブ)

spec 5. の 13〜19。

1. 偽 CLI で CLI 無し / v1 / ログイン切れ / リージョン無し / プロジェクト無し / GameLogic.dll 無しのとき、該当ボタンが無効で理由が返る (状態機械の問い合わせで検査)。[E]
2. 「準備」: 何も無い → location → fleet → compute の順に 3 本の作成。全部ある → 作成 0 本。`custom-` は 1 回。`custom-x` の入力は拒否。
   確認で「やめる」→ CLI 0 本。保存 → 読み込みで ID が戻り、`editor_settings.json` の中身に token の文字列が無い。[E]
3. 「サーバ起動」: Server.exe の argv が spec どおりでトークンを含まない。子の環境に `MYE_GAMELIFT_AUTH_TOKEN` が入る
   (Server の代わりに `cmd.exe` を差して確かめてよい)。ログの `ProcessReady sent` で待機中。「停止」で stop イベントが立つ。[E]
4. 「セッションを作って参加」: 容量不足を 2 回返す偽 CLI → 再試行 → ACTIVATING → ACTIVE → player session → クライアント起動の argv が spec どおり。[E]
5. 「片付け」: deregister → delete-fleet → (偽 CLI が 2 回「まだある」→ 消えた) → delete-location の順。[E]
6. スクショ: Anywhere タブ (準備済み・待機中の偽状態)、確認ダイアログ。日本語・英語。画像パスを SELF_EVAL に書く。[P]
7. `tools\check_rules.ps1` 0 error (翻訳の両言語・書式指定子・規則 22 を含む)。[R]

## 検証コマンド

- `tools\gen_project_files.ps1` → ビルド (Debug / Release)
- `MYE_SELFTEST_ONLY=<新スイート>` で `bin\x64\Debug\Editor.exe --selftest`
- `tools\check_rules.ps1`
- 一時プローブ + `bin\x64\Release\Editor.exe ... --screenshot ...` (窓を出す起動はリポジトリ外のプロジェクトで。imgui.ini を汚さない — メモリの M104 の注意)

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

実装:
- `Tools/GameLift/GameLiftFlow.{h,cpp}` — 手順の状態機械。状態確認 (--version → configure get region → sts) と Anywhere の 5 操作 (準備 / 起動 / 参加 / 停止 / 片付け)。`Query` (ボタンの有効 + 理由 `BlockReason`)・`Request → PendingConfirm → Confirm/Decline` (作成・削除は確認を経ないと始まらない)・`Tick` (CLI 完了・待ちの満了・サーバ状態を進める)・`Cancel`・`Shutdown`。純関数 `BuildAnywhereServerLaunch` / `BuildAnywhereClientLaunch` / `ProjectGameLogicPath` / `ServerLogPathFor`。
- `Tools/GameLift/GameLiftHost.{h,cpp}` — 外界への口。`IAwsCliRunner` (実装 `ProcessAwsCliRunner` = CliProcess + FindAwsExecutable)、`ILocalProcessHost` (実装 `LocalProcessHost` = ServerProcess + クライアントの CreateProcess)、`OpenAwsLoginConsole`。
- `Tools/GameLift/ServerProcess.{h,cpp}` — Server.exe 専用の子プロセス (stdout/stderr をファイルへ・名前付き stop イベント・Job kill-on-close・ログ行の読み取りと `ProcessReady sent` 検出・停止の猶予 → TerminateJob)。CliProcess は使っていない (sub-03 の決定どおり)。
- `Tools/GameLift/GameLiftSettings.{h,cpp}` — `GameLiftSettings` (editor_settings.json の `gameLift`)、`SanitizeGameLiftSettings`、`DefaultComputeName`。
- `App/EditorSettings.{h,cpp}` — `gameLift` の読み書き (キーごとに型を見て読む・Sanitize・既存キーは変えない)。
- `Windows/Project/GameLiftWindow.{h,cpp}` — 窓 (上段の状態・タブ・手順の行・サーバ欄・下段・確認ダイアログ)。`Pump` (閉じていても毎フレーム) と `OnImGui`。
- `App/EditorApp.{h,cpp}` — 窓の所有・Window メニュー (Network の隣)・レイアウト登録・`Bind` (OnStart)・`Pump`/`OnImGui`・OnShutdown で `Shutdown`。
- `Core/Localization/LocalizationTable.inl` — `GameLift_*` を両言語で追加 (約 100 行)。
- (9 項) `AwsCliCommands.{h,cpp}` — `IsValidComputeName` を英数と `-` だけに、`IsValidLocationSuffix` を英数と `-` だけ + "custom-" を足して 64 文字以内 (= 57 文字) に。`GameLiftCliSelfTest.cpp` に `my_pc` / `a.b` / 58 文字の拒否と 57 文字の許可を追加 (旧 50 文字・`_` `.` 許可の項目は新規則へ書き換え)。
- `Tools/GameLift/GameLiftFlowTestDoubles.h` (偽 CLI の「世界」・偽ホスト) と `GameLiftFlowSelfTest.{h,cpp}` (スイート `gameliftflow` 103 項目 + `gameliftserver` 25 項目)。`SelfTestSuites.cpp` の末尾へ 2 行。

技術解説:
- 技術: **操作 = 「1 本ずつ走る CLI の連鎖」の明示的な状態機械 (Stage 列挙 + 結果ハンドラ)**。前の CLI の結果で次の CLI が決まる手順 (list-locations の結果で create-location を出すか決める等) を、コルーチンやコールバックの入れ子にせず、`Stage` (= 「いま走っている CLI が何のためか」) の値で持つ。`Tick` が CLI の完了を拾うと `OnCallFinished` が (1) 取消・時間切れ (2) `ClassifyAwsFailure` (3) 操作ごとのハンドラ `HandlePrepare` など、の順に振り分け、ハンドラが次の `Issue(call, nextStage)` か `WaitFor(ms, kind, nextStage)` か `FinishOp`/`FailWith` を呼ぶ。1 窓で同時に走る CLI は 1 本 (`calling_` が真の間は何も始めない) なので、状態はメンバ変数だけで足りる。採らなかった案: 手順ごとに別スレッドで同期的に aws を呼ぶ案 (UI スレッドを止めない代わりに、取消・待ち・状態の観測が全部スレッド間の問題になり、「実行順序で結果が変わらない」を保ちにくい)、`std::function` の連鎖 (停止点が見えず、自己テストが書きにくい)。
- 技術: **差し替え口 (seam) を 2 つだけ設ける**。`IAwsCliRunner` (`Locate/Start/Poll/Cancel/ExitCode/StdoutText/StderrText/ClearOutput`) と `ILocalProcessHost` (`StartServer/PollServer/ServerStatus/RequestServerStop/StopServerBlocking/LaunchClient`)。仕様が「偽 CLI = プロセス内の差し替え」を求めたため。実装は 1 つずつの薄い包み (`ProcessAwsCliRunner` は sub-03 の CliProcess、`LocalProcessHost` は ServerProcess) で、状態機械は Windows API を直接触らない (GetTickCount64 などの時計は `Tick(nowMs)` の引数で受ける = 偽の時計を 5 ms ずつ進めて待ちを一瞬で回せる)。偽の側は `FakeAwsWorld`: AWS の状態 (ロケーション・フリート・コンピュート・ゲームセッション・player session) を持ち、argv に応じて公式応答例の形の JSON か型付きエラーを返す。「全部ある → 作成 0 本」「消えるまで 2 回まだある」「容量不足を 2 回」を、状態から自然に起こすので、冪等さを argv の列で直接検査できる。
- 技術: **確認ダイアログを状態機械側の 2 段 (Request → Confirm) にした**。作成・削除 (`Prepare` / `Cleanup`) は `Request` が `pending_` を立てるだけで CLI を出さず、`Confirm` でだけ `Begin` する。窓は `PendingConfirm()` を見てモーダルを描くだけ。UI を迂回して AWS を触る経路が型の上で存在せず、「やめる → CLI 0 本」が自己テストできる (`Decline` 後に `cli.calls.empty()`)。D18 (AI/自動操作が確認なしに作成・削除しない) の機械的な担保。
- 技術: **Server.exe の起動は CreateProcess + 継承ハンドルの限定 (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) + Job (KILL_ON_JOB_CLOSE)**。`ServerProcess::Start` は CliProcess::Start と同じ作り (環境ブロックは `BuildEnvironmentBlock`、CREATE_SUSPENDED → Job へ → ResumeThread)。違いは stdout/stderr が**同じログファイルのハンドル 1 本**で、長く動く子の出力をメモリへ溜めない点。ファイルは `FILE_SHARE_READ|WRITE` で作り、読み手のハンドルを別に開いておき、毎 Poll でオフセットを進めて行に切る (sub-01 で Server が行単位で書くようにしたので、`[gamelift] ProcessReady sent` の行が出た時点で見える)。停止は「名前付きイベントを起動前に作る → `--stop-event` で渡す → `SetEvent` → 猶予 → `TerminateJobObject`」。D9 のとおり Ctrl+Break の細工 (AttachConsole) は使わない。
- コード: `GameLiftFlow::Tick` (毎フレーム、窓の `Pump` から。状態はメンバに置く)
  ```cpp
  void GameLiftFlow::Tick(uint64_t nowMs)
  {
      now_ = nowMs;
      host_.PollServer();
      UpdateServerView();                       // サーバが終わったら clientsLaunched_ を 0 へ
      if (calling_) {
          const CliRunState state = runner_.Poll();
          if (state != CliRunState::Running) { calling_ = false; OnCallFinished(state); }
          return;                               // CLI 実行中は他に何も進めない (同時 1 本)
      }
      if (op_ == FlowOp::None || waitKind_ == WaitKind::None) return;
      if (waitKind_ == WaitKind::ServerStop) {  // 片付けの先頭: サーバが止まるまで CLI を出さない
          if (!server_.Active()) { waitKind_ = WaitKind::None; CleanupAfterServer(); }
          return;
      }
      if (now_ >= waitUntil_) { waitKind_ = WaitKind::None; OnTimer(); }   // 再試行・ポーリングの満了
  }
  ```
- コード: `GameLiftFlow::Request` / `Confirm` — ボタンの入口。`Query` が無効なら何もしない (理由は `BlockReason`)。作成・削除は確認待ちにするだけ。
  ```cpp
  bool GameLiftFlow::Request(FlowAction action)
  {
      if (!Query(action).enabled) return false;
      if (action == FlowAction::Prepare || action == FlowAction::Cleanup) { pending_ = action; return true; }
      return Begin(action);
  }
  bool GameLiftFlow::Confirm()
  {
      if (!pending_.has_value()) return false;
      const FlowAction action = *pending_; pending_.reset();
      if (!Query(action).enabled) return false;   // 確認している間に状態が変わった
      return Begin(action);
  }
  ```
- コード: `GameLiftFlow::Query` — 無効の理由の優先順は「実行中 → 未確認 → CLI → ログイン → リージョン・入力 → プロジェクト → GameLogic.dll → Server.exe/Runtime.exe → 準備済みか → サーバの状態」。停止だけは CLI を使わないので別扱い (サーバが Starting/Ready なら CLI 実行中でも押せる。片付けの最中は不可)。ファイルの有無 (`GameLogic.dll` = `<project>\cache\GameLogic.dll`、BuildSettings/EngineLoop と同じ出所) は問い合わせのたびに見る (数個の GetFileAttributes。ビルド直後に再確認なしで有効になる)。
- コード: `HandlePrepare` — 冪等さの要。list-locations に `custom-<x>` があれば create-location を飛ばし、保存済みフリート ID は describe-fleet-attributes で確かめる (空配列・NotFound・DELETING は「無い」= 保存 ID を捨てて作り直す)。コンピュートは describe-compute が NotFound のときだけ register。既にある (`ConflictException`) は成功扱い。
  ```cpp
  case Stage::PrepListLocations: {
      if (r.aws.kind != AwsErrorKind::None) return FailWith(FlowFailure::Aws, FirstErrorLines(r.err, 3), &r);
      std::vector<AwsLocation> locations;
      if (!ParseListLocations(r.out, locations, parseError)) return FailWith(FlowFailure::BadResponse, parseError);
      const std::string wanted = LocationName(settings_.locationSuffix);   // "custom-" はここで 1 回だけ付く
      const bool exists = std::any_of(locations.begin(), locations.end(), [&](const AwsLocation& l) { return l.name == wanted; });
      if (exists) PrepareNextFleet(); else Issue(MakeCall([&](AwsCall& c) { return GameLiftCreateLocationCall(settings_.region, settings_.locationSuffix, c); }), Stage::PrepCreateLocation);
      return;
  }
  ```
- コード: `LaunchServerWithToken` — トークンの扱い。応答 (`CallResult.out`) → `ParseComputeAuthToken` → `BuildAnywhereServerLaunch` が**環境変数 1 つ** (`MYE_GAMELIFT_AUTH_TOKEN`) だけへ入れる → `host_.StartServer(spec)` → `spec.WipeSecrets()` / `WipeString(input.authToken)` / `token.Wipe()` → `OnCallFinished` の末尾でも `result.out` をゼロ埋め + `runner_.ClearOutput()`。秘密の CLI は `Progress().cliLabel` も空 (「実行中: get-compute-auth-token」を出さない)。argv は `--project <proj> --hosting gamelift --gamelift-ws-url <ep> --gamelift-fleet-id <id> --gamelift-host-id <compute> --port <p> --max-players <n> --stop-event <名前>` の 16 要素で、自己テストが完全一致で検査している。
- コード: `HandleJoin` の容量不足の再試行 — `FleetCapacityExceededException` だけを間隔 (3 秒) を置いて最大 20 回再試行する。待ちは `WaitFor(ms, WaitKind::CapacityRetry, Stage::JoinCreateRetryWait)` で、`Tick` が `waitUntil_` 満了で `OnTimer` → `IssueCreateGameSession` を呼ぶ (スレッドも sleep も無い)。ACTIVATING の待ち (2 秒おき・60 秒) と、片付けの「フリートが消えるまで」(15 秒おき・15 分) も同じ仕組み。進捗 (`attempt_/maxAttempts_`) は `Progress()` で窓へ出る。
  ```cpp
  case Stage::JoinCreateSession: {
      if (r.aws.kind == AwsErrorKind::CapacityExceeded) {
          if (attempt_ >= timings_.capacityRetryMax)
              return FailWith(FlowFailure::CapacityRetriesExhausted, FirstErrorLines(r.err, 3), &r);
          ++attempt_;
          WaitFor(timings_.capacityRetryIntervalMs, WaitKind::CapacityRetry, Stage::JoinCreateRetryWait);
          return;
      }
      ...
  ```
- コード: `ServerProcess::Start` / `Poll` / `AppendLogText`。起動の順序 = (1) stop イベントを作る (既に同名があれば失敗) (2) ログの書き込み端 (継承可) と読み取り端 (継承不可) を開く (3) NUL (4) 環境ブロック・コマンドライン (秘密を含みうるので起動後に SecureZeroMemory) (5) `HANDLE_LIST` に NUL とログの 2 本だけ (6) CREATE_SUSPENDED → Job → Resume。`Poll` は最大 16 KiB x 16 回/フレームでログを読み、`AppendLogText` が `\n` で行に切って末尾 8 行を `deque` に保ち、行に `ProcessReady sent` があれば `ready_`。プロセス終了を見た Poll で読み残しを取り切り、改行なしの最後の行も確定する。`RequestStop(graceMs)` は `SetEvent` して時刻を記録し、`Poll` が猶予切れで `TerminateJobObject` (`forcedKill_`)。
  ```cpp
  // 継がせるハンドルを NUL とログの 2 本に限る (別の子へ漏らさない)
  HANDLE inheritList[2] = { nulIn.handle, logWrite.handle };
  ...
  startup.StartupInfo.hStdOutput = logWrite.handle;
  startup.StartupInfo.hStdError = logWrite.handle;     // stdout/stderr は同じファイル
  ```
- コード: `GameLiftWindow` — `Pump` (閉じていても毎フレーム `Tick(GetTickCount64())`。サーバの監視を止めない) と `OnImGui` (開いているときだけ描く。初めて開いたときに 1 回 `RefreshStatus`)。手順の行は `DrawStepRow`: 「[状態] ボタン 理由」。状態の色は `themeColor` (Accent=実行中 / Success=済 / Error=失敗 / TextDim=未)。無効の理由は灰色で、`Busy` だけは出さない (実行中に全行へ出ると騒がしい)。本体は `BeginChild("###GLBody", (0, -bottomHeight))` でスクロール、下段は実行中・取消の通知・エラーがあるときだけ固定の高さで出す。確認ダイアログは `OpenPopup("###GLConfirmPrepare"/"###GLConfirmCleanup")` + `BeginPopupModal`、ボタンは [作成/削除] [やめる] で `SetItemDefaultFocus()` を「やめる」へ。
- コード: `EditorSettings` の `gameLift` — 読み込みは `readString/readInt` でキーごとに型を見る (型違いで `value()` が例外を投げて全体を捨てないため)。読んだ後に `SanitizeGameLiftSettings` で不正値 (許可文字・範囲外) を既定へ戻す。保存は既存のマージ保存に `root["gameLift"]` を足すだけ。構造体にトークンの欄が無いので、書きようがない。
- 前提・落とし穴:
  - ★窓が使う CLI は 1 本だけ (`IAwsCliRunner` 1 インスタンス)。`RefreshStatus` を含む全操作が `Busy()` を共有する。sub-05 の EC2 操作も同じ `GameLiftFlow` か、同じ runner を `Busy` で排他して使うこと。
  - ★`server_` (Server.exe の状態の写し) は `Tick` の先頭で更新するが、操作が `StartServer` / `StopServer` を呼んだ直後にも `UpdateServerView()` を呼んでいる。呼ばないと「起動」が 1 フレーム押せる状態に見えて二重に押せる (自己テストで実際に踏んだ)。
  - ★トークンの有効期限は約 15 分。「起動」のたびに取り直す (M81 と同じ)。自動更新はしない。
  - ★クライアントの起動は `Runtime.exe --project <proj> ...` で作業ディレクトリ = プロジェクト。player session は作成から 60 秒以内に Accept されないと TIMEDOUT になるため、`create-player-session` の完了ハンドラの中で直ちに起動する (別フレームへ回さない)。
  - ★Server.exe の作業ディレクトリは**プロジェクト** (仕様は未指定。`cache\hot_server` / `save_server` がプロジェクトの cache 下へ出る。`HostServerProcess` は exe のフォルダ)。
  - ★停止の猶予は `serverStopGraceMs` (既定 10 秒)。エディタ終了 (`Shutdown`) は `StopBlocking` で最大その間 UI スレッドが待つ (通常は 1 秒未満で Server が終わる)。
  - ★GameLogic.dll の場所は `<project>\cache\GameLogic.dll` を固定で見る (プロジェクト起動時の EngineLoop/BuildSettings と同じ)。レガシー起動 (`--project` なし) では「プロジェクト未オープン」で無効。
  - ★コンピュート名の既定 (`DefaultComputeName`) は PC 名を英数と `-` へ整えるので、設定の `computeName` が空のとき register-compute / describe-compute / Server の `--gamelift-host-id` は**同じ整えた名前**を使う (`EffectiveComputeName()` 1 か所)。
  - 規則 13-c: `aws[\\/]gamelift` 等の文字列を書かない (フォルダは `Tools/GameLift`、文字列は "gamelift" 単独)。

仕様との差分:
- [追加] 窓の差し替え口 `GameLiftWindow::UseBackends(runner, host)` と、テスト用の偽物ヘッダ `GameLiftFlowTestDoubles.h` を本番ツリーへ置いた — 自己テストとスクショ検証 (偽の状態で窓を出す) の両方が使うため。窓の既定の実装は実プロセス。
- [追加] `IAwsCliRunner` / `ILocalProcessHost` の 2 つの抽象 — spec 5 の 13〜17 が「偽 CLI (プロセス内の差し替え)」を要求し、実 AWS 不可・時計を進めて待ちを回す必要があるため。実装は各 1 つだけで、Factory / Manager は作っていない。
- [追加] `ConfirmInfo` / `PendingConfirm` (確認を状態機械側の 2 段にした) — D18 を型で担保するため。
- [追加] `AnywhereServerLaunchInput` などの純関数と `FlowTimings` (待ちの間隔・上限。既定は spec 4.1.5 の値、自己テストは短縮) — 検査と時間短縮のため。
- [追加] Server.exe の作業ディレクトリ = プロジェクト (上記) — 仕様に無い。可逆 (`BuildAnywhereServerLaunch` の 1 行)。
- [追加] プレイヤー ID は `mye-<UTC 日時>-<連番>` (spec は player-id の値を規定していない。同名の player session を同じゲームセッションへ作ると AWS が拒む場合があるため、起動ごと・押すごとに変える)。
- [追加] 窓を開いた最初の 1 回だけ `RefreshStatus` を自動で走らせる (spec は「再確認」ボタンだけ。開いただけで状態が分からないと全ボタンが「先に再確認」になるため)。エディタ起動時には aws に触らない。
- [追加] 下段 (実行中・エラー) は内容があるときだけ固定の高さで出す、本体はスクロール。入力欄は 2 列 x 3 行。
- [逸脱なし・確認] 既定サイズは 640x560 のまま。この高さだと本体はスクロールになり、サーバのログ欄は下へスクロールしないと見えない (`ready-tall` の撮影は 640x760)。
- [追加] 9 項: 接尾辞の上限を 50 → **57 文字** (spec D14 の「custom- を足して 64 文字以内」に合わせた。sub-03 の 50 は安全側の暫定値だった)。
- [未実装] (本サブの範囲外のため): EC2 タブの中身 (「実装されていません」の 1 行だけ)、起動時の課金チェック。
- [未検証] `OpenAwsLoginConsole` の実起動 (新しいコンソール窓は自動テストで開けない)。コマンドラインの形 (`cmd.exe /d /s /c ""<aws>" login & pause"`) は、空白を含む偽 aws.bat で同じ形を走らせて引数 `login` が届くことだけ手で確かめた。

検証:
- `pwsh tools\gen_project_files.ps1` → Editor.vcxproj / filters 更新 (新規 13 ファイル)。
- `MSBuild build\Editor.vcxproj /p:Configuration=Debug|Release` → どちらもビルド成功 (警告の追加なし)。
- `MYE_SELFTEST_ONLY=gameliftcli,gameliftflow,gameliftserver` で `bin\x64\Debug\Editor.exe --selftest` → 3 スイート全 PASS (gameliftcli 98 項目 / gameliftflow 103 項目 / gameliftserver 25 項目)。Release の Editor.exe でも同じ 3 + serverpackage が PASS。
- `bin\x64\Debug\Editor.exe --selftest` (fast 層全体) → 123 スイート中 1 FAIL = 既知の別件 `modalsynth` (`transient burst increases initial impact energy`)。それ以外は全 PASS (servernet / session 系を含む)。
- `tools\check_rules.ps1` → 0 error (65 warning は既存。GameLift 関連の警告は 0)。規則 10 (両言語・`###`・書式指定子) / 規則 22 / 規則 13-c を含む。途中で「確認ダイアログの 2 つの題が同じ `###` ID」の規則 10 違反を出して直した (`###GLConfirmPrepare` / `###GLConfirmCleanup` に分けた)。
- スクショ (Release の Editor.exe + 一時プローブ。**プローブは EditorApp.cpp に入れて撮影後に削除済み = コミットに入らない**。リポジトリ外の cwd で起動、imgui.ini は汚していない)。偽 CLI の世界で RefreshStatus → 準備 → 起動 → ProcessReady → 参加 1 人まで進めた状態:
  - `plans\m107-gamelift-editor\shots\sub-04_ready_en.png` / `sub-04_ready_ja.png` (Anywhere タブ・準備済み・待機中、既定サイズ)、`sub-04_ready-tall_en.png` / `_ja.png` (640x760。ログの末尾 2 行が見える)
  - `plans\m107-gamelift-editor\shots\sub-04_confirm-prepare_en.png` / `_ja.png`、`sub-04_confirm-cleanup_en.png` / `_ja.png` (確認ダイアログ)
  - 追加で撮った (scratchpad のみ): `C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\598bfb6c-b927-4ea0-8bfe-f2e5f113f975\scratchpad\shots\gamelift_error_{en,ja}.png` (資格情報切れ + 直近のエラー)、`gamelift_running_{en,ja}.png` (CLI 実行中 + 取消)
  - 目視した点: 状態の色 (済=緑・失敗=赤・実行中=青)・無効ボタンの横の理由 (灰色 1 行)・確認ダイアログの資源名とリージョン・日本語と英語で崩れない。日本語のフォントがラテン文字と混ざって大きさが揃わないのは既存の窓と同じ (この窓固有ではない)。
- 受け入れ条件との対応: 1 (自己テスト: CLI 無し / v1 / ログイン切れ / リージョン無し / プロジェクト無し / GameLogic.dll 無し / Server.exe 無し / Runtime.exe 無し)、2 (準備: 3 本の作成・全部ある → 0 本・`custom-` 1 回・`custom-x` と `my_pc` の拒否・やめる → 0 本・往復・token 無し)、3 (argv 16 要素の完全一致・トークンは環境のみ・ProcessReady で待機中・停止で stop 要求が 1 回。実プロセス側は `gameliftserver` で stop イベントが実際に立つこと・猶予内に自分で終わる停止 (powershell が exit 7)・強制終了)、4 (容量不足 2 回 → ACTIVATING 2 回 → ACTIVE → player session → クライアント argv)、5 (deregister → delete-fleet → 「まだある」2 回 → delete-location)、6 (スクショ)、7 (check_rules 0 error)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜7 の各項目を自己テストとスクショで確認した。差分 (UseBackends・既定サイズのままスクロール・50→57 文字・初回の自動確認・player-id・作業ディレクトリ) は上に列挙。EC2 タブは範囲外。減点は「サーバ・クライアントの実起動 (Server.exe / Runtime.exe の本物) をこのサブでは動かしていない」点 (実 AWS は不可なので)。
  正しさ: 4 — 偽 CLI の世界 (状態を持つ) で冪等さ・順序・再試行・上限・取消・資格情報切れ・秘密の消去を検査し、実プロセス (cmd / powershell) で環境・ログ・イベント・猶予・強制終了を検査した。一方、本物の aws.exe と本物の Server.exe を通した確認は未実施 (手動確認 28 の範囲)。`aws login` の実起動も未検証。
  コード品質: 3 — `GameLiftFlow.cpp` が約 1100 行で大きい (操作ごとのハンドラは分かれているが 1 ファイル)。ビルダーを表駆動にできる余地 (planner の nit) には手を付けていない。ファイルを操作ごとに割る / EC2 を足すときに整理するのが妥当。
  テスト: 4 — 128 項目 (flow 103 + server 25)。argv は完全一致で見ている。未検証は窓の描画 (スクショの目視のみで自動検査なし)・`OpenAwsLoginConsole`・実 aws / 実 Server.exe。

不安・質問:
- (planner へ) `GameLiftFlow.cpp` を sub-05 の前に操作ごとのファイルへ割るか。EC2 の操作 (upload-build / create-fleet / 課金中帯) を足すと更に大きくなる。私の提案: sub-05 の先頭で `GameLiftFlow` を「共通 (確認・待ち・Issue) + `AnywhereOps` + `Ec2Ops`」に割る。
- (planner へ) 窓の既定サイズ 640x560 は、入力欄 + 手順 5 行 + サーバ欄 + 下段を全部は収められない (本体がスクロール)。サイズを 640x680 程度へ上げるか、このままか。受け入れには影響しない (推奨: 上げる。ただし spec 4.3 の数字なので判断を仰ぐ)。
- (planner へ) Server.exe の作業ディレクトリをプロジェクトにした (差分欄)。`HostServerProcess` は exe のフォルダ。ロビー側と揃えたいなら 1 行で変えられる。
- (R8 の関連) このサブでも Release の Editor.exe を 4 回再ビルドしたが Defender の再検疫は起きなかった。ただし `ServerProcess` は exe を起動する経路を増やしたので、手動確認 28 で再発しないか見てほしい。

触ったファイル:
- src/Editor/Tools/GameLift/GameLiftFlow.h / .cpp (新規)
- src/Editor/Tools/GameLift/GameLiftHost.h / .cpp (新規)
- src/Editor/Tools/GameLift/ServerProcess.h / .cpp (新規)
- src/Editor/Tools/GameLift/GameLiftSettings.h / .cpp (新規)
- src/Editor/Tools/GameLift/GameLiftFlowTestDoubles.h (新規)
- src/Editor/Tools/GameLift/GameLiftFlowSelfTest.h / .cpp (新規)
- src/Editor/Tools/GameLift/AwsCliCommands.h / .cpp (入力検査の項目別化)
- src/Editor/Tools/GameLift/GameLiftCliSelfTest.cpp (検査項目の更新)
- src/Editor/Windows/Project/GameLiftWindow.h / .cpp (新規)
- src/Editor/App/EditorApp.h / .cpp
- src/Editor/App/EditorSettings.h / .cpp
- src/Editor/SelfTest/SelfTestSuites.cpp
- src/Engine/Core/Localization/LocalizationTable.inl
- build/Editor.vcxproj / build/Editor.vcxproj.filters (gen_project_files の出力。司会の方針で生成物扱いなら除外)
- plans/m107-gamelift-editor/shots/sub-04_*.png (スクショ 8 枚)
- plans/m107-gamelift-editor/sub-04.md (本メモ)

申し送り:
- sub-05 へ: EC2 は `GameLiftFlow` の `Issue / WaitFor / FailWith / Confirm` をそのまま使える (Stage に EC2 の段を足し、`FlowAction` に Upload / CreateFleet / Capacity / DeleteFleet / DeleteBuild を足す)。upload-build は `CliRunSpec.timeoutMs = 0` が要る (`FlowTimings.cliTimeoutMs` は 60 秒の既定なので、操作ごとの上書きが要る) — `Issue` の第 3 引数で渡せるようにするのが素直。進捗 % は `runner_.StdoutText()` を `ParseUploadBuildOutput` するだけ (`IAwsCliRunner` は実行中でも StdoutText が呼べる)。
- sub-05 へ: 課金中帯の起動時チェックは `GameLiftWindow::Bind` の後・`RefreshStatus` が完了してから `describe-fleet-capacity` を 1 本、がやりやすい (CLI が 1 本ずつなので `RefreshStatus` と同じ Busy を使う)。
- sub-05 / 06 へ: `GameLiftSettings` は Anywhere の項目だけ。EC2 の項目 (ビルド ID・フリート ID・インスタンスタイプ・ビルド名・書き出しフォルダ等) は `GameLiftSettings` / `EditorSettings.cpp` の `gameLift` へ足す (読み込みは `readString/readInt`、保存はオブジェクトへ追記、`SanitizeGameLiftSettings` へ検査を足す)。
- sub-06 へ: `docs/gamelift-editor.md` に「窓の閉じ方: 窓を閉じても Server.exe は残り、エディタ終了時に停止手順」「秘密の扱い (環境変数のみ・即消去)」「ログは `<project>\cache\gamelift\server-<日時>.log`」「プレイヤー ID は `mye-<UTC>-<n>`」「コンピュート名・接尾辞は英数と `-`」を書く。
- 既知の別件 (触っていない): `modalsynth` の 1 項目 FAIL。

### round 2 (SELF_EVAL の写し)

- #1 原因: 撮影の段取りの差 + 実在した弱点。(a) 前回のプローブは `Bind` の後で `settings.region` を入れており、`region_` バッファは Bind 時 (空) のまま。confirm-prepare では設定を書き換える手順 (`ChangedSettings`) が 1 度も走らず、`SyncBuffersFromSettings` が呼ばれないので欄が空のまま。ready は準備で fleet ID が保存され handler が走ったので埋まっていた。(b) ただし「外から設定が変わっても欄が追従しない (編集中の欄は ImGui の内部状態を優先する)」弱点は実在するので直した: `SyncTextBuffer(buf,size,value)` (違えば写して true) を毎フレーム各欄の描画前に `SyncField` で呼び、写したら `ImGui::ClearActiveID()` で編集状態を捨てる (imgui_internal.h)。InputText は編集されたフレームだけ true を返し設定を書くので、設定が空で上書きされる経路は元々無い (古いバッファのまま次の編集で外の値を潰す経路を塞いだ)。自己テスト: `SyncTextBuffer` の 4 項目 (空の欄にリージョンが入る / 同値なら何もしない / 長い値は切る)。ImGui の active 状態そのものは自動検査できない (未検証)。再現: 前回と同じ段取り (Bind 後に region) をプローブの `MYE_GAMELIFT_PROBE_LATE` で再現 → 直し後は欄が埋まる (`sub-04_confirm-prepare-late_ja.png`)。
- #2 既定サイズを 640x720 へ (`kWindowHeight`)。
- #3 触っていない。
- 撮り直し (Release + 一時プローブ。プローブは削除済み、imgui.ini 無汚染): plans\m107-gamelift-editor\shots\sub-04_ready_{en,ja}.png (720 でサーバログ末尾まで見える)、sub-04_confirm-prepare_{en,ja}.png、sub-04_confirm-cleanup_{en,ja}.png、sub-04_confirm-prepare-late_ja.png。旧 sub-04_ready-tall_* は削除。
- 検証: Debug/Release ビルド成功、gameliftcli/gameliftflow/gameliftserver 全 PASS (Debug・Release)、check_rules 0 error。

## フィードバック履歴

- round 1: VERDICT REWORK (planner, 2026-11-27)。must 1 件: `shots/sub-04_confirm-prepare_ja.png` で、確認モーダルは「リージョン ap-northeast-1」と出しているのに、後ろの窓のリージョン欄が空 (`sub-04_ready_ja.png` では入っている)。原因を特定し、実際の不具合なら直して自己テストを足す。撮影の段取りの差なら、その根拠を示して同じ状態で撮り直す。should 1 件: 既定サイズを 640x720 にする (spec 4.3 を改訂)。planner の再実行では 5 スイート (serverpackage / gameliftcli / gameliftcliprocess / gameliftflow / gameliftserver) が exit 0、check_rules は 0 error、トークンを Log に出す箇所は grep で 0 件。
- round 2: VERDICT OK (planner, 2026-11-27)。原因の切り分け (撮影の段取りの差 + 編集中の欄が外からの変更に追従しない弱点) は、`late` の撮り直しで裏づけられた。planner が見た `sub-04_confirm-prepare-late_ja.png` と `sub-04_ready_en.png` は、リージョン欄が埋まり 640x720 で収まっている。3 スイートの再実行は exit 0 (Debug の exe はソースより新しい)。入力欄が設定へ書くのは直接代入だけで、実行中に Sanitize が走るのは読み込み時だけなので、打鍵中に欄から追い出されることはない。nit: `ImGui::ClearActiveID()` は、どの欄が編集中でも active を外す (別の欄を編集中に外から設定が変わるとその欄からも抜ける。頻度は低い)。
