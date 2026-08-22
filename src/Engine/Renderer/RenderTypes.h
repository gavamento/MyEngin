#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>

#include "Engine/Core/EntityID.h"

namespace mye {

// ---- sRGB → リニア変換 (M38a リニアパイプライン) ----
// authored な色 (マテリアル baseColor / ライト色 / スカイ / フォグ) は sRGB 認知色として
// 保存されている前提で、CB へ載せる直前に変換する。トーンマップ後の OETF (applyGamma)
// と対になり「作者が選んだ色 ≒ 画面の色」を保つ。render-only (sim/hash 非関与)。
inline float SrgbToLinear(float c)
{
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline DirectX::XMFLOAT3 SrgbToLinear(const DirectX::XMFLOAT3& c)
{
    return { SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z) };
}
inline DirectX::XMFLOAT4 SrgbToLinear(const DirectX::XMFLOAT4& c)
{
    return { SrgbToLinear(c.x), SrgbToLinear(c.y), SrgbToLinear(c.z), c.w }; // α はそのまま
}

// 「収集 → ソート → 提出」モデル (engine_spec.md 6.3)。
// 即時描画 API は提供しない — 将来のマルチスレッド化 / API 差し替えの余地を残す。

// ボーンパレット最大数 (M18、M45 で 64 → 128)。定数バッファは 128*64B = 8KB で D3D11 の
// 64KB 上限に十分収まる。Mixamo の標準ヒューマノイドが約 65 ジョイントで 64 を超えるため拡張。
// **HLSL 側の MYE_MAX_BONES (forward_skinned.hlsl / deferred_gbuffer_skinned.hlsl) と必ず一致
// させること** — 食い違うと定数バッファのサイズ不一致で描画が壊れる。
// tools\check_rules.ps1 の規則 9 が C++/HLSL 3 箇所の一致を静的に検査する。
constexpr int kMaxBones = 128;

// 自己発光強度を G-Buffer へ詰めるときの正規化上限 (M46i)。
// gbMaterial は R8G8B8A8_UNORM で b チャンネルが空いていたので、そこへ
// saturate(emissiveIntensity / kEmissiveMaxIntensity) を書き、ライトパスで逆変換する。
// **HLSL 側の MYE_EMISSIVE_MAX (common.hlsli) と必ず一致させること** — 食い違うと
// 発光の明るさが静かに定数倍ずれる。tools\check_rules.ps1 の規則 9 が一致を検査する。
// 8 は「1 = 白の拡散面と同じ明るさ」を基準に、屋内の面光源が飽和しない範囲として選んだ値
constexpr int kEmissiveMaxIntensity = 8;

// common.hlsli の EncodeEmissive / DecodeEmissive の CPU ミラー (PostFxMath.h と同じ方針)。
// selftest がこの 2 本で往復と飽和を検証し、HLSL 側との式の一致は目視 + 規則 9 で担保する。
// **0 はちょうど 0 に落ちる** — これが「発光を使わないマテリアルは M46i 以前と
// ビット単位で同じ絵になる」という受け入れ基準の根拠
inline float EncodeEmissive(float intensity)
{
    const float t = intensity / static_cast<float>(kEmissiveMaxIntensity);
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t); // HLSL saturate と同じ
}

inline float DecodeEmissive(float encoded)
{
    return encoded * static_cast<float>(kEmissiveMaxIntensity);
}

struct RenderItem {
    AssetID mesh = {};
    AssetID material = {};
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float viewZ = 0.0f; // ソート用 (カメラ空間深度)。RenderQueue::Sort が使用
    // スキニング (M18)。非 null = スキンメッシュ → パスがスキニングシェーダ + ボーン CB を使う。
    // 指す先は RenderSystem のフレームアリーナ (transpose 済みボーン行列、描画完了まで有効)。
    const DirectX::XMFLOAT4X4* bones = nullptr;
    int32_t boneCount = 0;
};

// ---- 局所ライトのシャドウアトラス (M54c) ----
// 4096^2 の深度テクスチャ 1 枚を正方タイルに割り、スポットは 1 枚 (透視 1 面)、
// 点光源は 6 枚 (M54d) を使う。タイル数は 4096^2 / 1024^2 = 16。
// **HLSL の MYE_MAX_SHADOW_TILES (common.hlsli) と必ず一致させること** —
// 定数バッファの配列長そのものなので、食い違うとレイアウト不一致として静かに壊れる。
// tools\check_rules.ps1 の規則 9 が一致を検査する。
// ★per-light パラメータを StructuredBuffer ではなく CB で渡しているのは、統合契約の
//   予約 2 が M54 に許した SRV スロットが t12 (アトラス本体) の 1 本きりだから
//   (計画本文の「StructuredBuffer で t7/t13」は予約表と食い違っており、予約表が正)。
//   16 枚 × 96 バイト = 1.5KB で、64KB の CB 上限には遠く届かない
constexpr int kMaxShadowTiles = 16;

// アトラスのタイル 1 枚。ShadowAtlas が描画に、光パスが CB 充填に使う (描画専用)。
// lightViewProj は**非転置** — ShadowAtlas が world と合成するため。CB へ載せる側が転置する
struct ShadowTile {
    DirectX::XMFLOAT4X4 lightViewProj = {}; // 行ベクトル規約 (world * lightViewProj)
    float uvScale[2] = { 0.0f, 0.0f };      // タイル UV [0,1] → アトラス UV の拡大率
    float uvOffset[2] = { 0.0f, 0.0f };
    int32_t pixelX = 0;     // アトラス内のピクセル矩形 (RSSetViewports 用)
    int32_t pixelY = 0;
    int32_t pixelSize = 0;
    float depthBias = 0.0f; // シェーダ側の定数バイアス (NDC 深度。ラスタライザ側と併用)
};

struct RenderView {
    DirectX::XMFLOAT4X4 view = {};
    DirectX::XMFLOAT4X4 proj = {};
    DirectX::XMFLOAT3 cameraPos = { 0, 0, 0 };
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    // ---- シャドウ (M17 単一 → M38d CSM)。RenderSystem がシャドウパス後に埋める。描画専用 ----
    DirectX::XMFLOAT4X4 lightViewProj[3] = {}; // 各カスケードの transpose(lightView*lightProj)
    float cascadeSplits[3] = { 0, 0, 0 };      // 各カスケードの far 境界 (view 深度、デバッグ用)
    int32_t cascadeCount = 0;                  // 0 = 影無効
    ID3D11ShaderResourceView* shadowSRV = nullptr; // シャドウ深度 Texture2DArray (R32_FLOAT)
    float shadowTexelSize = 0.0f;                  // 1/解像度 (PCF オフセット)
    // ---- 環境 (M29d)。RenderSystem が最初の active Skybox/Fog から埋める。描画専用 ----
    int32_t skyMode = -1; // -1=無効 (clearColor 背景) / 0=グラデーション / 1=cubemap (M38b)
    AssetID skyCubemapId = {};                          // CollectEnvironment が埋める (純データ)
    ID3D11ShaderResourceView* skyCubemap = nullptr;     // RenderSystem が解決 (null=フォールバック)
    DirectX::XMFLOAT3 skyTop = { 0.24f, 0.42f, 0.83f };
    DirectX::XMFLOAT3 skyHorizon = { 0.74f, 0.81f, 0.90f };
    DirectX::XMFLOAT3 skyBottom = { 0.28f, 0.25f, 0.22f };
    int32_t fogMode = -1; // -1=フォグ無効 / 0=linear 1=exp 2=exp2
    DirectX::XMFLOAT3 fogColor = { 0.65f, 0.70f, 0.75f };
    float fogDensity = 0.02f;
    float fogStart = 10.0f;
    float fogEnd = 80.0f;
    // ---- IBL (M38c)。RenderSystem が EnvMapBaker から埋める。null = 定数アンビエント ----
    ID3D11ShaderResourceView* iblIrradiance = nullptr;
    ID3D11ShaderResourceView* iblPrefiltered = nullptr;
    ID3D11ShaderResourceView* iblBrdfLut = nullptr;
    float iblSpecMips = 0.0f;
    // ---- SSAO (M38e)。Deferred のみ消費 (Forward は無視) ----
    int32_t ssaoEnabled = 0;
    float ssaoRadius = 0.8f;    // M40d: CameraPostFx から (シーンカメラ経路のみ上書き)
    float ssaoIntensity = 1.0f;
    // ---- メッシュ GPU インスタンシング (M38f)。0 = 全て per-item 描画 (A/B 比較用) ----
    int32_t instancingEnabled = 1;
    // ---- SceneView 表示モード (M40b)。0=Lit 1=Unlit (白ライト) 2=Wireframe (+Unlit)。
    //      CameraOverride 経由でエディタのみ設定 — GameView/Runtime は常に 0 ----
    int32_t debugViewMode = 0;
    // ---- M42a: シーン深度 SRV 基盤 (末尾 append)。FrameTarget からパススルー。
    //      depthSRV は dsv と同一テクスチャ — 読む側は dsvReadOnly バインド中のみ合法。
    //      null = 深度読み系効果 (ソフトパーティクル等) を自然無効化 (AssetPreview) ----
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11DepthStencilView* dsvReadOnly = nullptr;
    float nearZ = 0.1f;   // 深度線形化用 (カメラ/override から充填)
    float farZ = 1000.0f;
    // ---- M42d: 歪みバッファ (PostProcess::Target::distort)。RenderSystem が
    //      「blendMode=2 エミッタあり && HDR 経路」のときだけクリアして充填。
    //      null = 歪みパーティクルは描かれない (postfx off / AssetPreview) ----
    ID3D11RenderTargetView* distortionRTV = nullptr;
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等 = 従来と同一)。
    //      fog 系は CollectEnvironment のパススルー、太陽は RenderSystem が
    //      最初の type==0 平行光から充填 (リニア・強度込み。無ければ intensity を 0 に潰す) ----
    float fogHeightFalloff = 0.0f;      // 0 = 高さ一様 (従来)
    float fogBaseHeight = 0.0f;
    float fogInscatterIntensity = 0.0f; // 0 = 無効
    float fogInscatterPower = 8.0f;
    DirectX::XMFLOAT3 sunDirection = { 0.0f, -1.0f, 0.0f }; // 光の進行方向 (正規化)
    DirectX::XMFLOAT3 sunColor = { 0.0f, 0.0f, 0.0f };      // リニア・強度込み
    // ---- M44d: カメラモーションブラー (末尾 append)。RenderSystem が viewKey 毎の
    //      前フレーム viewProj を供給。valid=0 = 初フレーム/リサイズ = ブラー 0 ----
    DirectX::XMFLOAT4X4 prevViewProj = {}; // 未転置 (view*proj)
    int32_t prevViewProjValid = 0;
    // ---- M46b: ハイブリッド・パストレーシング (末尾 append。既定 = 0/null = 従来と同一)。
    //      rtScene/rtPasses が null のパス (Forward / AssetPreview) では自然に無効化される ----
    int32_t rtDebugMode = 0; // 0=off 1=BVH ヒートマップ 2=ヒット法線 3=インスタンス ID 4=生 GI
    const struct RtSceneBindings* rtScene = nullptr;
    class RtPasses* rtPasses = nullptr;
    // ---- M46c: 拡散 GI ----
    float rtResolutionScale = 0.5f; // GI を撃つ内部解像度の倍率 (0.25〜1.0)
    int32_t rtBounces = 1;          // 二次光線のバウンス数
    uint32_t rtFrameIndex = 0;      // 乱数列をフレームでずらす (freeze 時は 0 固定)
    // ---- M46d: テンポラル蓄積 (末尾 append)。RenderSystem が充填 ----
    int32_t rtTemporal = 1;    // 0 = 蓄積せず 1spp のまま (A/B 比較用)
    uint32_t rtViewKey = 0;    // 履歴の格納先 (FrameTarget::viewKey と同値)
    uint32_t rtViewSerial = 0; // このビューが描かれた通番。+1 で連続 = 履歴が使える
    // 前フレームのカメラ位置 (再投影の深度照合。prevViewProj とセットで有効)
    DirectX::XMFLOAT3 prevCameraPos = { 0, 0, 0 };
    // ---- M46e: SVGF 空間フィルタ (末尾 append)。テンポラル蓄積が前提 (幾何バッファの出所) ----
    int32_t rtSvgf = 1;       // 0 = 分散推定 + A-Trous を掛けない (A/B 比較用)
    int32_t rtFreezeSeed = 0; // 1 = 乱数固定 → 分散推定はテンポラルでなく空間へ落とす
    // ---- M46f: 最終画像への合成 (末尾 append)。1 = ライトパスの拡散環境項を GI で置換 ----
    int32_t rtGiEnabled = 0;
    // ---- M46g: RT 影 (末尾 append)。1 = 平行光のシャドウ係数を CSM でなくレイトレで作る ----
    int32_t rtShadowEnabled = 0;
    // ---- M46h: RT 反射 (末尾 append)。1 = ライトパスのスペキュラ環境項を
    //      roughness に応じてレイトレ反射で置換する (粗い面は IBL のまま) ----
    int32_t rtReflEnabled = 0;
    // ---- M54c: 局所ライト (スポット/点) のシャドウアトラス (末尾 append)。
    //      null / 0 = 従来と完全に同一の絵。RenderSystem がアトラス描画後に埋める。
    //      アトラスを持たない経路 (Forward は M54e まで / AssetPreview は永久に) は
    //      SRV が null のままなので、光パス側の「null ならフラグ 0」ゲートで自然に無効化される ----
    ID3D11ShaderResourceView* shadowAtlasSRV = nullptr; // Texture2D (R32_FLOAT)
    float shadowAtlasTexel = 0.0f;                      // 1/アトラス解像度 (PCF オフセット)
    int32_t shadowTileCount = 0;                        // 0 = 影を投げる局所ライトが居ない
    ShadowTile shadowTiles[kMaxShadowTiles] = {};
};

// GPU へ渡すライト 1 個 (定数バッファ配列要素、16 バイト境界に揃えた 64 バイト)。
// HLSL 側 common.hlsli の Light 構造体とレイアウト一致。
struct GpuLight {
    DirectX::XMFLOAT3 position = { 0, 0, 0 };    // Point/Spot: ワールド位置
    float range = 15.0f;                         // Point/Spot: 減衰半径
    DirectX::XMFLOAT3 direction = { 0, -1, 0 };  // 光の進行方向 (正規化、Dir/Spot)
    float intensity = 1.0f;
    DirectX::XMFLOAT3 color = { 1, 1, 1 };
    int32_t type = 0;      // 0=Directional 1=Point 2=Spot
    float cosInner = 0.9f; // Spot: cos(内角)
    float cosOuter = 0.8f; // Spot: cos(外角)
    // ---- M54c: シャドウアトラス (旧 pad0/pad1 の再利用。64 バイトのレイアウトは不変) ----
    // 統合契約 (plans/radiant-shimmering-lumen.md 付録 予約 2) が pad0/pad1 に予約した枠。
    // rt_common.hlsli の RtLight は同じ 8 バイトを _pad のまま持つ (RT は局所影を持たない)
    int32_t shadowTile = 0;  // アトラスのタイル index (先頭面)。shadowFaces==0 なら無意味
    int32_t shadowFaces = 0; // 面数: 0=影を投げない / 1=スポット (M54c) / 6=点光源 (M54d)
};
static_assert(sizeof(GpuLight) == 64, "GpuLight must match HLSL 16-byte packing");

// ライト配列長。**HLSL の MAX_LIGHTS (common.hlsli) / MYE_RT_MAX_LIGHTS (rt_common.hlsli) と
// 必ず一致させること** — 食い違うと定数バッファのレイアウト不一致として静かに壊れる。
// tools\check_rules.ps1 の規則 9 が静的に検査する (M55a で登録)
constexpr int kMaxLights = 16;

// シーンのライト一式 (アンビエント + ライト配列)。RenderSystem が構築し各パスへ渡す。
struct SceneLightData {
    DirectX::XMFLOAT3 ambient = { 0.15f, 0.16f, 0.18f };
    int32_t count = 0;
    GpuLight lights[kMaxLights] = {};
};

class RenderQueue {
public:
    std::vector<RenderItem> opaque;
    std::vector<RenderItem> transparent;

    void Clear()
    {
        opaque.clear();
        transparent.clear();
    }

    // 決定論的ソート (spec 11.2 規則 7: 明示キー + タイブレーク。ポインタ比較は禁止)。
    // opaque: material → mesh → 深度 (近い順) / transparent: 深度 (遠い順) → material → mesh
    void Sort();
};

} // namespace mye
