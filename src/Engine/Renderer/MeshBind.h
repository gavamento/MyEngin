#pragma once
#include <cstdint>

#include <DirectXMath.h>
#include <d3d11.h>

#include "Engine/Renderer/GpuResources.h"

namespace mye {

// ---- メッシュ 1 個を描くときのバインドと定数バッファ (ForwardPath と DeferredPath で共有) ----
// Forward の不透明 / インスタンス、Deferred の GBuffer / インスタンス / 透明後段の 5 か所が同じ手順で描く。
// 手順の中で意味のある違いは**ノーマルマップのスロットだけ**なので、それを引数に出して残りを 1 本にした

// forward_lit.hlsl / deferred_gbuffer.hlsl の PerObject (b1) と一致
struct PerObjectCB {
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4 baseColor;
    // ---- インスタンシング (M38f、末尾 append)。インスタンス版シェーダのみ参照 ----
    int32_t instanceBase;
    // 汎用タグ: RT を受ける面か (1/0)。GBuffer 系シェーダ 3 本だけが読み、material.a へ書く。
    // ★PerObjectCB は `po = {}` で作られるので既定は 0 — GBuffer へ描く 3 か所は必ず
    //   RenderItem::rtReceiver を明示的に入れること (入れ忘れると RT が全部の面で消える)
    float rtReceiver;
    float instPad[2];
};

// forward_lit.hlsl / deferred_gbuffer.hlsl の MaterialParams (b2) と一致 (16 バイト)
struct MaterialCB {
    float metallic;
    float roughness;
    int32_t hasNormal; // 0=ノーマルマップ無し
    float emissive;    // M46i: 自己発光の強さ (0 = 発光なし)
};

// ノーマルマップのスロット。★パスで違う — forward_lit は t2 (t1 は影)、GBuffer は t1。
//   Deferred の透明後段は forward_lit をそのまま使うので t2 側
inline constexpr UINT kForwardNormalSlot = 2;
inline constexpr UINT kGBufferNormalSlot = 1;

// 直前に張ったものを覚えておき、同じなら張り直さない (同じ材質・同じメッシュが続くと呼び出しが省ける)
struct MeshBindState {
    uint64_t mesh = 0;
    uint64_t texture = 0;
    uint64_t normal = 0;
};

// アルベドを t0、ノーマルマップを normalSlot へ張る。無ければ White (シェーダは hasNormal で使用可否を見る)
inline void BindMaterialTextures(ID3D11DeviceContext* dc, TextureLibrary& textures, const Material& mat,
                                 UINT normalSlot, MeshBindState& bound)
{
    const AssetID texId = mat.texture.IsNull() ? textures.White() : mat.texture;
    if (texId.value != bound.texture) {
        Texture* tex = textures.Get(texId);
        ID3D11ShaderResourceView* srv = tex ? tex->srv.Get() : nullptr;
        dc->PSSetShaderResources(0, 1, &srv);
        bound.texture = texId.value;
    }
    const AssetID nrmId = mat.normalTex.IsNull() ? textures.White() : mat.normalTex;
    if (nrmId.value != bound.normal) {
        Texture* ntex = textures.Get(nrmId);
        ID3D11ShaderResourceView* nsrv = ntex ? ntex->srv.Get() : nullptr;
        dc->PSSetShaderResources(normalSlot, 1, &nsrv);
        bound.normal = nrmId.value;
    }
}

// 同じメッシュが続く間は張り直さない (bound.mesh と比べる)
inline void BindMeshBuffers(ID3D11DeviceContext* dc, const Mesh& mesh, AssetID meshId, MeshBindState& bound)
{
    if (meshId.value != bound.mesh) {
        const UINT stride = sizeof(MeshVertex);
        const UINT offset = 0;
        ID3D11Buffer* vb = mesh.vb.Get();
        dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        dc->IASetIndexBuffer(mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        bound.mesh = meshId.value;
    }
}

inline MaterialCB MakeMaterialCB(const Material& mat)
{
    MaterialCB mc = {};
    mc.metallic = mat.metallic;
    mc.roughness = mat.roughness;
    mc.hasNormal = mat.normalTex.IsNull() ? 0 : 1;
    mc.emissive = mat.emissiveIntensity;
    return mc;
}

} // namespace mye
