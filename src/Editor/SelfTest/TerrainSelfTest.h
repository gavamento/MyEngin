#pragma once

namespace mye {

// 地形のチャンク分割 / メッシュ生成 / 視錐台カリング (M58b) のヘッドレス回帰テスト。
// GPU もウィンドウも要らない — 分割もメッシュも純関数で、MeshLibrary は
// GraphicsDevice 未 Init なら CPU 側 (AABB / positions / indices) だけを登録するため。
//
// このテストが守っている不変量:
//   - チャンクは地形のタイルを**過不足なく 1 回ずつ**覆う (穴も重なりも無い)
//   - チャンク AABB が生成メッシュを**必ず含む** (= カリングが可視物を落とさない根拠)
//   - 隣接チャンクが共有する縁の頂点は**位置も法線もビット一致** (継ぎ目が出ない根拠)
bool RunTerrainSelfTest();

} // namespace mye
