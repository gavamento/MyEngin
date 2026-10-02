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

## パッチを当てなかったもの (後続サブへの注意)

- `dtCrowd::m_agentAnims` (Off-Mesh Link を `dtCrowd` 自身が渡るときのアニメ状態) は private で、保存していない。
  M82 は Link の渡りをアプリ側 (sub-07) で行うので `dtCrowd` のアニメは使わない前提。使うなら friend 追加が要る。
- `dtNavMeshQuery::findRandomPoint` 系の `frand` は引数なしの関数ポインタ (`float (*)()`)。コンテキストを渡せないので、
  呼び出し中だけ有効な静的ポインタ (World の Pcg32) 経由にする。
- `DebugUtils\Source\RecastDump.cpp` は `FILE` を使うのでビルド対象に入れていない (`build\Engine.vcxproj`)。
- Recast / Detour に `rand()` / `time` 系の呼び出しは無い (grep で確認、2026-10-03)。乱数は `frand` 引数だけ。
