# sub-12: 到着余裕の適応目標と、server_verify の判定強化

- 依存: sub-11
- 状態: OK (commit f8a5cba)。D18 = 案 (a)
- 往復: 1

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

### round 1 (SELF_EVAL 要旨)
- 案 (a)。σ はサーバが計算して `Confirmed.flags` の上位 16 bit (1/4 ms 単位) に載せた。窓は 180 標本 (tick ごと 1 標本、約 3 秒)。クライアントは σ を 1/32 ずつなだらかにして目標 = clamp(16ms + 2σ, 16ms, 6 tick) にする。ペイロードの大きさ・`kNetProtoVersion` は不変 (旧クライアントは bit0 だけ見る。版を上げると P2P の出自と `.rep` を巻き込む)。
- V15: サーバが直近 180 標本の 9 割以上が締め切り後になった peer に 1 回 WARN (`cannot keep up`)。`ServerStats.unreachableWarnings`。文書 3 か所に対応上限 (往復約 250ms) を記載。
- server_verify A は WARP に戻し、窓 640x360 (2 台の WARP の奪い合いで片方が数十秒止まるため) と client 2 の記録を 3 倍 (30 秒) にした。V14 は `:check_client_resync` (ログの `requesting a resync` と `.rs1.rep`)。
- 検証: Debug / Release `Editor.exe --selftest` (Source control 既知 2 項目のみ FAIL、Server/client net は ALL PASS)、Debug / Release `Server.exe --selftest` PASS、check_rules 0 error、net_verify / replay_verify PASS、server_verify ABCD PASS (A: 0.37% / 1.05%)。

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。確認した受け入れ条件は次のとおり。V13 (selftest の 2 シナリオで目標 ± 1 tick・代替入力率 0%、ケース A を WARP に戻して 0.37% / 1.05%、σ を無視した対照では FAIL)、V14 (クライアント要求の再同期 0)、V15 (片道 150ms で WARN 1 回、片道 90ms では 0)、C1〜C7。実プロセス 3 構成で、Editor は 14% → 0.13%、WARP は 26% → 1.6% に改善した。承認した点: σ を Confirmed.flags の上位 16bit に載せ proto を据え置く判断、係数 (平滑 1/32、窓 180、上限 6 tick)、WARN の条件を割合にしたこと、ケース A の窓縮小と記録延長 (検証環境の措置。エンジンの挙動は不変)。
