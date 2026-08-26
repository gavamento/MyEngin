#include "Engine/Engine/RenderSystem.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Particles/ParticleSystem.h"
#include "Engine/Engine/Ragdoll.h" // M60g1: 剛体が骨を駆動しているときのパレット
#include "Engine/Engine/Vfx/VfxRenderer.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/PostFxMath.h" // M55b: camerajitter
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

// スポットライトの view-proj (M54c、行ベクトル規約 world*view*proj)。戻り値は非転置。
// 円錐をちょうど包む透視 1 面。fov は外角の 2 倍 + 余白 —
// ★余白が要る理由: 3x3 PCF はタイル境界の外へタップを伸ばす。円錐の縁が
//   ぴったり枠に接していると、縁だけ「タイル外 = 影なし」に落ちて輪郭が硬くなる。
// near は 0.05 固定 (これ以上手前は減衰式でもほぼ寄与しない)、far は減衰半径。
// far を range に合わせるのは深度精度のため — シーン全体に合わせると近傍が潰れる。
XMFLOAT4X4 ComputeSpotLightVP(const XMFLOAT3& position, const XMFLOAT3& direction, float cosOuter,
                              float range)
{
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    const bool nearlyVertical = std::fabs(XMVectorGetY(dir)) > 0.99f;
    const XMVECTOR up = nearlyVertical ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookToLH(eye, dir, up);

    const float clamped = std::min(std::max(cosOuter, -0.99f), 0.9999f);
    const float halfAngle = std::acos(clamped);
    // 余白 6 度。上限 85 度 (= fov 170 度) は透視行列が発散しない範囲
    const float fovY = std::min(2.0f * (halfAngle + XMConvertToRadians(6.0f)),
                                XMConvertToRadians(170.0f));
    const float farZ = std::max(range, 1.0f);
    const XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fovY, 1.0f, 0.05f, farZ);

    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(lightView, lightProj));
    return out;
}

// D3D cubemap 面順 (+X,-X,+Y,-Y,+Z,-Z) の forward/up 基底 (M54d)。
// **EnvMapBaker.cpp の kFaces と同一表を意図的に複製している** — あちらは IBL ベイクの
// 匿名 namespace にあり、公開すると「環境マップの都合」がシャドウ側の依存になる。
// HLSL 側 common.hlsli の CubeFaceIndex もこの順番。3 者がずれると絵は出るが合わない
struct CubeFaceBasis {
    XMFLOAT3 forward;
    XMFLOAT3 up;
};
constexpr CubeFaceBasis kCubeFaces[6] = {
    { { 1, 0, 0 }, { 0, 1, 0 } },  // +X
    { { -1, 0, 0 }, { 0, 1, 0 } }, // -X
    { { 0, 1, 0 }, { 0, 0, -1 } }, // +Y
    { { 0, -1, 0 }, { 0, 0, 1 } }, // -Y
    { { 0, 0, 1 }, { 0, 1, 0 } },  // +Z
    { { 0, 0, -1 }, { 0, 1, 0 } }, // -Z
};

// 点光源 1 面ぶんの lightViewProj (M54d)。face は kCubeFaces の添字。
// ★fov はちょうど 90 度ではなく「タイル境界に PCF 用の余白を marginTexels 取る」ぶんだけ
//   広い。CubeFaceIndex はちょうど 90 度で面を切り替えるので、90 度で焼くと境界画素の
//   3x3 タップがタイルの外へ出る → SampleShadowAtlas の clamp が働いて自分の深度でなく
//   縁の深度を舐め、面の継ぎ目に沿って影の線が走る。tan(fov/2) を 1+2m/S にすると
//   90 度境界がタイル内側 m テクセルへ寄る (実測 S=1024 / m=2 で継ぎ目が消える)
XMFLOAT4X4 ComputePointLightFaceVP(const XMFLOAT3& position, int face, float range, int tileSize)
{
    const CubeFaceBasis& b = kCubeFaces[(face < 0 || face > 5) ? 0 : face];
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMMATRIX lightView =
        XMMatrixLookToLH(eye, XMLoadFloat3(&b.forward), XMLoadFloat3(&b.up));
    constexpr float kMarginTexels = 2.0f;
    const float s = static_cast<float>(std::max(tileSize, 16));
    const float fovY = 2.0f * std::atan(1.0f + 2.0f * kMarginTexels / s);
    const float farZ = std::max(range, 1.0f);
    const XMMATRIX lightProj = XMMatrixPerspectiveFovLH(fovY, 1.0f, 0.05f, farZ);

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
    // M55b: カスケードのフィットは非ジッタ側で行う。ジッタ付きだとカメラフラスタムの
    // 8 隅がサブピクセル分毎フレーム動き、テクセルスナップで殺したはずの影の揺れが戻る
    const XMMATRIX camProj = XMLoadFloat4x4(&view.projNoJitter);
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
            view.fogHeightFalloff = fog->heightFalloff; // M43a (純パススルー)
            view.fogBaseHeight = fog->baseHeight;
            view.fogInscatterIntensity = fog->inscatterIntensity;
            view.fogInscatterPower = fog->inscatterPower;
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
    view.depthSRV = target.depthSRV;       // M42a: null なら深度読み系効果は自然無効
    view.dsvReadOnly = target.dsvReadOnly;

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
        if (cameraOverride->hasProj) {
            // M55b: 呼び出し側 (SceneView) が既に組んである行列をそのまま使う。
            // ここで組み直すと Ortho トグルが描画へ届かず、ギズモ/ピッキングの行列
            // (SceneViewWindow::lastProj_) と食い違ったままになる
            view.proj = cameraOverride->proj;
        } else {
            const XMMATRIX p = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(cameraOverride->fovYDeg), aspectRatio,
                cameraOverride->nearZ, cameraOverride->farZ);
            XMStoreFloat4x4(&view.proj, p);
        }
        view.cameraPos = cameraOverride->position;
        view.debugViewMode = cameraOverride->debugViewMode; // M40b (SceneView のみ非 0)
        view.nearZ = cameraOverride->nearZ; // M42a: 深度線形化用
        view.farZ = cameraOverride->farZ;
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
            view.nearZ = cam.nearZ; // M42a: 深度線形化用
            view.farZ = cam.farZ;
        } else {
            XMStoreFloat4x4(&view.view, XMMatrixIdentity());
            XMStoreFloat4x4(&view.proj, XMMatrixIdentity());
        }
    }

    // ---- M55b: カメラジッタの一元化 ----
    // 射影の組み立ては上の 2 経路 (CameraOverride / シーンカメラ) に分かれているが、
    // ジッタを載せるのは **ここ 1 箇所だけ**。projNoJitter が「ジッタ前」の正本で、
    // 再投影 (prevVP_ の保存 / モーションブラー / RT テンポラル)、シャドウのカスケード
    // フィット、視錐台カリング、太陽の画面位置はすべてそちらを読む
    // (混ぜると「カメラが毎フレーム半ピクセル動いた」ことになり履歴が毎回外れる)。
    // viewKey==0 (AssetPreview) は履歴も TAA も持たないので常に非ジッタ。
    view.projNoJitter = view.proj;
    // ワールド追従 UI 用: 補間済みカメラのジッタ無し view×proj を公開 (UIRenderer が
    // この Render の直後に読む)。ジッタ入りを渡すと UI が毎フレーム半ピクセル揺れる
    lastCamValid = cameraFound;
    if (cameraFound) {
        XMStoreFloat4x4(&lastViewProjNoJitter,
                        XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
    }
    view.viewKey = target.viewKey; // M55d: TAA の履歴スロット
    view.viewFrameIndex = (target.viewKey < 4) ? viewSerial_[target.viewKey] : 0u;
    // ---- M55d: TAA の有効判定 ----
    // ★ジッタと TAA は**必ず同じ条件**で on/off する。TAA 抜きでジッタだけ載せると
    //   画面が毎フレーム半ピクセル揺れるだけになるので、「velocity を書かないパス
    //   (Forward)」「HDR 配管なし」「AssetPreview」はジッタごと落とす。
    // 設定の出所はポスプロと同じ規則 (グローバル設定 → シーンカメラの CameraPostFx で上書き)
    // だが、判定はマージ (Resolve 直前) より前に要るのでここで先に引く
    {
        bool taaOn = postFxSettings.taaOn != 0;
        if (!cameraOverride && !camEntity.IsNull()) {
            if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
                taaOn = pfx->taaOn != 0;
            }
        }
        view.taaEnabled = (taaOn && cameraFound && hdr != nullptr && path.WritesVelocity()
                           && target.viewKey > 0 && target.viewKey < 4)
            ? 1 : 0;
    }
    // ---- M57c: フロクセルの有効判定とパラメータ ----
    // 設定の出所は TAA / ポスプロと同じ規則 (グローバル設定 → シーンカメラの
    // CameraPostFx があればそちらが勝つ)。**判定だけここで先に引く** — 実際の
    // ディスパッチはシャドウアトラスと環境の収集が終わったあと (path.Render の直前)。
    // ★SceneView のエディタカメラ (CameraOverride) には CameraPostFx が効かない
    //   規約なので、そちらは --froxel / Rendering メニューのグローバル設定だけで動く
    bool froxelOn = enableFroxel;
    FroxelSettings effectiveFroxel = froxelSettings;
    if (!cameraOverride && !camEntity.IsNull()) {
        if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
            froxelOn = pfx->froxelOn != 0;
            effectiveFroxel.density = pfx->froxelDensity;
            effectiveFroxel.anisotropy = pfx->froxelAnisotropy;
        }
    }
    froxelOn = froxelOn && cameraFound;
    if (view.taaEnabled != 0 && jitterAmplitude > 0.0f) {
        float px = 0.0f;
        float py = 0.0f;
        camerajitter::Sample(view.viewFrameIndex, px, py);
        px *= jitterAmplitude;
        py *= jitterAmplitude;
        float ndcX = 0.0f;
        float ndcY = 0.0f;
        camerajitter::PixelsToNdc(px, py, view.width, view.height, ndcX, ndcY);
        view.jitterPixels[0] = px;
        view.jitterPixels[1] = py;
        view.jitterNdc[0] = ndcX;
        view.jitterNdc[1] = ndcY;
        view.proj = camerajitter::ApplyToProj(view.proj, ndcX, ndcY);
    }

    // ---- 視錐台 (M16: メッシュのカリング用。カメラがある時のみ。描画専用でハッシュ非対象) ----
    // M54b からライト選別も同じ視錐台を使うので、収集ブロックの中からここへ引き上げた
    Frustum frustum = {};
    const bool cullEnabled = cameraFound;
    if (cullEnabled) {
        XMFLOAT4X4 vp;
        // M55b: カリングは非ジッタ側。サブピクセルで可視判定が反転すると
        // 境界の物体がフレーム毎に出入りして TAA の履歴に穴が空く
        XMStoreFloat4x4(&vp, XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
        frustum = BuildFrustum(vp);
    }

    // ---- ライト (Directional/Point/Spot) ----
    // M54b: ここは候補を集めるだけで、カリング / 決定論ソート / 上限 kMaxLights の適用は
    // LightSelection.cpp の純関数が行う (M54c のシャドウアトラスが「影を投げるライトの列」の
    // frame 間安定性を要求するため、順序を決める場所を 1 箇所に閉じた)。
    // M54b 以前はカリングもソートも無い「登録順の先着 16 本」だった
    SceneLightData lights;
    {
        const ComponentTypeId req[] = { LightComponent::sTypeId, WorldMatrixComponent::sTypeId };
        bool ambientSet = false;
        std::vector<LightCandidate> cands;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(LightComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue; // 無効化されたライトは寄与しない
                }
                const auto* l = static_cast<const LightComponent*>(arch.GetPtr(li, row));
                const auto* w = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                LightCandidate& c = cands.emplace_back();
                c.sortKey = e.index; // 決定論キー (アーキタイプの並び順に依存しない)
                c.castShadow = l->castShadow;
                GpuLight& g = c.light;
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
                    // アンビエントは最初のライトの値を全体に使う (M38a: リニアへ)。
                    // ★選別後の先頭ではなく**走査順の先頭** — 従来挙動を 1 ビットも変えない
                    lights.ambient = SrgbToLinear(l->ambient);
                    ambientSet = true;
                }
            }
        });
        lightSelection = SelectLights(cands.data(), static_cast<int>(cands.size()),
                                      cullEnabled ? &frustum : nullptr);
        lights.count = lightSelection.count;
        for (int i = 0; i < lightSelection.count; ++i) {
            lights.lights[i] = lightSelection.lights[i].light;
        }
        // カリングで意図せずライトが消えていないかを目視するための 1 行。
        // 毎フレーム出すとログが埋まるので、初めて見る組み合わせのときだけ出す
        {
            auto pack = [](int v) { return static_cast<uint64_t>((std::min)((std::max)(v, 0), 0xFFFF)); };
            const uint64_t key = pack(lightSelection.count) | (pack(lightSelection.culled) << 16)
                | (pack(lightSelection.overflow) << 32) | (pack(lightSelection.shadowCount) << 48);
            bool seen = false;
            for (int i = 0; i < lightLogSeenCount_; ++i) {
                seen = seen || lightLogSeen_[i] == key;
            }
            if (!seen && lightLogSeenCount_ < static_cast<int>(std::size(lightLogSeen_))) {
                lightLogSeen_[lightLogSeenCount_++] = key;
                MYE_LOG_INFO("Lights: %d selected (culled %d, dropped %d, shadow casters %d)",
                             lightSelection.count, lightSelection.culled, lightSelection.overflow,
                             lightSelection.shadowCount);
            }
        }
    }

    // ---- M55c: 「前フレームに実際に描いた world 行列」のストアを今フレームへ進める ----
    // viewKey==0 (AssetPreview) は履歴を持たない = velocity は常に 0 に落ちる。
    // viewSerial_ はこの Render の末尾で +1 されるので、ここでの値が「今フレームの通番」
    const uint32_t prevRenderKey = (target.viewKey > 0 && target.viewKey < 4) ? target.viewKey : 0u;
    PrevRenderWorldStore* prevRender = (prevRenderKey != 0) ? &prevRender_[prevRenderKey] : nullptr;
    if (prevRender != nullptr) {
        prevRender->Begin(viewSerial_[prevRenderKey], target.width, target.height);
    }

    // ---- 収集 ----
    queue_.Clear();
    skinPalettes_.clear(); // スキンメッシュのボーンパレット (M18、フレーム毎に再構築)
    {
        const XMMATRIX v = XMLoadFloat4x4(&view.view);
        // ---- 地形 (M58c): メッシュとは別レーンで収集する ----
        // TerrainComponent は kComponentNoHash = 描画専用。ここで作るのは
        // 「可視チャンクの描画指示」だけで sim には 1 バイトも触れない。
        // ★カメラが無いフレーム (cullEnabled==false) は収集しない — 視錐台がゼロ行列のままで
        //   CullChunks に渡しても意味のある結果にならない (メッシュ側がカリングを
        //   丸ごと飛ばしているのと同じ扱い)。
        // ★assetsRoot が空 = AssetPreviewCache の専用 RenderSystem。サムネイルに地形を
        //   混ぜないための元栓はここ 1 箇所 (「配線の 2 箇所目」で毎回漏れるところ)
        terrainList_.items.clear();
        if (cullEnabled && !assetsRoot.empty()) {
            terrainSystem_.Collect(world, resources.meshes, resources.textures, assetsRoot,
                                   frustum, view.view, terrainScratch_);
            terrainList_.items.reserve(terrainScratch_.size());
            for (const TerrainDrawItem& t : terrainScratch_) {
                TerrainRenderItem it;
                it.mesh = t.mesh;
                it.world = t.world;
                it.viewZ = t.viewZ;
                it.surface = t.surface; // M58d: スプラット + 4 レイヤの bind
                terrainList_.items.push_back(it);
            }
            // 近い順 (early-z が効く順)。比較規則の正本は TerrainPass.h の
            // TerrainDrawOrderLess (規則 7 のタイブレークつき。TerrainSelfTest が検査する)
            std::sort(terrainList_.items.begin(), terrainList_.items.end(),
                      TerrainDrawOrderLess);
        }
        view.terrain = &terrainList_;

        // ---- デカール (M56a): メッシュとは別レーンで収集する (地形と同じ流儀) ----
        // DecalComponent は kComponentNoHash = 描画専用。ここで作るのは「投影ボックス 1 個
        // ぶんの描画指示」だけで sim には 1 バイトも触れない。
        // ★視錐台カリングは v1 では**しない**。箱の外の受け面は PS 側の OBB 判定で
        //   捨てられるので絵は正しく、デカールは数個の想定 (数が問題になるのは
        //   「画面外の箱でも 12 三角形をラスタライズする」コストが見えてからでよい)。
        // ★AssetPreviewCache の専用 RenderSystem 用の元栓は要らない — プレビュー世界には
        //   デカールが 1 個も居ないので、下の収集が空リストを作り view.decals が
        //   「空 = 何もしない」に落ちる (地形は共有ワールドを見るので元栓が要った)
        decalList_.items.clear();
        {
            const ComponentTypeId req[] = { DecalComponent::sTypeId,
                                            WorldMatrixComponent::sTypeId };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int di = arch.FindTypeIndex(DecalComponent::sTypeId);
                const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID e = arch.EntityAt(row);
                    if (!IsEntityActive(world, e)) {
                        continue; // 無効化されたデカールは貼られない
                    }
                    const auto* d = static_cast<const DecalComponent*>(arch.GetPtr(di, row));
                    if (d->color.w <= 0.0f) {
                        continue; // 完全に透明 = 描いても 1 画素も変わらない
                    }
                    const auto* w =
                        static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                    DecalRenderItem it;
                    if (!FillDecalTransform(w->value, it)) {
                        continue; // スケール 0 等で逆行列が作れない
                    }
                    it.color = SrgbToLinear(XMFLOAT3(d->color.x, d->color.y, d->color.z));
                    it.opacity = d->color.w;
                    it.angleFadeCos = DecalAngleFadeCos(d->angleFadeDeg);
                    it.uvScale[0] = d->uvScale.x;
                    it.uvScale[1] = d->uvScale.y;
                    it.uvOffset[0] = d->uvOffset.x;
                    it.uvOffset[1] = d->uvOffset.y;
                    it.texture = d->texture;
                    it.sortOrder = d->sortOrder;
                    it.sortKey = e.index; // 決定論キー (アーキタイプの並び順に依存しない)
                    // M56b: 強度は [0,1] に丸めてから渡す。**そのままハードウェアの
                    // ブレンド係数になる**ので、1 を超えると dst 側の係数 (1-src) が負に
                    // なって法線が反転する (Inspector のスライダは止めるがスクリプト経由は素通り)
                    it.normalTexture = d->normalTex;
                    it.normalStrength = DecalStrength01(d->normalStrength);
                    it.roughness = DecalStrength01(d->roughness);
                    it.roughnessStrength = DecalStrength01(d->roughnessStrength);
                    decalList_.items.push_back(it);
                }
            });
            // 比較規則の正本は RenderTypes.h の DecalDrawOrderLess
            // (規則 7 のタイブレークつき。DecalSelfTest が検査する)
            std::sort(decalList_.items.begin(), decalList_.items.end(), DecalDrawOrderLess);
        }
        view.decals = &decalList_;

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
        const bool collectRt =
            rtDebugMode != 0 || enableRtGi || enableRtShadow || enableRtRefl; // M46b/f/g/h
        rtInstances_.clear();
        for (const CullCand& c : cullCands) {
            // M46b: レイトレ用の収集はフラスタムカリングしない (画面外の物体も
            // 反射や GI には効くため)。v1 制限: スキンメッシュ (CPU 頂点がバインドポーズ
            // のままなので姿勢が反映できない) と半透明は BVH に入れない
            if (collectRt && world.GetComponent<SkinnedMeshComponent>(c.e) == nullptr) {
                const Material* rtMat = resources.materials.Get(c.material);
                if (!rtMat || rtMat->transparent == 0) {
                    rtInstances_.push_back({ c.mesh, c.material, c.world });
                }
            }
            if (!c.visible) {
                ++culledCount;
                continue;
            }
            RenderItem item;
            item.mesh = c.mesh;
            item.material = c.material;
            item.world = c.world;
            item.viewZ = c.viewZ;
            // M55c: velocity 用に「前フレームに実際に描いた行列」を載せる。履歴が無い
            // (初回 / リサイズ / 前フレームは視錐台の外だった / 生成直後) ときは現在値と
            // 同値を入れる = 画面速度が厳密に 0 = カメラ再投影のみへ縮退する。
            // Lookup は Record より先 (同じスロットを読んでから上書きする)
            item.prevWorld = c.world;
            if (prevRender != nullptr) {
                if (const XMFLOAT4X4* pr = prevRender->Lookup(c.e)) {
                    item.prevWorld = *pr;
                }
                prevRender->Record(c.e, c.world);
            }

            // スキンメッシュ (M18): ポーズを評価してボーンパレットを構築し item に載せる。
            // ポーズは描画専用 (SkinnedMeshComponent は kComponentNoHash)
            if (auto* sm = world.GetComponent<SkinnedMeshComponent>(c.e)) {
                if (const SkinnedModel* model = resources.skinnedModels.Get(sm->model)) {
                    skinPalettes_.emplace_back();
                    std::vector<XMFLOAT4X4>& palette = skinPalettes_.back();
                    const float timeSec = static_cast<float>(sm->timeTicks) / 60.0f;
                    // M60g1: ラグドールが作動中なら、骨の姿勢はアニメではなく**部位の
                    // LocalTransform (= 剛体が置いた値)** から組む。入力が ECS 状態だけの
                    // 純関数なので、ビュー毎に Render() が呼ばれても同じ絵になる
                    if (const auto* rag = world.GetComponent<RagdollComponent>(c.e);
                        rag && rag->active) {
                        ragdoll::BuildBonePalette(world, c.e, *model, sm->clip, timeSec, palette);
                    } else {
                        ComputeBonePalette(*model, sm->clip, timeSec, palette);
                    }
                    if (palette.size() > static_cast<size_t>(kMaxBones)) {
                        // 上限超過は切り捨て (シェーダの定数バッファが kMaxBones 固定のため)。
                        // 黙って切ると姿勢が壊れた原因が追えないので model 毎に 1 回だけ WARN
                        if (std::find(boneOverflowWarned_.begin(), boneOverflowWarned_.end(),
                                      sm->model.value)
                            == boneOverflowWarned_.end()) {
                            boneOverflowWarned_.push_back(sm->model.value);
                            MYE_LOG_WARN("skinned model has %zu bones, clamped to %d "
                                         "(MYE_MAX_BONES): pose will be wrong for the extra bones",
                                         palette.size(), kMaxBones);
                        }
                        palette.resize(static_cast<size_t>(kMaxBones));
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
                                   ShadowPass::kCascades, enableInstancing);
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

        // ---- 局所ライトのシャドウアトラス (M54c: スポット 1 面 / M54d: 点光源 6 面) ----
        // 枠は「M54b の決定論キーで並んだライト順に前詰め」= シーンが変わらなければ
        // frame をまたいでも同じライトが同じ枠に落ちる (割当が揺れると影がポップする)。
        // ★点光源は 6 枚を**連番**で取る — シェーダは shadowTile + 面番号で引くので、
        //   途中に他のライトの枠が挟まると隣の深度を読んでしまう
        view.shadowAtlasSRV = nullptr;
        view.shadowTileCount = 0;
        shadowAtlasFaceCulled_ = 0;
        if (enableShadows && enableLocalShadows && hasScene) {
            int tileCount = 0;
            int culledFaces = 0;
            int casters = 0;
            for (int i = 0; i < lightSelection.count && i < lights.count; ++i) {
                if (lightSelection.lights[i].shadowSlot < 0) {
                    continue;
                }
                const GpuLight& g = lights.lights[i];
                const int faces = (g.type == 1) ? 6 : ((g.type == 2) ? 1 : 0);
                if (faces == 0) {
                    continue; // 平行光の影は CSM (ShadowPass) の担当
                }
                // アトラス未生成ならここで初めて作る (64MB。影を使わないシーンでは払わない)
                if (!shadowAtlas_.IsReady() && !shadowAtlas_.Init(device, shaders)) {
                    break;
                }
                // ★入り切らないライトは break ではなく skip。6 枚要る点光源が入らなくても
                //   後ろに並ぶ 1 枚のスポットはまだ入る。選別順に前詰めという規則は
                //   保たれるので、割当は決定論のまま
                if (tileCount + faces > shadowAtlas_.TileCapacity()) {
                    continue;
                }
                const int base = tileCount;
                for (int f = 0; f < faces; ++f) {
                    ShadowTile& tile = view.shadowTiles[base + f];
                    shadowAtlas_.FillTileRect(base + f, tile);
                    tile.lightViewProj = (faces == 6)
                        ? ComputePointLightFaceVP(g.position, f, g.range, shadowAtlas_.TileSize())
                        : ComputeSpotLightVP(g.position, g.direction, g.cosOuter, g.range);
                    // 定数バイアスはラスタライザ側 (傾斜依存) を主役にしているので極小。
                    // 0 にすると自己遮蔽の縞が出る距離帯が残る (実測で詰めた値)
                    tile.depthBias = 0.00015f;
                    // ★面カリング (M54d): この面の視錐台がシーン AABB に触れないなら描かない。
                    //   タイルは**確保したまま** pixelSize=0 にする — 連番を詰めると
                    //   shadowTile + 面番号の対応が崩れる。描かれないタイルはクリア値
                    //   1.0 (最遠) のままなので、サンプルしても「影なし」に落ちる
                    if (!WorldAabbInFrustum(BuildFrustum(tile.lightViewProj), sceneMin, sceneMax)) {
                        tile.pixelSize = 0;
                        ++culledFaces;
                    }
                }
                lights.lights[i].shadowTile = base;
                lights.lights[i].shadowFaces = faces;
                tileCount += faces;
                ++casters;
            }
            if (tileCount > 0) {
                shadowAtlas_.Render(device, shaders, queue_, resources, view.shadowTiles,
                                    tileCount, enableInstancing);
                view.shadowAtlasSRV = shadowAtlas_.SRV();
                view.shadowAtlasTexel =
                    1.0f / static_cast<float>(shadowAtlas_.Resolution());
                view.shadowTileCount = tileCount;
            }
            shadowAtlasFaceCulled_ = culledFaces;
            // M54d: 割当の実測をログへ。ヘッドレス撮影では ProfilerWindow が見えないので、
            // 「6 面が連番で取れたか / 面カリングが効いたか」はここでしか確認できない。
            // M54b のライト選別ログと同じ「出たことのある組み合わせを 1 回ずつ」方式
            // (SceneView と GameView で結果が食い違うと毎フレーム 2 行出続けるため)
            if (tileCount > 0) {
                auto pack = [](int v) { return static_cast<uint64_t>(v & 0xFFFF); };
                const uint64_t key = pack(tileCount) | (pack(culledFaces) << 16)
                    | (pack(casters) << 32) | (pack(shadowAtlas_.DrawCalls()) << 48);
                bool seen = false;
                for (int i = 0; i < shadowLogSeenCount_; ++i) {
                    seen = seen || shadowLogSeen_[i] == key;
                }
                if (!seen && shadowLogSeenCount_ < static_cast<int>(std::size(shadowLogSeen_))) {
                    shadowLogSeen_[shadowLogSeenCount_++] = key;
                    MYE_LOG_INFO("ShadowAtlas: %d tiles for %d local casters (%d faces culled, "
                                 "%d draws, %d draws culled)",
                                 tileCount, casters, culledFaces, shadowAtlas_.DrawCalls(),
                                 shadowAtlas_.CulledDraws());
                }
            }
        }
    }

    view.ssaoEnabled = enableSsao ? 1 : 0; // M38e (Deferred のみ消費)
    view.instancingEnabled = enableInstancing ? 1 : 0; // M38f
    view.velocityDebug = velocityDebugMode;            // M55c (Deferred のみ消費)
    view.hzbDebug = hzbDebugMip;                       // M56c (Deferred のみ消費)
    view.ssrEnabled = enableSsr ? 1 : 0;               // M56d (Deferred のみ消費)
    // M56f: 焼いたプローブ束をそのまま指す (Deferred のみ消費)。ベイクした所有者が
    // このポインタを立てるまで null = 1 命令も増えない。**トグルを設けていない**のは、
    // 「焼いていない = 無い」で十分だから (焼く操作そのものが明示 opt-in)
    view.probes = reflectionProbes;
    // M40d: シーンカメラの CameraPostFx から SSAO パラメータ (override = エディタ視界は既定)
    if (!cameraOverride && !camEntity.IsNull()) {
        if (const auto* pfx = world.GetComponent<CameraPostFxComponent>(camEntity)) {
            view.ssaoRadius = pfx->ssaoRadius;
            view.ssaoIntensity = pfx->ssaoIntensity;
            // M56d: SSR はグローバル設定をシーンカメラが上書きする (TAA と同じ規則)。
            // SceneView (cameraOverride あり) はグローバル設定のまま = エディタ視界は不変
            view.ssrEnabled = pfx->ssrOn ? 1 : 0;
            view.ssrMaxRoughness = pfx->ssrMaxRoughness;
            view.ssrIntensity = pfx->ssrIntensity;
        }
    }
    CollectEnvironment(world, view); // M29d: Skybox/Fog を view に反映
    // M38a: 環境の authored 色をリニアへ (CollectEnvironment 自体は純パススルーのまま =
    // selftest 不変)。スカイ/フォグは HDR 中間に描かれ、トーンマップ後の OETF と対になる
    view.skyTop = SrgbToLinear(view.skyTop);
    view.skyHorizon = SrgbToLinear(view.skyHorizon);
    view.skyBottom = SrgbToLinear(view.skyBottom);
    view.fogColor = SrgbToLinear(view.fogColor);
    // M43a: 太陽 = 最初の type==0 平行光 (CSM の dirIdx と同じ規則)。色はリニア・強度込み。
    // 平行光が無いシーンではインスキャッタ無効化 (黒い太陽へ lerp して暗転する事故を防ぐ)
    {
        int sunIdx = -1;
        for (int i = 0; i < lights.count; ++i) {
            if (lights.lights[i].type == 0) {
                sunIdx = i;
                break;
            }
        }
        if (sunIdx >= 0) {
            const GpuLight& g = lights.lights[sunIdx];
            view.sunDirection = g.direction;
            view.sunColor = { g.color.x * g.intensity, g.color.y * g.intensity,
                              g.color.z * g.intensity };
        } else {
            view.fogInscatterIntensity = 0.0f;
        }
    }
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
    // M44d: 前フレームの viewProj / カメラ位置 (viewKey 毎)。初フレーム/リサイズは invalid。
    // モーションブラー (ポスプロ) と RT のテンポラル再投影 (M46d) の共通の出所なので、
    // path.Render より前で埋める
    if (target.viewKey > 0 && target.viewKey < 4) {
        const PrevViewProj& p = prevVP_[target.viewKey];
        if (p.valid && p.w == target.width && p.h == target.height) {
            view.prevViewProj = p.m;
            view.prevCameraPos = p.pos;
            view.prevViewProjValid = 1;
        }
    }
    // M46b: レイトレ用シーン (BLAS 連結 + TLAS + インスタンス) を GPU へ。
    // デバッグ表示・GI 合成・RT 影のどれも off なら収集自体が空なので、
    // この節はまるごと従来経路と同じになる
    if ((rtDebugMode != 0 || enableRtGi || enableRtShadow || enableRtRefl)
        && !rtInstances_.empty()) {
        if (!rtPasses_.IsReady()) {
            rtPasses_.Init(device, shaders);
        }
        rtScene_.Init(device);
        rtScene_.Update(rtInstances_, resources);
        if (rtPasses_.IsReady() && rtScene_.Bindings().IsValid()) {
            view.rtDebugMode = rtDebugMode;
            view.rtScene = &rtScene_.Bindings();
            view.rtPasses = &rtPasses_;
            view.rtResolutionScale = rtResolutionScale;
            view.rtBounces = rtBounces;
            // freeze 中は毎フレーム同じ乱数列 = 決定的なスクリーンショットが撮れる
            view.rtFrameIndex = rtFreezeSeed ? 0u : rtFrameCounter_;
            ++rtFrameCounter_;
            // M46d: テンポラル蓄積。履歴は viewKey 別 (SceneView と GameView が混線しない)
            view.rtTemporal = rtTemporal ? 1 : 0;
            view.rtViewKey = (target.viewKey < 4) ? target.viewKey : 0u;
            view.rtViewSerial = viewSerial_[view.rtViewKey];
            // M46e: SVGF。凍結中はテンポラル分散が 0 に潰れるので空間推定へ落とす合図も渡す
            view.rtSvgf = rtSvgf ? 1 : 0;
            view.rtFreezeSeed = rtFreezeSeed ? 1 : 0;
            // M46f: 最終画像への合成 (ライトパスの拡散環境項を GI で置換)
            view.rtGiEnabled = enableRtGi ? 1 : 0;
            // M46g: 平行光のシャドウ係数を CSM でなくレイトレの可視率で作る。
            // 「Shadows」トグルは影全体の元栓なので、off なら RT 影も出さない
            view.rtShadowEnabled = (enableRtShadow && enableShadows) ? 1 : 0;
            // M46h: スペキュラ環境項をレイトレ反射で置換
            view.rtReflEnabled = enableRtRefl ? 1 : 0;
            // 合成は split-sum なので環境 BRDF LUT (t7) が要る。IBL は「スカイがある」
            // ことが条件だが LUT 自体はスカイに依らない純関数なので、スカイ無しの
            // シーンでも反射のためにベイクしておく。irradiance/prefiltered は
            // null のままなので pf.iblEnabled は false に留まる = IBL は有効化されない
            if (enableRtRefl && view.iblBrdfLut == nullptr) {
                view.iblBrdfLut = envBaker_.GetBrdfLut(device, shaders);
            }
        }
    }
    // M56d: SSR も split-sum なので環境 BRDF LUT が要る。RT 反射 (すぐ上) と同じ理屈で、
    // **スカイの無いシーンでも LUT だけは焼く** — LUT はスカイに依らない純関数で、
    // irradiance / prefiltered は null のままなので pf.iblEnabled は false に留まる。
    // ★これを忘れると LUT が null → SampleLevel が 0 を返す → 環境 BRDF が 0 →
    //   **SSR を on にしても絵が 1 ピクセルも変わらない** (--render-demo にはスカイが無い)
    // M56f: 反射プローブも split-sum で合成する = 同じ理由で LUT が要る。
    // ★これを忘れると「プローブを焼いたのに絵が 1 画素も変わらない」になる
    //   (--render-demo にはスカイが無いので LUT が誰にも焼かれない)
    if ((view.ssrEnabled != 0 || (view.probes != nullptr && view.probes->count > 0))
        && view.iblBrdfLut == nullptr) {
        view.iblBrdfLut = envBaker_.GetBrdfLut(device, shaders);
    }

    // ---- M57b/M57c/M57d: フロクセル (注入 → テンポラル → 前方積分 → 光パスへ供給) ----
    // ★置き場所はここしかない: 上流に CollectEnvironment (高度フォグのパラメータ) と
    //   シャドウアトラス (SampleShadowAtlas の入力) が要り、下流の path.Render より
    //   前でないと消費側 (Deferred 光パス) が積分結果を読めない。
    // M57d でここが初めて絵に出る。**積分が走らなかったフレームは SRV が null のまま** =
    //   光パス側のゲートで従来の ApplyFog へ落ちる (正射影ビュー / シェーダ未ロードなど)
    if (froxelOn && (froxelPass_.IsReady() || froxelPass_.Init(device, shaders))) {
        view.froxelSRV = froxelPass_.Render(device, shaders, view, lights, effectiveFroxel);
        view.froxelNearZ = froxelPass_.GridNearZ();
        view.froxelFarZ = froxelPass_.GridFarZ();
        view.froxelSlices = froxelPass_.GridSliceCount();
        // 「そのビューの N 回目の描画」= 決定的撮影モードでは frame 番号と一致する
        // (viewSerial_ はこの Render の末尾で +1 される = ここでの値が今フレームの通番)
        const uint32_t serial = (target.viewKey < 4) ? viewSerial_[target.viewKey] : 0u;
        if (froxelDumpFrame >= 0 && static_cast<uint32_t>(froxelDumpFrame) == serial) {
            froxelPass_.DebugDumpAB(device, shaders, view, lights, effectiveFroxel);
            // DebugDumpAB は検査のために注入をもう 2 回走らせてボリュームを塗り替える。
            // その状態の scatter_ を積分し直さないまま光パスへ渡すと「ダンプした
            // フレームだけ絵が違う」になるので、確定した設定でもう一度回して戻す
            view.froxelSRV = froxelPass_.Render(device, shaders, view, lights, effectiveFroxel);
        }
    }

    queue_.Sort();
    path.Render(device, view, queue_, lights, resources, shaders);
    // M55d: 画面速度 (GBuffer RT4) はパスが所有する — 描いた後でないと SRV が無い。
    // TAA (ポスプロ) がこの後で読む。Forward は null = TAA は自然に不成立になる
    view.velocitySRV = path.VelocitySRV();
    // M56c: HZB の GPU 時間を写す。Forward は既定の 0 を返すので、パスを切り替えると
    // 行が消えるのではなく 0.000 ms になる (「計っていない」と「速い」の区別は
    // ProfilerWindow が hzbDebugMip で行を出し分けることで付けている)
    hzbGpuMs_ = path.HzbGpuMs();
    ssrGpuMs_ = path.SsrGpuMs(); // M56d (同上)

    // VFX (M29c): Sprite/Trail/TextMesh をメッシュ (不透明+透明) の後・パーティクルの前に
    // 重ねる。HDR 中間へ描かれ postfx を通る。RT はパスがバインドしたまま
    if (vfx) {
        vfx->Render(world, device, shaders, resources, view);
    }

    // パーティクルは常に Forward 後段 (どのレンダリングパスでも共通)。HDR 中間へ加算される
    bool distortionActive = false; // M42d: このフレーム歪みバッファを使ったか
    if (particles) {
        // M42d: blendMode=2 (distortion) のエミッタが存在するときだけ歪みバッファを
        // クリアして配線する (HDR 経路限定。バッファ未使用フレームのクリアコストを避ける)
        if (hdr != nullptr && hdr->distort.IsValid()) {
            const ComponentTypeId req2[] = { ParticleEmitterComponent::sTypeId };
            bool hasDistortion = false;
            world.ForEachArchetype(req2, [&](Archetype& arch) {
                const int pi = arch.FindTypeIndex(ParticleEmitterComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const auto* p =
                        static_cast<const ParticleEmitterComponent*>(arch.GetPtr(pi, row));
                    if (p->blendMode == 2) {
                        hasDistortion = true;
                        return;
                    }
                }
            });
            if (hasDistortion) {
                const float zero[4] = { 0, 0, 0, 0 };
                device.Context()->ClearRenderTargetView(hdr->distort.RTV(), zero);
                view.distortionRTV = hdr->distort.RTV();
                distortionActive = true;
            }
        }
        // M42a: パーティクル系は深度書込みしない (WriteMask=ZERO) ので read-only DSV に
        // 差し替える。これで深度 SRV (view.depthSRV) との同時バインドが合法になり、
        // ソフトパーティクル (M42b) 等が深度を読める。read-only ビューが無ければ従来どおり
        if (view.dsvReadOnly != nullptr) {
            device.Context()->OMSetRenderTargets(1, &view.rtv, view.dsvReadOnly);
        }
        particles->Render(device, view, shaders, resources);

        // M42e: GPU 深度衝突へシーンカメラの深度を供給する。次 tick の sim が使う =
        // 衝突相手は前フレームの深度 (仕様)。SceneView エディタカメラ (cameraOverride) は
        // 衝突源にしない — ゲームカメラの見た目だけが物理感を持つ
        if (!cameraOverride && view.depthSRV != nullptr) {
            particles->Gpu().SetSceneDepth(view.depthSRV, view.view, view.proj, view.width,
                                           view.height, view.nearZ, view.farZ);
        }
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
        // M44d: 前フレーム viewProj は path.Render 前に充填済み (view.prevViewProj)。
        // SceneView (cameraOverride) はエディタ操作中のスミアが UX を阻害するため強制 off
        if (cameraOverride) {
            effective.motionBlurIntensity = 0.0f;
        }
        // M57d: フォグ三重計上の 3 つめを降ろす。ゴッドレイ (postfx_godray_mask/blur) は
        // 「遮蔽マスクが空だけ」の**スクリーンスペース近似**で、フロクセルはその上位互換
        // (深度を持つ全ての遮蔽物 + 局所ライト + シャドウアトラス) にあたる。
        // 両方走らせると太陽まわりの散乱を 2 回足すことになるので、フロクセルが
        // 実際に絵へ出たフレームだけ自動で降ろす。**ユーザー設定は書き換えない** —
        // ここで潰すのは「このフレームで使う実効値」だけなので、froxel を切れば戻る
        // ★条件に path.AppliesFroxel() が要る — Forward はまだ積分結果を読まない (M57e) ので、
        //   ここを SRV の有無だけで判定すると「ゴッドレイだけ消えて霧が増えない」になる
        if (view.froxelSRV != nullptr && path.AppliesFroxel()) {
            effective.godrayIntensity = 0.0f;
        }
        // M44a: LUT の SRV 解決 (MaterialLibrary の GUID→パス解決と同じ流儀)。
        // 未ロードなら遅延ロード — LUT はデータなので srgb=false (デコード禁止)。
        // 解決できなければ lutSRV=null のまま = Resolve 側で強制 off
        if (effective.lutIntensity > 0.0f && !effective.lutTexture.IsNull()) {
            Texture* lut = resources.textures.Get(effective.lutTexture);
            if (!lut) {
                const std::wstring lutPath = assetguid::ResolvePath(effective.lutTexture.value);
                if (!lutPath.empty()) {
                    resources.textures.LoadFile(lutPath, /*srgb=*/false);
                    lut = resources.textures.Get(effective.lutTexture);
                }
            }
            if (lut && lut->srv) {
                effective.lutSRV = lut->srv.Get();
            }
        }
        postFx_.Resolve(device, shaders, *hdr, target.rtv, target.width, target.height,
                        effective, view, distortionActive); // M43b: view = 深度/太陽の供給口
    }
    // M44d: 次フレームのモーションブラー用に viewProj を保存 (viewKey=0 = AssetPreview は対象外)。
    // M46d: カメラ位置と描画通番も同じ場所で更新する (再投影とテンポラル履歴の連続性判定)。
    // ★M55b: ここに保存するのは **非ジッタ側** (projNoJitter)。この 1 箇所を
    // RtPasses (M46d) と PostProcess::RunMotionBlur (M44d) の **両方** が読むので、
    // ジッタ付きを入れると RT テンポラルとモーションブラーが同時に壊れる
    if (target.viewKey > 0 && target.viewKey < 4) {
        PrevViewProj& p = prevVP_[target.viewKey];
        XMStoreFloat4x4(&p.m, XMLoadFloat4x4(&view.view) * XMLoadFloat4x4(&view.projNoJitter));
        p.pos = view.cameraPos;
        p.w = target.width;
        p.h = target.height;
        p.valid = true;
        ++viewSerial_[target.viewKey];
    }
    return cameraFound;
}

} // namespace mye
