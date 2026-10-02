# sub-12: 到着余裕の適応目標と、server_verify の判定強化

- 依存: sub-11
- 状態: 未着手 (D18 のユーザー回答待ち。回答によって「やること 1」が変わる)
- 往復: 0

## やること
spec D18 / 4.1.6 (改定) / V13〜V15 / R-13。

1. **到着余裕の目標 (D18)**。案ごとに次を行う。
   - 案 (a)、planner の裁定: 目標 = clamp(1 tick + 2σ, 1 tick, 6 tick)。
     - σ はサーバが測る余裕の標本の、直近の標準偏差。窓の長さは coder が決めて理由を書く。目安は直近 2〜5 秒の tick 標本。
     - σ を Confirmed に載せるか、クライアントが受け取った margin の系列から自分で求めるかは、coder が選ぶ。proto のレイアウトを変えるなら kNetProtoVersion を上げ、理由を書く。
     - 目標の変化はゆっくりにする (急に上下させない)。追いつきと速度係数は、引き続き「目標 − 到着余裕」の 1 つの誤差から導く。
     - **これは「いつ tick が回るか」だけを変え、sim には入らない** (C5)。
   - 案 (b) が選ばれた場合: 目標を 3 tick に固定する。
   - 案 (c) が選ばれた場合: 目標は変えない。やること 2〜4 だけを行う。ケース A は実 GPU のクライアントのまま残し、bat のヘッダにその理由を書く。
2. **V13 (案 a / b のとき)**
   - selftest で次を assert する。
     - ジッタ 0: 窓平均が 16ms ± 1 tick。
     - 片道 30ms ± 10ms のジッタ: 代替入力率 ≤ 5%、かつ窓平均 ≤ 1 tick + 2σ + 1 tick。案 b なら窓平均 ≤ 4 tick。
   - server_verify のケース A のクライアントを WARP に戻す (sub-11 の暫定措置を外す)。R5 (late-subst ≤ 5%) を通す。
   - 実測を並べる: WARP の Runtime 1 台、Editor、実 GPU の Runtime の 3 構成で、late-subst と定常の到着余裕。sub-11 の実測と比べる。
3. **V14**: server_verify のケース A の合否に、「クライアントが要求した再同期 (desync / EventGap / BadSnapshot) が 0」を加える。クライアントのログか .rsN.rep の有無で判定する。
4. **V15 / R-13**
   - サーバが、予測上限を超えて追いつけないクライアントを検出したら WARN を出す。条件は「最初の有効な入力が参加から N 秒届かない」や「余裕が継続して −(予測上限) を下回る」など。ログには peer とおよその RTT を出す。
   - 文書に対応上限 (RTT 約 250ms) を書く。対象は ADR-022、engine_spec §11.5、docs\gamelift-anywhere.md。
   - 再スナップショットによる救済はしない。
   - selftest: 片道 150ms の偽トランスポートで WARN が出ることを確認する。

## やらないこと (このサブでは)
- 遅すぎるクライアントの救済 (再スナップショット)
- AcceptPlayerSession の非同期化 (R-11)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Net\ClientSession.cpp/.h`、`ClientSimRunner.cpp`、`ServerSession.cpp/.h`、`ServerNetSelfTest.cpp`
- `tools\server_verify.bat`
- `docs\adr\ADR-022-dedicated-server.md`、`engine_spec.md` §11.5、`docs\gamelift-anywhere.md`、`docs\test_checklists.md`

## 受け入れ条件 (このサブ)
spec 5. の **V13 (案 a / b のとき)、V14、V15** と **C1〜C7**。

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (直列)、`Server.exe --selftest`
- `tools\server_verify.bat` (ABCD。ケース A は WARP で)、`tools\replay_verify.bat`、`tools\net_verify.bat`、`pwsh -File tools\check_rules.ps1`
- やること 2 の 3 構成の実測

## 実装メモ (coder が追記)

## フィードバック履歴
