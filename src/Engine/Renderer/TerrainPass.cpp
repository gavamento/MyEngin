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
// **末尾 append + 16 バイト境界**を守ること (既定値 = 恒等の規約は色に無いので、
// 増やすときは HLSL 側にも同じ順で足す)
struct TerrainObjectCB {
    XMFLOAT4X4 world;    // transpose 済み (HLSL は column_major 既定)
    XMFLOAT4 baseColor;  // リニア
    float metallic;
    float roughness;
    float pad[2];
};
static_assert(sizeof(TerrainObjectCB) == 96, "TerrainObjectCB must match the HLSL b4 layout");

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

        TerrainObjectCB oc = {};
        XMStoreFloat4x4(&oc.world, XMMatrixTranspose(XMLoadFloat4x4(&item.world)));
        oc.baseColor = SrgbToLinear(item.baseColor); // M38a: authored 色をリニアへ
        oc.metallic = item.metallic;
        oc.roughness = item.roughness;
        UploadCB(dc, objectCB_.Get(), oc);
        dc->DrawIndexed(mesh->indexCount, 0, 0);
        prof::AddDraw(static_cast<int>(mesh->indexCount / 3));
    }
}

} // namespace mye
