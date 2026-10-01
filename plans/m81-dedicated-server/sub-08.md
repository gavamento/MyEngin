# sub-08: GameLift Anywhere 実疎通 (ユーザー手動確認)

- 依存: sub-07
- 状態: 未着手 (ユーザーの AWS アカウント・IAM の準備待ち。他サブの完了を妨げない — spec R-5)
- 往復: 0

## やること
spec G5。**実行するのはユーザー**。coder の仕事は (1) 手順書 `docs\gamelift-anywhere.md` を見ながらユーザーが詰まった点の修正、(2) ログの整理。

1. 司会がユーザーに手順書を渡し、次を実行してもらう:
   - IAM の準備、`create-location`、`create-fleet` (ANYWHERE)、`register-compute` (この PC の IP)
   - `MYE_GAMELIFT_AUTH_TOKEN` を設定して `Server.exe --hosting gamelift ...` を起動 (トークン取得から 15 分以内に一連を終える)
   - `create-game-session` → `describe-game-sessions` で ACTIVE → `create-player-session` → `Runtime.exe --net-connect <IP:PORT> --player-session-id <ID>` で接続 → 数十秒プレイ → Runtime を閉じる → サーバの終了 (Terminate またはセッション終了)
   - `describe-game-sessions` で TERMINATED (または ACTIVATING→ACTIVE→TERMINATED の推移) を確認
2. ログ (サーバのログ、Runtime のログ、AWS CLI の出力。**トークン・アカウント ID・アクセスキーは伏せる**) を `plans\m81-dedicated-server\anywhere-log\` に置く。
3. 問題が出たら coder が修正し、修正は C1〜C6 を満たす。

## やらないこと (このサブでは)
- AWS 資格情報・トークンをリポジトリへ入れること (絶対にしない)
- EC2 フリート

## 触る場所 (planner の見立て)
- 修正が要れば `src\Server\Hosting\GameLiftHosting.*`、`docs\gamelift-anywhere.md`

## 受け入れ条件 (このサブ)
spec 5. の **G5** (+ 修正があれば C1〜C6)。
- 判定の根拠はユーザーが残したログ。ゲームセッションが ACTIVE になり、player session が Accept され (サーバログ)、Runtime が確定 tick を受けてプレイでき、セッションが終了状態になったこと。

## 検証コマンド
- (ユーザー作業) 手順書のコマンド列
- 修正があれば `tools\server_verify.bat`、`tools\replay_verify.bat`、`pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
