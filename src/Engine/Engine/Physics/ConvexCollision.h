#pragma once
#include <cstdint>

#include "Engine/Engine/Physics/ConvexHull.h"
#include "Engine/Engine/Physics/Shapes.h"

namespace mye {
namespace convex {

// ---- 凸体の衝突 (M60f) ----
// 「凸包 (shape=5) / 箱 / 三角形」をワールド空間の同じ形へ均してから、**1 本の SAT** に
// 通す。箱を専用経路に残さず凸体として扱えるのは、box-box の既存経路 (M28b) を一切
// 触らずに済むから — 凸包が絡むペアだけがこちらへ来るので、既存シーンのビットは動かない。
//
// 球とカプセルだけは SAT に載せない。丸い形は「頂点と球心を結ぶ軸」が分離軸になり得て
// 面法線と稜線外積の列挙では取りこぼすため、最近点で解く (カプセルは芯の線分に沿った
// 固定 32 回の黄金分割 = capsule-box が既に採っている手をそのまま踏襲する)。
//
// 決定論: 軸の列挙順は「a の面 → b の面 → 稜線ペア (a 昇順 × b 昇順)」で固定、
// 最良軸の更新は strict > (同値は先勝ち)、反復は全て固定回数。
struct Body {
    // 面と稜線の index 表だけを借りる (**頂点座標は使わない** — ワールド頂点は下の配列)。
    // 箱と三角形は専用の位相テーブルを指す
    const ConvexHullData* topo = nullptr;
    int32_t vertCount = 0;
    float vx[kConvexMaxVerts] = {};
    float vy[kConvexMaxVerts] = {};
    float vz[kConvexMaxVerts] = {};
    // 面の支持平面 (ワールド)。**非一様スケールでは法線が保存量でない**ので、
    // 保存済みのローカル法線を回すのではなくワールド頂点から Newell 法で組み直す
    int32_t faceCount = 0;
    float fnx[kConvexMaxFaces] = {};
    float fny[kConvexMaxFaces] = {};
    float fnz[kConvexMaxFaces] = {};
    float fd[kConvexMaxFaces] = {};
};

// ShapePose (shape=5) から。false = 凸包の実体が無い / 無効 (呼び出し側は衝突なしに落とす)
bool BuildFromPose(const ShapePose& p, Body& out);
// ShapePose (shape=1) から 8 頂点の箱として
void BuildFromBox(const ShapePose& p, Body& out);
// ワールド三角形から (表裏 2 面の潰れた凸体)。スープ (shape=3/4) との衝突に使う
void BuildFromTriangle(float ax, float ay, float az, float bx, float by, float bz, float cx,
                       float cy, float cz, Body& out);

// 凸 × 凸。normal は b→a (a を +normal へ押し出す) = shapes::Collide と同じ規約
bool Collide(const Body& a, const Body& b, float& nx, float& ny, float& nz, float& depth);
bool CollideManifold(const Body& a, const Body& b, shapes::Manifold& out);

// 表面までの符号付き距離 (内部は負) と表面最近点 q、q での外向き方向 (on*)
float SignedDistance(const Body& b, float px, float py, float pz, float& qx, float& qy, float& qz,
                     float& onx, float& ony, float& onz);

// 凸 × 球。normal は **凸→球** (球を押し出す向き)、q は凸体表面の接触点
bool SphereContact(const Body& b, float sx, float sy, float sz, float r, float& nx, float& ny,
                   float& nz, float& depth, float& qx, float& qy, float& qz);

// 凸 × カプセル。normal は **凸→カプセル**
bool CapsuleContact(const Body& b, const ShapePose& cap, float& nx, float& ny, float& nz,
                    float& depth);
bool CapsuleManifold(const Body& b, const ShapePose& cap, shapes::Manifold& out);

// レイ交差 (全面の平面でスラブクリップ)。dir は正規化済みであること
bool Raycast(const Body& b, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, float& outT, float& nx, float& ny, float& nz);

} // namespace convex
} // namespace mye
