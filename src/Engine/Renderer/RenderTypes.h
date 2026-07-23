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
    // ---- メッシュ GPU インスタンシング (M38f)。0 = 全て per-item 描画 (A/B 比較用) ----
    int32_t instancingEnabled = 1;
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
    float pad0 = 0.0f;
    float pad1 = 0.0f;
};
static_assert(sizeof(GpuLight) == 64, "GpuLight must match HLSL 16-byte packing");

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
