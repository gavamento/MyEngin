# sub-04: サーバ/クライアントのプロトコルと 1 プロセス内検証

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.1 / 4.1.4〜4.1.8 / 4.2 (プロトコル) / D4 / D11 / D12 を、**トランスポートとクロックを注入できる**形で実装し、1 プロセス内の決定的な偽トランスポートで検証する。プロセス間の配線 (Server.exe のループ、EngineLoop のクライアント経路) は sub-05。

1. `src\Engine\Engine\Net\` に:
   - `ServerSession`: Hello 受付 (ValidatePlayer はコールバックで注入)、eventSeq 採番、レーン割り当て (Session の純関数を呼ぶ)、締め切り判定 (現在時刻 ms は引数で受け取る)、確定入力 (レーン入力 + SystemInputTick) の生成と冗長配信、checkpoint ハッシュ、到着余裕、スナップショット送付 (チャンク ≤ 1024B、Ack ビットマップ、フレームあたり送信上限)、再接続、予約タイムアウト → Release、ResyncRequest への応答。
   - `ClientSession`: Hello/Welcome/Reject、自レーン入力の送信 (冗長 kNetRedundancy)、Confirmed の受信とリング、eventSeq 欠番検出 → ResyncRequest、スナップショット受信と組み立て、到着余裕からの tick 速度係数 (±2% 上限)、checkpoint 照合 → 不一致の報告。
   - パケットは既存 `NetPacketHeader` を流用し `NetMsg` に追加 (spec 4.2、既存値は動かさない)。
   - **トランスポート**: 送受信を `std::function` か小さな POD キュー経由にして、実 `UdpSocket` と偽トランスポートを差し替え可能にする (仮想関数の抽象クラスを増やさない方が既存の流儀に合う — coder が既存コードを見て選ぶ。選んだ理由を SELF_EVAL に)。
   - **クロック**: `NowMs()` を内部で読まない。呼び出し側が渡す。
2. ロールバック: `NetRollback` のリング容量をコンパイル時最大 (16+4) にし、上限を `Begin` の引数で受ける。P2P は 8 (現状と同じ挙動)、サーバ構成のクライアントは 12。投機記録 (`NetSpecTick`) に SystemInputTick を足し、予測が外れたとき (イベントが来た) も巻き戻す。
3. 1 プロセス内の検証ハーネス (selftest):
   - 偽トランスポート: seed 付き `Pcg32` で遅延 (固定 + ジッタ)・ロス・並べ替え・重複を作る。時刻は仮想時刻 (ms) をテストが進める。
   - sim: sub-01 のヘッドレス sim ホストを N+1 個 (サーバ 1 + クライアント 3) 立てる。GameLogic のスクリプトを複数インスタンスで共有できない場合 (spec R-4) は、スクリプト無しのコード構築シーン (PlayerInput のミラー + 物理) で行い、その判断を SELF_EVAL に書く。
   - シナリオは spec N1〜N4。

4. (sub-01 VERDICT から) 確定入力の差し込み口は HeadlessSim の verify ループの `ctx.inputs[p] = ...` を一般化して作る。EngineLoop の TickServices 組み立てと HeadlessSim::Impl::BuildTickServices の重複は sub-01 で許容したが、**ここで入力の置換 (レーン入力 + SystemInputTick) を足すときは EngineLoop と HeadlessSim で同じ関数を呼ぶ**こと (置換の規則が 2 か所に分かれると、サーバとクライアントで消費する列が食い違う)。複数インスタンスを交互に回すときは tick 直前に `Activate()` を呼ぶ (sub-01 申し送り)。

## やらないこと (このサブでは)
- Server.exe の実時間ループ、UDP の実配線、Runtime の `--net-connect` (sub-05)
- ホスティング (sub-05 で LocalHosting、sub-07 で GameLift)
- ABI (sub-06)

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Net\ServerSession.*`、`ClientSession.*`、`NetProtocol.h` (メッセージ型)、`ServerNetSelfTest.cpp/.h`
- `src\Engine\Engine\Net\NetRollback.h/.cpp` (容量と上限、SystemInputTick)
- `src\Engine\Engine\Net\NetSession.h` (NetMsg / NetRole の追加)
- sub-01 のヘッドレス sim ホスト (複数インスタンス化に必要なら)

## 受け入れ条件 (このサブ)
spec 5. の **N1, N2, N3, N4** と **C1, C2, C3, C5, C6, C7**。
- C5 の観点: サーバの確定処理の中で、実時間・到着順が「どの tick に何を入れたか」以外の形で sim へ入っていないこと。SELF_EVAL に「sim へ入る値の一覧と、それぞれが記録される場所」を書く。

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (新スイートの PASS 行を貼る)
- `tools\replay_verify.bat`、`tools\net_verify.bat`、`pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
