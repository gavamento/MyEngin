# GameLift Anywhere で Server.exe を動かす手順書 (M81)

専用サーバ `Server.exe` を AWS GameLift の **Anywhere フリート**に登録し、AWS 側からゲームセッションと
player session を作って、`Runtime.exe --net-connect` で入るまでの手順。EC2 フリートは対象外
(Windows の EC2 / コンテナは M81 のスコープ外)。

> **この手順書の状態**: AWS CLI のオプション名は AWS CLI リファレンス (docs.aws.amazon.com/cli/latest/reference/gamelift/)
> で照合した。Server.exe 側の GameLift 連携は偽の SDK を使った自己テスト (`Server.exe --selftest`) で確かめてある。
> **実際の AWS へはまだ一度も接続していない** (M81 sub-08 でこの手順をユーザーが実行して初めて確かめる)。
> 手順中の「未検証」は、その点を実機で確認するという印。食い違いが出たらこの文書を直すこと。

## 0. 全体像

```
 あなたの PC (Anywhere のコンピュート)                              AWS
 ┌──────────────────────────────┐   wss (SDK の WebSocket)   ┌────────────────────┐
 │ Server.exe --hosting gamelift │ ◄────────────────────────► │ GameLift Servers   │
 │   UDP :7777 を待受            │   InitSDK / ProcessReady    │  (Anywhere フリート) │
 └──────────────▲───────────────┘   ActivateGameSession 等    └─────────▲──────────┘
                │ UDP                                                    │ CreateGameSession
 ┌──────────────┴───────────────┐                                        │ CreatePlayerSession
 │ Runtime.exe --net-connect IP:7777 --player-session-id psess-...       │ (AWS CLI)
 └──────────────────────────────┘ ◄──────────────────────────────────────┘
```

- ゲームの通信 (UDP) は GameLift を通らない。GameLift は「このプロセスにセッションを割り当てる」「この player session ID は有効か」
  を仲介するだけ。認証は player session ID の照合のみ (通信の暗号化はしない)。
- Server.exe が SDK に対して行うこと: `InitSDK` → `ProcessReady` (UDP ポートとログのパスを渡す) → 待機 →
  `OnStartGameSession` を受けて `ActivateGameSession` → 参加者ごとに `AcceptPlayerSession` →
  予約の解放 (切断確定) で `RemovePlayerSession` → 終了時に `ProcessEnding` → `Destroy`。
- GameLift のコールバックは SDK の内部スレッドから来るが、Server.exe はキューに積むだけで、処理は tick の境界
  (メインループの先頭) で行う。**sim (ゲームの状態) は GameLift の出来事に直接触れない**。

## 1. 準備

- AWS アカウントと AWS CLI v2 (`aws --version`)。リージョンは GameLift Anywhere に対応している所 (例 `us-west-2`)。
  以降の例は `--region us-west-2` を省略している。`aws configure` か `AWS_REGION` で揃えること。
- `bin\x64\Release\Server.exe` と同じフォルダに `libssl-3-x64.dll` / `libcrypto-3-x64.dll` があること
  (ビルド後に自動でコピーされる)。配布先へ Server.exe を持っていくときは 2 つも一緒に。
- 費用: Anywhere のフリートやコンピュート登録に課金があるかは AWS の GameLift 料金ページで確認すること。
  作業が終わったら **必ず「9. 片付け」** まで行う。

### IAM

| 役割 | 必要な権限 (アクション) |
|---|---|
| 構築する人 (フリートを作る / 消す) | 開発中は `gamelift:*` でよい。後で絞るなら `CreateLocation` `DeleteLocation` `CreateFleet` `DeleteFleet` `DescribeFleetAttributes` `RegisterCompute` `DeregisterCompute` `DescribeCompute` |
| サーバを動かす人 (トークンを取る) | `gamelift:GetComputeAuthToken` `gamelift:RegisterCompute` `gamelift:DescribeCompute` |
| セッションを作る側 (バックエンドの代わり) | 最小: `gamelift:CreateGameSession` `gamelift:CreatePlayerSession` `gamelift:DescribeGameSessions` `gamelift:DescribePlayerSessions` |

セッション作成側の最小ポリシーの例 (フリート ARN でさらに絞れる):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "gamelift:CreateGameSession",
        "gamelift:CreatePlayerSession",
        "gamelift:DescribeGameSessions",
        "gamelift:DescribePlayerSessions"
      ],
      "Resource": "*"
    }
  ]
}
```

(`Resource` を絞れるアクションと絞れないアクションが混在するため、ここでは `*`。実運用で絞るときは AWS の
「GameLift のアクション・リソース・条件キー」表を確認すること。未検証。)

## 2. カスタムロケーションとフリート (1 回だけ)

```powershell
# ロケーション名は custom- で始める
aws gamelift create-location --location-name custom-mye-dev

# Anywhere フリート。出力の FleetId (fleet-...) を控える
aws gamelift create-fleet --name mye-anywhere --compute-type ANYWHERE --locations Location=custom-mye-dev
```

## 3. この PC をコンピュートとして登録する

`--ip-address` は**クライアントが Server.exe に UDP で届く IP**。GameSession / PlayerSession の応答に
この IP とサーバのポートが入って返る。同じ PC 内の試験なら `127.0.0.1` で足りるはず (未検証)。
別の PC から入れるなら、その PC から見えるアドレスにする (「7. ファイアウォールとポート」)。

```powershell
aws gamelift register-compute --fleet-id <FleetId> --compute-name mye-dev-pc --ip-address 127.0.0.1 --location custom-mye-dev
```

応答の `Compute.GameLiftServiceSdkEndpoint` (例 `wss://<region>.api.amazongamelift.com`) を控える。
これが Server.exe の `--gamelift-ws-url` になる。

## 4. 認証トークンを取って Server.exe を起動する

トークンは **約 15 分で失効する**。取ってからすぐ起動し、疎通確認 (CreateGameSession まで) を 15 分以内に終える。
トークンの自動更新は M81 では行わない (長く動かすなら取り直して再起動)。

トークンは**環境変数**で渡す (コマンドラインに出すとプロセス一覧や履歴に残るため、引数では受け付けない)。

```powershell
$env:MYE_GAMELIFT_AUTH_TOKEN = (aws gamelift get-compute-auth-token --fleet-id <FleetId> --compute-name mye-dev-pc --query AuthToken --output text)

bin\x64\Release\Server.exe --hosting gamelift `
  --gamelift-ws-url <GameLiftServiceSdkEndpoint> `
  --gamelift-fleet-id <FleetId> `
  --gamelift-host-id mye-dev-pc `
  --port 7777 --max-players 4 `
  --local-demo --synth-input `
  --replay-record cache\anywhere_server.rep
```

- `--gamelift-host-id` は register-compute の `--compute-name` と同じ値。`--gamelift-process-id` は省略すると PID。
- シーン指定 (`--local-demo` / `--scene` / `--project`) と、クライアントと一致が必要な起動オプション
  (`--synth-input` など。`Runtime.exe` 側にも同じものを付ける) は `--hosting local` のときと同じ。
- 接続情報が欠けていれば、InitSDK を呼ぶ前に足りないものを全部挙げて exit 1 する。
  接続先に繋がらないときは SDK が再試行を続けるため、**30 秒で打ち切って exit 1** する
  (原因は URL の誤り / フリート・コンピュートの ID の誤り / トークンの失効のいずれか)。
- 起動に成功するとログに `[gamelift] ProcessReady sent (UDP port 7777 ...)` が出て、セッションの割り当てを待つ
  (この間 tick は回らない)。`--server-timeout SEC` を付けると、割り当てが来ないまま SEC 秒たったときに exit 5 で終わる。

## 5. ゲームセッションと player session を作る

別のシェルで (セッション作成側の資格情報で):

```powershell
# game-properties は省略可。Server.exe が読むのは次の 2 つだけ (どちらも tick 数、範囲外・不正値は無視して既定のまま)
#   myeDeadlineTicks      遅い入力を待つ長さ (1..600、既定 3)
#   myeRejoinTimeoutTicks 切断から席を解放するまでの猶予 (1..216000、既定 1800 = 30 秒)
aws gamelift create-game-session --fleet-id <FleetId> --location custom-mye-dev `
  --maximum-player-session-count 4 `
  --game-properties Key=myeDeadlineTicks,Value=3
```

- 応答の `GameSessionId` を控える。Server.exe のログに `game session ... activated` が出れば成功。
  `aws gamelift describe-game-sessions --fleet-id <FleetId> --location custom-mye-dev --game-session-id <id>` で
  `Status` が `ACTIVATING` → `ACTIVE` になる。
- `--maximum-player-session-count` が `--max-players` (サーバのレーン数) より小さければ小さい方が入場の上限になる。
  大きければレーン数で頭打ち (超えた分は Reject)。レーン数は sim を作る時点で決まるので、セッションで広げることはできない。

プレイヤーごとに player session を作る (`--player-id` は任意の文字列):

```powershell
aws gamelift create-player-session --game-session-id <GameSessionId> --player-id player1
```

応答の `PlayerSessionId` (`psess-...`) と `IpAddress` / `Port` を控える。

## 6. クライアントから接続する

```powershell
bin\x64\Release\Runtime.exe --local-demo --synth-input `
  --net-connect <IpAddress>:<Port> --player-session-id <PlayerSessionId>
```

- サーバと同じシーン・同じ起動オプションにすること。出自 (エンジンのビルド / GameLogic.dll / assets の内容) が
  サーバと違うと Hello が拒否される (Reject のログに理由が出る)。Debug と Release を混ぜる検証では
  `--allow-game-mismatch` が要る (本番では使わない)。
- player session ID が無効 / 期限切れなら `AcceptPlayerSession` が失敗し、クライアントは `PlayerRejected` で拒否される。
- 切断しても `--net-rejoin-timeout` の間は席が予約され、同じ player session ID で戻れる (その間 GameLift には
  player session を ACTIVE のまま残す)。猶予が切れた時点で `RemovePlayerSession` を送る。

## 7. ファイアウォールとポート

- **Server.exe の外向き**: `wss://` (TCP 443) で GameLift の SDK エンドポイントへ。企業 LAN などで 443 が
  プロキシ越しのみの環境では繋がらない (SDK はプロキシを使わない: 未検証)。
- **Server.exe の内向き**: UDP `--port` (既定 7777)。Windows のファイアウォールで許可する:

  ```powershell
  New-NetFirewallRule -DisplayName "MyEngine Server UDP 7777" -Direction Inbound -Protocol UDP -LocalPort 7777 -Action Allow
  ```

  (管理者権限。試験が済んだら `Remove-NetFirewallRule -DisplayName "MyEngine Server UDP 7777"` で消す。)
- 別の PC / 外部から入るには、ルータのポート転送 (UDP) と `register-compute --ip-address` に外から見える IP が要る。
  NAT 越え・IPv6 は M81 の対象外。

## 8. 終了の仕方と確認

- Server.exe を止める: GameLift から `OnProcessTerminate` が来る、または Ctrl+C。どちらも**同じ経路**で
  記録中の .rep を閉じてから `ProcessEnding` → `Destroy` を送って終了する (exit 0)。
  `--exit-when-empty` は全員が出てから 2 秒で、`--replay-ticks N` は N tick で、同じ経路で終わる。
- .rep (`--replay-record`) は走りながら書かれる (`flush` 1 秒おき)。**異常終了しても、完了済みの tick までは**
  `Server.exe --replay-verify` / `Runtime.exe --replay-verify` で読める (ヘッダの tickCount が 0 のまま残るので
  ファイル長から数える)。`--crash-test av --crash-at-tick 200` で本当に落として確かめられる。
  クラッシュ時は `<exe のフォルダ>\crash\<日時>\` に `minidump.dmp` と `crash.txt` が出る。
- AWS 側の確認: `describe-game-sessions` の `Status` が `ACTIVE` → `TERMINATED` になること。
  Anywhere ではログの自動回収は行われない (`ProcessReady` に渡した .rep のパスはローカルにそのまま残る)。
- 実疎通の記録は `plans\m81-dedicated-server\anywhere-log\` に残す (サーバのログ + AWS CLI の出力。
  トークンと資格情報は**必ず伏せる**)。

## 9. 片付け (必ず行う)

```powershell
aws gamelift deregister-compute --fleet-id <FleetId> --compute-name mye-dev-pc
aws gamelift delete-fleet --fleet-id <FleetId>
aws gamelift delete-location --location-name custom-mye-dev
```

実行中のゲームセッションがあるとフリートは消せない。Server.exe を止めて `TERMINATED` を確認してから行う。
ファイアウォール規則も消す (7 章)。

## 10. うまくいかないとき

| 症状 | 原因の見当 |
|---|---|
| `--hosting gamelift needs a GameLift Anywhere connection; missing: ...` | 引数 / 環境変数 `MYE_GAMELIFT_AUTH_TOKEN` の不足。メッセージに挙がったものを足す |
| `InitSDK did not finish within 30 s ...` | `--gamelift-ws-url` の誤り、`--gamelift-fleet-id` / `--gamelift-host-id` の誤り、トークンの失効 (15 分)、443 が通らない。SDK のログ `[gamelift-sdk] ...` に接続の再試行が出る |
| `InitSDK failed: ...` | SDK が返したエラー名とメッセージがそのまま出る (多くはトークン / ID の不一致) |
| ProcessReady のあと何も起きない | `create-game-session` の `--location` がフリートのカスタムロケーションと違う。`describe-game-sessions` で状態を見る |
| クライアントが `PlayerRejected` | player session ID の誤り / 期限切れ / 別のゲームセッションのもの。Server のログに `AcceptPlayerSession(...) failed` が出る |
| クライアントが Reject (出自の違い) | サーバとクライアントでビルド / GameLogic.dll / assets / 起動オプションが違う (6 章) |
| 人数が足りないのに入れない | `--maximum-player-session-count` か `--max-players` が小さい (5 章)。Server のログに `game session is full` |

## 11. 既知の制限

- `AcceptPlayerSession` / `RemovePlayerSession` は SDK の同期呼び出しで、**メインループ (tick) の中で**呼ばれる。
  応答が速い間は問題にならないが、GameLift への WebSocket が詰まると SDK 内の再試行の間サーバのループが止まる
  (その間は tick が進まず、他の参加者の入力も処理されない)。実疎通で参加時の tick 時間を見て、
  問題があれば別スレッド化を検討する (M81 sub-08 / 後続)。
- トークンの自動更新はしない。`ProcessReady` 後にトークンが切れると SDK が WebSocket を張り直せず、
  `OnProcessTerminate` も来なくなりうる (未検証)。長く動かすときは取り直して再起動する。
- SDK の TLS は証明書を検証しない設定になっている (SDK 側の実装。M81 では変えない)。
- 1 プロセスが担当できるゲームセッションは 1 つ。2 つ目が割り当てられても無視する (ログに警告)。
