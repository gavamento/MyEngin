#include "Engine/Renderer/TerrainPass.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/GpuBufferUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

using namespace gpubuf;

// deferred_terrain.hlsl / forward_terrain.hlsl の TerrainObject (b4) と同一レイアウト。
// **末尾 append + 16 バイト境界**を守ること (増やすときは HLSL 側にも同じ順で足す)
struct TerrainObjectCB {
    XMFLOAT4X4 world;      // transpose 済み (HLSL は column_major 既定)
    XMFLOAT4 surfaceParams; // x=metallic y=roughness zw=予約
    // レイヤ表 (M58d)。rgb = リニア tint / a = 有効フラグ、xy = tiling / zw = 予約。
    // float4 に詰めているのは HLSL の配列が 16 バイト刻みでしか置けないため
    // (float2 の配列にすると 1 要素ごとに 8 バイトのパディングが入って C++ 側とずれる)
    XMFLOAT4 layerTint[kTerrainLayerCount];
    XMFLOAT4 layerTiling[kTerrainLayerCount];
};
static_assert(sizeof(TerrainObjectCB) == 208, "TerrainObjectCB must match the HLSL b4 layout");

// t20..t28 を 1 回で張る (splat + albedo x4 + normal x4)。スロットが連続していることが前提
constexpr uint32_t kTerrainSrvCount = 1 + kTerrainLayerCount * 2;
static_assert(kTerrainAlbedoSrvSlot == kTerrainSplatSrvSlot + 1
                  && kTerrainNormalSrvSlot == kTerrainAlbedoSrvSlot + kTerrainLayerCount,
              "terrain SRV slots must be contiguous (they are bound in one call)");

} // namespace

bool TerrainPass::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    deferredShader_ = shaders.Load("deferred_terrain");
    forwardShader_ = shaders.Load("forward_terrain");
    return CreateConstant(device.Device(), sizeof(TerrainObjectCB), objectCB_);
}

void TerrainPass::Shutdown()
{
    objectCB_.Reset();
}

void TerrainPass::RenderGBuffer(GraphicsDevice& device, ShaderManager& shaders,
                                const RenderView& view, RenderResources& resources)
{
    Draw(device, shaders.Get(deferredShader_), view, resources);
}

void TerrainPass::RenderForward(GraphicsDevice& device, ShaderManager& shaders,
                                const RenderView& view, RenderResources& resources)
{
    Draw(device, shaders.Get(forwardShader_), view, resources);
}

void TerrainPass::Draw(GraphicsDevice& device, ShaderProgram* prog, const RenderView& view,
                       RenderResources& resources)
{
    // 地形が 1 枚も無いフレームは**何も触らない** — ここで早期に返すことが
    // 「地形を置かないシーンは従来とビット一致」という受入基準の実体
    if (view.terrain == nullptr || view.terrain->items.empty()) {
        return;
    }
    if (prog == nullptr || !prog->valid || !objectCB_) {
        return;
    }
    ID3D11DeviceContext* dc = device.Context();
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = objectCB_.Get();
    dc->VSSetConstantBuffers(kTerrainObjectCbSlot, 1, &cb);
    dc->PSSetConstantBuffers(kTerrainObjectCbSlot, 1, &cb);

    // 欠けた AssetID の受け皿。地形の bind は 9 枚が常に埋まっている前提なので
    // (シェーダは分岐を持たない = 常に 9 枚サンプルする)、null は必ず白へ倒す
    ID3D11ShaderResourceView* white = nullptr;
    if (Texture* w = resources.textures.Get(resources.textures.White())) {
        white = w->srv.Get();
    }
    auto srvOf = [&](AssetID id) -> ID3D11ShaderResourceView* {
        Texture* t = resources.textures.Get(id);
        return (t != nullptr && t->srv) ? t->srv.Get() : white;
    };

    for (const TerrainRenderItem& item : view.terrain->items) {
        Mesh* mesh = resources.meshes.Get(item.mesh);
        if (mesh == nullptr) {
            continue;
        }
        // チャンクは 1 枚ごとに別メッシュなので bind のキャッシュは効かない (毎回張る)
        const UINT stride = sizeof(MeshVertex);
        const UINT offset = 0;
        ID3D11Buffer* vb = mesh->vb.Get();
        dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        dc->IASetIndexBuffer(mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);

        // ---- スプラット + レイヤ x4 の SRV (M58d) ----
        // サンプラは**ホストが張った s0 (異方性 WRAP) と s2 (LINEAR CLAMP) を流用**する。
        // 計画の付録「予約 2」が「サンプラは 1 つも増やさない」なので、地形も従う
        ID3D11ShaderResourceView* srvs[kTerrainSrvCount] = {};
        srvs[0] = srvOf(item.surface.splat);
        for (uint32_t l = 0; l < kTerrainLayerCount; ++l) {
            srvs[1 + l] = srvOf(item.surface.layers[l].albedo);
            srvs[1 + kTerrainLayerCount + l] = srvOf(item.surface.layers[l].normal);
        }
        dc->PSSetShaderResources(kTerrainSplatSrvSlot, kTerrainSrvCount, srvs);

        TerrainObjectCB oc = {};
        XMStoreFloat4x4(&oc.world, XMMatrixTranspose(XMLoadFloat4x4(&item.world)));
        oc.surfaceParams = { item.surface.metallic, item.surface.roughness, 0.0f, 0.0f };
        for (uint32_t l = 0; l < kTerrainLayerCount; ++l) {
            const TerrainLayerBinding& lb = item.surface.layers[l];
            oc.layerTint[l] = lb.tint; // TerrainSystem がリニアへ変換済み (a = 有効フラグ)
            oc.layerTiling[l] = { lb.tilingU, lb.tilingV, 0.0f, 0.0f };
        }
        UploadCB(dc, objectCB_.Get(), oc);
        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }

    // 借りた 9 枚を返す。ホストは t20 以降を一切知らないので、剥がさないと
    // フレームを跨いで残り続ける (次にそのテクスチャを RT にした瞬間に警告になる)
    ID3D11ShaderResourceView* nulls[kTerrainSrvCount] = {};
    dc->PSSetShaderResources(kTerrainSplatSrvSlot, kTerrainSrvCount, nulls);
}

} // namespace mye
