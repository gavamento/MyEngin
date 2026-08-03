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

// ---- M46g: RT 影 (太陽コーンサンプル) ----

// 太陽の見かけ半径 (度)。実測値 (視直径 0.53°) の半分。
// 大きいほど半影が広がるが、1spp のノイズも同じだけ増える
constexpr float kRtShadowSunAngleDeg = 0.265f;

// G-Buffer の可視点から二次光線を撃つときの原点オフセット (自己交差回避)。
// ワールド座標が半精度 (R16G16B16A16_FLOAT = 相対誤差 ~5e-4) なので、定数だけでは
// 遠景でアクネが出る。実効値 = max(絶対下限, 相対係数 * 距離)。
// **影 (M46g) と反射 (M46h) で共有** — 面の自己交差はレイの種類によらないため
constexpr float kRtSurfaceEpsMin = 1e-3f;
constexpr float kRtSurfaceEpsRel = 1e-3f;

// 影の空間フィルタ (SVGF のスカラー軽量版) の反復回数。刻み幅は 1 から倍化。
// GI (3 回) より弱いのは、太陽コーンが狭く 1spp のノイズが半影に限られるため
constexpr int kRtShadowFilterIterations = 1;

// ---- M46h: RT 反射 (GGX VNDF 1 本 + IBL フォールバック) ----

// これを超える roughness ではレイを撃たず IBL スペキュラへ完全に委ねる。
// GGX ローブが広がるほど 1spp の分散が跳ね上がる一方、プリフィルタ IBL との
// 見た目の差は縮むので、コストを払う意味が無くなる境界
constexpr float kRtReflMaxRoughness = 0.6f;

// IBL へのフェード開始 roughness。ここから kRtReflMaxRoughness まで smoothstep で
// 混ぜる (段差を作らないため。両者は同じ次元の入射放射輝度なので混色して良い)
constexpr float kRtReflFadeStart = 0.4f;

// 反射のテンポラル履歴長の上限。GI (32) より短いのは、鏡面ほど反射像が
// カメラ運動で大きく動くため — 長く積むとラグ (引きずり) として見える。
// 実効値は HLSL の MYE_RT_TEMPORAL_MAX_HISTORY とのより小さい方
constexpr float kRtReflMaxHistory = 8.0f;

// 反射の A-Trous 反復回数。GI (3 回) より少ないのは、反射像は「本物のディテール」を
// 持つので広く均すと像そのものが溶けるため
constexpr int kRtReflAtrousIterations = 2;

// 反射の輝度エッジ停止 σ。GI (4.0) より厳しくして反射像のディテールを残す
// (分散が高い = ノイズのときだけ均し、収束したら像を保つ)
constexpr float kRtReflSigmaLuma = 1.0f;

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
