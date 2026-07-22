#include "Engine/Engine/EffectSystem.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye {
namespace {

// root サブツリー全体 (root 含む) のエミッタ放出を切替
void SetSubtreeEmission(World& world, EntityID root, bool on)
{
    const int32_t v = on ? 1 : 0;
    std::function<void(EntityID)> visit = [&](EntityID e) {
        if (auto* em = world.GetComponent<ParticleEmitterComponent>(e)) {
            em->playing = v;
        }
        if (auto* tr = world.GetComponent<TrailRendererComponent>(e)) {
            tr->emitting = v;
        }
        auto* h = world.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

// root サブツリーの Animator を先頭へ巻き戻す (ループ再生用)
void RestartSubtreeAnimators(World& world, EntityID root)
{
    std::function<void(EntityID)> visit = [&](EntityID e) {
        if (auto* an = world.GetComponent<AnimatorComponent>(e)) {
            an->timeTicks = 0;
        }
        auto* h = world.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

} // namespace

void EffectSystem::RestartEffect(World& world, EntityID root)
{
    if (auto* fx = world.GetComponent<EffectComponent>(root)) {
        fx->elapsedTicks = 0;
        fx->playing = 1;
    }
    SetSubtreeEmission(world, root, true);
    RestartSubtreeAnimators(world, root);
}

void EffectSystem::Update(World& world)
{
    // EffectComponent を持つエンティティを index 昇順で収集 (決定論)
    std::vector<EntityID> effects;
    const ComponentTypeId req[] = { EffectComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            effects.push_back(arch.EntityAt(row));
        }
    });
    std::sort(effects.begin(), effects.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; });

    for (EntityID e : effects) {
        if (!IsEntityActive(world, e)) {
            continue;
        }
        auto* fx = world.GetComponent<EffectComponent>(e);
        if (!fx || !fx->playing) {
            continue;
        }
        const int32_t prev = fx->elapsedTicks;
        ++fx->elapsedTicks;

        if (fx->durationTicks <= 0) {
            continue; // 手動制御 (自動停止/破棄なし)
        }
        if (fx->elapsedTicks < fx->durationTicks) {
            continue; // 放出フェーズ継続中
        }
        // ---- ウィンドウ末に到達 ----
        if (fx->looping) {
            RestartEffect(world, e); // 巻き戻し + 子エミッタ/Animator 再開
            continue;
        }
        if (prev < fx->durationTicks) {
            // ちょうど停止した tick: 子エミッタの新規放出を止める (残粒子は寿命で消える)
            SetSubtreeEmission(world, e, false);
        }
        if (fx->autoDestroy && fx->elapsedTicks >= fx->durationTicks + fx->lingerTicks) {
            world.DestroyEntity(e); // tick 末に子孫ごと破棄 (ADR-005 構造変更)
        }
    }
}

} // namespace mye
