#include "Engine/Engine/RenderSystem.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/RenderPath.h"

using namespace DirectX;

namespace mye {

bool RenderSystem::Render(World& world, GraphicsDevice& device, IRenderPath& path,
                          ShaderManager& shaders, RenderResources& resources,
                          const FrameTarget& target, const CameraOverride* cameraOverride,
                          ParticleSystem* particles)
{
    RenderView view;
    view.rtv = target.rtv;
    view.dsv = target.dsv;
    view.width = target.width;
    view.height = target.height;
    memcpy(view.clearColor, target.clearColor, sizeof(view.clearColor));

    const float aspectRatio = (target.height > 0)
        ? static_cast<float>(target.width) / static_cast<float>(target.height) : 1.0f;

    // ---- カメラ ----
    bool cameraFound = false;
    if (cameraOverride) {
        view.view = cameraOverride->view;
        const XMMATRIX p = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(cameraOverride->fovYDeg), aspectRatio,
            cameraOverride->nearZ, cameraOverride->farZ);
        XMStoreFloat4x4(&view.proj, p);
        view.cameraPos = cameraOverride->position;
        cameraFound = true;
    } else {
        const ComponentTypeId req[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
        XMFLOAT4X4 camWorld = {};
        CameraComponent cam = {};
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
                if (!cameraFound || c->isPrimary != 0) {
                    cam = *c;
                    camWorld = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                    cameraFound = true;
                    if (c->isPrimary != 0) {
                        return;
                    }
                }
            }
        });
        if (cameraFound) {
            const XMMATRIX w = XMLoadFloat4x4(&camWorld);
            const XMMATRIX v = XMMatrixInverse(nullptr, w);
            const float aspect = (target.height > 0)
                ? static_cast<float>(target.width) / static_cast<float>(target.height) : 1.0f;
            const XMMATRIX p = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(cam.fovYDeg), aspect, cam.nearZ, cam.farZ);
            XMStoreFloat4x4(&view.view, v);
            XMStoreFloat4x4(&view.proj, p);
            view.cameraPos = { camWorld._41, camWorld._42, camWorld._43 };
        } else {
            XMStoreFloat4x4(&view.view, XMMatrixIdentity());
            XMStoreFloat4x4(&view.proj, XMMatrixIdentity());
        }
    }

    // ---- ライト ----
    DirectionalLightData light;
    {
        const ComponentTypeId req[] = { LightComponent::sTypeId, WorldMatrixComponent::sTypeId };
        bool found = false;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            if (found) {
                return;
            }
            const int li = arch.FindTypeIndex(LightComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            if (arch.Count() > 0) {
                const auto* l = static_cast<const LightComponent*>(arch.GetPtr(li, 0));
                const auto* w = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, 0));
                // ワールド行列の第 3 行 = ローカル +Z の向き
                XMVECTOR dir = XMVectorSet(w->value._31, w->value._32, w->value._33, 0);
                dir = XMVector3Normalize(dir);
                XMStoreFloat3(&light.dir, dir);
                light.color = l->color;
                light.intensity = l->intensity;
                light.ambient = l->ambient;
                found = true;
            }
        });
    }

    // ---- 収集 ----
    queue_.Clear();
    {
        const XMMATRIX v = XMLoadFloat4x4(&view.view);
        const ComponentTypeId req[] = { MeshRendererComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (!IsEntityActive(world, arch.EntityAt(row))) {
                    continue; // 無効エンティティは描画しない (M10)
                }
                const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
                if (mr->mesh.IsNull() || mr->material.IsNull()) {
                    continue;
                }
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                RenderItem item;
                item.mesh = mr->mesh;
                item.material = mr->material;
                item.world = wm->value;
                const XMVECTOR posWS = XMVectorSet(wm->value._41, wm->value._42, wm->value._43, 1);
                item.viewZ = XMVectorGetZ(XMVector3TransformCoord(posWS, v));

                const Material* mat = resources.materials.Get(mr->material);
                if (mat && mat->transparent != 0) {
                    queue_.transparent.push_back(item);
                } else {
                    queue_.opaque.push_back(item);
                }
            }
        });
    }

    queue_.Sort();
    path.Render(device, view, queue_, light, resources, shaders);

    // パーティクルは常に Forward 後段 (どのレンダリングパスでも共通)
    if (particles) {
        particles->Render(device, view, shaders);
    }
    return cameraFound;
}

} // namespace mye
