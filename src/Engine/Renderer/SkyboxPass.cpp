#include "Engine/Renderer/SkyboxPass.h"

#include <cstring>

#include <DirectXMath.h>

#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

// skybox.hlsl / skybox_cubemap.hlsl の SkyCB (b3) と同一レイアウト
struct SkyCB {
    XMFLOAT4X4 invViewProj; // transpose(inverse(view*proj))
    XMFLOAT4 top;
    XMFLOAT4 horizon;
    XMFLOAT4 bottom;
    // ---- M57e: フロクセル (末尾 append。x=0 = 従来と 1 ビットも変わらない) ----
    XMFLOAT4 froxel;       // x = enabled / y = スライス数 / zw = 未使用
    XMFLOAT4 froxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
};

} // namespace

bool SkyboxPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("skybox");
    shaderCube_ = shaders.Load("skybox_cubemap"); // M38b

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, sampler_.GetAddressOf()))) {
        return false;
    }

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
    // M38b: cubemap モード (SRV が揃っている時のみ。無ければ gradient にフォールバック)
    const bool useCube = (view.skyMode == 1) && (view.skyCubemap != nullptr);
    ShaderProgram* prog = shaders.Get(useCube ? shaderCube_ : shader_);
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
    // M57e: 空にもフロクセルを載せる。空は深度を持たないので「グリッド全体ぶん」を引く。
    // ★SRV もサンプラも**ここでは張らない** — t7 / s2 はホストのパスが張ったものを
    //   そのまま読む。スカイは不透明と透明の間に挟まるパスなので、ここで別スロットを
    //   触ると後段の半透明メッシュへ漏れる (Forward の t1 = CSM を潰した形で顕在化する)。
    //   Deferred は光パスの後で t0-t15 を剥がしているので、呼ぶ側が t7 を張り直している
    const bool froxelBound = FroxelIsBound(view);
    cb.froxel = { froxelBound ? 1.0f : 0.0f, static_cast<float>(view.froxelSlices), 0.0f, 0.0f };
    cb.froxelScreen = { static_cast<float>(view.width), static_cast<float>(view.height), 0.0f,
                        0.0f };
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
    if (useCube) {
        ID3D11ShaderResourceView* srvs[1] = { view.skyCubemap };
        dc->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samps[1] = { sampler_.Get() };
        dc->PSSetSamplers(0, 1, samps);
    }
    dc->OMSetDepthStencilState(depthReadOnly_.Get(), 0);
    dc->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xFFFFFFFFu);
    // ラスタライザは呼び出し元のメッシュ用設定を継承する (deferred_light と同じ流儀。
    // フルスクリーン三角形は既存設定で front-facing になる頂点列)
    dc->Draw(3, 0);
    if (useCube) {
        // TextureCube を t0 に残さない (後段は Texture2D を bind する — デバッグレイヤ警告回避)
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        dc->PSSetShaderResources(0, 1, nullSrv);
    }
}

} // namespace mye
