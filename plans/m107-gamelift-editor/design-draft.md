# M107 事前調査メモ: エディタから AWS GameLift を操作する

司会 (Claude) が harness 起動前にユーザーと確認した内容と、読んだコードの事実。planner はこれを出発点にし、
事実は実コードで裏取りしてから spec.md へ移すこと (このメモは仕様ではない)。

## 1. 依頼とユーザー決定 (2026-11-27)

依頼原文: 「AWSとつなげてオンラインできる機能をもっと簡単に使えるように死体」 (「したい」の誤変換)

| 論点 | ユーザーの答え | 却下した選択肢 |
|---|---|---|
| 誰のどの場面を簡単にするか | **エディタの窓から操作** (開発者向け) | tools のスクリプトにまとめるだけ / 配布ゲームのプレイヤーがロビーから AWS サーバへ入る (API Gateway + Lambda 等の受付が要る) |
| Server.exe をどこで動かすか | **両方を選べる** (自分の PC = Anywhere、AWS 上 = マネージド EC2) | Anywhere のみ / EC2 のみ |
| AWS の呼び方 | **aws CLI を子プロセスで呼び JSON を読む** | AWS SDK for C++ の組み込み |
| 進め方 | harness | このセッションで段ごと / Anywhere だけ先に |

## 2. 現状 (確認済みの事実)

- M81 で `Server.exe --hosting gamelift` (Anywhere) を実装済み。手順書 `docs/gamelift-anywhere.md`、ADR-022、
  実疎通記録 `plans/m81-dedicated-server/anywhere-log/2026-10-02-anywhere.md`。
- 今の手順はすべて手打ち:
  1. `create-location` → `create-fleet --compute-type ANYWHERE` → `register-compute` (FleetId と `GameLiftServiceSdkEndpoint` を手で控える)
  2. `get-compute-auth-token` を環境変数 `MYE_GAMELIFT_AUTH_TOKEN` へ入れ (約 15 分で失効)、引数 8 個ほどの 1 行で Server.exe を起動
  3. `create-game-session` (ProcessReady の後でないと `FleetCapacityExceededException`) → `create-player-session` → **60 秒以内に** `Runtime.exe --net-connect IP:PORT --player-session-id psess-...`
  4. 片付け: `deregister-compute` → `delete-fleet` (数分) → `delete-location`
- 実疎通でつまずいた 3 点: コマンドの折り返し貼り付けで引数が落ちた / player session の 60 秒切れ / ロケーション名の `custom-custom-` 二重付け。いずれも「手で写す」ことが原因。
- `src/Server/ServerMain.cpp` の `MakeHosting`: `--gamelift-ws-url` / `--gamelift-fleet-id` / `--gamelift-host-id` / env トークンが揃わないと exit 1。トークンを引数で受けないのは設計 (プロセス一覧・履歴に残さない)。
- `src/Server/Hosting/GameLiftSdk.h` の `IGameLiftSdk::InitSdk(const GameLiftConnection&)` は Anywhere 用の引数つきだけ。`GameLiftSdkAws.cpp` は `Gl::Server::InitSDK(params)` を 30 秒の上限つきで呼ぶ。
- 同梱 SDK は Server SDK for C++ **v5.6.0** (`external/VERSIONS.md`)。`external/gamelift-server-sdk/include/aws/gamelift/server/GameLiftServerAPI.h:53` に**引数なしの `InitSDK()`** がある (マネージド EC2 では引数なしで呼ぶのが SDK の想定。※この点は公式ドキュメントで未照合)。
- エディタ側:
  - `src/Editor/Windows/Project/NetWindow.cpp`: セッションが無いとき「HOST:PORT」「player session ID」の入力欄と、`--net-connect ... --autoplay` 付きで自 exe (Editor) をもう 1 つ `ShellExecuteW` で起動するボタンがある。引数の注入対策 (`BuildConnectArgs` が空白・引用符・制御文字を拒否) あり。
  - `src/Editor/App/ChildProcess.h`: `StartChildProcess` (窓なし、stdout/stderr → ログファイル、stdin = NUL) / `PollChildProcess` / `CloseChildProcess`。非同期で aws CLI を呼ぶ土台に使える候補。
  - UI 文字列は `LocalizationTable.inl` + `Tr()` (AGENTS.md 4.3)。
- 書き出し: `src/Editor/Windows/Project/BuildSettingsWindow.cpp` は `Runtime.exe` / `GameLogic.dll` / `nethost.dll` 等を書き出す。**Server.exe + `libssl-3-x64.dll` / `libcrypto-3-x64.dll` をまとめるサーバ用の書き出しは無い**。EC2 の `upload-build` にはこれが要る。
- M93 のロビー (`src/Engine/Engine/Online/`、`online.backend = eos|local|none`) は EOS / local 専用で GameLift とは無関係。今回は触らない想定。
- aws CLI はこの PC に `C:\Program Files\Amazon\AWSCLIV2\aws.exe` がある。認証は `aws login` (期限切れで再ログイン)。

## 3. 外部仕様 (Web で確認した範囲)

- マネージド EC2 フリート: ビルドを `upload-build` (`--operating-system WINDOWS_2022`、`--server-sdk-version`) で上げ、READY になってから `create-fleet` (compute type EC2、インスタンスタイプ、inbound permissions = UDP ポート、runtime configuration = 起動パス `c:\game\...` と引数・同時プロセス数)。inbound permissions は後から `update-fleet-port-settings` でも設定できる。
  出典: https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-creating.html 、https://docs.aws.amazon.com/cli/latest/reference/gamelift/create-fleet.html 、https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-wrapper-tutorial.html
- 費用: Anywhere はゲームセッション配置と接続分数の従量 (12 か月の無料枠あり)。EC2 は起動から終了まで秒課金 (最低 1 分)。無料枠に c5.large On-Demand 月 125 時間の記載があるが期間・東京 Windows 単価は未確認。
  出典: https://aws.amazon.com/gamelift/servers/pricing/instance-pricing/ 、https://aws.amazon.com/gamelift/servers/faqs/

## 4. 司会が示した方針案 (ユーザーへ提示済み、細部は planner が詰める)

| 段 | 内容 |
|---|---|
| 1 | aws CLI を非同期で呼び JSON を読む層を `src/Editor/` に置く。CLI の有無・ログイン状態・リージョンを窓に出す。偽 CLI を差して selftest |
| 2 | 自分の PC (Anywhere): 「準備」「サーバ起動」「セッション作成して参加」「片付け」。トークンは環境変数で子プロセスへ |
| 3 | Server.exe に引数なし `InitSDK()` 経路を**明示フラグ**で追加 (暗黙の自動判定にしない)。サーバ用の書き出し |
| 4 | AWS 上 (EC2): 「アップロード」「フリート作成」「セッション作成して参加」「台数 0」「削除」。動いている間は「課金中」を目立たせる |
| 5 | 文書: gamelift-anywhere.md の改訂 (または新しい手順書)、ADR、engine_spec.md、両言語の UI 文字列、test_checklists |

守ること:
- sim / 通信プロトコル / ABI / SimSnapshot の版は変えない想定。AWS 操作はエディタの表示レーンだけ (決定性に触れない)。
- 削除・課金開始の操作は確認ダイアログを挟み、押すのはユーザー。Claude (AI) は自動モードで AWS のリソース削除を実行しない (M81 で拒否された経緯)。
- 自動検証は偽の CLI / 偽 SDK。本物の AWS 疎通はユーザーの手作業 (費用が出る)。
- ソース管理と同じく Editor 層に置き、Engine / Runtime / Shared から依存させない。

## 5. 未知・リスク (planner が詰める)

- **最大の不確実性**: まっさらな Windows Server 2022 (GameLift EC2) で Server.exe が起動するか。VC++ ランタイム (OpenSSL の DLL は vcpkg `x64-windows` = 動的 CRT の可能性)、`d3dcompiler_47` 等の遅延ロード DLL が無くても起動するか、assets / GameLogic.dll / シーンの同梱範囲。GameLift の install script (`install.bat`) で再頒布パッケージを入れる案もある。
- 出自照合 (エンジンのビルド / GameLogic.dll / contentHash) が一致するよう、アップロードするサーバとエディタから起動するクライアントを同じビルドに揃える方法。
- EC2 フリートの public IP へ UDP で届くか (inbound permissions)。往復 250ms 制限 (gamelift-anywhere.md 11 章) とリージョン選択。
- 実行中の CLI 呼び出し (フリート作成は数分〜数十分で ACTIVE) の進捗表示・キャンセル・タイムアウト。
- 設定の保存先: リージョン・フリート ID 等をどこに持つか (プロジェクト設定 / エディタのユーザー設定)。秘密 (トークン) は保存しない。
- ロケーション名の `custom-` 接頭辞はエディタが付け、ユーザーに入力させない。
- 窓を閉じても EC2 フリートが残る = 課金が続く。エディタ起動時に既存フリートを検出して警告するか。
- M81 の未確定 2 点 (Ctrl+C の ProcessEnding 即時反映、RemovePlayerSession) と R-11 は、今回の実疎通の確認項目に入れられる。
