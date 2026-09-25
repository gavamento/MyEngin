//====================================================================================
//                          FractureVoxel.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア: 開いたメッシュのボクセル化+surface nets
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Physics/FractureMesh.h"

namespace mye {

// ---- 破壊分割コア: 開いたメッシュの経路 (M80d) ----
// spec §4.1 焼き1「openMeshMode==1」。CheckClosedMesh (FractureMesh.h) が閉じていないと
// 判定した入力を、解像度可変の占有格子へ焼いてから surface nets でメッシュ化することで、
// 必ず閉じた外向きメッシュへ変換する (境界は空きセルで囲むので閉じないことがない)。
// Modal/Voxelizer.h (32^3 固定、Deep-Modal と学習/実行時で契約を共有) とは別実装であり、
// 触らない・呼ばない。TriBoxOverlap 相当もこのファイル内に独立して持つ

inline constexpr int32_t kFractureVoxelMinResolution = 16;
inline constexpr int32_t kFractureVoxelMaxResolution = 256;

struct FractureVoxelizeResult {
    bool success = false;
    std::string failReason; // success == false のときだけ意味を持つ
    FractureMesh mesh;      // 閉じた外向きメッシュ (成功時のみ有効。法線は面から、UV は箱投影)
    int32_t resolutionUsed = 0; // [16,256] へクランプした後の実際の解像度
};

// resolution は [kFractureVoxelMinResolution, kFractureVoxelMaxResolution] へクランプする。
// 入力メッシュ (開いていてよい。閉じている必要はない) の AABB の最長辺を resolution セルに
// 割った立方セルの格子へ焼き、外周に 1 セル以上の空きを置いた上で、三角形に触れたセルを
// 占有とし、外側から塗りつぶして到達しない空洞を内部として埋める。曖昧な配置 (2x2 の対角占有)
// を事前に解消してから、8隅の占有が混在するキューブごとに1頂点を置く surface nets で
// 占有境界をメッシュ化する (占有領域の境界は必ず閉じる)。出力が実際に閉じているかを関数内で
// CheckClosedMesh により確認し、通らなければ失敗を返す。決定論: 同じ入力で同じバイト列。並列化しない
bool VoxelizeMeshForFracture(const FractureMesh& source, int32_t resolution, FractureVoxelizeResult& out);

// SelfTest 専用: 三角形メッシュの焼き込みを経由せず、占有格子を直接与えて曖昧な配置の解消 +
// surface nets のパイプラインだけを検証する。occ は index = x + nx*(y + ny*z)、
// 外周1セル以上を非占有にしておくこと (VoxelizeMeshForFracture と同じ不変量)。
// h はセルサイズ (頂点位置の間隔に使うだけで、原点は常に (0,0,0))
struct RawOccupancyGrid {
    int32_t nx = 0, ny = 0, nz = 0;
    std::vector<uint8_t> occ;
};
bool BuildSurfaceNetsFromOccupancy(const RawOccupancyGrid& grid, float h, FractureMesh& outMesh);

} // namespace mye
