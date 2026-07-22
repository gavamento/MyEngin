#include "Engine/Engine/RenderSystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "Engine/Core/Components.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Vfx/VfxRenderer.h"
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

// 前 tick → 現 tick のワールド行列を成分 lerp する (M36b 描画補間)。
// 平行移動は厳密 lerp、回転 3x3 は成分 lerp — 1/60s の姿勢差では非直交化は不可視。
// render-only なので float 誤差は sim/hash に無関係
XMFLOAT4X4 LerpWorld(const XMFLOAT4X4& a, const XMFLOAT4X4& b, float t)
{
    XMFLOAT4X4 o;
    const float* pa = &a._11;
    const float* pb = &b._11;
    float* po = &o._11;
    for (int i = 0; i < 16; ++i) {
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
    return o;
}

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

// CSM のカスケード VP 列 (M38d)。カメラの部分フラスタム 8 隅をライトビューへ射影して
// フィットした ortho を作る。practical split (λ=0.5)、影距離は kShadowMaxDist まで。
// テクセルスナップで安定化。ライト方向のキャスター (フラスタム外) を拾うため
// 近平面はシーン AABB のライト空間 z まで引き戻す。
// 非 perspective (エディタ Ortho ビュー) は従来のシーン全体フィットを全カスケードに複製。
constexpr float kShadowMaxDist = 60.0f;

void ComputeCascadeVPs(const XMFLOAT3& lightDir, const XMFLOAT3& sceneMin,
                       const XMFLOAT3& sceneMax, const RenderView& view, int resolution,
                       XMFLOAT4X4* outVPs, float* outSplits, int count)
{
    const XMMATRIX camView = XMLoadFloat4x4(&view.view);
    const XMMATRIX camProj = XMLoadFloat4x4(&view.proj);
    XMFLOAT4X4 pj;
    XMStoreFloat4x4(&pj, camProj);
    const bool perspective = std::fabs(pj._34 - 1.0f) < 1e-3f; // LH perspective は _34 == 1

    if (!perspective || resolution <= 0) {
        const XMFLOAT4X4 whole = ComputeDirectionalLightVP(lightDir, sceneMin, sceneMax);
        for (int c = 0; c < count; ++c) {
            outVPs[c] = whole;
            outSplits[c] = kShadowMaxDist;
        }
        return;
    }

    // 射影行列から near/far を復元 (行ベクトル規約: zNdc = P33 + P43/viewZ)
    const float nearZ = (std::fabs(pj._33) > 1e-6f) ? (-pj._43 / pj._33) : 0.1f;
    const float rawFar = (std::fabs(1.0f - pj._33) > 1e-6f) ? (pj._43 / (1.0f - pj._33)) : 1000.0f;
    const float farZ = std::min(std::max(rawFar, nearZ + 1.0f), kShadowMaxDist);
    ComputeCascadeSplits(nearZ, farZ, count, 0.5f, outSplits);

    // 共有ライトビュー (原点はシーン中心)
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMFLOAT3 cf = { (sceneMin.x + sceneMax.x) * 0.5f, (sceneMin.y + sceneMax.y) * 0.5f,
                          (sceneMin.z + sceneMax.z) * 0.5f };
    const XMFLOAT3 ext = { sceneMax.x - sceneMin.x, sceneMax.y - sceneMin.y,
                           sceneMax.z - sceneMin.z };
    const float radius = 0.5f * std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
    const XMVECTOR eye =
        XMVectorSubtract(XMLoadFloat3(&cf), XMVectorScale(dir, radius * 2.0f + 1.0f));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    // シーン AABB のライト空間 z 範囲 (フラスタム外キャスターの取りこぼし防止)
    float sceneMinZ = FLT_MAX, sceneMaxZ = -FLT_MAX;
    for (int i = 0; i < 8; ++i) {
        const XMFLOAT3 c = { (i & 1) ? sceneMax.x : sceneMin.x, (i & 2) ? sceneMax.y : sceneMin.y,
                             (i & 4) ? sceneMax.z : sceneMin.z };
        const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&c), lightView));
        sceneMinZ = std::min(sceneMinZ, z);
        sceneMaxZ = std::max(sceneMaxZ, z);
    }

    const XMMATRIX invVP = XMMatrixInverse(nullptr, XMMatrixMultiply(camView, camProj));
    auto ndcZ = [&](float viewZ) { return pj._33 + pj._43 / viewZ; };
    float splitNear = nearZ;
    for (int c = 0; c < count; ++c) {
        const float splitFar = outSplits[c];
        // 部分フラスタムの 8 隅 (NDC → ワールド)
        float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
        float minZ = FLT_MAX, maxZ = -FLT_MAX;
        for (int i = 0; i < 8; ++i) {
            const float nx = (i & 1) ? 1.0f : -1.0f;
            const float ny = (i & 2) ? 1.0f : -1.0f;
            const float nz = (i & 4) ? ndcZ(splitFar) : ndcZ(splitNear);
            const XMVECTOR w = XMVector3TransformCoord(XMVectorSet(nx, ny, nz, 0), invVP);
            const XMVECTOR lv = XMVector3TransformCoord(w, lightView);
            const float x = XMVectorGetX(lv), y = XMVectorGetY(lv), z = XMVectorGetZ(lv);
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
        }
        // テクセルスナップ (カメラ移動でのシャドウエッジのちらつき防止)
        const float margin = 1.0f;
        minX -= margin; maxX += margin;
        minY -= margin; maxY += margin;
        const float texelX = (maxX - minX) / static_cast<float>(resolution);
        const float texelY = (maxY - minY) / static_cast<float>(resolution);
        if (texelX > 0.0f && texelY > 0.0f) {
            minX = std::floor(minX / texelX) * texelX;
            minY = std::floor(minY / texelY) * texelY;
            maxX = std::floor(maxX / texelX) * texelX;
            maxY = std::floor(maxY / texelY) * texelY;
        }
        // 近平面はシーン AABB まで引き戻す (ライト方向の手前にいるキャスターを含める)
        const float zNear = std::min(minZ, sceneMinZ) - 1.0f;
        const float zFar = std::min(maxZ + 1.0f, sceneMaxZ + 1.0f) + 1.0f;
        const XMMATRIX lightProj =
            XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, zNear, std::max(zFar, zNear + 1.0f));
        XMStoreFloat4x4(&outVPs[c], XMMatrixMultiply(lightView, lightProj));
        splitNear = splitFar;
    }
}

} // namespace

void CollectEnvironment(World& world, RenderView& view)
{
    // 最初 (entity.index 最小) の active な Skybox
    uint32_t bestSky = 0xFFFFFFFFu;
    const ComponentTypeId skyReq[] = { SkyboxComponent::sTypeId };
    world.ForEachArchetype(skyReq, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SkyboxComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestSky || !IsEntityActive(world, e)) {
                continue;
            }
            bestSky = e.index;
            const auto* sb = static_cast<const SkyboxComponent*>(arch.GetPtr(si, row));
            // M38b: cubemap 実装 — SRV 解決は RenderSystem 側 (この関数は純データのまま)
            view.skyMode = (sb->mode == 1) ? 1 : 0;
            view.skyCubemapId = sb->cubemapTexture;
            view.skyTop = { sb->topColor.x, sb->topColor.y, sb->topColor.z };
            view.skyHorizon = { sb->horizonColor.x, sb->horizonColor.y, sb->horizonColor.z };
            view.skyBottom = { sb->bottomColor.x, sb->bottomColor.y, sb->bottomColor.z };
        }
    });

    // 最初の active な Fog
    uint32_t bestFog = 0xFFFFFFFFu;
    const ComponentTypeId fogReq[] = { FogComponent::sTypeId };
    world.ForEachArchetype(fogReq, [&](Archetype& arch) {
        const int fi = arch.FindTypeIndex(FogComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestFog || !IsEntityActive(world, e)) {
                continue;
            }
            bestFog = e.index;
            const auto* fog = static_cast<const FogComponent*>(arch.GetPtr(fi, row));
            view.fogMode = (fog->mode >= 0 && fog->mode <= 2) ? fog->mode : 0;
            view.fogColor = { fog->color.x, fog->color.y, fog->color.z };
            view.fogDensity = fog->density;
            view.fogStart = fog->start;
            view.fogEnd = fog->end;
        }
    });
}

bool RenderSystem::Render(World& world, GraphicsDevice& device, IRenderPath& path,
                          ShaderManager& shaders, RenderResources& resources,
                          const FrameTarget& target, const CameraOverride* cameraOverride,
                          ParticleSystem* particles, VfxRenderer* vfx)
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
    if (hdr != nullptr) {
        // M38a: 背景クリア色も authored 色 → リニアへ (トーンマップ + OETF と対)。
        // 直描き (postFx 無効) 経路は OETF が掛からないので変換しない
        for (int i = 0; i < 3; ++i) {
            view.clearColor[i] = SrgbToLinear(view.clearColor[i]);
        }
    }

    const float aspectRatio = (target.height > 0)
        ? static_cast<float>(target.width) / static_cast<float>(target.height) : 1.0f;

    // ---- カメラ ----
    bool cameraFound = false;
    EntityID camEntity = kNullEntity; // シーンカメラの実体 (CameraPostFx 参照用、M29e)
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
                    camEntity = arch.EntityAt(row); // M29e: CameraPostFx 参照用
                    cameraFound = true;
                    if (c->isPrimary != 0) {
                        return;
                    }
                }
            }
        });
        if (cameraFound) {
            // M36b: カメラも補間 (スクリプト駆動カメラの tick 刻みを消す)
            if (prevWorld && interpAlpha < 1.0f) {
                if (const XMFLOAT4X4* pw = prevWorld->Get(camEntity)) {
                    camWorld = LerpWorld(*pw, camWorld, interpAlpha);
                }
            }
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
                g.color = SrgbToLinear(l->color); // M38a: authored 色をリニアへ
                g.intensity = l->intensity;
                g.type = l->type;
                g.range = l->range;
                g.cosInner = std::cos(XMConvertToRadians(l->spotInnerDeg));
                g.cosOuter = std::cos(XMConvertToRadians(l->spotOuterDeg));
                if (!ambientSet) {
                    // アンビエントは最初のライトの値を全体に使う (M38a: リニアへ)
                    lights.ambient = SrgbToLinear(l->ambient);
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
        const bool interp = prevWorld != nullptr && interpAlpha < 1.0f; // M36b
        const ComponentTypeId req[] = { MeshRendererComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue; // 無効エンティティは描画しない (M10)
                }
                const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
                if (mr->mesh.IsNull() || mr->material.IsNull()) {
                    continue;
                }
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                XMFLOAT4X4 worldMat = wm->value;
                if (interp) {
                    // M36b: 前 tick との補間 (新規 spawn は prev 無し → 現在値)
                    if (const XMFLOAT4X4* pw = prevWorld->Get(e)) {
                        worldMat = LerpWorld(*pw, wm->value, interpAlpha);
                    }
                }
                cullCands.push_back({ e, mr->mesh, mr->material, worldMat,
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
                // M38d: カメラフィットの 3 カスケード (非 perspective はシーン全体×3 に縮退)
                XMFLOAT4X4 lightVPs[ShadowPass::kCascades];
                float splits[ShadowPass::kCascades];
                ComputeCascadeVPs(lights.lights[dirIdx].direction, sceneMin, sceneMax, view,
                                  shadowPass_.Resolution(), lightVPs, splits,
                                  ShadowPass::kCascades);
                shadowPass_.Render(device, shaders, queue_, resources, lightVPs,
                                   ShadowPass::kCascades);
                for (int c = 0; c < ShadowPass::kCascades; ++c) {
                    XMStoreFloat4x4(&view.lightViewProj[c],
                                    XMMatrixTranspose(XMLoadFloat4x4(&lightVPs[c])));
                    view.cascadeSplits[c] = splits[c];
                }
                view.cascadeCount = ShadowPass::kCascades;
                view.shadowSRV = shadowPass_.SRV();
                view.shadowTexelSize = 1.0f / static_cast<float>(shadowPass_.Resolution());
            }
        }
    }

    CollectEnvironment(world, view); // M29d: Skybox/Fog を view に反映
    // M38a: 環境の authored 色をリニアへ (CollectEnvironment 自体は純パススルーのまま =
    // selftest 不変)。スカイ/フォグは HDR 中間に描かれ、トーンマップ後の OETF と対になる
    view.skyTop = SrgbToLinear(view.skyTop);
    view.skyHorizon = SrgbToLinear(view.skyHorizon);
    view.skyBottom = SrgbToLinear(view.skyBottom);
    view.fogColor = SrgbToLinear(view.fogColor);
    // M38b: cubemap スカイの SRV 解決 (未ロード/不正なら gradient にフォールバック)
    if (view.skyMode == 1) {
        Texture* cube = resources.textures.Get(view.skyCubemapId);
        if (cube && cube->srv) {
            view.skyCubemap = cube->srv.Get();
        } else {
            view.skyMode = 0;
        }
    }
    // M38c: スカイがあるなら IBL 環境マップを取得 (初回のみ GPU ベイク、以後キャッシュ)。
    // gradient も同じベイクに通す — シェーダ側は「IBL on/off」の 2 択で済む。
    // ベイクは RT/シェーダ状態を触るが、この後の path.Render が全て再設定するので安全
    if (view.skyMode == 1 && view.skyCubemap != nullptr) {
        const EnvMaps em =
            envBaker_.GetForCubemap(device, shaders, view.skyCubemapId, view.skyCubemap);
        view.iblIrradiance = em.irradiance;
        view.iblPrefiltered = em.prefiltered;
        view.iblBrdfLut = em.brdfLut;
        view.iblSpecMips = em.specMips;
    } else if (view.skyMode == 0) {
        const EnvMaps em = envBaker_.GetForGradient(device, shaders, view.skyTop, view.skyHorizon,
                                                    view.skyBottom); // リニア変換済みの色
        view.iblIrradiance = em.irradiance;
        view.iblPrefiltered = em.prefiltered;
        view.iblBrdfLut = em.brdfLut;
        view.iblSpecMips = em.specMips;
    }
    queue_.Sort();
    path.Render(device, view, queue_, lights, resources, shaders);

    // VFX (M29c): Sprite/Trail/TextMesh をメッシュ (不透明+透明) の後・パーティクルの前に
    // 重ねる。HDR 中間へ描かれ postfx を通る。RT はパスがバインドしたまま
    if (vfx) {
        vfx->Render(world, device, shaders, resources, view);
    }

    // パーティクルは常に Forward 後段 (どのレンダリングパスでも共通)。HDR 中間へ加算される
    if (particles) {
        particles->Render(device, view, shaders, resources);
    }

    // スクリプトの DebugDrawLine (v7、M37): シーン空間の線を深度テスト付きで重ねる。
    // ポスプロ解決前 = HDR 中間 (直描き時は最終 RT) に描く
    if (debugLines != nullptr && !debugLines->empty()) {
        if (!linePass_.IsReady()) {
            linePass_.Init(device, shaders); // 遅延 Init (postFx_ 前例)
        }
        if (linePass_.IsReady()) {
            linePass_.Begin();
            for (const DebugLineCmd& l : *debugLines) {
                linePass_.AddLine({ l.ax, l.ay, l.az }, { l.bx, l.by, l.bz }, l.rgba, false);
            }
            linePass_.Render(device, shaders, view.rtv, view.dsv, target.width, target.height,
                             view.view, view.proj);
        }
    }

    // HDR → LDR 解決 (トーンマップ)。HDR 中間を使った時のみ。
    // シーンカメラに CameraPostFx があれば上書きマージ (M29e。CameraOverride 経路は
    // エディタ視界なのでグローバル設定のまま)
    if (hdr != nullptr) {
        PostProcess::Settings effective = postFxSettings;
        if (!cameraOverride && !camEntity.IsNull()) {
            if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
                effective = MergeCameraPostFx(postFxSettings, *pfx);
            }
        }
        postFx_.Resolve(device, shaders, *hdr, target.rtv, target.width, target.height,
                        effective);
    }
    return cameraFound;
}

} // namespace mye
