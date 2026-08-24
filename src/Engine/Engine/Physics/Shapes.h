#pragma once
// 衝突形状ライブラリ (M28a)。PhysicsSystem (ソリッド解決) / CollisionSystem (トリガー) /
// RaycastWorld (レイ) が共有する唯一の形状判定実装。
// 決定論契約 (spec 11.2): 全て scalar float 演算 (XMVECTOR SIMD 禁止)、反復は固定回数
// (収束による早期終了なし)、分岐は入力のみに依存する決定論的なもの。
// 互換: 無回転 (単位クォータニオン / 単位基底) の sphere/box は M20 の判定コードを
// そのまま通す fast-path を持つ — 無回転コライダーのみの既存シーンはビット同一挙動。

#include <cstdint>

#include <DirectXMath.h>

namespace mye {

struct ColliderComponent;

// ワールド空間の形状ポーズ。回転は正規直交基底 3 ベクトル (bx/by/bz = ローカル X/Y/Z 軸の
// ワールド方向)。スケールは radius / half extents / halfSeg に折り込み済み。
// 非一様スケール × 回転のシアーは無視する近似 (基底正規化 — M20 までの流儀を踏襲)。
struct ShapePose {
    int32_t shape = 0;        // 0=sphere 1=box(OBB) 2=capsule(ローカル Y 軸)
                              // 3=triangle mesh (M41) 4=terrain heightfield (M59i)
                              // 5=convex hull (M60f、**動的剛体で使える唯一のメッシュ由来形状**)
    int32_t identityRot = 1;  // 基底が単位 (無回転) — M20 互換 fast-path 用
    float px = 0, py = 0, pz = 0;
    float bx[3] = { 1, 0, 0 };
    float by[3] = { 0, 1, 0 };
    float bz[3] = { 0, 0, 1 };
    float radius = 0;              // sphere / capsule (ワールドスケール適用済み)
    float hx = 0, hy = 0, hz = 0;  // box half extents (ワールドスケール適用済み)
    float halfSeg = 0;             // capsule 線分半長 (ワールドスケール適用済み)
    // ---- 静的メッシュ (M41、shape=3。静的/kinematic 専用) ----
    // 実体は呼び出し側が meshcol::Resolve(col.meshAsset) で注入する (POD 維持のポインタ参照)。
    // null のままの shape=3 は全判定が「衝突なし」に落ちる (安全なフォールバック)
    // ---- 地形ハイトフィールド (M59i、shape=4。静的/kinematic 専用) ----
    // shape=3 と同じスロットを使う**タグ付き共用体** — 判別は shape 値。
    // 実体は terraincol::Resolve(col.meshAsset) で注入する (TerrainCollisionData*)。
    // 三角形の集まりという一点でメッシュと同じ扱いができるので、衝突・マニフォールド・
    // 最近点の本体は共有し、「候補の集め方」と「番号→三角形」だけを差し替えてある
    // ---- 凸包 (M60f、shape=5。動的剛体でも使える) ----
    // ここも同じスロットの**タグ付き共用体**。実体は convexcol::Resolve(col.meshAsset)。
    // メッシュ (3) と違って閉じた凸体なので SAT で貫通量が定義でき、動的剛体同士でも解ける
    const void* meshData = nullptr; // shape=3: MeshColliderData* / shape=4: TerrainCollisionData*
                                    // shape=5: ConvexHullData*
    float sx = 1, sy = 1, sz = 1;   // メッシュ/地形のローカル→ワールドスケール (基底とは別持ち)
};

namespace shapes {

// ---- 衝突レイヤー判定 (M36a、純関数) ----
// クエリマスクがレイヤー layer のコライダーをヒット対象にするか
inline bool LayerHit(uint32_t queryMask, int32_t layer)
{
    return ((queryMask >> (static_cast<uint32_t>(layer) & 31u)) & 1u) != 0u;
}
// ペア判定は双方向: 互いの mask が相手の layer を許可して初めて衝突する (Unity のマトリクス
// と違い per-collider mask。対称にするには両側の mask を揃える)
inline bool CanCollide(int32_t layerA, uint32_t maskA, int32_t layerB, uint32_t maskB)
{
    return LayerHit(maskA, layerB) && LayerHit(maskB, layerA);
}

// LocalTransform の成分から (物理ソルバ用 — TransformSystem 前でも使える)
ShapePose MakePose(const ColliderComponent& col, const DirectX::XMFLOAT3& position,
                   const DirectX::XMFLOAT4& rotation, const DirectX::XMFLOAT3& scale);

// WorldMatrix から (トリガー / レイ / ギズモ用)。行ベクトル長 = スケール、正規化行 = 基底
ShapePose MakePoseFromMatrix(const ColliderComponent& col, const DirectX::XMFLOAT4X4& wm);

// a と b の貫通を判定。ヒットで true、normal (nx,ny,nz) は b→a 方向 (a を +normal へ
// 押し出す)、depth は貫通量。全 6 形状ペア (s-s / s-b / s-c / b-b / b-c / c-c) 対応。
bool Collide(const ShapePose& a, const ShapePose& b, float& nx, float& ny, float& nz,
             float& depth);

// トリガー用の重なり判定 (Collide と同一判定で出力を捨てる)
bool Overlap(const ShapePose& a, const ShapePose& b);

// ---- 接触マニフォールド (M28b、回転剛体ソルバ用) ----
// box 系の面接触は最大 4 点 (参照面クリッピング)、capsule の側面接触は最大 2 点、
// 球系と辺-辺接触は 1 点。法線は Collide と同じ b→a 方向で全接触点共通。
// 接触点はワールド座標 (トルク計算の作用点)、depth は点毎の貫通量。
struct Contact {
    float px = 0, py = 0, pz = 0;
    float depth = 0;
};
struct Manifold {
    float nx = 0, ny = 1, nz = 0; // b→a (a を押し出す方向)
    int count = 0;
    Contact pts[4];
};
bool CollideManifold(const ShapePose& a, const ShapePose& b, Manifold& out);

// レイ交差。dir (dx,dy,dz) は正規化済みであること。ヒットで true、outT は距離、
// (nx,ny,nz) はヒット面の外向き法線。
bool Raycast(const ShapePose& s, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, float& outT, float& nx, float& ny, float& nz);

// 点から形状表面までの距離 (内部なら 0)。SphereCast の保守的前進などに使う (M28c)
float DistanceToShape(const ShapePose& s, float px, float py, float pz);

// 形状表面上 (内部の点はその点自身) の最近点 (M28c)
void ClosestPointOnShape(const ShapePose& s, float px, float py, float pz, float& qx, float& qy,
                         float& qz);

// 形状を包む保守的ワールド AABB (M28d、ブロードフェーズ用)
void ComputeAabb(const ShapePose& s, float& minX, float& minY, float& minZ, float& maxX,
                 float& maxY, float& maxZ);

} // namespace shapes
} // namespace mye
