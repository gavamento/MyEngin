#include "Engine/Engine/Replay/GhostTrack.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye {

void GhostTrack::Reset()
{
    firstTick = 0;
    lastTick = 0;
    bytes = 0;
    keyCount = 0;
    truncated = false;
    verified = false;
    endHash = 0;
    entities.clear();
    slotByIndex_.clear();
}

void GhostTrack::Begin(uint64_t tick)
{
    Reset();
    firstTick = tick;
    lastTick = tick;
}

void GhostTrack::ToMatrix(const GhostKey& k, DirectX::XMFLOAT4X4& out)
{
    out = DirectX::XMFLOAT4X4(k.m[0], k.m[1], k.m[2], 0.0f, k.m[3], k.m[4], k.m[5], 0.0f, k.m[6],
                              k.m[7], k.m[8], 0.0f, k.m[9], k.m[10], k.m[11], 1.0f);
}

void GhostTrack::FromMatrix(const DirectX::XMFLOAT4X4& in, GhostKey& out)
{
    out.m[0] = in._11;
    out.m[1] = in._12;
    out.m[2] = in._13;
    out.m[3] = in._21;
    out.m[4] = in._22;
    out.m[5] = in._23;
    out.m[6] = in._31;
    out.m[7] = in._32;
    out.m[8] = in._33;
    out.m[9] = in._41;
    out.m[10] = in._42;
    out.m[11] = in._43;
}

size_t GhostTrack::SlotFor(EntityID id, AssetID mesh, const char* name)
{
    if (id.index >= slotByIndex_.size()) {
        slotByIndex_.resize(static_cast<size_t>(id.index) + 1, 0);
    }
    const uint32_t stored = slotByIndex_[id.index];
    if (stored != 0 && entities[stored - 1].id == id) {
        return stored - 1;
    }
    // 未登録、またはスロットが別世代に使い回された = 別のエンティティとして積む
    GhostEntityTrack t;
    t.id = id;
    t.mesh = mesh;
    if (name != nullptr) {
        std::strncpy(t.name, name, sizeof(t.name) - 1);
    }
    entities.push_back(std::move(t));
    bytes += sizeof(GhostEntityTrack);
    slotByIndex_[id.index] = static_cast<uint32_t>(entities.size());
    return entities.size() - 1;
}

bool GhostTrack::Sample(World& world, uint64_t tick, size_t maxBytes)
{
    if (truncated) {
        return false;
    }
    const ComponentTypeId req[] = { WorldMatrixComponent::sTypeId, MeshRendererComponent::sTypeId };
    const uint32_t tick32 = static_cast<uint32_t>(tick);
    bool over = false;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        if (over) {
            return;
        }
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count() && !over; ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
            GhostKey k;
            k.tick = tick32;
            FromMatrix(wm->value, k);
            // ★SlotFor は entities を伸ばしうるので、参照は取り直す
            const size_t slot = SlotFor(e, mr->mesh, world.GetName(e));
            GhostEntityTrack& t = entities[slot];
            t.lastSeenTick = tick32;
            if (!t.keys.empty() && t.keys.back().alive != 0
                && std::memcmp(t.keys.back().m, k.m, sizeof(k.m)) == 0) {
                continue; // 動いていない
            }
            if (bytes + sizeof(GhostKey) > maxBytes) {
                truncated = true;
                over = true;
                return;
            }
            t.keys.push_back(k);
            bytes += sizeof(GhostKey);
            ++keyCount;
        }
    });
    if (over) {
        return false;
    }
    // 今回見なかった (消えた) エンティティには墓標を 1 個だけ積む
    for (GhostEntityTrack& t : entities) {
        if (t.lastSeenTick == tick32 || t.keys.empty() || t.keys.back().alive == 0) {
            continue;
        }
        if (bytes + sizeof(GhostKey) > maxBytes) {
            truncated = true;
            return false;
        }
        GhostKey tomb;
        tomb.tick = tick32;
        tomb.alive = 0;
        t.keys.push_back(tomb);
        bytes += sizeof(GhostKey);
        ++keyCount;
    }
    lastTick = tick;
    return true;
}

const GhostKey* GhostTrack::KeyAt(size_t ent, uint64_t tick) const
{
    if (ent >= entities.size()) {
        return nullptr;
    }
    const auto& keys = entities[ent].keys;
    const uint32_t tick32 = static_cast<uint32_t>(std::min<uint64_t>(tick, 0xFFFFFFFFull));
    auto it = std::upper_bound(keys.begin(), keys.end(), tick32,
                               [](uint32_t v, const GhostKey& k) { return v < k.tick; });
    if (it == keys.begin()) {
        return nullptr;
    }
    return &*(it - 1);
}

size_t GhostTrack::MovingCount() const
{
    size_t n = 0;
    for (const GhostEntityTrack& t : entities) {
        if (t.keys.size() > 1) {
            ++n;
        }
    }
    return n;
}

} // namespace mye
