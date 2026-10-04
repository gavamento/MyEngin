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

// セルサイズの下限 (m)。Surface.cellSize の最小値と同じ。これより細かくするとタイル数とベイク時間が跳ね上がる
inline constexpr float kNavMinCellSize = 0.05f;

// ベイクで実際に使うセルサイズとセルの高さ。autoCellSize が false なら cellSize / cellHeight そのまま。
// true なら min(agentRadius / 2, climb / (2 * tan(maxSlopeDeg))) を kNavMinCellSize まで切り上げる。
// climb は Recast が使う段差 (floor(maxClimb / cellHeight) * cellHeight)。
// セルの高さも自動: ch = min(cs / 2, maxClimb / 6) (段差のボクセル量子化の誤差を 1/6 以下に抑える)。
// Recast は隣のセルとの高さの差が climb を超える面を歩けないと捨てるので、
// 傾斜 maxSlopeDeg をセルごとに表現できる細かさがこの式 (tan 傾斜 * 2 * cs <= climb)
struct NavCellSize {
    float cellSize = 0.0f;
    float cellHeight = 0.0f;
    bool clampedToMinimum = false; // 下限に当たった = 実効の坂上限が maxSlopeDeg を下回る
};
NavCellSize NavResolveCellSize(const NavMeshSurfaceComponent& surface);

// 実効の坂の上限 (度) = atan(climb / (2 * cellSize))。maxSlopeDeg がこれを超えても、その坂は経路にならない
float NavEffectiveSlopeLimitDeg(const NavMeshSurfaceComponent& surface, float cellSize);

// インスペクタの警告の判定 (ImGui なしで SelfTest から検査するため分けてある)
bool NavSurfaceSlopeUnreachable(const NavMeshSurfaceComponent& surface); // maxSlopeDeg が実効の上限を超える
// worldScaleY: CC を持つエンティティのワールドの Y スケール (実効の段差 = stepOffset x |scale.y|)
bool NavAgentStepBelowClimb(const CharacterControllerComponent& cc, float worldScaleY,
                            const NavMeshSurfaceComponent& surface);

// Surface コンポーネントとワールド行列から、ベイク設定 (範囲はワールド AABB) を作る
NavBakeConfig NavMakeBakeConfig(const NavMeshSurfaceComponent& surface, const DirectX::XMFLOAT4X4& worldMatrix);

} // namespace mye
