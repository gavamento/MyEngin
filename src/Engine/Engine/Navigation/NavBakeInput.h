//====================================================================================
//                          NavBakeInput.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh のベイク入力の収集 (World -> 三角形)
//====================================================================================
#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Navigation/NavTileCacheSupport.h"

namespace mye {

class World;

// ベイクへ渡す三角形の集まり。頂点は三角形ごとに持つ (共有しない)
struct NavTriangleSoup {
    std::vector<float> verts; // xyz * 頂点数
    std::vector<int> tris;    // 3 * 三角形数

    int TriangleCount() const { return static_cast<int>(tris.size() / 3); }
    void AddTriangle(const float* a, const float* b, const float* c);
    NavTriangleInput View() const;
};

// 球・カプセルの分割数 (正八面体の 1 辺あたり)。値を変えるとベイク結果が変わるので kNavBakeVersion も上げる
inline constexpr int kNavSphereSubdivisions = 4;

// World から、boundsMin..boundsMax (ワールド AABB) と重なる静的コライダーの三角形を集める。
// 選別は AcousticField::BakeOccupancy と同じ (Collider + WorldMatrix、Rigidbody / CharacterController 持ちと
// トリガーを除く、collectLayerMask、非アクティブ除外)。順序は entity.index 昇順で、同じ World からは同じ並びになる。
// 範囲はタイルの AABB でもよい (将来の実行時再ベイクの入口)。メインスレッド専用 (meshcol / terraincol / convexcol を引く)
void NavCollectTriangles(World& world, const float* boundsMin, const float* boundsMax, uint32_t collectLayerMask,
                         NavTriangleSoup& out);

// Surface コンポーネントとワールド行列から、ベイク設定 (範囲はワールド AABB) を作る
NavBakeConfig NavMakeBakeConfig(const NavMeshSurfaceComponent& surface, const DirectX::XMFLOAT4X4& worldMatrix);

} // namespace mye
