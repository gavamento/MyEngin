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
    int32_t identityRot = 1;  // 基底が単位 (無回転) — M20 互換 fast-path 用
    float px = 0, py = 0, pz = 0;
    float bx[3] = { 1, 0, 0 };
    float by[3] = { 0, 1, 0 };
    float bz[3] = { 0, 0, 1 };
    float radius = 0;              // sphere / capsule (ワールドスケール適用済み)
    float hx = 0, hy = 0, hz = 0;  // box half extents (ワールドスケール適用済み)
    float halfSeg = 0;             // capsule 線分半長 (ワールドスケール適用済み)
};

namespace shapes {

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

} // namespace shapes
} // namespace mye
