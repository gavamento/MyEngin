//====================================================================================
//                          Voxelizer.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          32^3 ボクセル化 (学習とランタイムが共有する唯一の実装)
//====================================================================================
#pragma once
// Deep-Modal の入力ボクセルを作る (spec §4.1「ボクセル化」)。学習 (Python) は
// `Editor.exe --modal-voxelize` 経由でこのファイルを呼ぶだけで、Python 側は
// 自前のボクセライザを持たない (学習とランタイムの規則一致を「同じ関数」で担保する)。
#include <array>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Modal/ModalTypes.h"

namespace mye {
namespace modal {

// ボクセルグリッドの座標系 (メッシュローカル空間 → voxel 空間への写像)。
// L = メッシュ AABB の最長辺、h = L/28、origin = AABB中心 - 16.5h (spec §4.1、M76b round 2)。
// これにより AABB は voxel 座標 2.5..30.5 に収まり (軸平行面が voxel 中心を通る)、
// **AABB 中心も voxel 16 の中心 (16.5) に乗る** — 中心対称な薄い特徴が voxel 境界を
// またいで隣接 2 行に割れることが構造的に無くなる (奇数 29 で割って center-16h にすると
// 中心が必ず境界に乗ってしまうことが sub-02 round 1 の実測で判明し、偶数 28 + 半 voxel
// オフセットに直した)。単位立方体はちょうど 29^3 = 24389 voxel を占める
struct VoxelFrame {
    float origin[3] = {};
    float voxelSize = 0.0f;
    float aabbMin[3] = {};
    float aabbMax[3] = {};
    float longestEdge = 0.0f;
};

// index = x + 32*(y + 32*z) (AcousticGrid の CellIndex と同じ x 最内)
constexpr int VoxelIndexOf(int x, int y, int z)
{
    return x + kModalVoxelN * (y + kModalVoxelN * z);
}

// 特徴マップの cell index (16^3)。cell = voxel >> 1 なので 2 voxel が 1 cell に畳まれる
constexpr int CellIndexOf(int cx, int cy, int cz)
{
    return cx + kModalMapN * (cy + kModalMapN * cz);
}

struct VoxelGrid {
    VoxelFrame frame;
    std::array<uint8_t, static_cast<size_t>(kModalVoxelN) * kModalVoxelN * kModalVoxelN> occ{};
    uint32_t surfaceCount = 0;
    uint32_t interiorCount = 0;
};

// 三角形メッシュ (ローカル座標) を 32^3 ボクセルへ焼く。成功 = true。
// 縮退メッシュ (頂点 0 件・全頂点が同一点で AABB の最長辺が 0 等) は false を返す
bool VoxelizeMesh(const DirectX::XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m,
                  VoxelGrid& out);

// Akenine-Möller の三角形/箱オーバーラップ判定 (13 軸: 箱 3 軸 + 三角形面法線 1 軸 +
// 辺×箱軸の外積 9 軸)。boxCenter/boxHalfSize/tri はすべてボクセル化と同じローカル座標系。
// 退化三角形 (面積 0) も辺として扱われ、正しく重なりを検出する
bool TriBoxOverlap(const float boxCenter[3], const float boxHalfSize[3], const float tri[3][3]);

// pad リング (外周 1 層。VoxelizeMesh のマージンにより必ず非占有) から 6 近傍で
// 外部を塗り、到達しない非表面 voxel を内部として occ に立てる。明示スタック + 固定順
// (unordered を使わない) なので同入力 → 同出力。grid.interiorCount を更新する
void FloodFillInterior(VoxelGrid& grid);

// ローカル座標点 → cell index (0..4095)。範囲外は最近傍 voxel へクランプしてから求める
uint16_t LocalPointToCell(const VoxelFrame& frame, const float p[3]);

// 4096 cell それぞれについて、占有 voxel (surface か interior) を 1 個以上含む cell は
// 自身の index、含まない cell は最も近い有効 cell の index を cellSlot に書く
// (距離同値は index の小さい方。総当たりなので O(4096^2) だが焼き時 1 回だけ)
void BuildCellSlotTable(const VoxelGrid& grid, uint16_t cellSlot[4096]);

// .mvox のバイト列を作る/読む。**VoxelFrame/VoxelGrid を memcpy しない** —
// フィールド単位で書く (Material の暗黙パディングの罠と同じ理由)。
// ヘッダの実バイト数は kVoxHeaderBytes = 72 B (18 フィールド × 4 B、spec §4.2 が正本)
inline constexpr uint32_t kVoxMagic = 0x584F564Du; // リトルエンディアンで読むと "MVOX"
inline constexpr uint32_t kVoxVersion = 1;
inline constexpr size_t kVoxHeaderBytes = 72;
inline constexpr size_t kVoxOccBytes = static_cast<size_t>(kModalVoxelN) * kModalVoxelN * kModalVoxelN;

void SerializeVox(const VoxelGrid& grid, std::vector<uint8_t>& bytes);
// magic/version 不一致やサイズ不足は false (grid は変更しない)
bool DeserializeVox(const std::vector<uint8_t>& bytes, VoxelGrid& grid);

} // namespace modal
} // namespace mye
