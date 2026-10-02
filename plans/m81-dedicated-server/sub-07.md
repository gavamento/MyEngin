# sub-07: GameLiftHosting と SDK 組込・Anywhere 手順書

- 依存: sub-05
- 状態: OK (コミット待ち — .lib のコミット可否をユーザーに確認してから)
- 往復: 1

## やること
spec 4.1.9 / R-2 / G1〜G4。

1. **最初に CRT の未知を潰す** (spec R-2): SDK 5.x (最新 5.6.0、`https://github.com/amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk`) を **/MT・/MTd** で静的ビルドできるかを確かめる。既定は /MD で、MyEngine は /MT (`build\Common.props:53,61`)。手当ての候補: `gamelift-server-sdk\` を直接 configure して `CMAKE_MSVC_RUNTIME_LIBRARY` を渡す / CMake ファイルへの最小パッチ (パッチは external\ に .patch として残す)。`-DBUILD_SHARED_LIBS=0 -DGAMELIFT_USE_STD=1 -DRUN_UNIT_TESTS=0`。CMake 4.x なら `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` が要る可能性。
   - 通らなければ**ここで止めて** SELF_EVAL で報告 (代替案: Server.exe だけ /MD にする — Engine.lib との CRT 混在になるので planner 裁定が要る)。
2. `external\gamelift-server-sdk\` に include / Debug・Release の .lib / LICENSE / NOTICE / ビルド手順 (`BUILD.md`: 取得したコミット、CMake の引数、パッチ)。`external\openssl\` に OpenSSL 3 の import lib と DLL (`libssl-3-x64.dll` / `libcrypto-3-x64.dll`) と LICENSE。`external\VERSIONS.md` に 2 行追加。Server.vcxproj のビルド後処理で DLL を出力先へコピー。
3. `src\Server\Hosting\GameLiftHosting.*`:
   - `InitSDK(ServerParameters{webSocketUrl, authToken, fleetId, hostId, processId})` → `ProcessReady(ProcessParameters{onStartGameSession, onProcessTerminate, onHealthCheck, port, logParameters})`。
   - コールバックは**ミューテックス付きキューへ積むだけ** (SDK の内部スレッドから来る)。`onHealthCheck` はメインループが最後に健康だった時刻 (atomic) を見て即答する (メインスレッドを待たない、応答期限 50 秒)。
   - Poll (tick 境界) で: StartSession → SessionConfig を確定 (GameSession の最大人数を `--max-players` と突き合わせ、小さい方) → `ActivateGameSession`、Terminate → `ProcessEnding` → `Destroy` → 終了。
   - `ValidatePlayer` = `AcceptPlayerSession(playerSessionId)` の成否。切断確定 (Release) で `RemovePlayerSession`。
   - 接続情報: `--gamelift-ws-url`、`--gamelift-fleet-id`、`--gamelift-host-id`、`--gamelift-process-id` (省略時 PID)、認証トークンは**環境変数 `MYE_GAMELIFT_AUTH_TOKEN`** (コマンドラインに出さない)。
   - SDK 呼び出し部を関数テーブル等で差し替え可能にし、G3 の selftest で偽の SDK を使う。
   - 5.6 の `InitCustomLogger` があれば SDK のログをエンジンのログへ流す (任意のスレッドから来るので、Log がスレッド安全かを確認してから)。
4. `docs\gamelift-anywhere.md`: IAM (開発者は `gamelift:*`、セッション作成側の最小権限)、`create-location custom-*`、`create-fleet --compute-type ANYWHERE`、`register-compute` (応答の `GameLiftServiceSdkEndpoint` を ws-url に)、`get-compute-auth-token` (約 15 分で失効)、Server.exe の起動例、`create-game-session` / `create-player-session`、Runtime の `--net-connect` と `--player-session-id`、ファイアウォール、片付け (deregister-compute / delete-fleet / delete-location)。

5. (sub-05 VERDICT から、GameLift 運用の前提) **サーバの記録を異常終了で失わない**。現状、サーバの .rep はメモリに溜めて正常終了 / Terminate / timeout のときにだけ書く。1 時間で約 140MB 溜まり、異常終了すると何も残らない。
   - `ReplayRecorder` に逐次書出しモードを足す: 開始時にヘッダ (tickCount = 0) と埋め込みスナップショットを書き、tick レコードは追記して一定間隔 (既定 60 tick) で flush する。Finish でヘッダの tickCount を書き戻す。tickCount が 0 のまま残った .rep (= 異常終了) は、Load 側でファイル長から完了済みの tick 数を求めて読めるようにする。途中で切れたレコードは捨てる。
   - Server.exe にクラッシュハンドラを付ける (Editor / Runtime と同じ CrashHandler)。落ちた .rep が上の規則で読めることを selftest で確かめる (書きかけのファイルを切り詰めて Load)。
   - Editor / Runtime の既存の記録 (replay_verify) は今までどおり (逐次モードは Server が使う)。逐次モードと一括モードで .rep がバイト一致することも selftest で確かめる。
6. (sub-05 から) LocalHosting の Ctrl+C → Terminate の経路と、GameLiftHosting の OnProcessTerminate → ProcessEnding の経路を、同じ「Terminate を受けたら .rep を閉じて終了」の 1 関数に通し、G3 の偽 SDK テストで Terminate 時に .rep が閉じられることを確かめる。

## やらないこと (このサブでは)
- 実 AWS との疎通 (sub-08、ユーザー作業)
- トークンの自動更新、EC2 フリート、FlexMatch

## 触る場所 (planner の見立て)
- `build\Server.vcxproj` (include / lib / 定義 `GAMELIFT_USE_STD=1` / ビルド後コピー)
- 新規 `external\gamelift-server-sdk\**`、`external\openssl\**`、`external\VERSIONS.md`
- 新規 `src\Server\Hosting\GameLiftHosting.*`、`src\Server\ServerMain.cpp` (`--hosting gamelift`)
- 新規 `docs\gamelift-anywhere.md`

## 受け入れ条件 (このサブ)
spec 5. の **G1, G2, G3, G4**、**R4** と **C1〜C6**。
- G1 には「/MT・/MTd で Engine.lib とリンクでき、LNK2038 が出ない」を含む。

## 検証コマンド
- MSBuild で Debug / Release の Server.exe をビルド (ログの警告ゼロを確認、`/p:MyeWarnAsError=true`)
- G2: `bin\x64\Release\Server.exe --hosting gamelift` (環境変数なし) の出力と所要時間
- G3: selftest (Server.exe 側の selftest 入口を作るなら `Server.exe --selftest`)
- `tools\server_verify.bat` (LocalHosting を壊していない)、`tools\replay_verify.bat`、`pwsh -File tools\check_rules.ps1` (規則 13-c)

## 実装メモ (coder が追記)

### round 1 (coder)

- 最初の未知 (CRT) は解決: SDK v5.6.0 は `gamelift-server-sdk\` を直接 configure (`CMAKE_POLICY_DEFAULT_CMP0091=NEW` + `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`) で /MT・/MTd の静的 .lib にでき、Server.exe が Engine.lib と LNK2038 なしでリンクできた。
- 落とし穴: 既定のままだと .lib が Release 281MB / Debug 347MB (git に入らない)。`/Zi` 除去パッチ + unity ビルド (1 翻訳単位) で Release 53MB / Debug 72MB (gzip で計 8.5MB)。手順は `external\gamelift-server-sdk\BUILD.md` + `build_sdk.ps1` (別ディレクトリへ再ビルドして再現確認済み: サイズは 4 バイト差)。
- 取得元: SDK = https://github.com/amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk タグ v5.6.0 (`7c2a5a7cae6616b1ca2f217aaceff36b06393c80`)。SDK の CMake が取る依存 = asio-1-20-0 / websocketpp 0.8.2 / rapidjson v1.1.0 / spdlog v1.14.0 / concurrentqueue v1.0.4 (ヘッダオンリー、.lib に取り込み済み)。OpenSSL = vcpkg (microsoft/vcpkg `fbb0f7bb200b07a9eb9081c7a3cf51d1aa1c51a1`) の openssl 3.6.5 x64-windows。AWS CLI のオプション名は docs.aws.amazon.com/cli/latest/reference/gamelift/ で照合。

詳細な SELF_EVAL は司会への返信を正本とする (下の要点)。

- 実装: Replay.{h,cpp} の逐次書出しモード (Start の streamFlushTicks) と Load の tickCount = 0 救済 / GameLiftSdk.h (IGameLiftSdk 差し替え口) / GameLiftHosting.{h,cpp} / GameLiftSdkAws.cpp / ServerSelfTest.{h,cpp} (`Server.exe --selftest`) / ServerLoop の InterpretHostingEvents + CloseSession (Terminate の 1 本の経路) / ServerMain (--hosting gamelift、--gamelift-*、クラッシュハンドラ、--crash-test) / ServerSession の playerReleased フック / Server.vcxproj / docs\gamelift-anywhere.md / external の SDK と OpenSSL。
- 検証: Server.exe --selftest 49 項目 PASS (Debug / Release、実ループ + 偽 SDK の Terminate、実プロセスへの Ctrl+Break を含む) / replay_verify PASS / server_verify ABCD PASS / net_verify PASS / check_rules 0 error / Editor --selftest は Debug・Release とも既知 2 項目以外 PASS (初回の Debug だけ Fracture editor の weight cache 3 項目が落ち、再実行で再現せず) / G2 0.86 秒 exit 1。実 AWS には未接続。

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。最初の未知 (/MT) を解決し、.lib を 281/347MB から 53/72MB へ縮めた。確認した受け入れ条件は G1〜G4 と R4 (偽 SDK の 49 項目、実ループ、Ctrl+Break、実クラッシュから 198 tick 回復して全一致)、C1〜C7。追加分 (playerReleased、ゲームプロパティ、InitSDK の 30 秒タイムアウト、待機ループの Terminate 取りこぼしの修正) はすべて承認。ビルド済み .lib (125MB) のコミットは spec D17 で裁定済みだが、リポジトリ履歴に恒久的に残るのでユーザーに確認してからコミットする。nit: クラッシュバンドルの crash.txt の文面が crash.rep 前提になっている (Server では逐次 .rep が代わり)。直すのは sub-09 でよい。
