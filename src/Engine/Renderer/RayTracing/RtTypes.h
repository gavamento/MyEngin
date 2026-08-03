#pragma once
#include <cstdint>

#include <DirectXMath.h>

// ハイブリッド・パストレーシング (M46) の GPU データレイアウト。
// HLSL 側 assets/shaders/rt_common.hlsli の同名構造体とバイト単位で一致させること
// (変更時は両方更新)。全て 16 バイト境界に揃える。
namespace mye {

// トラバーサルスタックの深さ。TLAS/BLAS で同じ値を使う。
// HLSL の MYE_RT_STACK_DEPTH と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtStackDepth = 32;

// 1 レイあたりのノード訪問上限 (TDR 保険)。超えたら miss 扱いで打ち切る。
// HLSL の MYE_RT_MAX_VISIT と一致検査される (規則 9)
constexpr int kRtMaxVisit = 512;

// TLAS の葉あたりインスタンス数 (BLAS 側は MeshColliderLibrary の kLeafTris=8 に従う)
constexpr int kRtTlasLeafSize = 2;

// ---- M46d: テンポラル蓄積 ----

// 履歴長の上限。移動平均の重み下限 = 1/この値 (32 → 約 3% で追従が止まらない)。
// HLSL の MYE_RT_TEMPORAL_MAX_HISTORY と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtTemporalMaxHistory = 32;

// 再投影の妥当性しきい値。CB 経由で HLSL へ渡す (= C++ 側が唯一の出所)。
// 深度はカメラ距離の相対差、法線は cos。どちらかを外れたら履歴を捨てて 1spp に戻す
constexpr float kRtTemporalDepthThreshold = 0.05f;
constexpr float kRtTemporalNormalThreshold = 0.9f;

// ---- M46e: SVGF 空間フィルタ (分散推定 + エッジ停止 A-Trous) ----

// A-Trous のカーネル半径 (5x5 = B3 スプライン)。分散の空間フォールバックも同じ半径。
// HLSL の MYE_RT_ATROUS_RADIUS と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtAtrousRadius = 2;

// A-Trous の反復回数。刻み幅を 1,2,4 と倍化しながら掛ける (GI の推奨は 3)
constexpr int kRtAtrousIterations = 3;

// テンポラルモーメントから分散を取るのに必要な履歴長。
// これ未満は 5x5 の空間推定に落とす (蓄積が浅いと μ,μ² が信用できない)
constexpr float kRtVarianceHistoryMin = 4.0f;

// エッジ停止関数のパラメータ (C++ が唯一の出所 → CB で HLSL へ渡す)。
//   depth  = タップ 1 画素あたりに許す相対深度差 (真の深度勾配を持たないための近似)
//   normal = cos の指数 (大きいほど法線の違いに厳しい)
//   luma   = 推定標準偏差の何倍までを「ノイズ」として均すか
constexpr float kRtAtrousSigmaDepth = 0.02f;
constexpr float kRtAtrousSigmaNormal = 64.0f;
constexpr float kRtAtrousSigmaLuma = 4.0f;

// BVH ノード (BLAS / TLAS 共通)。
//   内部ノード: left/right = 子ノードの絶対 index (どちらも >= 0)
//   葉:         left = -(start + 1) で負、right = 個数
//               BLAS の葉は三角形の連続範囲、TLAS の葉はインスタンスの連続範囲を指す
struct RtBvhNode {
    DirectX::XMFLOAT3 aabbMin = { 0, 0, 0 };
    int32_t left = -1;
    DirectX::XMFLOAT3 aabbMax = { 0, 0, 0 };
    int32_t right = 0;
};
static_assert(sizeof(RtBvhNode) == 32, "HLSL RtBvhNode と一致させること");

// 三角形 (ローカル空間、Möller-Trumbore 用に辺を前計算済み)
struct RtTri {
    DirectX::XMFLOAT3 p0 = { 0, 0, 0 };
    float pad0 = 0.0f;
    DirectX::XMFLOAT3 e1 = { 0, 0, 0 }; // p1 - p0
    float pad1 = 0.0f;
    DirectX::XMFLOAT3 e2 = { 0, 0, 0 }; // p2 - p0
    float pad2 = 0.0f;
};
static_assert(sizeof(RtTri) == 48, "HLSL RtTri と一致させること");

// 三角形の頂点属性 (最近ヒットが確定してからしか読まないので別バッファに分ける)
struct RtTriAttr {
    DirectX::XMFLOAT4 n0u0 = { 0, 1, 0, 0 }; // xyz = 法線 0, w = u0
    DirectX::XMFLOAT4 n1v0 = { 0, 1, 0, 0 }; // xyz = 法線 1, w = v0
    DirectX::XMFLOAT4 n2u1 = { 0, 1, 0, 0 }; // xyz = 法線 2, w = u1
    DirectX::XMFLOAT4 uvRest = { 0, 0, 0, 0 }; // x = v1, y = u2, z = v2, w = 未使用
};
static_assert(sizeof(RtTriAttr) == 64, "HLSL RtTriAttr と一致させること");

// インスタンス。行列は worldToLocal のみ持つ (行ベクトル規約の 4x3)。
// レイをローカルへ移すとき方向ベクトルを正規化しないので、求まる t は
// ワールド空間のパラメータのまま = インスタンス間で t を直接比較できる。
// 法線をワールドへ戻すときは mul(float3x3(invRow0..2), nLocal) (= nLocal * transpose)
struct RtInstance {
    DirectX::XMFLOAT4 invRow0 = { 1, 0, 0, 0 }; // xyz = worldToLocal の行 0
    DirectX::XMFLOAT4 invRow1 = { 0, 1, 0, 0 };
    DirectX::XMFLOAT4 invRow2 = { 0, 0, 1, 0 };
    DirectX::XMFLOAT4 invRow3 = { 0, 0, 0, 0 }; // xyz = 平行移動成分
    int32_t blasRoot = 0;      // 連結ノード配列における BLAS のルート index
    int32_t materialIndex = 0; // マテリアル配列の index
    int32_t pad0 = 0;
    int32_t pad1 = 0;
};
static_assert(sizeof(RtInstance) == 80, "HLSL RtInstance と一致させること");

// ヒット点のシェーディングに使うマテリアル定数。
// baseColor はリニア (SrgbToLinear 済み)。emissive は M46i まで 0
struct RtMaterial {
    DirectX::XMFLOAT3 baseColor = { 1, 1, 1 };
    float metallic = 0.0f;
    DirectX::XMFLOAT3 emissive = { 0, 0, 0 };
    float roughness = 0.5f;
};
static_assert(sizeof(RtMaterial) == 32, "HLSL RtMaterial と一致させること");

} // namespace mye
