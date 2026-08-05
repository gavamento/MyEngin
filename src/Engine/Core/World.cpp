#include "Engine/Core/World.h"

#include <algorithm>
#include <cstring>

#include "Engine/Core/Check.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/NameUtil.h"

namespace mye {

World::World()
{
    RegisterBuiltinComponents();
    baseTypes_ = { NameComponent::sTypeId, LocalTransform::sTypeId,
                   WorldMatrixComponent::sTypeId, HierarchyComponent::sTypeId };
    std::sort(baseTypes_.begin(), baseTypes_.end());
    // 基本アーキタイプを先に作っておく:
    // イテレーション中の CreateEntity が archetypes_ を伸ばさないようにするため
    GetOrCreateArchetype(baseTypes_);
    rng_.Seed(0x4D794531ull); // 'MyE1' — シーンロード/リプレイ側で再シードされる
}

// ---------------------------------------------------------------- entities

EntityID World::CreateEntity(std::string_view name)
{
    uint32_t index;
    if (!freeIndices_.empty()) {
        index = freeIndices_.back();
        freeIndices_.pop_back();
    } else {
        index = static_cast<uint32_t>(records_.size());
        records_.emplace_back();
    }
    EntityRecord& rec = records_[index];
    const EntityID e{ index, rec.generation };

    Archetype* arch = GetOrCreateArchetype(baseTypes_);
    rec.archetype = arch;
    rec.row = arch->AddRow(e);
    ++aliveCount_;

    auto* nc = static_cast<NameComponent*>(arch->GetPtr(arch->FindTypeIndex(NameComponent::sTypeId), rec.row));
    // 切り詰めは UTF-8 の文字境界で行う (M48b)。生バイトで切ると多バイト文字が分断され、
    // シーン保存時に nlohmann が不正 UTF-8 で例外を投げる。SetEntityName と同じ規則に揃える
    const std::string_view fit = nameutil::TruncateUtf8(name, sizeof(nc->value) - 1);
    memcpy(nc->value, fit.data(), fit.size());
    nc->value[fit.size()] = '\0';

    hierarchyDirty_ = true;
    AppendToChildList(kNullEntity, e); // ルート兄弟リスト末尾へ (兄弟順 = 生成順で決定論的)
    return e;
}

void World::DestroyEntity(EntityID e)
{
    if (!IsAlive(e)) {
        return;
    }
    Command c = {};
    c.type = CmdType::Destroy;
    c.entity = e;
    commands_.push_back(c);
}

bool World::IsAlive(EntityID e) const
{
    return e.index < records_.size()
        && records_[e.index].generation == e.generation
        && records_[e.index].archetype != nullptr;
}

// ---------------------------------------------------------------- components

void* World::AddComponentRaw(EntityID e, ComponentTypeId t)
{
    if (!IsAlive(e)) {
        MYE_LOG_WARN("AddComponent on dead entity (%u:%u)", e.index, e.generation);
        return nullptr;
    }
    if (void* existing = GetComponentRaw(e, t)) {
        return existing; // 既に所持
    }
    if (!IsIterating()) {
        return AddComponentImmediate(e, t);
    }
    // イテレーション中: scratch にデフォルト値を作って返し、tick 末に実体へコピー
    const ComponentDesc& desc = ComponentRegistry::Get().Desc(t);
    auto payload = std::make_unique<std::byte[]>(desc.size);
    desc.construct(payload.get());
    void* scratch = payload.get();

    Command c = {};
    c.type = CmdType::AddComponent;
    c.entity = e;
    c.component = t;
    c.payloadIndex = static_cast<uint32_t>(cmdPayloads_.size());
    cmdPayloads_.push_back(std::move(payload));
    commands_.push_back(c);
    return scratch;
}

void World::RemoveComponentRaw(EntityID e, ComponentTypeId t)
{
    if (!IsAlive(e)) {
        return;
    }
    if (!IsIterating()) {
        RemoveComponentImmediate(e, t);
        return;
    }
    Command c = {};
    c.type = CmdType::RemoveComponent;
    c.entity = e;
    c.component = t;
    commands_.push_back(c);
}

void* World::GetComponentRaw(EntityID e, ComponentTypeId t)
{
    if (!IsAlive(e)) {
        return nullptr;
    }
    const EntityRecord& rec = records_[e.index];
    const int ti = rec.archetype->FindTypeIndex(t);
    if (ti < 0) {
        return nullptr;
    }
    return rec.archetype->GetPtr(ti, rec.row);
}

bool World::HasComponent(EntityID e, ComponentTypeId t) const
{
    if (!IsAlive(e)) {
        return false;
    }
    return records_[e.index].archetype->HasType(t);
}

// ---------------------------------------------------------------- hierarchy

void World::SetParent(EntityID child, EntityID parent)
{
    if (!IsAlive(child)) {
        return;
    }
    Command c = {};
    c.type = CmdType::SetParent;
    c.entity = child;
    c.parent = parent;
    commands_.push_back(c);
}

void World::SetSiblingIndex(EntityID e, uint32_t index)
{
    if (!IsAlive(e)) {
        return;
    }
    Command c = {};
    c.type = CmdType::SetSiblingIndex;
    c.entity = e;
    c.order = index;
    commands_.push_back(c);
}

EntityID World::GetParent(EntityID e)
{
    auto* h = GetComponent<HierarchyComponent>(e);
    return h ? h->parent : kNullEntity;
}

const char* World::GetName(EntityID e)
{
    auto* n = GetComponent<NameComponent>(e);
    return n ? n->value : "";
}

// ---------------------------------------------------------------- structural

Archetype* World::GetOrCreateArchetype(std::vector<ComponentTypeId> sortedTypes)
{
    const uint64_t hash = ComputeSignatureHash(sortedTypes);
    for (auto& arch : archetypes_) {
        if (arch->SignatureHash() == hash) {
            // ハッシュ衝突対策: 型リストの完全一致まで確認する
            auto types = arch->Types();
            if (types.size() == sortedTypes.size()
                && std::equal(types.begin(), types.end(), sortedTypes.begin())) {
                return arch.get();
            }
        }
    }
    MYE_CHECKF(!IsIterating(), "archetype creation during iteration (invalidates iteration)");
    archetypes_.push_back(std::make_unique<Archetype>(std::move(sortedTypes), hash));
    return archetypes_.back().get();
}

void World::MoveEntity(EntityID e, Archetype* dst)
{
    EntityRecord& rec = records_[e.index];
    Archetype* src = rec.archetype;
    const uint32_t srcRow = rec.row;
    const uint32_t dstRow = dst->AddRow(e);

    // 共通コンポーネントをコピー (新規コンポーネントは AddRow でデフォルト構築済み)
    const ComponentRegistry& reg = ComponentRegistry::Get();
    auto srcTypes = src->Types();
    for (size_t i = 0; i < srcTypes.size(); ++i) {
        const int di = dst->FindTypeIndex(srcTypes[i]);
        if (di >= 0) {
            memcpy(dst->GetPtr(di, dstRow), src->GetPtr(static_cast<int>(i), srcRow),
                   reg.Desc(srcTypes[i]).size);
        }
    }

    const EntityID moved = src->RemoveRow(srcRow);
    if (!moved.IsNull()) {
        records_[moved.index].row = srcRow;
    }
    rec.archetype = dst;
    rec.row = dstRow;
}

void* World::AddComponentImmediate(EntityID e, ComponentTypeId t)
{
    EntityRecord& rec = records_[e.index];
    if (rec.archetype->HasType(t)) {
        return rec.archetype->GetPtr(rec.archetype->FindTypeIndex(t), rec.row);
    }
    auto types = rec.archetype->Types();
    std::vector<ComponentTypeId> newTypes(types.begin(), types.end());
    newTypes.insert(std::upper_bound(newTypes.begin(), newTypes.end(), t), t);
    Archetype* dst = GetOrCreateArchetype(std::move(newTypes));
    MoveEntity(e, dst);
    return dst->GetPtr(dst->FindTypeIndex(t), rec.row);
}

void World::RemoveComponentImmediate(EntityID e, ComponentTypeId t)
{
    // 基本 4 コンポーネントの除去は禁止 (Transform/Hierarchy 系の前提が壊れる)
    for (ComponentTypeId base : baseTypes_) {
        if (t == base) {
            MYE_LOG_WARN("RemoveComponent: base component '%s' cannot be removed",
                         ComponentRegistry::Get().Desc(t).name);
            return;
        }
    }
    EntityRecord& rec = records_[e.index];
    if (!rec.archetype->HasType(t)) {
        return;
    }
    auto types = rec.archetype->Types();
    std::vector<ComponentTypeId> newTypes;
    newTypes.reserve(types.size() - 1);
    for (ComponentTypeId x : types) {
        if (x != t) {
            newTypes.push_back(x);
        }
    }
    Archetype* dst = GetOrCreateArchetype(std::move(newTypes));
    MoveEntity(e, dst);
}

EntityID* World::ChildListHead(EntityID parent)
{
    if (parent.IsNull()) {
        return &firstRoot_;
    }
    auto* ph = GetComponent<HierarchyComponent>(parent);
    return ph ? &ph->firstChild : nullptr;
}

void World::AppendToChildList(EntityID parent, EntityID child)
{
    EntityID* head = ChildListHead(parent);
    if (!head) {
        return;
    }
    if (head->IsNull()) {
        *head = child;
        return;
    }
    EntityID cur = *head;
    for (;;) {
        auto* ch = GetComponent<HierarchyComponent>(cur);
        if (!ch || ch->nextSibling.IsNull()) {
            if (ch) {
                ch->nextSibling = child;
            }
            break;
        }
        cur = ch->nextSibling;
    }
}

void World::UnlinkFromParent(EntityID e)
{
    auto* h = GetComponent<HierarchyComponent>(e);
    if (!h) {
        return;
    }
    // ルート (parent=null) も firstRoot_ を先頭とする兄弟リストで管理する
    EntityID* head = ChildListHead(h->parent);
    if (head) {
        if (*head == e) {
            *head = h->nextSibling;
        } else {
            EntityID cur = *head;
            while (!cur.IsNull()) {
                auto* ch = GetComponent<HierarchyComponent>(cur);
                if (!ch) {
                    break;
                }
                if (ch->nextSibling == e) {
                    ch->nextSibling = h->nextSibling;
                    break;
                }
                cur = ch->nextSibling;
            }
        }
    }
    h->parent = kNullEntity;
    h->nextSibling = kNullEntity;
}

void World::ApplySetParent(EntityID child, EntityID parent)
{
    if (!IsAlive(child)) {
        return;
    }
    if (!parent.IsNull()) {
        if (!IsAlive(parent)) {
            return;
        }
        // 循環チェック: parent の祖先に child がいたら拒否
        EntityID cur = parent;
        while (!cur.IsNull()) {
            if (cur == child) {
                MYE_LOG_WARN("SetParent: cycle detected (child %u)", child.index);
                return;
            }
            cur = GetParent(cur);
        }
    }

    UnlinkFromParent(child);
    auto* h = GetComponent<HierarchyComponent>(child);
    h->parent = parent;
    h->nextSibling = kNullEntity;
    AppendToChildList(parent, child); // 末尾に追加 (適用順 = 兄弟順で決定論的)
    hierarchyDirty_ = true;
}

void World::ApplySetSiblingIndex(EntityID e, uint32_t index)
{
    if (!IsAlive(e)) {
        return;
    }
    auto* h = GetComponent<HierarchyComponent>(e);
    if (!h) {
        return;
    }
    const EntityID parent = h->parent;
    UnlinkFromParent(e);   // 兄弟リストから外す (h->parent は null になる)
    h->parent = parent;    // 同じ親へ復帰
    EntityID* head = ChildListHead(parent);
    if (!head) {
        return;
    }
    if (index == 0 || head->IsNull()) {
        h->nextSibling = *head;
        *head = e;
    } else {
        EntityID cur = *head;
        uint32_t i = 1;
        while (i < index) {
            auto* ch = GetComponent<HierarchyComponent>(cur);
            if (!ch || ch->nextSibling.IsNull()) {
                break; // index が末尾を超える → 末尾に挿入
            }
            cur = ch->nextSibling;
            ++i;
        }
        auto* ch = GetComponent<HierarchyComponent>(cur);
        if (ch) {
            h->nextSibling = ch->nextSibling;
            ch->nextSibling = e;
        }
    }
    hierarchyDirty_ = true;
}

void World::CollectSubtree(EntityID root, std::vector<EntityID>& out)
{
    out.push_back(root);
    auto* h = GetComponent<HierarchyComponent>(root);
    if (!h) {
        return;
    }
    EntityID child = h->firstChild;
    while (!child.IsNull()) {
        auto* ch = GetComponent<HierarchyComponent>(child);
        const EntityID next = ch ? ch->nextSibling : kNullEntity;
        if (IsAlive(child)) {
            CollectSubtree(child, out);
        }
        child = next;
    }
}

void World::DestroyImmediate(EntityID e)
{
    if (!IsAlive(e)) {
        return;
    }
    std::vector<EntityID> subtree;
    CollectSubtree(e, subtree);
    UnlinkFromParent(e);

    for (EntityID s : subtree) {
        EntityRecord& rec = records_[s.index];
        const EntityID moved = rec.archetype->RemoveRow(rec.row);
        if (!moved.IsNull()) {
            records_[moved.index].row = rec.row;
        }
        rec.archetype = nullptr;
        ++rec.generation; // 古いハンドルを無効化
        freeIndices_.push_back(s.index);
        --aliveCount_;
    }
    hierarchyDirty_ = true;
}

void World::ReplaceComponentStorage(ComponentTypeId t, ComponentDesc newDesc)
{
    MYE_CHECK(!IsIterating());
    // UpdateDesc で参照が無効になる前に旧フィールド表をコピー
    const ComponentDesc& oldRef = ComponentRegistry::Get().Desc(t);
    const std::vector<FieldDesc> oldFields = oldRef.fields;
    const uint32_t oldSize = oldRef.size;
    (void)oldSize;

    for (auto& arch : archetypes_) {
        const int ti = arch->FindTypeIndex(t);
        if (ti < 0) {
            continue;
        }
        const uint32_t count = arch->Count();
        std::vector<std::byte> fresh(static_cast<size_t>(count) * newDesc.size);
        for (uint32_t row = 0; row < count; ++row) {
            std::byte* dst = fresh.data() + static_cast<size_t>(row) * newDesc.size;
            newDesc.construct(dst);
            const auto* src = static_cast<const std::byte*>(arch->GetPtr(ti, row));
            // 名前と型が一致するフィールドのみ引き継ぐ (spec 8.4 の状態保存規則)
            for (const FieldDesc& nf : newDesc.fields) {
                for (const FieldDesc& of : oldFields) {
                    if (nf.type == of.type && strcmp(nf.name, of.name) == 0) {
                        memcpy(dst + nf.offset, src + of.offset, FieldTypeSize(nf.type));
                        break;
                    }
                }
            }
        }
        arch->ReplaceColumn(ti, std::move(fresh), newDesc.size);
    }
    ComponentRegistry::Get().UpdateDesc(t, std::move(newDesc));
}

void World::Clear()
{
    MYE_CHECK(!IsIterating());
    commands_.clear();
    cmdPayloads_.clear();
    archetypes_.clear();
    freeIndices_.clear();
    firstRoot_ = kNullEntity; // ルートリストも破棄 (全エンティティ削除)
    // 世代を進めて全スロットを解放 (降順 push → 昇順割り当てで新規ワールドと同じ順になる)
    for (uint32_t i = static_cast<uint32_t>(records_.size()); i > 0; --i) {
        EntityRecord& rec = records_[i - 1];
        if (rec.archetype != nullptr) {
            rec.archetype = nullptr;
            ++rec.generation;
        }
        freeIndices_.push_back(i - 1);
    }
    aliveCount_ = 0;
    hierarchyDirty_ = true;
    GetOrCreateArchetype(baseTypes_); // 基本アーキタイプを再生成
}

void World::ApplyStructuralChanges()
{
    MYE_CHECK(!IsIterating());
    if (commands_.empty()) {
        return;
    }
    // 適用中に新コマンドが積まれても安全なようスワップして処理 (呼び出し順に適用)
    std::vector<Command> cmds;
    cmds.swap(commands_);
    std::vector<std::unique_ptr<std::byte[]>> payloads;
    payloads.swap(cmdPayloads_);

    for (const Command& c : cmds) {
        switch (c.type) {
        case CmdType::AddComponent: {
            if (!IsAlive(c.entity)) {
                break;
            }
            void* dst = AddComponentImmediate(c.entity, c.component);
            if (dst && c.payloadIndex != 0xFFFFFFFFu) {
                memcpy(dst, payloads[c.payloadIndex].get(),
                       ComponentRegistry::Get().Desc(c.component).size);
            }
            break;
        }
        case CmdType::RemoveComponent:
            if (IsAlive(c.entity)) {
                RemoveComponentImmediate(c.entity, c.component);
            }
            break;
        case CmdType::Destroy:
            DestroyImmediate(c.entity);
            break;
        case CmdType::SetParent:
            ApplySetParent(c.entity, c.parent);
            break;
        case CmdType::SetSiblingIndex:
            ApplySetSiblingIndex(c.entity, c.order);
            break;
        }
    }
}

} // namespace mye
