# Recast Navigation へ当てたパッチ (M82a)

ベース: v1.6.0 (`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`)。パッチは `MYE-PATCH(M82a)` というコメントで探せる。
`Debug` / `Release` / `Server.exe` のハッシュ不一致は出なかった (`NavDeterminismSelfTest`)。
下の 2 件は**ビット一致のためではなく、状態の保存・復元のため**のパッチ。

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

## パッチを当てなかったもの (後続サブへの注意)

- `dtCrowd::m_agentAnims` (Off-Mesh Link を `dtCrowd` 自身が渡るときのアニメ状態) は private で、保存していない。
  M82 は Link の渡りをアプリ側 (sub-07) で行うので `dtCrowd` のアニメは使わない前提。使うなら friend 追加が要る。
- `dtNavMeshQuery::findRandomPoint` 系の `frand` は引数なしの関数ポインタ (`float (*)()`)。コンテキストを渡せないので、
  呼び出し中だけ有効な静的ポインタ (World の Pcg32) 経由にする。
- `DebugUtils\Source\RecastDump.cpp` は `FILE` を使うのでビルド対象に入れていない (`build\Engine.vcxproj`)。
- Recast / Detour に `rand()` / `time` 系の呼び出しは無い (grep で確認、2026-10-03)。乱数は `frand` 引数だけ。
