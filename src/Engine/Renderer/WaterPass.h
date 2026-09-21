#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/MeshBind.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct RenderResources;

// water_surface.hlsl の WaterMaterialParams (b2) と完全一致 (144 バイト = 16バイトアライメント)
struct alignas(16) WaterMaterialCB {
    DirectX::XMFLOAT4 deepColor = { 0.02f, 0.08f, 0.18f, 0.85f };
    DirectX::XMFLOAT4 shallowColor = { 0.10f, 0.35f, 0.45f, 0.60f };
    DirectX::XMFLOAT4 waveParams0 = { 0.18f, 24.0f, 0.8f, 20.0f }; // amp, wavelength, speed, dirAngleDeg
    DirectX::XMFLOAT4 waveParams1 = { 0.10f, 14.0f, 1.0f, 55.0f };
    DirectX::XMFLOAT4 waveParams2 = { 0.05f,  8.0f, 1.2f, -35.0f };
    DirectX::XMFLOAT4 waveParams3 = { 0.02f,  4.0f, 1.4f, 110.0f };
    DirectX::XMFLOAT4 waveSteepness = { 0.20f, 0.18f, 0.15f, 0.12f };
    DirectX::XMFLOAT4 waterSettings = { 0.0f, 1.0f, 0.0f, 0.15f }; // baseHeight, overallScale, time, foamStrength
    DirectX::XMFLOAT4 waterOptics = { 4.0f, 0.95f, 0.0f, 0.0f };   // fresnelPower, smoothness, pad, pad
};

// RenderView::water が指す純描画データ (Renderer 層の純データ)
struct WaterDrawData {
    DirectX::XMFLOAT4X4 world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    WaterMaterialCB material = {};
    bool active = false;
};

class WaterPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    // 水面の描画処理。ForwardPath / DeferredPath から呼び出す。
    // view.water が null または非アクティブの場合は即座に return。
    // perFrameCB: スロット b0 に載せる PerFrame 定数バッファ (Deferred では光パスが上書きするため必須)。
    // rtReflSRV: Deferred の RT 反射テクスチャ (rtRefl.filtered)。null の場合は IBL 反射のみ使用。
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                RenderResources& resources, ID3D11Buffer* perFrameCB = nullptr,
                ID3D11ShaderResourceView* rtReflSRV = nullptr);

private:
    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> objectCB_;   // PerObjectCB (b1)
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialCB_; // WaterMaterialCB (b2)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendTransparent_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerCullNone_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerWire_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerShadow_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerClamp_;
};

} // namespace mye
