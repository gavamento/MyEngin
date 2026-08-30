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
    // ---- M55c: 前フレームに **実際に描いた** ワールド行列 (末尾 append、velocity 用) ----
    // 「前 tick」ではない (詳細は RenderSystem.h の PrevRenderWorldStore の頭)。
    // 履歴が無い場合は world と同値が入る = 画面速度が厳密に 0 になり、
    // 消費側は「カメラ再投影のみ」へ自然に縮退する
    DirectX::XMFLOAT4X4 prevWorld = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
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

// 定数バッファへ載せる形のタイル (HLSL common.hlsli の ShadowTile と同一 96 バイト、M54c)。
// 上の ShadowTile (描画側の生データ) を転置 + 詰め替えたもの。
// ★M54e で Deferred 光パス / Forward / Deferred 透明後段の **3 箇所**が同じ変換を要求する
//   ようになったのでここへ引き上げた。転置を 1 箇所でも書き忘れると
//   「その経路だけ影が明後日の方向に出る」という、絵は出るのに合わないだけの壊れ方をする
struct ShadowTileCB {
    DirectX::XMFLOAT4X4 lightViewProj = {}; // transpose(lightView*lightProj)
    DirectX::XMFLOAT4 uvScaleBias = {};     // xy = スケール / zw = オフセット
    DirectX::XMFLOAT4 params = {};          // x = 定数深度バイアス (NDC) / yzw = 予約
};
static_assert(sizeof(ShadowTileCB) == 96, "ShadowTileCB must match HLSL 16-byte packing");

// ---- ローカル反射プローブ (M56f) ----
// 同時に合成できるプローブ数 = 定数バッファ内の配列長そのもの。
// **HLSL の MYE_MAX_REFLECTION_PROBES (common.hlsli) と必ず一致させること** —
// tools\check_rules.ps1 の規則 9 が静的に検査する。
// 8 個 × 48 バイト = 384 バイトで CB 上限には遠く、プリフィルタ済みキューブの実体も
// 8 × (128² × 6面 × 5mip × 8B) ≒ 10MB に収まる
constexpr int kMaxReflectionProbes = 8;

// プローブ 1 個ぶんの GPU パラメータ (HLSL common.hlsli の ReflProbe と 48 バイトで一致)。
// **箱は軸平行** (v1)。エンティティの回転は見ない — 視差補正が箱ローカルへの往復になり、
// CPU ミラーとの一致を保つ手間に見合わないため (engine_spec §6.10 に制限として明記)
struct ReflectionProbeGpu {
    DirectX::XMFLOAT4 centerIntensity = { 0.0f, 0.0f, 0.0f, 1.0f }; // xyz=撮影位置 / w=強度
    DirectX::XMFLOAT4 boxMin = { -1.0f, -1.0f, -1.0f, 1.0f };       // xyz=min / w=ブレンド距離
    DirectX::XMFLOAT4 boxMax = { 1.0f, 1.0f, 1.0f, 1.0f };          // xyz=max / w=1=ボックス投影
};
static_assert(sizeof(ReflectionProbeGpu) == 48, "ReflectionProbeGpu must match HLSL packing");

// 焼き上がったプローブ束の**非所有ビュー**。実体 (テクスチャ) は ProbeBaker が持つ。
// count==0 / cubeArray==nullptr = プローブ無し = 従来と完全に同じ絵
struct ReflectionProbeSet {
    ID3D11ShaderResourceView* cubeArray = nullptr; // TextureCubeArray (プリフィルタ済み + mips)
    int32_t count = 0;
    float specMips = 0.0f; // 最終 mip index (roughness 1.0 の行き先)
    ReflectionProbeGpu probes[kMaxReflectionProbes] = {};
};

// 影響の重み。箱の外 = 0 / 内側へブレンド距離ぶん入ると 1。
// **HLSL ミラー: common.hlsli の ReflProbeWeight — 変更時は両方更新**
// (ProbeBakerSelfTest が両者の一致を…ではなく、CPU 側の値を機械で固定する)
inline float ReflProbeWeight(const ReflectionProbeGpu& p, const DirectX::XMFLOAT3& posW)
{
    const float dx = (posW.x - p.boxMin.x < p.boxMax.x - posW.x) ? posW.x - p.boxMin.x
                                                                 : p.boxMax.x - posW.x;
    const float dy = (posW.y - p.boxMin.y < p.boxMax.y - posW.y) ? posW.y - p.boxMin.y
                                                                 : p.boxMax.y - posW.y;
    const float dz = (posW.z - p.boxMin.z < p.boxMax.z - posW.z) ? posW.z - p.boxMin.z
                                                                 : p.boxMax.z - posW.z;
    float m = (dx < dy) ? dx : dy;
    m = (m < dz) ? m : dz;
    if (m <= 0.0f) {
        return 0.0f;
    }
    const float blend = (p.boxMin.w > 1e-4f) ? p.boxMin.w : 1e-4f;
    const float w = m / blend;
    return (w > 1.0f) ? 1.0f : w;
}

// 視差補正 (ボックス投影)。**HLSL ミラー: common.hlsli の ReflProbeDir**
inline DirectX::XMFLOAT3 ReflProbeDir(const ReflectionProbeGpu& p, const DirectX::XMFLOAT3& posW,
                                      const DirectX::XMFLOAT3& R)
{
    if (p.boxMax.w < 0.5f) {
        return R;
    }
    const float rx = (R.x >= 0.0f ? 1.0f : -1.0f) * ((std::fabs(R.x) > 1e-6f) ? std::fabs(R.x) : 1e-6f);
    const float ry = (R.y >= 0.0f ? 1.0f : -1.0f) * ((std::fabs(R.y) > 1e-6f) ? std::fabs(R.y) : 1e-6f);
    const float rz = (R.z >= 0.0f ? 1.0f : -1.0f) * ((std::fabs(R.z) > 1e-6f) ? std::fabs(R.z) : 1e-6f);
    const float tx = std::fmax((p.boxMax.x - posW.x) / rx, (p.boxMin.x - posW.x) / rx);
    const float ty = std::fmax((p.boxMax.y - posW.y) / ry, (p.boxMin.y - posW.y) / ry);
    const float tz = std::fmax((p.boxMax.z - posW.z) / rz, (p.boxMin.z - posW.z) / rz);
    float t = (tx < ty) ? tx : ty;
    t = (t < tz) ? t : tz;
    if (t <= 0.0f) {
        return R;
    }
    DirectX::XMFLOAT3 d = { posW.x + R.x * t - p.centerIntensity.x,
                            posW.y + R.y * t - p.centerIntensity.y,
                            posW.z + R.z * t - p.centerIntensity.z };
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len <= 1e-12f) {
        return R;
    }
    return { d.x / len, d.y / len, d.z / len };
}

// 一番効いているプローブ 1 個。**同点は添字が小さい方** (収集が EntityID 順なので決定論)。
// **HLSL ミラー: common.hlsli の ReflProbeSelect**
inline int ReflProbeSelect(const ReflectionProbeGpu* probes, int count,
                           const DirectX::XMFLOAT3& posW, float& weight)
{
    weight = 0.0f;
    int best = -1;
    for (int i = 0; i < count; ++i) {
        const float w = ReflProbeWeight(probes[i], posW);
        if (w > weight) {
            weight = w;
            best = i;
        }
    }
    return best;
}

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
    //      アトラスを持たない経路 (AssetPreview は永久に = enableShadows=false) は
    //      SRV が null のままなので、光パス側の「null ならフラグ 0」ゲートで自然に無効化される ----
    ID3D11ShaderResourceView* shadowAtlasSRV = nullptr; // Texture2D (R32_FLOAT)
    float shadowAtlasTexel = 0.0f;                      // 1/アトラス解像度 (PCF オフセット)
    int32_t shadowTileCount = 0;                        // 0 = 影を投げる局所ライトが居ない
    ShadowTile shadowTiles[kMaxShadowTiles] = {};
    // ---- M55b: カメラジッタ (末尾 append。既定 = 振幅 0 = proj と 1 ビットも変わらない) ----
    //   proj         = ラスタライズに使う射影 (ジッタ込み)。
    //   projNoJitter = ジッタを載せる前の射影。**再投影 (prevViewProj / モーションブラー /
    //     RT テンポラル)・シャドウのカスケードフィット・視錐台カリング・太陽の画面位置は
    //     必ずこちらを読む** (詳細は PostFxMath.h の camerajitter の頭)。
    //   RenderSystem::Render が両方を必ず埋める。手組みの RenderView (selftest) では
    //     projNoJitter が単位行列のままになるので、そちらを読む経路は通らないこと。
    DirectX::XMFLOAT4X4 projNoJitter = {};
    float jitterPixels[2] = { 0.0f, 0.0f }; // proj に載せたサブピクセル量 (0,0 = ジッタ無効)
    float jitterNdc[2] = { 0.0f, 0.0f };    // 同じものを NDC で (TAA が履歴サンプルに使う)
    uint32_t viewFrameIndex = 0;            // viewKey 別の描画通番 = ジッタ列のインデックス
    // ---- M55c: velocity バッファの可視化 (末尾 append。0 = 何も起きない) ----
    // Deferred のみ。GBuffer RT4 を画面へ貼り替える純デバッグ表示で、消費側は誰もいない
    int32_t velocityDebug = 0;
    // ---- M55d: TAA (末尾 append。既定 0/null = 従来と 1 ビットも変わらない) ----
    //   taaEnabled  = このビューで TAA を走らせる (= カメラジッタも載っている)。
    //     RenderSystem が「CameraPostFx の taaOn / グローバル設定」と
    //     「パスが velocity を書くか (Deferred のみ)」の両方を見て決める。
    //     ★ジッタと TAA は**必ず同じ条件**で on/off する — 片方だけだと画面が
    //       毎フレーム半ピクセル揺れるだけになる。
    //   velocitySRV = GBuffer RT4。path.Render の直後に RenderSystem が
    //     IRenderPath::VelocitySRV() から充填する (Forward は null)。
    //   viewKey     = 履歴スロット (FrameTarget::viewKey と同値。0=AssetPreview は履歴なし)。
    //     履歴の連続性判定は既存の viewFrameIndex (viewKey 毎の描画通番) を使う
    int32_t taaEnabled = 0;
    ID3D11ShaderResourceView* velocitySRV = nullptr;
    uint32_t viewKey = 0;
    // ---- M58c: 地形の可視チャンク (末尾 append)。RenderSystem が TerrainSystem の
    //      収集結果を指す。実体は TerrainPass.h の TerrainDrawList (Renderer 層の純データ)。
    //      **null / 空 = 地形なし = 従来と完全に同じ絵** — AssetPreviewCache の
    //      RenderSystem はここを埋めないので、サムネイルは地形を一切描かない ----
    const struct TerrainDrawList* terrain = nullptr;
    // ---- M56a: デカール (末尾 append)。RenderSystem が毎フレーム作り直す実体を指す。
    //      **null / 空 = デカール 0 個 = 従来と完全に同じ絵** (Deferred のデカールパスは
    //      1 命令も発行せずに return する) — golden 全枚がビット一致し続ける根拠。
    //      Forward は v1 非対応なのでここを読まない (engine_spec.md §6.4) ----
    const struct DecalDrawList* decals = nullptr;
    // ---- M56c: HZB (min-Z ピラミッド) の可視化 (末尾 append。0 = 何も起きない) ----
    //      0 = off / N = ミップ N-1 を画面へ貼る。**このサブではピラミッドを作るかどうかも
    //      この値だけで決まる** — 0 なら確保も CS ディスパッチも 1 つも走らない
    //      (本番の消費者 = SSR は M56d。そちらが入ったら ssrOn も「作る」条件に加わる)。
    //      Deferred のみ。depthSRV が null の経路 (AssetPreview) では自然に無効化される
    int32_t hzbDebug = 0;
    // ---- M56d: SSR (スクリーンスペース反射、末尾 append。既定 0 = 従来と 1 ビットも同じ) ----
    //      1 = 光パス + スカイボックスの後に反射の**差分**を加算する。**HZB を組む条件は
    //      hzbDebug との or** — SSR は min-Z ピラミッドを唯一の加速構造として使う。
    //      Deferred のみ (GBuffer と HZB が前提)。Forward / AssetPreview は読まない。
    //      ssrMaxRoughness 以上の粗さの面には厳密に 0 を足す = 粗い面は IBL のまま
    int32_t ssrEnabled = 0;
    float ssrMaxRoughness = 0.6f; // RT 反射の kRtReflMaxRoughness と同じ既定値
    float ssrIntensity = 1.0f;    // 1 = 「IBL スペキュラを反射で置き換える」ちょうど 100%
    // ---- M56f: ローカル反射プローブ (末尾 append)。**null / count 0 = 従来と同じ絵** ----
    //      焼いた束を指すだけの非所有ポインタ。ベイクは明示指示のときしか走らないので、
    //      「プローブを置いただけ」のシーンではここは null のまま = 1 命令も増えない。
    //      Deferred のみ (Forward は v1 非対応。engine_spec §6.10)。
    //      AssetPreviewCache の RenderSystem はここを埋めない = サムネイルは常にプローブ無し
    const ReflectionProbeSet* probes = nullptr;
    // ---- M57d: フロクセルの積分結果 (末尾 append。null/0 = 従来と 1 ビットも変わらない) ----
    //   froxelSRV = Texture3D (rgb = カメラからそこまでに積算した内向き散乱 /
    //     a = そこまでの透過率)。FroxelPass::Render が返したものを RenderSystem が入れる。
    //     ★null のままになる経路が 3 つある (Forward / AssetPreviewCache の RenderSystem /
    //       froxel off)。**消費側は「null ならフラグ 0」のゲートを必ず置くこと** —
    //       置かないとサムネイルだけが前フレームの残骸をサンプルする。
    //   froxelNearZ/FarZ/Slices = 注入時に確定したグリッドの深度分割。**カメラの
    //     near/far ではない** (FroxelSettings::maxDistance で手前に切ってある) ので、
    //     ここを取り違えるとサンプル位置が丸ごとずれる
    ID3D11ShaderResourceView* froxelSRV = nullptr;
    float froxelNearZ = 0.0f;
    float froxelFarZ = 0.0f;
    int32_t froxelSlices = 0;
};

// ---- デカール (M56a) ----
// 投影ボックス 1 個の描画指示 (Renderer 層の純データ)。箱は単位立方体 [-0.5,0.5]^3 を
// world で変換したもので、**投影方向はローカル +Z** (ライトの向きと同じ規約)。
struct DecalRenderItem {
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    DirectX::XMFLOAT4X4 invWorld = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    DirectX::XMFLOAT3 projDir = { 0.0f, 0.0f, 1.0f }; // ローカル +Z のワールド向き (正規化)
    float angleFadeCos = 0.0f;                        // cos(角度フェード上限)
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };   // リニア済み tint
    float opacity = 1.0f;
    float uvScale[2] = { 1.0f, 1.0f };
    float uvOffset[2] = { 0.0f, 0.0f };
    AssetID texture = {}; // null = 白 (DeferredPath が TextureLibrary::White() を張る)
    int32_t sortOrder = 0;
    uint32_t sortKey = 0; // EntityID::index (規則 7 の決定論タイブレーク)
    // ---- M56b: 法線 / roughness (末尾 append) ----
    // TBN の T / B。**投影パスでは posW の微分から TBN を作れない**ので、デカール自身の
    // OBB 基底を CPU 側で正規化して渡す (シェーダは正規化しない)。
    // B は「ローカル -Y」— UV の v を反転している (uv.y = 0.5 - lp.y) ぶん符号が入れ替わる
    DirectX::XMFLOAT3 axisX = { 1.0f, 0.0f, 0.0f }; // ローカル +X のワールド向き (正規化)
    DirectX::XMFLOAT3 axisY = { 0.0f, 1.0f, 0.0f }; // ローカル +Y のワールド向き (正規化)
    AssetID normalTexture = {};     // 接線空間の法線マップ (null = 平坦)
    float normalStrength = 0.0f;    // そのままブレンド係数になる。0 = RT1 を触らない
    float roughness = 0.5f;         // 上書きする roughness (RT3 の g)
    float roughnessStrength = 0.0f; // そのままブレンド係数になる。0 = RT3 を触らない
};

// この 1 枚が GBuffer の**表面属性** (法線 / roughness) にも書くか。
// ★DeferredPath はリスト全体でこれを or して「RT1/RT3 を bind するか」「法線バッファを
//   コピーするか」を決める。1 枚も書かないフレームは M56a と 1 命令も違わない経路に落ちる
//   = デカールを albedo だけで使っている限り M56b の追加コストはゼロ
inline bool DecalWritesSurface(const DecalRenderItem& d)
{
    return d.normalStrength > 0.0f || d.roughnessStrength > 0.0f;
}

// M56b の強度を [0,1] へ丸める。★**強度はそのままハードウェアのブレンド係数**なので、
// 1 を超えると dst 側の係数 (1-src) が負になって法線が裏返り、負なら符号が反転する。
// Inspector のスライダは止めるがスクリプト / 手書き JSON は素通りするので収集側で潰す
inline float DecalStrength01(float v)
{
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

// デカールの描画順。**タイブレークが本体**: sortOrder が同値のデカールが並んだとき、
// 比較が「収集順」に依存するとアーキタイプの並びの揺れで上下が入れ替わる (規則 7)。
// ポインタ比較も禁止なので EntityID::index を second key にしてある。
// 純関数なので DecalSelfTest が直接検査する (TerrainDrawOrderLess と同じ流儀)
inline bool DecalDrawOrderLess(const DecalRenderItem& a, const DecalRenderItem& b)
{
    if (a.sortOrder != b.sortOrder) {
        return a.sortOrder < b.sortOrder;
    }
    return a.sortKey < b.sortKey;
}

// 角度フェードのしきい値 [度] → cos。**既定 90 度 = cos 0** で、シェーダ側の式が
// saturate(dot(N,-projDir)) そのもの (= 正対で 1、直角で 0 の素直な cos フェード) に落ちる。
// 範囲外は [0,180] に丸める — cos がその外で単調でなくなり「角度を狭めたのに広がる」が起きる
inline float DecalAngleFadeCos(float degrees)
{
    const float d = (degrees < 0.0f) ? 0.0f : ((degrees > 180.0f) ? 180.0f : degrees);
    return std::cos(d * 3.14159265358979323846f / 180.0f);
}

// world 行列からデカールの GPU パラメータ (逆行列 + 投影方向) を作る。
// ★**行列を 2 箇所で作らないための 1 本**。逆行列の転置を片方で忘れると
//   「絵は出るのに箱の外へはみ出す」という気付きにくい壊れ方をするので、
//   RenderSystem はここだけを呼び、DecalSelfTest はここを直接検査する。
// 戻り値 false = 退化スケール (行列式 0) = そのデカールは描かない
inline bool FillDecalTransform(const DirectX::XMFLOAT4X4& world, DecalRenderItem& dst)
{
    const DirectX::XMMATRIX w = DirectX::XMLoadFloat4x4(&world);
    DirectX::XMVECTOR det = {};
    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, w);
    if (std::fabs(DirectX::XMVectorGetX(det)) < 1e-12f) {
        return false;
    }
    dst.world = world;
    DirectX::XMStoreFloat4x4(&dst.invWorld, inv);
    // 第 3 行 = ローカル +Z のワールド向き (LightComponent の向きと同じ規約)
    const DirectX::XMVECTOR z =
        DirectX::XMVectorSet(world._31, world._32, world._33, 0.0f);
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(z)) < 1e-20f) {
        return false;
    }
    DirectX::XMStoreFloat3(&dst.projDir, DirectX::XMVector3Normalize(z));
    // M56b: TBN の元になる第 1 行 (ローカル +X) と 第 2 行 (ローカル +Y)。
    // 退化 (スケール 0 の軸) は行列式 0 で既に弾かれているので長さ 0 にはならないが、
    // 「正規化はここ 1 箇所」を守るためにシェーダ側では一切正規化しない
    const DirectX::XMVECTOR ax = DirectX::XMVectorSet(world._11, world._12, world._13, 0.0f);
    const DirectX::XMVECTOR ay = DirectX::XMVectorSet(world._21, world._22, world._23, 0.0f);
    DirectX::XMStoreFloat3(&dst.axisX, DirectX::XMVector3Normalize(ax));
    DirectX::XMStoreFloat3(&dst.axisY, DirectX::XMVector3Normalize(ay));
    return true;
}

// このフレームのデカール描画リスト。RenderSystem が所有し RenderView から指す。
// 空 = デカール無し = 従来と完全に同じ絵 (AssetPreview の RenderSystem は常に空)
struct DecalDrawList {
    std::vector<DecalRenderItem> items;
};

// M55c: 「**前フレームに実際に描いた** world 行列」の viewKey 別ストア (velocity の出所)。
//
// ★★RenderSystem.h の PrevWorldStore (M36b) では代用できない。あちらは **tick 頭**の
//   スナップショットで、実際に描かれるのは LerpWorld(prev, cur, interpAlpha)。つまり
//   前フレームの画面にあった行列は「LerpWorld(prev, cur, 前フレームの alpha)」であって
//   prev ではない。prev をそのまま velocity に使うと最大 1 tick 分過大になり、
//   TAA が履歴を外しモーションブラーが過剰にブレる。
//   ★さらに悪いことに **決定的撮影モードでは dt 固定で interpAlpha == 1.0 になる**ので、
//   この誤りは golden に 1 ピクセルも現れない (対話プレイでだけ出る)。だから機械検査は
//   スクショではなく selftest (RenderSelfTest の TestPrevRenderWorldStore) 側にある。
//
// 構造は RtPasses::RtHistory (viewKey 別 + 描画通番の連続性判定 + リサイズ破棄) に倣う。
// 「前フレームも描かれたか」はスロット毎の通番で見るので、消えた/カリングされた
// エンティティの古い行列を拾うことがない (毎フレームのクリアも要らない)。
struct PrevRenderWorldStore {
    std::vector<DirectX::XMFLOAT4X4> world; // entity.index キー
    std::vector<uint32_t> generation;       // entity.generation + 1 (0 = 未使用スロット)
    std::vector<uint32_t> slotSerial;       // そのスロットを書いたときの描画通番

    // 今フレームの描画通番とビューサイズを宣言する。
    // 戻り値 = 前フレームの記録が使えるか (通番がちょうど 1 つ違い かつ 同サイズ)。
    // 使えないときも記録は続ける (次フレームのため)
    bool Begin(uint32_t frameSerial, int width, int height)
    {
        usable_ = valid_ && w_ == width && h_ == height && lastSerial_ + 1u == frameSerial;
        cur_ = frameSerial;
        prev_ = frameSerial - 1u; // frameSerial==0 は 0xFFFFFFFF へ回る = どのスロットとも不一致
        lastSerial_ = frameSerial;
        w_ = width;
        h_ = height;
        valid_ = true;
        return usable_;
    }

    // 前フレームに描いた行列 (無ければ null)。同じエンティティの Record より **先**に呼ぶこと
    const DirectX::XMFLOAT4X4* Lookup(EntityID e) const
    {
        if (!usable_ || e.index >= world.size()) {
            return nullptr;
        }
        if (generation[e.index] != e.generation + 1u || slotSerial[e.index] != prev_) {
            return nullptr; // 前フレームは描かれていない / index が再利用された
        }
        return &world[e.index];
    }

    void Record(EntityID e, const DirectX::XMFLOAT4X4& m)
    {
        if (e.index >= world.size()) {
            world.resize(e.index + 1);
            generation.resize(e.index + 1, 0);
            slotSerial.resize(e.index + 1, 0);
        }
        world[e.index] = m;
        generation[e.index] = e.generation + 1u;
        slotSerial[e.index] = cur_;
    }

private:
    uint32_t cur_ = 0;
    uint32_t prev_ = 0;
    uint32_t lastSerial_ = 0;
    int w_ = 0;
    int h_ = 0;
    bool valid_ = false;
    bool usable_ = false;
};

// view のタイル列を CB 形式へ詰め替える (M54e)。dst は kMaxShadowTiles 要素を要求する。
// 呼び出し側が「アトラスを使うか」を判定した後で呼ぶ — ここは判定しない
// (使わない経路では dst をゼロのまま渡せばよく、シェーダ側のフラグが 0 なら読まれない)。
inline void FillShadowTilesCB(const RenderView& view, ShadowTileCB* dst)
{
    for (int t = 0; t < view.shadowTileCount && t < kMaxShadowTiles; ++t) {
        const ShadowTile& src = view.shadowTiles[t];
        DirectX::XMStoreFloat4x4(
            &dst[t].lightViewProj,
            DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&src.lightViewProj)));
        dst[t].uvScaleBias = { src.uvScale[0], src.uvScale[1], src.uvOffset[0], src.uvOffset[1] };
        dst[t].params = { src.depthBias, 0.0f, 0.0f, 0.0f };
    }
}

// M57e: 「このビューでフロクセルの積分結果を合成してよいか」の唯一の判定。
// **消費者が 5 つに増えた**ので式を 1 本に畳んである (Deferred 光パス / Deferred の
// 透明後段 / ForwardPath / SkyboxPass / CpuParticleBackend)。
//   ・froxelSRV == nullptr になる経路が 3 つある (froxel off / 正射影などで注入が
//     走らなかったフレーム / AssetPreviewCache の別 RenderSystem)。**ここを通さないと
//     サムネイルだけが前フレームの残骸をサンプルする。**
//   ・Unlit / Wireframe (debugViewMode != 0) も外す — 大気散乱はライティングの一部で、
//     既に fogMode を -1 に潰してある表示モードに霧だけ載せるのは筋が通らない
inline bool FroxelIsBound(const RenderView& view)
{
    return view.debugViewMode == 0 && view.froxelSRV != nullptr && view.froxelSlices > 0
        && view.froxelFarZ > 0.0f;
}

// M57e: Forward 系 PerFrame CB (b0) の末尾に置くフロクセルのブロック。
// **C++ ミラーが 2 つある** (ForwardPath.cpp / DeferredPath.cpp の PerFrameCB) ので、
// 形を 1 本にして「片方だけ直す」を構造的に不可能にしてある。
// HLSL 側は forward_lit / forward_lit_instanced / forward_skinned / forward_terrain の 4 本
struct FroxelForwardCB {
    int32_t enabled = 0;
    float nearZ = 0.0f;
    float farZ = 0.0f;
    float slices = 0.0f;
    // dot(float4(posW,1), これ) = view 深度。view 行列の第 3 列そのもの。
    // ★カメラ前方ベクトルを正規化して内積する式にしない — 非一様スケールの入った
    //   ビュー行列で静かにずれる (M57d が Deferred 側で確定させた規約)
    DirectX::XMFLOAT4 viewZRow = { 0.0f, 0.0f, 0.0f, 0.0f };
    float screenSize[2] = { 0.0f, 0.0f }; // SV_Position → uv (Forward に gScreenSize は無い)
    float pad[2] = { 0.0f, 0.0f };
};

inline FroxelForwardCB MakeFroxelForwardCB(const RenderView& view, bool bound)
{
    FroxelForwardCB out;
    out.enabled = bound ? 1 : 0;
    out.nearZ = view.froxelNearZ;
    out.farZ = view.froxelFarZ;
    out.slices = static_cast<float>(view.froxelSlices);
    out.viewZRow = { view.view._13, view.view._23, view.view._33, view.view._43 };
    out.screenSize[0] = static_cast<float>(view.width);
    out.screenSize[1] = static_cast<float>(view.height);
    return out;
}

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

// ---- M57a: フロクセル (視錐台に沿った 3D グリッド) の幾何 ----
//
// 視錐台を XY は画面タイル、Z は指数分布のスライスに割った 3D テクスチャへ散乱と消散を
// 積み、最後に手前から積分して「そのピクセルまでの inscatter / transmittance」を作る。
// ここに置いてあるのは**グリッドの幾何だけ** (パスの実体は M57b の FroxelPass)。
// 全部純関数なので RenderSelfTest が機械検査できる — GPU を起こさずに済む部分は
// 起こさずに検査するのがこのリポジトリの流儀 (テストは機能の隣に置く)。
namespace froxel {

// CS のスレッドグループ (XY のみ。Z はディスパッチ側でスライス数ぶん並べる)。
// XY だけをタイルにしているのは、注入も積分も「同じ (x,y) の Z 列」を扱うから —
// 積分パス (M57c) は 1 スレッドが 1 本の Z 列を手前から舐めるので Z を割れない。
// **HLSL の MYE_FROXEL_GROUP (assets\shaders\froxel_*.cs.hlsl) と必ず一致させること** —
// 食い違うとグリッドの一部が書かれないまま残り、前フレームの残骸を積分する形で
// 静かに壊れる。tools\check_rules.ps1 の規則 9 が一致を検査する
constexpr int kGroupSize = 8;

// 既定のグリッド解像度。**M57a の WARP 実測 (Editor.exe --froxel-probe) で決めた値**。
// 960x540 に対して 6x6 画素タイル x 64 スライス = 921,600 セル / 7.03MB。
//
//   グリッド        セル数    WARP clear   RTX3060 clear   1 枚の VRAM
//   160x90x64      921,600     0.95 ms       0.026 ms        7.03 MB   ← 既定
//   128x72x48      442,368     0.55 ms       0.014 ms        3.38 MB
//   80x45x32       115,200     0.27 ms       0.007 ms        0.88 MB
//
// 落とさなかった理由: WARP のスループットが 8 倍のセル数域でほぼ一定 (約 0.9 Gcell/s) =
// **コストがセル数に線形**で、固定費に食われていない。つまり解像度は後から素直に効く
// 品質/コストのつまみで、先に絞る必要が無い。注入 (M57b) が WARP で重すぎたら
// この表の下段へ落とす — 数字が線形なので予測が立つ。
// ★clear は「空の CS」= 下限であって、注入パスのコストではない
constexpr int kGridX = 160;
constexpr int kGridY = 90;
constexpr int kGridZ = 64;

// ディスパッチのグループ数 (切り上げ)。extent <= 0 でも 0 を返して Dispatch を空振りさせる
constexpr int DispatchGroups(int extent, int group)
{
    return (extent <= 0 || group <= 0) ? 0 : (extent + group - 1) / group;
}

// スライス境界の view 深度 (指数分布)。slice = 0 → nearZ、slice = sliceCount → farZ。
// 手前を厚く割るのは、フォグの見た目の情報量がカメラ近傍に集中しているから
// (等間隔だと近景が 1 スライスに潰れて縞が出る)。
// nearZ は正でなければならない — 0 だと log が発散するので下限で潰す
inline float SliceToViewDepth(float slice, int sliceCount, float nearZ, float farZ)
{
    const float n = (nearZ > 1e-4f) ? nearZ : 1e-4f;
    const float f = (farZ > n) ? farZ : (n * 2.0f);
    const int count = (sliceCount > 0) ? sliceCount : 1;
    return n * std::pow(f / n, slice / static_cast<float>(count));
}

// 上の逆関数。view 深度 → スライス座標 (小数)。範囲外もそのまま外挿して返す
// (クランプは呼び出し側の責任 — グリッド外を最遠スライスへ丸めると空が濁る)
inline float ViewDepthToSlice(float depth, int sliceCount, float nearZ, float farZ)
{
    const float n = (nearZ > 1e-4f) ? nearZ : 1e-4f;
    const float f = (farZ > n) ? farZ : (n * 2.0f);
    const int count = (sliceCount > 0) ? sliceCount : 1;
    const float d = (depth > 1e-6f) ? depth : 1e-6f;
    return static_cast<float>(count) * std::log(d / n) / std::log(f / n);
}

// ---- M57b: 注入パスの数式 (HLSL froxel_inject.cs.hlsl と同一式) ----
//
// GPU 側と CPU 側で式を二重に持つのは、位相関数の正規化やセル中心の取り方が
// 「絵はそれらしく出るのに物理的に間違っている」形で壊れるため。
// RenderSelfTest が CPU 版を全立体角で数値積分して 1 になることまで見ている。

// セル中心の view 深度。**境界ではなく中心**を代表点にする — 境界だと隣のセルと
// 同じ点を評価してしまい、1 スライスぶんの厚みが消える。
// M57c のジッタはこの 0.5 を [0,1) の擬似乱数で置き換える形で入る
inline float SliceCenterViewDepth(int slice, int sliceCount, float nearZ, float farZ)
{
    return SliceToViewDepth(static_cast<float>(slice) + 0.5f, sliceCount, nearZ, farZ);
}

// Henyey-Greenstein 位相関数。cosTheta = dot(光の進行方向, セル→カメラ方向)。
// g > 0 = 前方散乱 = 「光源のほうを向くと明るい」。全立体角の積分が 1 になる正規化つき
// (この 1/4π を落とすと霧の明るさが密度と一緒にしか調整できなくなる)
inline float HenyeyGreenstein(float cosTheta, float g)
{
    // ±1 は分母が 0 に落ちる特異点。CameraPostFx から ±1 が来る経路は無いが、
    // ここで inf を作るとグリッド全体が NaN で埋まる (積分結果が丸ごと消える)
    const float gg = (g < -0.95f) ? -0.95f : ((g > 0.95f) ? 0.95f : g);
    const float d = 1.0f + gg * gg - 2.0f * gg * cosTheta;
    const float dd = (d > 1e-4f) ? d : 1e-4f;
    // x^1.5 を pow で書かない — HLSL 側は WARP で pow が exp/log の 2 段になり、
    // セル × ライト本数ぶん効く。**両方を同じ形に揃えておく**のがこのミラーの意味
    return (1.0f - gg * gg) / (4.0f * 3.14159265f * (dd * std::sqrt(dd)));
}

// 高度による密度スケール。M43a のハイトフォグ ρ(y)=e^{-k(y-base)} と同じプロファイル
// (falloff == 0 なら厳密に 1 = 一様媒質)
inline float HeightDensityScale(float y, float baseHeight, float falloff)
{
    return std::exp(-falloff * (y - baseHeight));
}

// ---- M57c: 深度スライスジッタ + 前方積分 (HLSL froxel_common.hlsli と同一式) ----

// スライスジッタ列の周期。camerajitter::kSequenceLength (= 8) と同じ長さにしてある。
// 別の周期にすると TAA の収束周期との最小公倍数まで「1 巡」が伸びて、
// 決定的撮影で撮った 2 枚が「どちらも収束前」の別状態になる
constexpr uint32_t kJitterSequenceLength = 8;

// frameIndex → セル中心のスライス方向オフセット [0,1)。0.5 = ジッタ無し (M57b と同じ位置)。
// ★実時間ではなく **viewKey 別の描画通番** から引く = 決定的撮影モード
//   (frame 番号 == tick 番号) でフレーム列がそのまま再現する。
// 中身は基数 2 の van der Corput で、camerajitter::RadicalInverse(i, 2) と**同じ数列**。
// PostFxMath.h の実装を呼ばないのは RenderTypes.h がそれより下の層で include できないため
// (逆向きの依存になる)。**両者が一致することは RenderSelfTest が機械で照合している**。
// frameIndex==0 が 0.5 になるのは意図的 — テンポラル 1 フレーム目が M57b の注入と
// ビット一致し、「ジッタを入れても初期状態は変わっていない」ことを言えるようにするため
inline float SliceJitter(uint32_t frameIndex)
{
    uint32_t i = (frameIndex % kJitterSequenceLength) + 1u;
    float result = 0.0f;
    float f = 0.5f;
    while (i > 0u) {
        result += static_cast<float>(i & 1u) * f;
        i >>= 1;
        f *= 0.5f;
    }
    return result;
}

// スライス 1 枚ぶんの透過率 T = e^{-σ_t·d} (Beer-Lambert)
inline float SliceTransmittance(float sigmaT, float thickness)
{
    const float s = (sigmaT > 0.0f) ? sigmaT : 0.0f;
    const float d = (thickness > 0.0f) ? thickness : 0.0f;
    return std::exp(-s * d);
}

// スライス 1 枚を均質と見なしたときの散乱の解析積分 ∫₀^d e^{-σ_t·s} ds = (1-e^{-σ_t·d})/σ_t。
// ★ここを「厚み d をそのまま掛ける」で済ませてはいけない — 濃い霧で 1 スライス内の
//   自己遮蔽が消え、透過率が下がるほど明るくなるという逆向きの絵になる (Hillaire 2015)。
//   σ_t → 0 の極限はちょうど d なので、薄い霧では素朴な式と一致する
inline float IntegratedSliceScatter(float sigmaT, float thickness)
{
    const float s = (sigmaT > 0.0f) ? sigmaT : 0.0f;
    const float d = (thickness > 0.0f) ? thickness : 0.0f;
    if (s < 1e-5f) {
        return d; // 極限。ゼロ除算よけを兼ねる
    }
    return (1.0f - std::exp(-s * d)) / s;
}

// 積分結果ボリュームのサンプル座標 (w = 正規化スライス方向)。
// **格納規約: テクセル z には「スライス z の奥端 (= sliceCoord z+1) までの積分」が入る。**
// テクセル中心は sliceCoord z+0.5 の位置にあるので、任意の sliceCoord s を引くには
// 半テクセルぶん手前を指す必要がある: w = (s - 0.5) / count。
// ★この -0.5 を落とすと霧が 1 スライスぶん手前にずれる (スライスが薄い近景ほど効く)
inline float IntegratedSampleW(float sliceCoord, int sliceCount)
{
    const int count = (sliceCount > 0) ? sliceCount : 1;
    return (sliceCoord - 0.5f) / static_cast<float>(count);
}

// テンポラルの混合。feedback = 履歴の残し率 [0, kMaxTemporalFeedback]。
// ★histValid=false / feedback=0 は **厳密に現フレームそのまま** を返す —
//   ここがビット恒等でないと「テンポラル off で直前コミットとビット一致」という
//   このロードマップの受入基準が成立しない (lerp の丸めで最下位ビットが動く)
constexpr float kMaxTemporalFeedback = 0.95f;
inline float TemporalBlend(float cur, float hist, float feedback, bool histValid)
{
    if (!histValid || feedback <= 0.0f) {
        return cur;
    }
    const float f = (feedback > kMaxTemporalFeedback) ? kMaxTemporalFeedback : feedback;
    return cur + (hist - cur) * f; // HLSL の lerp(cur, hist, f) と同じ展開順
}

// ---- M57d: 最終画像への合成 (HLSL froxel_common.hlsli と同一式) ----

// Deferred 光パスが積分結果を読む SRV スロット (統合契約 予約 2 の t15)。
// **deferred_light.hlsl の register(t15) と食い違うと、霧が丸ごと 0 になるだけで
// コンパイルも実行も通る** (張られていないスロットは 0 を返す) ので、
// tools\check_rules.ps1 の規則 9 で機械照合している
constexpr int kSrvSlot = 15;

// view 深度 → 積分ボリュームの w 座標。
// 奥はグリッドの最終テクセル (= グリッド全体ぶんの霧) で止める — 最遠スライスより
// 奥の区間は「解析フォグの残り」が持つので、ここを外挿すると二重に霧が乗る。
// 手前は最初のテクセル中心で止める (w<0 は CLAMP サンプラでも同じ値になるが、
// 「意図して止めている」ことを式に残す。それが無いと 0.5 の由来が読めない)
inline float IntegratedSampleWForDepth(float viewZ, int sliceCount, float nearZ, float farZ)
{
    const int count = (sliceCount > 0) ? sliceCount : 1;
    const float hi = static_cast<float>(count);
    float s = ViewDepthToSlice(viewZ, count, nearZ, farZ);
    s = (s < 0.5f) ? 0.5f : ((s > hi) ? hi : s);
    return IntegratedSampleW(s, count);
}

// M57d: 解析フォグ (common.hlsli::ApplyFog) の起点をどこまで押し出すかの割合 [0,1]
// (カメラ → サーフェスの線分上のパラメータ)。
//
// ★**フォグ三重計上を解く鍵がこの 1 本**。フロクセルが持つのは [nearZ, gridFarZ] の
//   区間だけなので、解析フォグの起点を「グリッドの奥端とレイの交点」まで押し出せば
//   両者の受け持ちが 1m も重ならない。1.0 = サーフェスがグリッドの中 = 残り区間ゼロ =
//   起点が posW と厳密に一致 = ApplyFog は距離 0 で恒等になる。
//   グリッドの奥端は view 深度が gridFarZ の点なので、相似比は gridFarZ / viewZ
inline float FogHandoffFraction(float viewZ, float gridFarZ)
{
    if (gridFarZ <= 0.0f || !(viewZ > gridFarZ)) {
        return 1.0f; // グリッドの中 (NaN もここへ落ちる = 恒等側で安全に潰れる)
    }
    return gridFarZ / viewZ;
}

// M57d: 参加媒質の合成 (1 チャンネルぶん)。scene = 霧が無いときの色。
// transmittance==1 && inscatter==0 は**厳密に恒等** (積が 1 倍・和が 0 なので IEEE でも)
inline float CompositeFroxel(float scene, float inscatter, float transmittance)
{
    return scene * transmittance + inscatter;
}

// ---- M57e: 深度を持つ残りの描画物への適用 (HLSL froxel_common.hlsli と同一式) ----

// Forward 系 (forward_lit / _instanced / _skinned / _terrain) と**スカイボックス**が
// 積分結果を読む SRV スロット (統合契約 予約 2 の t7)。
//   ・Deferred の透明後段も forward_lit をそのまま使うので、この 1 本が「透明メッシュ」と
//     「Forward パス全体」の両方の口になる。
//   ・スカイボックスも同じ t7 を読む。**スカイに専用スロットを与えてはいけない** —
//     スカイは不透明と透明の間に挟まるパスなので、別スロットを張ると Forward で
//     後段の半透明が読む t1 (CSM) / t3-5 (IBL) を潰す。既に張ってある席に相乗りする。
// Deferred 光パス側の kSrvSlot と同じ理由で check_rules.ps1 規則 9 が機械照合する
constexpr int kForwardSrvSlot = 7;
// パーティクル PS のスロット (t0 = インスタンス / t1 = テクスチャ / t2 = 深度 の次)。
// パーティクルは独立したシェーダで、上の 4 本とはバインド空間を共有しない
constexpr int kParticleSrvSlot = 3;
// M57追補: **GPU** パーティクル PS のスロット。
// ★上の kParticleSrvSlot (=3) は**流用できない** — GPU バックエンドは
//   t0 = プール / t1 = alive list (どちらも VS 専用) / t2 = シーン深度 / t3 = フリップブック
//   で t3 が既に埋まっている (particle_render_gpu.hlsl)。t4 が最初の空き。
//   番号を揃えようとして CPU 側を動かすと、あちらの t3 剥がしと check_rules の既存
//   エントリまで巻き込むことになる。両者は別シェーダでバインド空間を共有しないので、
//   番号が違っていても実害は無い (規則は「自分が使っている最後の t の次」で 3 者共通)
constexpr int kGpuParticleSrvSlot = 4;

// スカイ / 背景 (= 深度が無いピクセル) 用のサンプル w。**グリッド全体ぶんの積分**を指す。
// ★ここは必ず IntegratedSampleWForDepth(viewZ >= farZ) と**同じ値**でなければならない —
//   食い違うと地平線で「最遠のジオメトリ」と「その真上の空」が別のテクセルを引き、
//   1 テクセルぶんの段が水平線に残る (RenderSelfTest が両者の一致を見ている)
inline float IntegratedSampleWFar(int sliceCount)
{
    const int count = (sliceCount > 0) ? sliceCount : 1;
    return IntegratedSampleW(static_cast<float>(count), count);
}

// M57e: **加算合成**の描画物 (additive パーティクル) への適用。
// ★内向き散乱を足してはいけない — 背後のサーフェス (またはスカイ) が既に 1 回足して
//   いるので、加算で重ねるたびに足すと粒子の枚数ぶん霧が濃くなる。
//   加算の粒子が受け取るのは「自分からカメラまでの減衰」だけ。
//   透過率 1 で厳密に恒等 (1 倍なので IEEE でも)
inline float CompositeFroxelAdditive(float src, float transmittance)
{
    return src * transmittance;
}

} // namespace froxel

} // namespace mye
