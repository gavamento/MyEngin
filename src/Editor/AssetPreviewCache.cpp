#include "Editor/AssetPreviewCache.h"

#include <algorithm>
#include <filesystem>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"

namespace fs = std::filesystem;
using namespace DirectX;

namespace mye {

namespace {

constexpr int kPreviewSize = 128;
constexpr int kMaxRendersPerFrame = 2;
constexpr size_t kMaxCacheEntries = 256; // 128^2 RGBA * 256 = 約 16MB

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

int64_t FileWriteTime(const std::wstring& path)
{
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

} // namespace

bool AssetPreviewCache::IsPreviewable(const std::wstring& path)
{
    return EndsWith(path, L".glb") || EndsWith(path, L".gltf") || EndsWith(path, L".fbx")
           || EndsWith(path, L".prefab.json");
}

ID3D11ShaderResourceView* AssetPreviewCache::GetOrRequest(EngineContext& ctx,
                                                          const std::wstring& path)
{
    const std::wstring key = NormalizePathKey(path);
    Entry& e = cache_[key];
    e.lastUsedFrame = ctx.frameIndex;

    const int64_t writeTime = FileWriteTime(path);
    const bool stale = (e.fileWriteTime != 0 && writeTime != e.fileWriteTime);
    if (e.srv && !stale) {
        return e.srv.Get();
    }
    if (e.failed && !stale) {
        return nullptr; // 失敗済み (ファイルが変わるまで再試行しない)
    }
    if (pendingSet_.insert(key).second) {
        pending_.push_back(key);
    }
    return e.srv.Get(); // 再生成待ちの間は古い絵を出す (初回は null)
}

void AssetPreviewCache::OnRenderViews(EngineContext& ctx)
{
    if (pending_.empty()) {
        return;
    }
    int budget = kMaxRendersPerFrame;
    while (budget > 0 && !pending_.empty()) {
        const std::wstring key = pending_.front();
        pending_.erase(pending_.begin());
        pendingSet_.erase(key);
        Entry& e = cache_[key];
        e.fileWriteTime = FileWriteTime(key);
        e.failed = !RenderOne(ctx, key, e);
        --budget;
    }
    EvictIfNeeded();
}

bool AssetPreviewCache::RenderOne(EngineContext& ctx, const std::wstring& path, Entry& entry)
{
    rt_.Resize(*ctx.device, kPreviewSize, kPreviewSize);
    if (!rt_.IsValid()) {
        return false;
    }

    // ---- 一時シーンに対象 1 体 + プレビュー用平行光を構築 ----
    tempScene_.Clear();
    World& world = tempScene_.GetWorld();
    if (EndsWith(path, L".prefab.json")) {
        const uint64_t hash = ctx.prefabs ? ctx.prefabs->LoadFromFile(path) : 0;
        if (hash == 0 || Prefab::Instantiate(tempScene_, *ctx.prefabs, hash, 0) == 0) {
            return false;
        }
    } else {
        GameObject o = EndsWith(path, L".fbx")
                           ? FbxLoader::Load(tempScene_, *ctx.resources, *ctx.shaders, path)
                           : ModelLoader::Load(tempScene_, *ctx.resources, *ctx.shaders, path);
        if (!o) {
            return false;
        }
    }
    tempScene_.CreateGameObject("preview_sun").AddComponent<LightComponent>(); // 既定 = 白平行光
    world.ApplyStructuralChanges();
    transforms_.Update(world);

    // ---- 包囲 AABB からカメラをフィット (30° 見下ろし、-Z 側 = ライトが当たる面) ----
    XMFLOAT3 bmin = { 0, 0, 0 };
    XMFLOAT3 bmax = { 0, 0, 0 };
    bool hasBounds = false;
    const ComponentTypeId req[] = { MeshRendererComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            auto* mr = world.GetComponent<MeshRendererComponent>(e);
            auto* wm = world.GetComponent<WorldMatrixComponent>(e);
            const Mesh* mesh = (mr && wm) ? ctx.resources->meshes.Get(mr->mesh) : nullptr;
            if (!mesh) {
                continue;
            }
            XMFLOAT3 wmin, wmax;
            WorldAabb(wm->value, mesh->aabbMin, mesh->aabbMax, wmin, wmax);
            if (!hasBounds) {
                bmin = wmin;
                bmax = wmax;
                hasBounds = true;
            } else {
                bmin = { std::min(bmin.x, wmin.x), std::min(bmin.y, wmin.y),
                         std::min(bmin.z, wmin.z) };
                bmax = { std::max(bmax.x, wmax.x), std::max(bmax.y, wmax.y),
                         std::max(bmax.z, wmax.z) };
            }
        }
    });
    if (!hasBounds) {
        return false; // 描けるメッシュが無い (空プレハブ等)
    }
    const XMFLOAT3 center = { (bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f,
                              (bmin.z + bmax.z) * 0.5f };
    const float radius = std::max(
        0.05f, 0.5f
                   * std::sqrt((bmax.x - bmin.x) * (bmax.x - bmin.x)
                               + (bmax.y - bmin.y) * (bmax.y - bmin.y)
                               + (bmax.z - bmin.z) * (bmax.z - bmin.z)));
    const float fovDeg = 40.0f;
    const float dist = radius / std::sin(XMConvertToRadians(fovDeg) * 0.5f) * 1.1f;
    const XMVECTOR eyeDir = XMVector3Normalize(XMVectorSet(-0.5f, 0.45f, -0.75f, 0.0f));
    const XMVECTOR centerV = XMLoadFloat3(&center);
    const XMVECTOR eye = XMVectorAdd(centerV, XMVectorScale(eyeDir, dist));

    CameraOverride cam;
    XMStoreFloat4x4(&cam.view, XMMatrixLookAtLH(eye, centerV, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat3(&cam.position, eye);
    cam.fovYDeg = fovDeg;
    cam.nearZ = std::max(0.01f, dist * 0.01f);
    cam.farZ = dist * 10.0f;

    // ---- 専用 RenderSystem で描画 (postFx/shadow 無し → 中間 RT を確保しない) ----
    previewRender_.enablePostFx = false;
    previewRender_.enableShadows = false;
    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    // M42a: depthSRV/dsvReadOnly は意図的に null のまま (viewKey=0) — プレビューは
    // パーティクル無しで深度読み系効果が不要。null なら効果側が自然に無効化される
    target.clearColor[0] = 0.13f;
    target.clearColor[1] = 0.13f;
    target.clearColor[2] = 0.15f;
    target.clearColor[3] = 1.0f;
    previewRender_.Render(world, *ctx.device, *ctx.renderPathForward, *ctx.shaders,
                          *ctx.resources, target, &cam, nullptr);
    tempScene_.Clear(); // 一時エンティティを残さない

    // ---- 結果をエントリ保持テクスチャへ複製 (rt_ は次のプレビューで上書きされるため) ----
    ID3D11Device* device = ctx.device->Device();
    ID3D11DeviceContext* dc = ctx.device->Context();
    if (!entry.tex) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = kPreviewSize;
        desc.Height = kPreviewSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, entry.tex.GetAddressOf()))
            || FAILED(device->CreateShaderResourceView(entry.tex.Get(), nullptr,
                                                       entry.srv.GetAddressOf()))) {
            entry.tex.Reset();
            entry.srv.Reset();
            return false;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> src;
    rt_.RTV()->GetResource(src.GetAddressOf());
    dc->CopyResource(entry.tex.Get(), src.Get());
    return true;
}

void AssetPreviewCache::EvictIfNeeded()
{
    while (cache_.size() > kMaxCacheEntries) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.lastUsedFrame < oldest->second.lastUsedFrame) {
                oldest = it;
            }
        }
        cache_.erase(oldest);
    }
}

} // namespace mye
