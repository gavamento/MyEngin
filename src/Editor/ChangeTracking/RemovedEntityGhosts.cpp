//====================================================================================
//                          RemovedEntityGhosts.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          削除されたエンティティのゴーストと Before の読み取り専用の表示 (実装)
//====================================================================================
#include "Editor/ChangeTracking/RemovedEntityGhosts.h"

#include <algorithm>
#include <set>

#include "Editor/ChangeTracking/ChangeFormat.h"
#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Editor/ChangeTracking/SceneChangeWireBoxes.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Scene/Scene.h"

using namespace DirectX;
using nlohmann::json;

namespace mye {

namespace {

// 表示の値を切る長さ (UTF-8 の文字単位)
constexpr size_t kBeforeValueMaxChars = 160;

bool ReadFloats(const json& value, size_t count, float* out)
{
    if (!value.is_array() || value.size() != count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!value[i].is_number()) {
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = value[i].get<float>();
    }
    return true;
}

bool ReadFileId(const json& value, uint64_t& out)
{
    if (value.is_number_unsigned()) {
        out = value.get<uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        out = static_cast<uint64_t>(value.get<int64_t>());
        return true;
    }
    return false;
}

const json* Component(const json& root, const char* name)
{
    if (!root.contains("components") || !root["components"].is_object()) {
        return nullptr;
    }
    const json& components = root["components"];
    const auto it = components.find(name);
    return it != components.end() && it->is_object() ? &*it : nullptr;
}

} // namespace

bool ParseRemovedBefore(const std::string& fragment, ParsedBefore& out)
{
    out = ParsedBefore();
    const json root = json::parse(fragment, nullptr, false);
    if (!root.is_object() || !root.contains("fileId") || !ReadFileId(root["fileId"], out.fileId)) {
        return false;
    }
    if (root.contains("name") && root["name"].is_string()) {
        out.name = root["name"].get<std::string>();
    }
    if (root.contains("parent") && ReadFileId(root["parent"], out.parentFileId) && out.parentFileId != 0) {
        out.hasParent = true;
    }
    if (const json* transform = Component(root, "LocalTransform")) {
        out.hasTransform = true;
        if (transform->contains("position")) {
            float v[3];
            if (ReadFloats((*transform)["position"], 3, v)) {
                out.position = { v[0], v[1], v[2] };
            }
        }
        if (transform->contains("rotation")) {
            float v[4];
            if (ReadFloats((*transform)["rotation"], 4, v)) {
                out.rotation = { v[0], v[1], v[2], v[3] };
            }
        }
        if (transform->contains("scale")) {
            float v[3];
            if (ReadFloats((*transform)["scale"], 3, v)) {
                out.scale = { v[0], v[1], v[2] };
            }
        }
    }
    if (const json* meshRenderer = Component(root, "MeshRenderer")) {
        out.hasMeshRenderer = true;
        if (meshRenderer->contains("mesh")) {
            const json& mesh = (*meshRenderer)["mesh"];
            if (mesh.is_number_unsigned()) {
                out.meshAsset = mesh.get<uint64_t>();
            }
        }
    }
    for (const char* name : { "SkinnedMesh", "SpriteRenderer", "ParticleEmitter", "Light", "UIElement" }) {
        out.hasOtherRenderable = out.hasOtherRenderable || Component(root, name) != nullptr;
    }
    return true;
}

XMFLOAT4X4 GhostLocalMatrix(const ParsedBefore& before)
{
    const XMVECTOR s = XMLoadFloat3(&before.scale);
    const XMVECTOR r = XMLoadFloat4(&before.rotation);
    const XMVECTOR t = XMLoadFloat3(&before.position);
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixAffineTransformation(s, XMVectorZero(), r, t));
    return m;
}

bool ComposeGhostWorld(uint64_t fileId, const std::unordered_map<uint64_t, ParsedBefore>& removed, const LiveWorldLookup& live, XMFLOAT4X4& world,
                       uint32_t& notes)
{
    notes = 0;
    if (removed.find(fileId) == removed.end()) {
        return false;
    }
    // 子 → 親の順に local を掛ける (ワールド = local * 親のワールド)。親が居なければ、そこがルート
    XMMATRIX composed = XMMatrixIdentity();
    std::set<uint64_t> seen;
    uint64_t current = fileId;
    for (int depth = 0;; ++depth) {
        const auto it = removed.find(current);
        const ParsedBefore& entity = it->second;
        seen.insert(current);
        const XMFLOAT4X4 local = GhostLocalMatrix(entity);
        composed = XMMatrixMultiply(composed, XMLoadFloat4x4(&local));
        if (!entity.hasParent) {
            break;
        }
        const uint64_t parent = entity.parentFileId;
        if (removed.find(parent) != removed.end()) {
            if (seen.count(parent) != 0 || depth + 1 >= kMaxGhostParentChain) {
                notes |= kGhostNoteParentUnknown; // 輪・段数の超過 (壊れた Before)
                break;
            }
            current = parent;
            continue;
        }
        XMFLOAT4X4 liveWorld;
        if (live && live(parent, liveWorld)) {
            composed = XMMatrixMultiply(composed, XMLoadFloat4x4(&liveWorld));
        } else {
            notes |= kGhostNoteParentUnknown;
        }
        break;
    }
    XMStoreFloat4x4(&world, composed);
    return true;
}

void CollectRemovedGhostCandidates(Scene& scene, const SceneChangeOverlay& overlay, const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                                   const MeshAvailable& meshAvailable, RemovedGhostScan& out)
{
    out = RemovedGhostScan();
    if (!overlay.Show() || !overlay.ShowGhosts() || !overlay.Tracking()) {
        return;
    }
    // 走査順は不定なので、fileId の昇順に並べてから読む (候補の並びを決定的にする)
    std::vector<const OverlayEntity*> removedEntities;
    for (const auto& [fileId, entity] : overlay.Entities()) {
        if (entity.status == OverlayStatus::Removed) {
            removedEntities.push_back(&entity);
        }
    }
    std::sort(removedEntities.begin(), removedEntities.end(), [](const OverlayEntity* a, const OverlayEntity* b) { return a->fileId < b->fileId; });

    std::unordered_map<uint64_t, ParsedBefore> parsed;
    for (const OverlayEntity* entity : removedEntities) {
        ParsedBefore before;
        if (entity->hasBefore && !entity->beforeOversize && ParseRemovedBefore(entity->before, before)) {
            before.fileId = static_cast<uint64_t>(entity->fileId); // 表の鍵を正とする
            parsed[before.fileId] = std::move(before);
        } else {
            ++out.undrawable;
        }
    }

    World& world = scene.GetWorld();
    const LiveWorldLookup live = [&](uint64_t fileId, XMFLOAT4X4& matrix) {
        const GameObject object = scene.FindByFileId(fileId);
        if (!object) {
            return false;
        }
        const auto* worldMatrix = world.GetComponent<WorldMatrixComponent>(object.Id());
        if (worldMatrix == nullptr) {
            return false;
        }
        matrix = worldMatrix->value;
        return true;
    };

    for (const OverlayEntity* entity : removedEntities) {
        const uint64_t fileId = static_cast<uint64_t>(entity->fileId);
        const auto it = parsed.find(fileId);
        if (it == parsed.end()) {
            continue;
        }
        const ParsedBefore& before = it->second;
        RemovedGhostCandidate c;
        c.fileId = fileId;
        c.name = before.name.empty() ? entity->name : before.name;
        c.uncertain = entity->uncertain;
        ComposeGhostWorld(fileId, parsed, live, c.world, c.notes);
        c.position = { c.world._41, c.world._42, c.world._43 };
        if (!before.hasTransform) {
            c.notes |= kGhostNoteNoTransform;
        }
        if (before.hasMeshRenderer && before.meshAsset != 0 && meshAvailable && meshAvailable(before.meshAsset)) {
            c.shape = GhostShape::Mesh;
            c.meshAsset = before.meshAsset;
            c.notes |= kGhostNoteMeshCurrent;
        } else if (before.hasMeshRenderer || before.hasOtherRenderable) {
            c.shape = GhostShape::Box;
        } else {
            c.shape = GhostShape::Marker;
        }
        c.visible = ProjectChangePoint(c.position, view, proj, c.depth);
        out.candidates.push_back(std::move(c));
    }
}

RemovedGhostSelection SelectRemovedGhosts(std::vector<RemovedGhostCandidate> candidates, int maxGhosts)
{
    RemovedGhostSelection result;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const RemovedGhostCandidate& c) { return !c.visible; }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const RemovedGhostCandidate& a, const RemovedGhostCandidate& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.fileId < b.fileId;
    });
    const size_t limit = maxGhosts > 0 ? static_cast<size_t>(maxGhosts) : 0;
    if (candidates.size() > limit) {
        result.omitted = static_cast<int>(candidates.size() - limit);
        candidates.resize(limit);
    }
    result.chosen = std::move(candidates);
    return result;
}

uint64_t HitTestRemovedGhosts(const std::vector<GhostPickTarget>& targets, const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float width, float height,
                              float mouseX, float mouseY, float radiusPx)
{
    const XMMATRIX viewProj = XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&proj));
    uint64_t best = 0;
    float bestDistance2 = radiusPx * radiusPx;
    for (const GhostPickTarget& target : targets) {
        const XMVECTOR clip = XMVector4Transform(XMVectorSet(target.position.x, target.position.y, target.position.z, 1.0f), viewProj);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) {
            continue; // カメラの後ろ
        }
        const float px = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * width;
        const float py = (0.5f - XMVectorGetY(clip) / w * 0.5f) * height;
        const float distance2 = (px - mouseX) * (px - mouseX) + (py - mouseY) * (py - mouseY);
        if (distance2 < bestDistance2 || (distance2 == bestDistance2 && best != 0 && target.fileId < best)) {
            best = target.fileId;
            bestDistance2 = distance2;
        }
    }
    return best;
}

RemovedBeforeView BuildRemovedBeforeView(int64_t fileId, int64_t revisionId, bool hasBefore, bool oversize, const std::string& fragment)
{
    RemovedBeforeView view;
    view.shown = true;
    view.fileId = fileId;
    view.revisionId = revisionId;
    if (oversize) {
        view.problem = BeforeProblem::Oversize;
        return view;
    }
    if (!hasBefore) {
        view.problem = BeforeProblem::NoBefore;
        return view;
    }
    const json root = json::parse(fragment, nullptr, false);
    if (!root.is_object()) {
        view.problem = BeforeProblem::Unreadable;
        return view;
    }
    if (root.contains("name") && root["name"].is_string()) {
        view.name = root["name"].get<std::string>();
    }
    uint64_t parent = 0;
    if (root.contains("parent") && ReadFileId(root["parent"], parent) && parent != 0) {
        view.hasParent = true;
        view.parentFileId = static_cast<int64_t>(parent);
    }
    auto clipped = [](const json& value) { return ClipValue(value.dump(), kBeforeValueMaxChars); };
    for (const auto& [key, value] : root.items()) {
        if (key == "fileId" || key == "name" || key == "parent" || key == "components") {
            continue;
        }
        view.entityFields.push_back({ key, clipped(value) });
    }
    if (root.contains("components") && root["components"].is_object()) {
        for (const auto& [componentName, body] : root["components"].items()) {
            BeforeComponent component;
            component.name = componentName;
            if (body.is_object()) {
                for (const auto& [fieldName, value] : body.items()) {
                    component.fields.push_back({ fieldName, clipped(value) });
                }
            } else {
                component.fields.push_back({ std::string(), clipped(body) });
            }
            view.components.push_back(std::move(component));
        }
    }
    return view;
}

} // namespace mye
