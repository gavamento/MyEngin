#include "Editor/AssetPreviewCache.h"

#include <algorithm>
#include <cmath>
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

constexpr int kPreviewSize = AssetPreviewCache::kPreviewSize;
constexpr int kMaxRendersPerFrame = 2;
constexpr size_t kMaxCacheEntries = 256; // 128^2 RGBA * 256 = 約 16MB

// ライブマテリアルプレビューのキャッシュキー (M53)。'<' '>' は Windows のパスに使えない
// 文字なので、NormalizePathKey が返す実パスと衝突しない
const wchar_t* const kLiveMaterialKey = L"<live-material>";
// 同じくライブマテリアルを MaterialLibrary へ置くときの AssetID。名前ハッシュ空間とは
// 別枠でよい固定値 (RegisterAnonymous は names_ に載せないので列挙にも出ない)
constexpr uint64_t kLiveMaterialAssetKey = 0x9E3779B97F4A7C15ull;

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

uint64_t FileWriteTime(const std::wstring& path)
{
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
}

// 平行光の向きは「エンティティの前方 (+Z)」なので、望む進行方向 dir を +Z に持つ
// 回転行列を組んでクォータニオンにする
void SetForward(GameObject& o, DirectX::FXMVECTOR dir)
{
    auto* t = o.GetComponent<LocalTransform>();
    if (!t) {
        return;
    }
    const XMVECTOR fwd = XMVector3Normalize(dir);
    XMVECTOR upHint = XMVectorSet(0, 1, 0, 0);
    if (std::fabs(XMVectorGetX(XMVector3Dot(fwd, upHint))) > 0.99f) {
        upHint = XMVectorSet(0, 0, 1, 0); // 真上/真下向きで基底が潰れるのを避ける
    }
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(upHint, fwd));
    XMMATRIX m = XMMatrixIdentity();
    m.r[0] = right;
    m.r[1] = XMVector3Cross(fwd, right);
    m.r[2] = fwd;
    XMStoreFloat4(&t->rotation, XMQuaternionRotationMatrix(m));
}

} // namespace

bool AssetPreviewCache::IsMaterialPath(const std::wstring& path)
{
    return EndsWith(path, L".mat.json");
}

bool AssetPreviewCache::IsPreviewable(const std::wstring& path)
{
    return EndsWith(path, L".glb") || EndsWith(path, L".gltf") || EndsWith(path, L".fbx")
           || IsMaterialPath(path) || PrefabLibrary::IsComposePath(path);
}

// スタンプ (= 今の絵がどの入力に対応しているか) を突き合わせて、要れば生成キューに積む。
// ファイルもライブ値もこの 1 本に集約してある (M53)
ID3D11ShaderResourceView* AssetPreviewCache::Touch(EngineContext& ctx, const std::wstring& key,
                                                   uint64_t wantStamp, Entry& e)
{
    e.lastUsedFrame = ctx.frameIndex;
    if (e.stamp == wantStamp && (e.srv || e.failed)) {
        return e.failed ? nullptr : e.srv.Get(); // 失敗済みは入力が変わるまで再試行しない
    }
    e.wantStamp = wantStamp;
    if (pendingSet_.insert(key).second) {
        pending_.push_back(key);
    }
    return e.srv.Get(); // 再生成待ちの間は古い絵を出す (初回は null)
}

ID3D11ShaderResourceView* AssetPreviewCache::GetOrRequest(EngineContext& ctx,
                                                          const std::wstring& path)
{
    const std::wstring key = NormalizePathKey(path);
    return Touch(ctx, key, FileWriteTime(path), cache_[key]);
}

ID3D11ShaderResourceView* AssetPreviewCache::GetOrRequestMaterial(EngineContext& ctx,
                                                                  const Material& mat,
                                                                  PreviewShape shape,
                                                                  uint64_t valueHash)
{
    const std::wstring key = kLiveMaterialKey;
    Entry& e = cache_[key];
    e.live = true;
    e.liveMat = mat;
    e.liveShape = shape;
    // 形状もスタンプに混ぜる (材質が同じでも形状を変えたら描き直しが要る)。
    // +1 は「まだ一度も描いていない = stamp 0」と衝突させないため
    const uint64_t stamp = valueHash * 1099511628211ull + static_cast<uint64_t>(shape) + 1;
    return Touch(ctx, key, stamp, e);
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
        e.stamp = e.wantStamp; // 成否によらず更新 (失敗の再試行も入力が変わるまで止める)
        e.failed = !RenderOne(ctx, key, e);
        --budget;
    }
    EvictIfNeeded();
}

bool AssetPreviewCache::RenderOne(EngineContext& ctx, const std::wstring& path, Entry& entry)
{
    // ★描画先を **_SRGB** にする (M53)。プレビューは postFx を切っている = トーンマップも
    // OETF も通らないので、UNORM RT だとリニア値がそのまま画面に出て軒並み暗くなる
    // (linear 0.20 が 51/255 として表示される)。_SRGB RTV なら書き込み時に HW が
    // linear→sRGB を掛けてくれるので、ImGui へは「そのまま出せるバイト列」が渡る。
    // 複製先 (entry.tex) は UNORM のまま = 同じ型グループなので CopyResource は合法で、
    // SRV でデコードし直さない (= 二重変換にならない)
    rt_.Resize(*ctx.device, kPreviewSize, kPreviewSize, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (!rt_.IsValid()) {
        return false;
    }

    // ---- 一時シーンに対象 1 体 + プレビュー用平行光を構築 ----
    tempScene_.Clear();
    World& world = tempScene_.GetWorld();

    // マテリアルは「実体」を持たないので、材質を当てる形状をこちらで用意する (M53)。
    // ファイル版 (AssetBrowser のタイル) は球固定、ライブ版 (Inspector) は形状トグルに従う
    const bool isMaterial = entry.live || IsMaterialPath(path);
    XMFLOAT3 center = { 0, 0, 0 };
    float radius = 0.0f;
    // カメラの立ち位置 (中心から見た方向)。既定は 30° 見下ろしの斜め — モデル/プレハブは従来どおり
    XMVECTOR eyeDir = XMVector3Normalize(XMVectorSet(-0.5f, 0.45f, -0.75f, 0.0f));
    bool hasBounds = false;

    if (isMaterial) {
        AssetID matId;
        if (entry.live) {
            matId = ctx.resources->materials.RegisterAnonymous(kLiveMaterialAssetKey,
                                                               entry.liveMat);
        } else {
            matId = ctx.resources->materials.LoadFromFile(path, ctx.resources->textures,
                                                          ctx.assetsRoot);
        }
        if (matId.IsNull()) {
            return false;
        }
        GameObject o = tempScene_.CreateGameObject("preview_material");
        auto* mr = o.AddComponent<MeshRendererComponent>();
        if (!mr) {
            return false;
        }
        mr->material = matId;
        // 形状ごとにメッシュ・カメラ方向・包囲半径を決め打つ。下の AABB 自動フィットに
        // 任せると球でも「対角 = sqrt(3) 倍」を半径に取ってしまい、タイルの中で小さく写る
        switch (entry.live ? entry.liveShape : PreviewShape::Sphere) {
        case PreviewShape::Cube:
            mr->mesh = ctx.resources->meshes.Cube();
            radius = 0.866f; // 単位キューブの半対角 sqrt(3)/2
            break;
        case PreviewShape::Plane:
            // 1x1 の板をほぼ正対で見る (テクスチャ/ノーマルマップの確認用)。
            // Plane() は XZ 平面なので真横から見ることになり材質確認には向かない。
            // ★Quad() の法線は **-Z** (実装が正、GpuResources.h の宣言コメントは誤記だった)
            // なので、カメラは -Z 側に置かないと裏面を見て背面カリングで消える
            mr->mesh = ctx.resources->meshes.Quad();
            eyeDir = XMVector3Normalize(XMVectorSet(0.0f, 0.18f, -1.0f, 0.0f));
            radius = 0.72f; // 1x1 の半対角 sqrt(2)/2 に少し余白
            break;
        case PreviewShape::Sphere:
        default:
            mr->mesh = ctx.resources->meshes.Sphere();
            radius = 0.5f; // Sphere() は半径 0.5
            break;
        }
        hasBounds = true;
    } else if (PrefabLibrary::IsComposePath(path)) {
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

    GameObject sun = tempScene_.CreateGameObject("preview_sun");
    sun.AddComponent<LightComponent>(); // 既定 = 白平行光 (+Z 向き)
    if (isMaterial) {
        // マテリアルは**カメラの左上から**当てる。既定の +Z 向きのままだと形状によって
        // 見えている面が丸ごと影になる (法線 +Z の板が典型) し、斜めから当たらないと
        // metallic / roughness の差がハイライトに出ない。モデル側の絵は変えない
        const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        // LH の右 = up × forward。forward はカメラの視線 (中心へ向かう = -eyeDir)
        XMVECTOR viewRight = XMVector3Cross(worldUp, XMVectorNegate(eyeDir));
        if (XMVectorGetX(XMVector3LengthSq(viewRight)) < 1e-6f) {
            viewRight = XMVectorSet(1, 0, 0, 0); // 真上から覗く構図の保険
        }
        viewRight = XMVector3Normalize(viewRight);
        const XMVECTOR lightFrom =
            XMVector3Normalize(XMVectorAdd(XMVectorAdd(eyeDir, XMVectorScale(worldUp, 0.55f)),
                                           XMVectorScale(viewRight, -0.45f)));
        SetForward(sun, XMVectorNegate(lightFrom)); // 光源の「居場所」の逆 = 進行方向
    }
    world.ApplyStructuralChanges();
    transforms_.Update(world);

    // ---- 包囲 AABB からカメラをフィット (マテリアルは上で決め打ち済み) ----
    if (!hasBounds) {
        XMFLOAT3 bmin = { 0, 0, 0 };
        XMFLOAT3 bmax = { 0, 0, 0 };
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
        center = { (bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f,
                   (bmin.z + bmax.z) * 0.5f };
        radius = std::max(0.05f,
                          0.5f
                              * std::sqrt((bmax.x - bmin.x) * (bmax.x - bmin.x)
                                          + (bmax.y - bmin.y) * (bmax.y - bmin.y)
                                          + (bmax.z - bmin.z) * (bmax.z - bmin.z)));
    }
    const float fovDeg = 40.0f;
    const float dist = radius / std::sin(XMConvertToRadians(fovDeg) * 0.5f) * 1.1f;
    const XMVECTOR centerV = XMLoadFloat3(&center);
    const XMVECTOR eye = XMVectorAdd(centerV, XMVectorScale(eyeDir, dist));

    CameraOverride cam;
    XMStoreFloat4x4(&cam.view, XMMatrixLookAtLH(eye, centerV, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat3(&cam.position, eye);
    cam.fovYDeg = fovDeg;
    cam.nearZ = std::max(0.01f, dist * 0.01f);
    cam.farZ = dist * 10.0f;

    // ---- 専用 RenderSystem で描画 (postFx/shadow 無し → 中間 RT を確保しない) ----
    // ★M54e: enableShadows=false は局所ライトのシャドウアトラス (M54c/M54d) も止める —
    //   RenderSystem がアトラス節に入らない = view.shadowAtlasSRV が null のまま +
    //   GpuLight::shadowFaces が全部 0 のまま。ForwardPath はその null を見て
    //   gShadowAtlasEnabled=0 を送り、t6 にも null を張る。**このゲートが 1 枚でも
    //   抜けるとサムネイルだけが未初期化のタイル行列でアトラスをサンプルする**
    //   (プレビューは 64MB のアトラスを一生確保しないので中身は他ビューの残骸)
    previewRender_.enablePostFx = false;
    previewRender_.enableShadows = false;
    FrameTarget target;
    target.rtv = rt_.RTV();
    target.dsv = rt_.DSV();
    target.width = rt_.Width();
    target.height = rt_.Height();
    // M42a: depthSRV/dsvReadOnly は意図的に null のまま (viewKey=0) — プレビューは
    // パーティクル無しで深度読み系効果が不要。null なら効果側が自然に無効化される
    // 背景色はリニアで置く — _SRGB RTV の Clear は指定値を linear と解釈して符号化するので、
    // 従来と同じ見た目 (sRGB 0.13/0.13/0.15) にするにはその逆変換値を書く必要がある
    target.clearColor[0] = 0.0152f; // = SrgbToLinear(0.13)
    target.clearColor[1] = 0.0152f;
    target.clearColor[2] = 0.0194f; // = SrgbToLinear(0.15)
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
