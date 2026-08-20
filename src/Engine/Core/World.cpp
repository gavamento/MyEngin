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

namespace {

bool MatchesAll(const Archetype& arch, std::span<const ComponentTypeId> required)
{
    for (ComponentTypeId t : required) {
        if (!arch.HasType(t)) {
            return false;
        }
    }
    return true;
}

} // namespace

const std::vector<uint32_t>* World::QueryArchetypes(std::span<const ComponentTypeId> required)
{
    if (!sSimCacheEnabled_) {
        return nullptr; // 素通し: 呼び出し側が線形マッチに落ちる (A/B ゲート)
    }
    const uint64_t key = ComputeSignatureHash(required);
    auto& bucket = queryCache_[key];
    for (const auto& entry : bucket) {
        if (entry->required.size() == required.size()
            && std::equal(entry->required.begin(), entry->required.end(), required.begin())) {
            return &entry->archetypeIndices;
        }
    }
    // 初出クエリ: 現在の全アーキタイプを線形マッチして充填。
    // マッチ判定は Count に依存させない (空アーキタイプも集合に含め、列挙時に飛ばす) —
    // 行数の増減はキャッシュへ通知されないため、集合は型構成だけで決める
    auto entry = std::make_unique<QueryCacheEntry>();
    entry->required.assign(required.begin(), required.end());
    for (uint32_t i = 0; i < static_cast<uint32_t>(archetypes_.size()); ++i) {
        if (MatchesAll(*archetypes_[i], entry->required)) {
            entry->archetypeIndices.push_back(i);
        }
    }
    bucket.push_back(std::move(entry));
    return &bucket.back()->archetypeIndices;
}

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
    // M51a: クエリキャッシュへ追記マッチ。アーキタイプは Clear() まで append-only なので
    // 生成点での追記だけで全エントリが最新に保たれる (新 index は末尾 = 生成順も不変)。
    // ゲート OFF 中も維持する — フラグを実行中に切り替えても集合が欠けないようにするため
    Archetype* arch = archetypes_.back().get();
    const uint32_t newIndex = static_cast<uint32_t>(archetypes_.size() - 1);
    for (auto& [key, bucket] : queryCache_) {
        for (auto& entry : bucket) {
            if (MatchesAll(*arch, entry->required)) {
                entry->archetypeIndices.push_back(newIndex);
            }
        }
    }
    return arch;
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
    queryCache_.clear(); // archetype index が全て無効化されるため必ず同時破棄 (M51a)
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

// ---------------------------------------------------------------- snapshot (M52d)

namespace {
// blob の節ごとに小さな目印を置く。壊れた/別形式の blob を「途中まで復元」してしまう
// 事故を、確保する前に止めるため
constexpr uint32_t kWorldSnapshotMagic = 0x314C5257u; // 'WRL1'
constexpr uint32_t kNoArchetype = 0xFFFFFFFFu;
} // namespace

void World::SnapshotWrite(ByteWriter& w) const
{
    // 遅延コマンドを抱えたまま撮ると、復元後にその構造変更が消える。撮影点は
    // ApplyStructuralChanges 直後 (tick 末) 以外にありえない
    MYE_CHECKF(commands_.empty() && cmdPayloads_.empty(),
               "SnapshotWrite with %zu pending structural command(s)", commands_.size());
    MYE_CHECK(iterationDepth_ == 0);

    w.U32(kWorldSnapshotMagic);

    // ---- アーキタイプ (生成順 = ハッシュの畳み込み順) ----
    w.Count(archetypes_.size());
    for (const std::unique_ptr<Archetype>& arch : archetypes_) {
        const std::span<const ComponentTypeId> types = arch->Types();
        w.Count(types.size());
        for (ComponentTypeId t : types) {
            w.U32(t);
        }
        for (size_t i = 0; i < types.size(); ++i) {
            w.U32(arch->ElemSize(static_cast<int>(i)));
        }
        const std::span<const EntityID> ents = arch->Entities();
        w.Count(ents.size());
        w.Raw(ents.data(), ents.size() * sizeof(EntityID));
        for (size_t i = 0; i < types.size(); ++i) {
            const std::vector<std::byte>& col = arch->ColumnBytes(static_cast<int>(i));
            w.Blob(col.data(), col.size());
        }
    }

    // ---- レコード表 (index 昇順。generation はここでしか保てない) ----
    // アーキタイプの生ポインタは index へ落とす (復元先では別アドレスになるため)
    std::unordered_map<const Archetype*, uint32_t> indexOf;
    indexOf.reserve(archetypes_.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(archetypes_.size()); ++i) {
        indexOf.emplace(archetypes_[i].get(), i);
    }
    w.Count(records_.size());
    for (const EntityRecord& rec : records_) {
        w.U32(rec.generation);
        uint32_t archIndex = kNoArchetype;
        if (rec.archetype != nullptr) {
            const auto it = indexOf.find(rec.archetype);
            MYE_CHECK(it != indexOf.end());
            archIndex = (it != indexOf.end()) ? it->second : kNoArchetype;
        }
        w.U32(archIndex);
        w.U32(rec.row);
    }

    w.PodVector(freeIndices_); // LIFO の並びそのまま = 次に払い出す index まで再現する
    w.U32(firstRoot_.index);
    w.U32(firstRoot_.generation);
    w.U32(aliveCount_);
    w.U64(rng_.State());
    w.U64(rng_.Inc());
}

bool World::SnapshotRead(ByteReader& r)
{
    MYE_CHECK(iterationDepth_ == 0);
    if (r.U32() != kWorldSnapshotMagic) {
        MYE_LOG_ERROR("[snapshot] world section magic mismatch");
        return false;
    }

    // ---- 読み切ってから差し替える (途中で失敗しても現世界を壊さない) ----
    std::vector<std::unique_ptr<Archetype>> archetypes;
    const size_t archCount = r.Count(sizeof(uint64_t)); // 1 アーキタイプ最低 8B は消費する
    archetypes.reserve(archCount);
    for (size_t a = 0; a < archCount && r.Ok(); ++a) {
        const size_t typeCount = r.Count(sizeof(uint32_t));
        std::vector<ComponentTypeId> types(typeCount);
        for (size_t i = 0; i < typeCount; ++i) {
            types[i] = r.U32();
        }
        std::vector<uint32_t> sizes(typeCount);
        for (size_t i = 0; i < typeCount; ++i) {
            sizes[i] = r.U32();
        }
        const size_t rowCount = r.Count(sizeof(EntityID));
        std::vector<EntityID> ents(rowCount);
        r.Raw(ents.data(), rowCount * sizeof(EntityID));
        std::vector<std::vector<std::byte>> columns(typeCount);
        for (size_t i = 0; i < typeCount && r.Ok(); ++i) {
            const size_t n = r.Count(sizeof(std::byte));
            columns[i].resize(n);
            r.Raw(columns[i].data(), n);
        }
        if (!r.Ok()) {
            break;
        }
        const uint64_t sig = ComputeSignatureHash(types);
        auto arch = std::make_unique<Archetype>(types, sig);
        arch->SnapshotLoad(std::move(columns), std::move(sizes), std::move(ents));
        archetypes.push_back(std::move(arch));
    }

    const size_t recCount = r.Count(sizeof(uint32_t) * 3);
    struct RawRecord {
        uint32_t generation;
        uint32_t archIndex;
        uint32_t row;
    };
    std::vector<RawRecord> raw(recCount);
    for (size_t i = 0; i < recCount && r.Ok(); ++i) {
        raw[i].generation = r.U32();
        raw[i].archIndex = r.U32();
        raw[i].row = r.U32();
    }
    std::vector<uint32_t> freeIndices = r.PodVector<uint32_t>();
    EntityID firstRoot;
    firstRoot.index = r.U32();
    firstRoot.generation = r.U32();
    const uint32_t aliveCount = r.U32();
    const uint64_t rngState = r.U64();
    const uint64_t rngInc = r.U64();
    if (!r.Ok()) {
        MYE_LOG_ERROR("[snapshot] world section truncated");
        return false;
    }
    for (const RawRecord& rec : raw) {
        if (rec.archIndex != kNoArchetype && rec.archIndex >= archetypes.size()) {
            MYE_LOG_ERROR("[snapshot] world record points at archetype %u of %zu", rec.archIndex,
                          archetypes.size());
            return false;
        }
    }

    // ---- 差し替え ----
    commands_.clear();
    cmdPayloads_.clear();
    queryCache_.clear(); // archetype index を持つ派生物なので必ず破棄 (M51a)
    archetypes_ = std::move(archetypes);
    records_.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        records_[i].generation = raw[i].generation;
        records_[i].archetype =
            (raw[i].archIndex == kNoArchetype) ? nullptr : archetypes_[raw[i].archIndex].get();
        records_[i].row = raw[i].row;
    }
    freeIndices_ = std::move(freeIndices);
    firstRoot_ = firstRoot;
    aliveCount_ = aliveCount;
    rng_.Restore(rngState, rngInc);
    // 側テーブル (TransformSystem M51c) を Rebuild 経由で全無効化させる。
    // ここを立て忘れると「LocalTransform が前回と同ビットだから据え置き」で
    // 復元前の WorldMatrix が残る
    hierarchyDirty_ = true;
    return true;
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
