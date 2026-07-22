#include "Engine/Renderer/SkyboxPass.h"

#include <cstring>

#include <DirectXMath.h>

#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// skybox.hlsl の SkyCB (b3) と同一レイアウト
struct SkyCB {
    XMFLOAT4X4 invViewProj; // transpose(inverse(view*proj))
    XMFLOAT4 top;
    XMFLOAT4 horizon;
    XMFLOAT4 bottom;
};

} // namespace

bool SkyboxPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("skybox");

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(SkyCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    // 深度 1.0 のピクセル (= ジオメトリ無し) だけ通す。書き込みはしない
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&dd, depthReadOnly_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bld = {}; // 不透明 (ブレンド無し)
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blendOpaque_.GetAddressOf()))) {
        return false;
    }

    ready_ = true;
    return true;
}

void SkyboxPass::Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view)
{
    if (!ready_ || view.skyMode < 0 || !view.rtv || !view.dsv) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();

    SkyCB cb = {};
    const XMMATRIX v = XMLoadFloat4x4(&view.view);
    const XMMATRIX p = XMLoadFloat4x4(&view.proj);
    const XMMATRIX inv = XMMatrixInverse(nullptr, XMMatrixMultiply(v, p));
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(inv));
    cb.top = { view.skyTop.x, view.skyTop.y, view.skyTop.z, 1.0f };
    cb.horizon = { view.skyHorizon.x, view.skyHorizon.y, view.skyHorizon.z, 1.0f };
    cb.bottom = { view.skyBottom.x, view.skyBottom.y, view.skyBottom.z, 1.0f };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        dc->Unmap(cb_.Get(), 0);
    }

    // 深度テストのため RTV+DSV を再バインド (deferred のライトパス後は DSV が外れている)
    dc->OMSetRenderTargets(1, &view.rtv, view.dsv);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->PSSetConstantBuffers(3, 1, cbs); // b3 (b0-b2 はメッシュ描画が使用中)
    dc->OMSetDepthStencilState(depthReadOnly_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    // ラスタライザは呼び出し元のメッシュ用設定を継承する (deferred_light と同じ流儀。
    // フルスクリーン三角形は既存設定で front-facing になる頂点列)
    dc->Draw(3, 0);
}

} // namespace mye
