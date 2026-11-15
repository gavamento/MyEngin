//====================================================================================
//                          ScenePreviewSession.cpp
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の 3D 比較の状態 (プレビュー専用の Scene・モード・カメラ。GPU に触れない、実装)
//====================================================================================
#include "Editor/ChangeTracking/ScenePreviewSession.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>

#include "Editor/ChangeTracking/TrackClient.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/GameObject.h"

namespace mye {

using namespace DirectX;

namespace {

std::atomic<int> g_livePreviewScenes{ 0 };

constexpr float kPi = 3.14159265f;
constexpr float kOrbitRadPerPixel = 0.01f;
constexpr float kPitchLimit = 1.5f;
constexpr float kPanPerPixel = 0.0015f;
// 範囲に余裕を持たせる倍率と、小さすぎる範囲の下限 (1 エンティティだけでも寄りすぎない)
constexpr float kFitMargin = 1.3f;
constexpr float kFitMinRadius = 1.0f;

bool EndsWithIgnoreCase(const std::string& text, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    if (text.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char a = text[text.size() - n + i];
        const char b = suffix[i];
        if ((a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) != b) {
            return false;
        }
    }
    return true;
}

// fileId のエンティティのワールド位置と名前。居なければ false
bool FindWorldPosition(PreviewScene* preview, uint64_t fileId, XMFLOAT3& position, std::string& name)
{
    if (preview == nullptr) {
        return false;
    }
    GameObject object = preview->scene.FindByFileId(fileId);
    if (!object) {
        return false;
    }
    World& world = preview->scene.GetWorld();
    const WorldMatrixComponent* matrix = world.GetComponent<WorldMatrixComponent>(object.Id());
    if (matrix == nullptr) {
        return false;
    }
    position = { matrix->value._41, matrix->value._42, matrix->value._43 };
    name = world.GetName(object.Id());
    return true;
}

} // namespace

// ---- PreviewScene ----

PreviewScene::PreviewScene()
{
    ++g_livePreviewScenes;
}

PreviewScene::~PreviewScene()
{
    --g_livePreviewScenes;
}

int PreviewScene::LiveCount()
{
    return g_livePreviewScenes.load();
}

// ---- ScenePreviewSession ----

bool ScenePreviewSession::Open(TrackClient& client, RestoreFileIo& io, ScenePreviewOpenArgs args)
{
    Close();
    if (!EndsWithIgnoreCase(args.revision.path, ".scene.json")) {
        return false;
    }
    args_ = std::move(args);
    io_ = &io;
    open_ = true;
    afterSource_ = PreviewAfterSource::Revision;
    camera_ = PreviewCamera();
    StartSide(before_, true, client);
    StartSide(after_, false, client);
    return true;
}

void ScenePreviewSession::Close()
{
    before_ = Side();
    after_ = Side();
    markers_.clear();
    markersOmitted_ = 0;
    assets_.Cancel();
    assetsStarted_ = false;
    settledHandled_ = false;
    settleCount_ = 0;
    args_ = ScenePreviewOpenArgs();
    io_ = nullptr;
    open_ = false;
}

void ScenePreviewSession::StartSide(Side& side, bool isBefore, TrackClient& client)
{
    side = Side();
    side.state = SideState::Fetching;
    if (isBefore || afterSource_ == PreviewAfterSource::Revision) {
        side.fetch.StartBlob(client, isBefore ? args_.revision.before : args_.revision.after);
    } else {
        side.fetch.StartDisk(args_.projectRoot, args_.revision.path, *io_);
    }
    AdvanceSide(side);
}

void ScenePreviewSession::AdvanceSide(Side& side)
{
    if (side.state != SideState::Fetching) {
        return;
    }
    side.fetch.Tick();
    switch (side.fetch.State()) {
    case PreviewFetchState::Failed:
        side.state = SideState::Failed;
        side.failure = side.fetch.Failure();
        return;
    case PreviewFetchState::Ready: {
        auto preview = std::make_unique<PreviewScene>();
        side.load = LoadPreviewScene(preview->scene, side.fetch.Text(), args_.isManaged);
        side.fetch.Cancel(); // 読み終えた JSON の文字列を手放す
        if (side.load.status != PreviewLoadStatus::Ok) {
            side.state = SideState::Failed;
            return; // preview は破棄される
        }
        World& world = preview->scene.GetWorld();
        world.ApplyStructuralChanges();
        preview->transforms.Update(world);
        side.scene = std::move(preview);
        side.state = SideState::Loaded;
        return;
    }
    case PreviewFetchState::Idle:
    case PreviewFetchState::Loading: return;
    }
}

void ScenePreviewSession::Tick(TrackClient& client)
{
    if (!open_) {
        return;
    }
    AdvanceSide(before_);
    AdvanceSide(after_);
    if (Settled() && !settledHandled_) {
        OnSettled(client);
    }
}

void ScenePreviewSession::SetAfterSource(PreviewAfterSource source, TrackClient& client)
{
    if (!open_ || source == afterSource_) {
        return;
    }
    afterSource_ = source;
    settledHandled_ = false;
    markers_.clear();
    markersOmitted_ = 0;
    StartSide(after_, false, client);
}

void ScenePreviewSession::OnSettled(TrackClient& client)
{
    settledHandled_ = true;
    ++settleCount_;
    BuildMarkers();
    FitCamera();
    if (!assetsStarted_) {
        StartAssetProbe(client);
    }
}

void ScenePreviewSession::BuildMarkers()
{
    markers_.clear();
    markersOmitted_ = 0;
    // fileId ごとの色。追加・削除の行が、フィールドの更新より優先する
    std::map<uint64_t, OverlayColorId> colors;
    for (const TrackSceneChange& row : args_.sceneChanges) {
        if (!row.hasFileId || row.fileId <= 0) {
            continue;
        }
        const uint64_t fileId = static_cast<uint64_t>(row.fileId);
        if (row.changeType == "EntityAdded") {
            colors[fileId] = OverlayColorId::Added;
        } else if (row.changeType == "EntityRemoved") {
            colors[fileId] = OverlayColorId::Removed;
        } else if (colors.find(fileId) == colors.end()) {
            colors[fileId] = OverlayColorId::Modified;
        }
    }
    for (const auto& [fileId, color] : colors) { // fileId の昇順 = 決定的
        PreviewMarker marker;
        marker.fileId = fileId;
        marker.color = color;
        const bool removed = color == OverlayColorId::Removed;
        bool found = false;
        if (!removed) {
            found = FindWorldPosition(AfterScene(), fileId, marker.position, marker.name);
        }
        if (!found) {
            found = FindWorldPosition(BeforeScene(), fileId, marker.position, marker.name);
            marker.fromBefore = found;
        }
        if (!found) {
            continue; // どちらの Scene にも居ない (読めなかった側の変更)
        }
        if (markers_.size() >= static_cast<size_t>(kPreviewMaxMarkers)) {
            ++markersOmitted_;
            continue;
        }
        markers_.push_back(std::move(marker));
    }
}

void ScenePreviewSession::StartAssetProbe(TrackClient& client)
{
    if (!args_.assetPath || (BeforeScene() == nullptr && AfterScene() == nullptr)) {
        return;
    }
    std::set<uint64_t> refs;
    for (PreviewScene* preview : { BeforeScene(), AfterScene() }) {
        if (preview != nullptr) {
            for (const uint64_t guid : CollectPreviewAssetRefs(preview->scene.GetWorld())) {
                refs.insert(guid);
            }
        }
    }
    std::vector<std::string> paths;
    for (const uint64_t guid : refs) {
        std::string path = args_.assetPath(guid);
        if (!path.empty() && !EndsWithIgnoreCase(path, ".scene.json")) {
            paths.push_back(std::move(path));
        }
    }
    assetsStarted_ = true;
    assets_.Start(client, std::move(paths), args_.revision.revisionId);
}

// ---- カメラ ----

void ScenePreviewSession::OrbitCamera(float dx, float dy)
{
    camera_.yaw += dx * kOrbitRadPerPixel;
    camera_.pitch = std::clamp(camera_.pitch + dy * kOrbitRadPerPixel, -kPitchLimit, kPitchLimit);
}

void ScenePreviewSession::ZoomCamera(float wheel)
{
    if (wheel == 0.0f) {
        return;
    }
    camera_.distance = std::clamp(camera_.distance * (wheel > 0.0f ? 0.9f : 1.1f), kPreviewMinDistance, kPreviewMaxDistance);
}

void ScenePreviewSession::PanCamera(float dx, float dy)
{
    const XMVECTOR dir = XMVectorSet(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), -std::cos(camera_.pitch) * std::cos(camera_.yaw), 0.0f);
    const XMVECTOR forward = XMVectorNegate(dir);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
    const XMVECTOR up = XMVector3Cross(forward, right);
    const float scale = camera_.distance * kPanPerPixel;
    XMVECTOR center = XMLoadFloat3(&camera_.center);
    center = XMVectorAdd(center, XMVectorAdd(XMVectorScale(right, -dx * scale), XMVectorScale(up, dy * scale)));
    XMStoreFloat3(&camera_.center, center);
}

void ScenePreviewSession::FitCamera()
{
    XMVECTOR lo = XMVectorReplicate(1e30f);
    XMVECTOR hi = XMVectorReplicate(-1e30f);
    bool any = false;
    auto add = [&](const XMFLOAT3& p) {
        const XMVECTOR v = XMLoadFloat3(&p);
        lo = XMVectorMin(lo, v);
        hi = XMVectorMax(hi, v);
        any = true;
    };
    for (const PreviewMarker& marker : markers_) {
        add(marker.position);
    }
    if (!any) {
        // 変わったエンティティの位置が分からなければ、全エンティティの範囲 (After、無ければ Before)
        PreviewScene* preview = AfterScene() != nullptr ? AfterScene() : BeforeScene();
        if (preview != nullptr) {
            const ComponentTypeId required[] = { WorldMatrixComponent::sTypeId };
            preview->scene.GetWorld().ForEachArchetype(required, [&](Archetype& archetype) {
                const int column = archetype.FindTypeIndex(WorldMatrixComponent::sTypeId);
                for (uint32_t row = 0; row < archetype.Count(); ++row) {
                    const WorldMatrixComponent* matrix = static_cast<const WorldMatrixComponent*>(archetype.GetPtr(column, row));
                    add({ matrix->value._41, matrix->value._42, matrix->value._43 });
                }
            });
        }
    }
    if (!any) {
        camera_.center = { 0.0f, 0.0f, 0.0f };
        camera_.distance = PreviewCamera().distance;
        return;
    }
    XMStoreFloat3(&camera_.center, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    const float radius = (std::max)(XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) * 0.5f, kFitMinRadius);
    camera_.distance = std::clamp(radius / std::sin(kPreviewFovYDeg * 0.5f * kPi / 180.0f) * kFitMargin, kPreviewMinDistance, kPreviewMaxDistance);
}

XMMATRIX ScenePreviewSession::ViewMatrix() const
{
    const XMVECTOR center = XMLoadFloat3(&camera_.center);
    const XMVECTOR dir = XMVectorSet(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), -std::cos(camera_.pitch) * std::cos(camera_.yaw), 0.0f);
    const XMVECTOR eye = XMVectorAdd(center, XMVectorScale(dir, camera_.distance));
    return XMMatrixLookAtLH(eye, center, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

XMMATRIX ScenePreviewSession::ProjMatrix(float aspect) const
{
    return XMMatrixPerspectiveFovLH(kPreviewFovYDeg * kPi / 180.0f, aspect, kPreviewNearZ, kPreviewFarZ);
}

} // namespace mye
