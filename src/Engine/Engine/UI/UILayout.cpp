#include "Engine/Engine/UI/UILayout.h"

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye {
namespace uilayout {
namespace {

// 壊れたデータ (親循環など) でも必ず停止する上限。正常なシーンの UI 階層はこれより浅い
constexpr int kMaxDepth = 64;

// 最寄りの UIElement 持ち祖先 (間の非 UI ノードは読み飛ばす)。無ければ kNullEntity
EntityID FindUIParent(World& world, EntityID e)
{
    EntityID p = world.GetParent(e);
    for (int guard = 0; guard < kMaxDepth && p != kNullEntity; ++guard) {
        if (world.GetComponent<UIElementComponent>(p)) {
            return p;
        }
        p = world.GetParent(p);
    }
    return kNullEntity;
}

UIRect ResolveRectImpl(World& world, EntityID e, int screenW, int screenH, int depth)
{
    const auto* el = world.GetComponent<UIElementComponent>(e);
    if (!el) {
        return {};
    }
    UIRect base = { 0, 0, static_cast<float>(screenW), static_cast<float>(screenH) };
    if (el->space == 1 && depth < kMaxDepth) {
        const EntityID p = FindUIParent(world, e);
        if (p != kNullEntity) {
            base = ResolveRectImpl(world, p, screenW, screenH, depth + 1);
        }
    }
    UIRect r;
    AnchorOrigin(el->anchor, base, r.x, r.y);
    r.x += el->x;
    r.y += el->y;
    r.w = el->w;
    r.h = el->h;
    return r;
}

} // namespace

UIRect ResolveRect(World& world, EntityID e, int screenW, int screenH)
{
    return ResolveRectImpl(world, e, screenW, screenH, 0);
}

UIRect ResolveClipRect(World& world, EntityID e, int screenW, int screenH)
{
    UIRect clip = { 0, 0, static_cast<float>(screenW), static_cast<float>(screenH) };
    EntityID p = FindUIParent(world, e);
    for (int guard = 0; guard < kMaxDepth && p != kNullEntity; ++guard) {
        const auto* el = world.GetComponent<UIElementComponent>(p);
        if (el && el->clipChildren != 0) {
            clip = Intersect(clip, ResolveRect(world, p, screenW, screenH));
        }
        p = FindUIParent(world, p);
    }
    return clip;
}

UIRect ResolveVisibleRect(World& world, EntityID e, int screenW, int screenH)
{
    return Intersect(ResolveRect(world, e, screenW, screenH),
                     ResolveClipRect(world, e, screenW, screenH));
}

} // namespace uilayout
} // namespace mye
