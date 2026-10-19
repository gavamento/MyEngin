//====================================================================================
//                          Sprite2DPass.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D (2D スプライト) の描画パス
//====================================================================================
#pragma once
#include <cstdint>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Passes/Sprite2DDrawList.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class TextureLibrary;

// Sprite2DRenderer が並べた描画リストを、パスがバインド済みの HDR シーン RT へ重ねる (VfxRenderer と同じ位置)。
// ブレンド SRC_ALPHA / INV_SRC_ALPHA、ZTest LEqual・ZWrite off、CULL_NONE。サンプラは point / linear の 2 つを自前で持ち、
// バッチごとに切り替える。バッチが 0 件なら Draw を 1 回も出さない。
// レイヤ規約: 生 D3D11 はこの Renderer 層に閉じる。決定論規約: sim には触れない
class Sprite2DPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // RT / ビューポートはバインド済みのものを使う。描画したバッチ数を返す (SRV が引けないバッチは飛ばす)
    uint32_t Render(GraphicsDevice& device, ShaderManager& shaders, TextureLibrary& textures,
                    const Sprite2DDrawList& list, const DirectX::XMFLOAT4X4& view,
                    const DirectX::XMFLOAT4X4& proj);

private:
    bool Upload(GraphicsDevice& device, const Sprite2DDrawList& list);

    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0; // 頂点数
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPoint_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
};

} // namespace mye
