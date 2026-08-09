#include "Engine/Engine/Replay/WorldHasher.h"

#include <algorithm>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameFlow.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"

namespace mye {
namespace {

// 1 エンティティ分: 親リンク + シリアライズ対象コンポーネントの登録フィールド
uint64_t HashEntity(World& world, EntityID e)
{
    const ComponentRegistry& reg = ComponentRegistry::Get();
    uint64_t h = kFnvOffset;
    h = HashCombine(h, e.index);
    h = HashCombine(h, e.generation);

    const EntityID parent = world.GetParent(e);
    h = HashCombine(h, parent.index);
    h = HashCombine(h, parent.generation);

    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        return h;
    }
    for (ComponentTypeId t : arch->Types()) { // TypeId 昇順 = 決定論
        const ComponentDesc& desc = reg.Desc(t);
        if (desc.flags & (kComponentNoSerialize | kComponentNoHash)) {
            continue; // WorldMatrix (派生値) / Hierarchy / FileId / C# スクリプト状態 (別レーン)
        }
        const void* comp = world.GetComponentRaw(e, t);
        if (!comp) {
            continue;
        }
        h = HashCombine(h, desc.nameHash);
        for (const FieldDesc& f : desc.fields) {
            if (f.flags & kFieldNoSerialize) {
                continue;
            }
            // ビットパターンをそのままハッシュ (float 演算で比較しない — spec 11.3)
            h = HashBytes(static_cast<const uint8_t*>(comp) + f.offset, FieldTypeSize(f.type), h);
        }
    }
    return h;
}

uint64_t HashCpuParticles(const CpuParticleBackend& cpu)
{
    uint64_t h = kFnvOffset;
    for (const CpuParticleBackend::EmitterPool& pool : cpu.Pools()) { // owner.index 昇順
        h = HashCombine(h, pool.owner.index);
        h = HashCombine(h, pool.owner.generation);
        h = HashCombine(h, pool.alive);
        h = HashBytes(&pool.emitAccum, sizeof(pool.emitAccum), h);
        h = HashCombine(h, static_cast<uint32_t>(pool.ageTicks)); // M32a: 放出ウィンドウ状態
        const uint64_t rngState = pool.rng.State();
        const uint64_t rngInc = pool.rng.Inc();
        h = HashCombine(h, rngState);
        h = HashCombine(h, rngInc);
        const uint32_t n = pool.alive;
        if (n > 0) {
            h = HashBytes(pool.px.data(), n * sizeof(float), h);
            h = HashBytes(pool.py.data(), n * sizeof(float), h);
            h = HashBytes(pool.pz.data(), n * sizeof(float), h);
            h = HashBytes(pool.vx.data(), n * sizeof(float), h);
            h = HashBytes(pool.vy.data(), n * sizeof(float), h);
            h = HashBytes(pool.vz.data(), n * sizeof(float), h);
            h = HashBytes(pool.life.data(), n * sizeof(float), h);
            h = HashBytes(pool.invLife.data(), n * sizeof(float), h);
            h = HashBytes(pool.size0.data(), n * sizeof(float), h);
        }
    }
    return h;
}

// M51g: ゲームフロー状態 (決定台帳 5)。RNG の直後・パーティクルの前に畳み込む。
// PersistStore は std::map = キー昇順走査 (挿入順に依存しない — selftest が固定)
uint64_t HashGameFlow(uint64_t h, const TimeControl* time, const PersistStore* persist)
{
    if (time) {
        h = HashCombine(h, time->paused ? 1u : 0u);
        h = HashCombine(h, static_cast<uint32_t>(time->scalePercent));
        h = HashCombine(h, static_cast<uint32_t>(time->accum));
    }
    if (persist) {
        for (const auto& [key, blob] : persist->Entries()) {
            h = HashCombine(h, key);
            h = HashCombine(h, static_cast<uint64_t>(blob.size()));
            if (!blob.empty()) {
                h = HashBytes(blob.data(), blob.size(), h);
            }
        }
    }
    return h;
}

void CollectEntitiesSorted(World& world, std::vector<EntityID>& out)
{
    out.clear();
    const ComponentTypeId req[] = { NameComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            out.push_back(arch.EntityAt(row));
        }
    });
    std::sort(out.begin(), out.end(),
              [](EntityID a, EntityID b) { return a.index < b.index; }); // 明示キー (規則 7)
}

} // namespace

void HashWorldDetailed(World& world, const CpuParticleBackend* cpuParticles,
                       std::vector<EntityHash>& outEntities, uint64_t& outTotal,
                       const TimeControl* time, const PersistStore* persist)
{
    std::vector<EntityID> entities;
    CollectEntitiesSorted(world, entities);

    outEntities.clear();
    outEntities.reserve(entities.size());
    uint64_t total = kFnvOffset;
    for (EntityID e : entities) {
        const uint64_t eh = HashEntity(world, e);
        outEntities.push_back({ e, eh });
        total = HashCombine(total, eh);
    }
    // ワールド RNG ストリーム
    total = HashCombine(total, world.Rng().State());
    total = HashCombine(total, world.Rng().Inc());
    // ゲームフロー状態 (M51g: RNG の直後)
    total = HashGameFlow(total, time, persist);
    // CPU パーティクル (spec 11.3: ハッシュ対象)
    if (cpuParticles) {
        total = HashCombine(total, HashCpuParticles(*cpuParticles));
    }
    outTotal = total;
}

uint64_t HashWorld(World& world, const CpuParticleBackend* cpuParticles,
                   const TimeControl* time, const PersistStore* persist)
{
    std::vector<EntityID> entities;
    CollectEntitiesSorted(world, entities);

    uint64_t total = kFnvOffset;
    for (EntityID e : entities) {
        total = HashCombine(total, HashEntity(world, e));
    }
    total = HashCombine(total, world.Rng().State());
    total = HashCombine(total, world.Rng().Inc());
    total = HashGameFlow(total, time, persist);
    if (cpuParticles) {
        total = HashCombine(total, HashCpuParticles(*cpuParticles));
    }
    return total;
}

} // namespace mye
