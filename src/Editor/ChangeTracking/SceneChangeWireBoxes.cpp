//====================================================================================
//                          SceneChangeWireBoxes.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Scene View に枠を出す変更済みエンティティの選び方 (実装)
//====================================================================================
#include "Editor/ChangeTracking/SceneChangeWireBoxes.h"

#include <algorithm>
#include <cmath>

#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Scene/Scene.h"

using namespace DirectX;

namespace mye {

bool ProjectChangePoint(const XMFLOAT3& position, const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float& depth)
{
    const XMMATRIX viewMatrix = XMLoadFloat4x4(&view);
    const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, XMLoadFloat4x4(&proj));
    const XMVECTOR point = XMVectorSet(position.x, position.y, position.z, 1.0f);
    XMFLOAT3 inView;
    XMStoreFloat3(&inView, XMVector3Transform(point, viewMatrix));
    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector4Transform(point, viewProj));
    depth = inView.z;
    const float limit = clip.w * kChangeBoxVisibleMargin;
    return inView.z > 0.0f && clip.w > 0.0f && std::fabs(clip.x) <= limit && std::fabs(clip.y) <= limit;
}

void CollectChangeBoxCandidates(Scene& scene, const SceneChangeOverlay& overlay, const XMFLOAT4X4& view, const XMFLOAT4X4& proj,
                                std::vector<ChangeBoxCandidate>& out)
{
    out.clear();
    if (!overlay.Show()) {
        return;
    }
    World& world = scene.GetWorld();
    for (const auto& [fileId, entity] : overlay.Entities()) {
        if (entity.status != OverlayStatus::Added && entity.status != OverlayStatus::Modified) {
            continue; // 削除は、ライブのシーンに居ない (ゴーストは別のサブ)
        }
        const GameObject object = scene.FindByFileId(static_cast<uint64_t>(fileId));
        if (!object) {
            continue;
        }
        const auto* worldMatrix = world.GetComponent<WorldMatrixComponent>(object.Id());
        if (worldMatrix == nullptr) {
            continue;
        }
        ChangeBoxCandidate c;
        c.fileId = static_cast<uint64_t>(fileId);
        c.entity = object.Id();
        c.position = { worldMatrix->value._41, worldMatrix->value._42, worldMatrix->value._43 };
        c.color = entity.status == OverlayStatus::Added ? OverlayColorId::Added : OverlayColorId::Modified;
        c.uncertain = entity.uncertain;
        c.visible = ProjectChangePoint(c.position, view, proj, c.depth);
        out.push_back(c);
    }
}

ChangeBoxSelection SelectChangeBoxes(std::vector<ChangeBoxCandidate> candidates, int maxBoxes)
{
    ChangeBoxSelection result;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const ChangeBoxCandidate& c) { return !c.visible; }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const ChangeBoxCandidate& a, const ChangeBoxCandidate& b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.fileId < b.fileId;
    });
    const size_t limit = maxBoxes > 0 ? static_cast<size_t>(maxBoxes) : 0;
    if (candidates.size() > limit) {
        result.omitted = static_cast<int>(candidates.size() - limit);
        candidates.resize(limit);
    }
    result.chosen = std::move(candidates);
    return result;
}

} // namespace mye
