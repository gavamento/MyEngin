# sub-11: NavMesh の高さを歩行面に合わせる (塗り・輪郭・ABI クエリの y)

- 依存: sub-09 (レビュー round 1 の修正。sub-12 より先に回す)
- 状態: OK (commit e145a66)
- 往復: 1

## 背景
review-1 #2 (major、coder 宛) と #3 (minor、planner 宛) は原因が同じ。
TileCache が作るポリゴンは頂点の高さしか持たず、詳細メッシュ (detail mesh) が無い。
- 段差 (天面 0.30 m) の上の 15 点は、ポリゴンの高さがすべて 0.050 になる。
- 坂では x=4.5 で +0.42 m、x=7.0 で −0.33 m ずれる。

この結果、塗りは段差の天面で床下に埋もれ、坂で浮き沈みする (`NavDebugDraw.cpp:219` → `:95-107`)。`NavSamplePosition` などの ABI が返す y も同じだけずれる。
この 1 つの原因を 1 つの仕組みで直す (spec 4.2 / 4.3、受け入れ条件 21)。

## やること
1. **歩行面の高さの出どころを 1 つ決める。** 候補は次の 2 つ。どちらにするかは coder が試作して決め、理由と計測値を SELF_EVAL に書く。
   - (a) TileCache の層が持つセルごとの高さ (`dtTileCacheLayer::heights`) を使う。
   - (b) `dtTileCacheMeshProcess` で、層の高さから詳細メッシュ (detailMeshes / detailVerts / detailTris) を作って `dtNavMeshCreateParams` に渡す。こうすると Detour の `getPolyHeight` / `closestPointOnPoly` が正しい高さを返す。
   (b) を選んだ場合の注意:
   - dtCrowd の位置の y や経路が変わりうるので、ハッシュと golden が動く。理由を書いて焼き直す。
   - `.mnav` には層しか入っていないので形式は不変 (タイルは読み込み時に作る)。kNavBakeVersion を上げるかどうかも判断する。
2. **塗りと輪郭** (`NavDebugView`): 歩行面の高さに沿って描く。段差の天面が塗られ、坂で浮き沈みしないこと。既存の持ち上げ (塗り 2 cm / 線 3 cm) は維持する。三角形の数が増えるなら、作り直しの時間と三角形の数をログで測る。NavMesh が変わらない tick では作り直さない (既存の決まり) を守る。
3. **ABI クエリの y**: 次の 4 つが返す y を、歩行面との差 0.1 m 以内にする。
   - `NavSamplePosition`
   - `NavFindPath` のコーナー
   - `NavRaycast` の当たり点
   - `NavFindRandomPoint`
   (b) を選べば Detour の関数がそのまま正しい値を返す。(a) を選ぶなら、クエリの結果に高さの補正を 1 か所で掛ける。sim の決定論 (Debug / Release / Server の一致、restore 後の一致) を保つこと。
4. **SelfTest**: 段差 (天面 0.3 m) の上と 30 度の坂の上の点ごとに次を検査する。
   - `NavSamplePosition` などの y と歩行面の差 ≤ 0.1 m
   - 塗りの三角形の高さと歩行面の差 ≤ 0.1 m
5. **golden `nav`**: 段差の天面が塗られた絵で撮り直す (`tests\golden\nav.png` だけを手撮り)。
6. ADR-023 の「既知の限界」に高さの扱いを書く。engine_spec.md の 10.9 と、`EngineAPI.h` の v24 節のコメント (返す y の精度) を更新する。

## やらないこと (このサブでは)
- Stuck と到着の規則 (sub-12)
- ABI の版を上げること (関数の署名は変えない。返す値の精度が上がるだけ)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Navigation\NavTileCacheSupport.cpp` (`NavMeshProcess::process`、`dtNavMeshCreateParams`)、`NavDebugDraw.cpp`、`NavSystem.cpp` (Query*)
- 期待値: `NavAgentSelfTest.cpp` / `NavSurfaceSelfTest.cpp` / `NavDeterminismSelfTest.cpp`、`tests\golden\nav.png`
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`src\Shared\EngineAPI.h` (コメントのみ)

## 受け入れ条件 (このサブ)
1. (spec 21) 段差と 30 度の坂の上で、クエリの y と塗りの高さが歩行面との差 0.1 m 以内。— SelfTest
2. golden `nav` で段差の天面が塗られている (画像を目視し、パスを書く)。shot_verify は nav が PASS、既知の 5 枚は値が不変。
3. replay_verify 全 15 ジョブ PASS。Debug / Release の selftest で、Nav 系は ALL PASS。焼き直した期待値は「旧値 → 新値、理由」を列挙する。
4. 警告 0、check_rules 0。

## 検証コマンド
- 両構成ビルド、`Editor.exe --selftest` と `Server.exe --selftest` を両構成で直列実行
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`
- `bin\x64\Release\Runtime.exe --nav-demo --screenshot` と、Editor の編集中の SceneView のスクショ

## 実装メモ (coder が追記)

### round 1
- 方式は (a) 層のセルの高さ。`NavTileStore::SampleSurfaceHeight(x, z, yHint, outY)` を足し (`entries_` の層のバイト列だけを読む純関数)、
  NavSystem の 4 クエリは `SnapToSurface` 1 関数で補正、`NavDebugView` の `GeometryCollector` が DebugUtils の三角形・線分を適応分割して高さを合わせる。
- 補正式: セルの値 − 半セル (ベイクは上端を切り上げ最低 1 セルの厚み) − 斜面の隅の分 `cs/2 (|gx|+|gz|)` + 勾配 × セル内の位置。勾配は隣のセルとの差が `1.2 cs` 以内のときだけ。
  半セルだけだと 30 度の坂で 0.107〜0.138 m 残り、勾配補正で 0.03 m 台になった。
- 却下 (b) 詳細メッシュは作っていない (見積もりのみ): ライブラリに層からの詳細メッシュ生成が無い・dtCrowd の y と経路が動いてハッシュ/replay/golden が全部焼き直しになる。(a) はハッシュが 1 つも動かない。
- 詳細は ADR-023 決定 13。ABI の署名・版、kNavBakeVersion、kSimSnapshotVersion は不変。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。方式 (a) を、(b) を試作せずに決めたことを承認する。(b) は Recast に、層から詳細メッシュを作る口が無い (rcBuildPolyMeshDetail は rcCompactHeightfield を要る)。(a) は sim に触れず、ハッシュが 1 つも動かないことを実測している。この 2 点は、試作の代わりの根拠として十分。適応分割 (1,182 三角形、約 1.2 ms) を採用。補正を外すと 4 件が FAIL することも確認済み。残り: 編集中の SceneView のスクショ → reviewer。台の中に取り残された床と、Agent の経路線の y は既知の限界として ADR に記録済み。
