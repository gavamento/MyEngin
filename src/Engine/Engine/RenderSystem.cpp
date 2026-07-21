#include "Engine/Engine/RenderSystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/RenderPath.h"

using namespace DirectX;

namespace mye {
namespace {

// フラスタムカリング候補 (M25: 収集は直列、可視判定 + viewZ を並列、キュー構築は直列)。
struct CullCand {
    EntityID e;
    AssetID mesh;
    AssetID material;
    XMFLOAT4X4 world;
    const Mesh* meshPtr;
    float viewZ;
    uint8_t visible;
};

constexpr size_t kCullGrain = 256; // これ未満は直列 (スレッド起動コスト回避)

// 平行光の view-proj (行ベクトル規約 world*view*proj)。シーン AABB にフィットした正射影。
// 戻り値は非転置 (ShadowPass が world と合成、RenderView 用に別途転置する)。
XMFLOAT4X4 ComputeDirectionalLightVP(const XMFLOAT3& lightDir, const XMFLOAT3& sceneMin,
                                     const XMFLOAT3& sceneMax)
{
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMFLOAT3 cf = { (sceneMin.x + sceneMax.x) * 0.5f, (sceneMin.y + sceneMax.y) * 0.5f,
                          (sceneMin.z + sceneMax.z) * 0.5f };
    const XMVECTOR center = XMLoadFloat3(&cf);
    const XMFLOAT3 ext = { sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y,
                           sceneMax.z - sceneMin.z };
    const float radius = 0.5f * std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
    const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(dir, radius * 2.0f + 1.0f));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    const XMFLOAT3 corners[8] = {
        { sceneMin.x, sceneMin.y, sceneMin.z }, { sceneMax.x, sceneMin.y, sceneMin.z },
        { sceneMin.x, sceneMax.y, sceneMin.z }, { sceneMax.x, sceneMax.y, sceneMin.z },
        { sceneMin.x, sceneMin.y, sceneMax.z }, { sceneMax.x, sceneMin.y, sceneMax.z },
        { sceneMin.x, sceneMax.y, sceneMax.z }, { sceneMax.x, sceneMax.y, sceneMax.z },
    };
    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
    for (const XMFLOAT3& c : corners) {
        const XMVECTOR vv = XMVector3TransformCoord(XMLoadFloat3(&c), lightView);
        const float x = XMVectorGetX(vv), y = XMVectorGetY(vv), z = XMVectorGetZ(vv);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
    }
    const float margin = radius * 0.05f + 0.5f;
    const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX - margin, maxX + margin,
                                                               minY - margin, maxY + margin,
                                                               std::max(0.05f, minZ - 1.0f),
                                                               maxZ + 1.0f);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(lightView, lightProj));
    return out;
}

} // namespace

bool RenderSystem::Render(World& world, GraphicsDevice& device, IRenderPath& path,
                          ShaderManager& shaders, RenderResources& resources,
                          const FrameTarget& target, const CameraOverride* cameraOverride,
                          ParticleSystem* particles)
{
    RenderView view;
    view.dsv = target.dsv;
    view.width = target.width;
    view.height = target.height;
    memcpy(view.clearColor, target.clearColor, sizeof(view.clearColor));

    // ---- HDR ポストプロセス経路 (M16) ----
    // シーン + パーティクルを HDR 中間 (R16F, color のみ) へ描き、最後にトーンマップ解決で
    // target.rtv へ書く。depth は target.dsv を共有するため、解決後にエディタが重ねる
    // ギズモ/線 (rt_.DSV() 利用) の深度テストは従来どおり成立する。
    if (!postFx_.IsReady()) {
        postFx_.Init(device, shaders);
    }
    PostProcess::Target* hdr = nullptr;
    if (enablePostFx && postFx_.IsReady()) {
        hdr = postFx_.Acquire(device, target.width, target.height);
    }
    view.rtv = (hdr != nullptr) ? hdr->scene.RTV() : target.rtv; // 確保失敗時は従来直描きにフォールバック

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

    // ---- ライト (最大 kMaxLights 個収集。Directional/Point/Spot) ----
    SceneLightData lights;
    {
        const ComponentTypeId req[] = { LightComponent::sTypeId, WorldMatrixComponent::sTypeId };
        bool ambientSet = false;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(LightComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (lights.count >= kMaxLights) {
                    return;
                }
                if (!IsEntityActive(world, arch.EntityAt(row))) {
                    continue; // 無効化されたライトは寄与しない
                }
                const auto* l = static_cast<const LightComponent*>(arch.GetPtr(li, row));
                const auto* w = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                GpuLight& g = lights.lights[lights.count++];
                // ワールド行列の第 3 行 = ローカル +Z の向き、第 4 行 = 位置
                XMVECTOR dir = XMVector3Normalize(
                    XMVectorSet(w->value._31, w->value._32, w->value._33, 0));
                XMStoreFloat3(&g.direction, dir);
                g.position = { w->value._41, w->value._42, w->value._43 };
                g.color = l->color;
                g.intensity = l->intensity;
                g.type = l->type;
                g.range = l->range;
                g.cosInner = std::cos(XMConvertToRadians(l->spotInnerDeg));
                g.cosOuter = std::cos(XMConvertToRadians(l->spotOuterDeg));
                if (!ambientSet) {
                    lights.ambient = l->ambient; // アンビエントは最初のライトの値を全体に使う
                    ambientSet = true;
                }
            }
        });
        // ライトが 1 つも無いシーンでも見えるよう、既定の平行光を 1 つ補う (従来挙動)
        if (lights.count == 0) {
            GpuLight& g = lights.lights[lights.count++];
            XMStoreFloat3(&g.direction,
                          XMVector3Normalize(XMVectorSet(0.3f, -0.8f, 0.5f, 0)));
            g.type = 0;
        }
    }

    // ---- 収集 ----
    queue_.Clear();
    skinPalettes_.clear(); // スキンメッシュのボーンパレット (M18、フレーム毎に再構築)
    {
        const XMMATRIX v = XMLoadFloat4x4(&view.view);
        // フラスタムカリング用の視錐台 (カメラがある時のみ。描画専用でハッシュ非対象 — M16)
        Frustum frustum = {};
        const bool cullEnabled = cameraFound;
        if (cullEnabled) {
            XMFLOAT4X4 vp;
            XMStoreFloat4x4(&vp, v * XMLoadFloat4x4(&view.proj));
            frustum = BuildFrustum(vp);
        }
        int culledCount = 0;
        // 不透明キャスターの world AABB を集約 → シャドウ範囲のフィットに使う (M17)
        XMFLOAT3 sceneMin = { FLT_MAX, FLT_MAX, FLT_MAX };
        XMFLOAT3 sceneMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        bool hasScene = false;

        // ---- ステージ 1 (直列): 候補を収集 (順序 = ForEachArchetype/row = 決定的) ----
        std::vector<CullCand> cullCands;
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
                cullCands.push_back({ arch.EntityAt(row), mr->mesh, mr->material, wm->value,
                                       resources.meshes.Get(mr->mesh), 0.0f, 1 });
            }
        });

        // ---- ステージ 2 (並列): 視錐台テスト + viewZ (要素独立・純関数、M25) ----
        jobs::System().ParallelRanges(cullCands.size(), kCullGrain, [&](size_t a, size_t b) {
            for (size_t i = a; i < b; ++i) {
                CullCand& c = cullCands[i];
                if (cullEnabled && c.meshPtr
                    && !AabbInFrustum(frustum, c.world, c.meshPtr->aabbMin, c.meshPtr->aabbMax)) {
                    c.visible = 0;
                    continue;
                }
                const XMVECTOR posWS = XMVectorSet(c.world._41, c.world._42, c.world._43, 1);
                c.viewZ = XMVectorGetZ(XMVector3TransformCoord(posWS, v));
            }
        });

        // ---- ステージ 3 (直列): 可視候補をキュー化 (スキン/AABB/キューは順序依存で直列) ----
        for (const CullCand& c : cullCands) {
            if (!c.visible) {
                ++culledCount;
                continue;
            }
            RenderItem item;
            item.mesh = c.mesh;
            item.material = c.material;
            item.world = c.world;
            item.viewZ = c.viewZ;

            // スキンメッシュ (M18): ポーズを評価してボーンパレットを構築し item に載せる。
            // ポーズは描画専用 (SkinnedMeshComponent は kComponentNoHash)
            if (auto* sm = world.GetComponent<SkinnedMeshComponent>(c.e)) {
                if (const SkinnedModel* model = resources.skinnedModels.Get(sm->model)) {
                    skinPalettes_.emplace_back();
                    std::vector<XMFLOAT4X4>& palette = skinPalettes_.back();
                    const float timeSec = static_cast<float>(sm->timeTicks) / 60.0f;
                    ComputeBonePalette(*model, sm->clip, timeSec, palette);
                    if (palette.size() > 64) { // kMaxBones (シェーダ MYE_MAX_BONES と一致)
                        palette.resize(64);
                    }
                    item.bones = palette.data();
                    item.boneCount = static_cast<int32_t>(palette.size());
                }
            }

            const Material* mat = resources.materials.Get(c.material);
            if (mat && mat->transparent != 0) {
                queue_.transparent.push_back(item);
            } else {
                queue_.opaque.push_back(item);
                if (c.meshPtr) {
                    XMFLOAT3 wmin, wmax;
                    WorldAabb(c.world, c.meshPtr->aabbMin, c.meshPtr->aabbMax, wmin, wmax);
                    sceneMin = { std::min(sceneMin.x, wmin.x), std::min(sceneMin.y, wmin.y),
                                 std::min(sceneMin.z, wmin.z) };
                    sceneMax = { std::max(sceneMax.x, wmax.x), std::max(sceneMax.y, wmax.y),
                                 std::max(sceneMax.z, wmax.z) };
                    hasScene = true;
                }
            }
        }
        prof::AddCulled(culledCount);

        // ---- シャドウパス (M17): 最初の平行光でシーンにフィットした深度マップを描く ----
        if (!shadowPass_.IsReady()) {
            shadowPass_.Init(device, shaders);
        }
        view.shadowSRV = nullptr;
        if (enableShadows && shadowPass_.IsReady() && hasScene) {
            int dirIdx = -1;
            for (int i = 0; i < lights.count; ++i) {
                if (lights.lights[i].type == 0) {
                    dirIdx = i;
                    break;
                }
            }
            if (dirIdx >= 0) {
                const XMFLOAT4X4 lightVP =
                    ComputeDirectionalLightVP(lights.lights[dirIdx].direction, sceneMin, sceneMax);
                shadowPass_.Render(device, shaders, queue_, resources, lightVP);
                XMStoreFloat4x4(&view.lightViewProj,
                                XMMatrixTranspose(XMLoadFloat4x4(&lightVP)));
                view.shadowSRV = shadowPass_.SRV();
                view.shadowTexelSize = 1.0f / static_cast<float>(shadowPass_.Resolution());
            }
        }
    }

    queue_.Sort();
    path.Render(device, view, queue_, lights, resources, shaders);

    // パーティクルは常に Forward 後段 (どのレンダリングパスでも共通)。HDR 中間へ加算される
    if (particles) {
        particles->Render(device, view, shaders);
    }

    // HDR → LDR 解決 (トーンマップ)。HDR 中間を使った時のみ
    if (hdr != nullptr) {
        postFx_.Resolve(device, shaders, *hdr, target.rtv, target.width, target.height,
                        postFxSettings);
    }
    return cameraFound;
}

} // namespace mye
