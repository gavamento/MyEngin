# Recast Navigation へ当てたパッチ (M82a)

ベース: v1.6.0 (`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`)。パッチは `MYE-PATCH(M82a)` というコメントで探せる。
`Debug` / `Release` / `Server.exe` のハッシュ不一致は出なかった (`NavDeterminismSelfTest`)。
下の 6 件のうち、1・2 は**ビット一致のためではなく、状態の保存・復元のため**のパッチ。3 は動的な障害物でエリアを塗り替えるため、4 は Off-Mesh Link の渡りを NavSystem が持つため、5・6 は NavMeshAgent の細かい制御 (M84d) のためのパッチ。

## 1. DetourCrowd.cpp: `MAX_ITERS_PER_UPDATE` を 100 から実質無制限へ

- 場所: `DetourCrowd\Source\DetourCrowd.cpp:48-50`
- 内容: `static const int MAX_ITERS_PER_UPDATE = 100;` を `0x3fffffff` にした。
- 理由: `dtCrowd::update` は経路要求 (`dtPathQueue`) を 100 反復ごとの時間分割で進める。長い経路は update をまたいで
  途中状態 (`dtPathQueue` の `m_queue` と、`dtNavMeshQuery` の sliced find path の状態) を持ち越すが、これらは
  private で外から保存できない。tick 境界に途中状態を残さなければ、保存すべき dtCrowd の状態はエージェント配列だけになる。
- 影響: 経路探索は node pool (4096) で必ず終わる。重い経路が 1 tick で完走するので、その tick の処理時間が伸びる。
  経路の結果は変わらない (時間分割しても同じ探索を続けるだけ)。
- 確認方法: 上限を 1 にして `NavDeterminismSelfTest` を回すと、要求が途中の tick で `NavSaveCrowd` が失敗し、
  「連続実行が成功し tick 50 を保存できた」を含む 21 項目が FAIL する (2026-10-03、Debug / Server.exe)。
  100 → 無制限へ戻すと全 PASS。

## 2. DetourLocalBoundary.h: `friend struct dtLocalBoundaryAccess;` を追加

- 場所: `DetourCrowd\Include\DetourLocalBoundary.h:27-29`
- 内容: friend 宣言 1 行。挙動は変えない。
- 理由: `dtLocalBoundary` はエージェントごとの衝突境界のキャッシュ (`m_segs` / `m_polys`) を private で持つ。
  エージェントが最後に境界を作った位置 (`m_center`) は現在位置と違うことがあるので、復元で作り直すと
  次の tick の回避が変わりうる。`NavTileCacheSupport.cpp` の `dtLocalBoundaryAccess` が読み書きする。
- 確認方法: 復元時に境界を捨てる変異を入れると、毎 tick の 保存 -> 復元 -> 再保存 のバイト列比較が FAIL する
  (軌道のハッシュは割れなかった = 試験ジオメトリでは境界が結果を変えない。キャッシュが古い状態の保存は
  バイト比較でだけ守られている)。

## 3. DetourTileCache: 障害物を「エリアの塗り替え」にできるようにする (M82g、NavMeshModifier)

- 場所: `DetourTileCache\Include\DetourTileCache.h` (`dtTileCacheObstacle` に `priority` / `areaId` / `paint`、`setObstaclePaint`)、
  `DetourTileCache\Source\DetourTileCache.cpp` (`setObstaclePaint`、`buildNavMeshTile` の障害物ループ)、
  `DetourTileCache\Source\DetourTileCacheBuilder.cpp` (`dtMarkCylinderArea` / `dtMarkBoxArea` 2 種)
- 内容: `paint` の立った障害物は切り抜かず、範囲内の層セルのエリアを `areaId` にする。`buildNavMeshTile` は先に paint を
  `priority` 昇順で塗り、そのあとに従来の切り抜き (エリア 0) を行う。`dtMark*` は `areaId != 0` のとき通行不可のセルを復活させない。
  `paint` を立てない既存の使い方 (切り抜き) の結果は変わらない。
- 理由: Modifier を実行時に動かす / 外すとき、層 (ベイク結果) へ焼き込むと元のエリアに戻せない。障害物と同じく層の上の
  オーバーレイにして、タイルを作り直すたびに塗る。障害物のスロット番号は追加・撤去の履歴で決まるので、塗り順には
  呼び出し側が与える `priority` (エンティティキー) を使う。
- 確認方法: `NavAgentSelfTest` の Modifier 項目 (経路が高コスト域を避ける / 外すと戻る / 保存 -> 復元 -> 連続実行一致)。

## 4. DetourCrowd: Off-Mesh Link の渡りを dtCrowd の外へ出す (M82h、NavMeshLink)

- 場所: `DetourCrowd\Source\DetourCrowd.cpp` (`dtCrowd::update` 末尾の「Update agents using off-mesh connection」ループ、`getAgentAnimation`)、
  `DetourCrowd\Include\DetourCrowd.h` (`getAgentAnimation` の宣言)
- 内容: 元のループは OFFMESH の Agent の位置を `m_agentAnims` の時刻で補間し、`tmax` を超えたら歩行へ戻す。これを、
  OFFMESH の間は速度を 0 にするだけ (位置も状態も触らない) に差し替えた。OFFMESH に入る処理 (`moveOverOffmeshConnection` で
  corridor を出口へ進める) は元のまま。`getAgentAnimation(idx)` は、OFFMESH に入った update の startPos / endPos / polyRef を
  読み取り専用で公開する。
- 理由: `m_agentAnims` は private で保存できず、dtCrowd の補間は tick をまたぐ。渡りの状態 (フェーズ・経過 tick・入口・出口) を
  NavSystem の Nav 節 (スナップショット対象) に持ち、Linear / Jump / Manual の動きを自前で決める。
- 影響: Link を使わない Crowd の結果は変わらない (このループは OFFMESH の Agent にしか効かない)。
- 確認方法: `NavAgentSelfTest` の Link 項目 (3 種の渡り、渡りの途中で撮った状態の復元 -> 連続実行一致、毎 tick の 撮影 -> 復元 -> 再撮影)。

## 5. DetourCrowd: 終点の手前の減速を切るフラグ `DT_CROWD_NO_AUTO_BRAKING` (M84d、autoBraking)

- 場所: `DetourCrowd\Include\DetourCrowd.h` (`UpdateFlags` に `DT_CROWD_NO_AUTO_BRAKING = 32`)、
  `DetourCrowd\Source\DetourCrowd.cpp` (`dtCrowd::update` の「Calculate steering」の `speedScale`)
- 内容: フラグが立った Agent は `speedScale` を 1 にする (半径の 2 倍の距離からの減速をしない)。
- 理由: Unity の `NavMeshAgent.autoBraking = false` (巡回で経由点を速度を落とさず通る) に当たる設定が dtCrowd に無い。
- 影響: フラグを立てない Agent の結果は変わらない。
- 確認方法: `NavAgentSelfTest` 13b (終点の手前 0.3 m の望む速度: 既定 2.09 m/s、フラグあり 3.50 m/s)。

## 6. DetourCrowd: 回避の優先度 `avoidancePriority` で避ける量を重み付けする (M84d)

- 場所: `DetourCrowd\Include\DetourCrowd.h` (`dtCrowdAgentParams::avoidancePriority`)、
  `DetourCrowd\Source\DetourCrowd.cpp` (`avoidanceShare`、分離・回避の円・押し戻しの 3 か所)、
  `DetourCrowd\Include\DetourObstacleAvoidance.h` / `Source\DetourObstacleAvoidance.cpp` (`dtObstacleCircle::share`、`addCircle` の引数、`processSample`)
- 内容: Agent の組 (自分 A、近傍 B) ごとに分担 `share = (pA+1) / ((pA+1)+(pB+1))` を求める (優先度は 0..99、小さいほど重要)。
  分離の重みに `2 * share` を掛け、回避の円との衝突までの時間を `2 * share` で割り、押し戻しの量を半分ずつから `share` の比にする。
  Unity は「優先度の高い側は低い側を無視する」だが、両方が分担に応じて避ける重み付けを採った (2026-10-05 ユーザー回答)。
- 理由: dtCrowd には Agent の優先度が無い。
- 影響: 同じ優先度どうしは `share` が厳密に 0.5 (整数の x / 2x) で、掛ける係数がちょうど 1 になるので、元の dtCrowd とビット一致する。
  `dtCrowdAgentParams` は NavSystem が `memset` してから埋めるので、優先度を使わない呼び出しは全員 0 = 元の挙動。
  crowd の保存 (`NavSaveCrowd` / `NavLoadCrowd`) にこのフィールドを足した。
- 確認方法: `NavAgentSelfTest` 13c (A=0 / B=99 の正面すれ違いで横のずれ A 0.049 m / B 1.138 m、同じ優先度は 0.693 m ずつ。
  50/50 と 0/0 の軌跡がビット一致)。`NavDeterminismSelfTest` の軌跡のハッシュ (`crowd.*.tick*`) は不変で、保存バイト列 (`capture.*.crowd`) だけが変わった。

## パッチを当てなかったもの (後続サブへの注意)

- `dtCrowd::m_agentAnims` は保存していない。Link の渡りは NavSystem が持ち (パッチ 4)、dtCrowd のアニメは使わない。
- `dtNavMeshQuery::findRandomPoint` 系は使っていない (`frand` は引数なしの関数ポインタでコンテキストを渡せない)。
  ランダムな点は `NavSystem::QueryRandomPoint` が World の Pcg32 から円内の点を選び、`findNearestPoly` で吸着して作る。
- `DebugUtils\Source\RecastDump.cpp` は `FILE` を使うのでビルド対象に入れていない (`build\Engine.vcxproj`)。
- Recast / Detour に `rand()` / `time` 系の呼び出しは無い (grep で確認、2026-10-03)。乱数は `frand` 引数だけ。
