//====================================================================================
//                          SceneRestorePlan.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の計画 (照合して何を戻すか決める。World を書かない)
//====================================================================================
#include "Editor/ChangeTracking/SceneRestorePlan.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "Editor/ChangeTracking/RestorePlan.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Util/JsonUtil.h"

namespace mye {

namespace {

using nlohmann::json;
using Verdict = SceneRestoreVerdict;
using Issue = SceneRestoreIssue;

constexpr const char* kLocalTransformName = "LocalTransform";

const ComponentDesc* FindDesc(const std::string& name)
{
    const ComponentRegistry& registry = ComponentRegistry::Get();
    const ComponentTypeId type = registry.FindByName(name);
    return type == kInvalidComponentType ? nullptr : &registry.Desc(type);
}

const FieldDesc* FindField(const ComponentDesc& desc, const std::string& name)
{
    for (const FieldDesc& field : desc.fields) {
        if ((field.flags & kFieldNoSerialize) == 0 && name == field.name) {
            return &field;
        }
    }
    return nullptr;
}

// コンポーネント / フィールド 1 つ分の作業用の領域 (max_align_t 境界で 0 埋め)
std::vector<std::max_align_t> Scratch(size_t bytes)
{
    return std::vector<std::max_align_t>((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1);
}

bool DefaultNative(const ComponentDesc& desc, const FieldDesc& field, json& out)
{
    if (field.type == FieldType::EntityRef) {
        out = 0;
        return true;
    }
    std::vector<std::max_align_t> storage = Scratch(desc.size);
    ConstructComponent(desc, storage.data());
    out = FieldToJson(storage.data(), field);
    return true;
}

CanonStatus CanonicalNative(const FieldDesc& field, const json& raw, json& canon)
{
    if (field.type == FieldType::EntityRef) {
        if (!raw.is_number_integer() && !raw.is_number_unsigned()) {
            return CanonStatus::Unreadable;
        }
        canon = raw.get<uint64_t>();
        return CanonStatus::Ok;
    }
    std::vector<std::max_align_t> storage = Scratch(static_cast<size_t>(field.offset) + FieldTypeSize(field.type));
    if (!FieldFromJson(storage.data(), field, raw)) {
        return CanonStatus::Unreadable;
    }
    canon = FieldToJson(storage.data(), field);
    return CanonStatus::Ok;
}

bool IsProtected(const std::string& name, const ComponentDesc& desc)
{
    return name == kLocalTransformName || (desc.flags & (kComponentHidden | kComponentNoSerialize)) != 0;
}

// ---- 比べる ----

// コンポーネント 1 つの内容が expected (ファイルの JSON) と live で一致するか。通常のコンポーネントは全フィールドを正規形で比べ、
// expected に無いキーは既定値として比べる。C# は expected にあるキーだけ。未知のコンポーネントは生 JSON どうし
bool ComponentMatches(const std::string& name, const json& expected, const json& live, bool managed)
{
    const ComponentDesc* desc = FindDesc(name);
    if (desc == nullptr) {
        return expected == live;
    }
    if (!expected.is_object() || !live.is_object()) {
        return false;
    }
    if (managed) {
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (!live.contains(it.key()) || !(live[it.key()] == it.value())) {
                return false;
            }
        }
        return true;
    }
    for (const FieldDesc& field : desc->fields) {
        if ((field.flags & kFieldNoSerialize) != 0) {
            continue;
        }
        json want;
        if (expected.contains(field.name)) {
            if (CanonicalNative(field, expected[field.name], want) != CanonStatus::Ok) {
                return false;
            }
        } else {
            DefaultNative(*desc, field, want);
        }
        if (!live.contains(field.name) || !(live[field.name] == want)) {
            return false;
        }
    }
    return true;
}

// エンティティ 1 つが expected (ファイルの JSON) と一致するか。childIndex と overrides は見ない (兄弟の並びと印は付随する)
bool EntityMatches(const json& expected, const json& live, const SceneReadView& view)
{
    if (expected.value("name", std::string()) != live.value("name", std::string())) {
        return false;
    }
    if (expected.value("parent", 0ull) != live.value("parent", 0ull)) {
        return false;
    }
    if (expected.value("tagMask", 0ull) != live.value("tagMask", 0ull)) {
        return false;
    }
    const json emptyObject = json::object();
    const json& want = expected.contains("components") && expected["components"].is_object() ? expected["components"] : emptyObject;
    const json& have = live.contains("components") && live["components"].is_object() ? live["components"] : emptyObject;
    for (auto it = want.begin(); it != want.end(); ++it) {
        if (!have.contains(it.key()) || !ComponentMatches(it.key(), it.value(), have[it.key()], view.IsManagedComponent(it.key()))) {
            return false;
        }
    }
    for (auto it = have.begin(); it != have.end(); ++it) {
        // 必須の LocalTransform は、ファイルに無くても必ずある (既定値で作られる)
        if (!want.contains(it.key()) && it.key() != kLocalTransformName) {
            return false;
        }
    }
    return true;
}

// ---- 行の読み取り ----

struct Sides {
    bool ok = true;
    bool hasBefore = false;
    bool hasAfter = false;
    json before;
    json after;
};

Sides ReadSides(const TrackSceneChange& row)
{
    Sides sides;
    sides.hasBefore = row.hasBefore;
    sides.hasAfter = row.hasAfter;
    if (row.hasBefore) {
        sides.before = json::parse(row.beforeValue, nullptr, false);
        sides.ok = sides.ok && !sides.before.is_discarded();
    }
    if (row.hasAfter) {
        sides.after = json::parse(row.afterValue, nullptr, false);
        sides.ok = sides.ok && !sides.after.is_discarded();
    }
    return sides;
}

// 上書きの印の行か。overrides の要素 (文字列そのもの) が片側だけに入っている
bool IsOverrideKeyRow(const TrackSceneChange& row)
{
    if (row.changeType != "PrefabOverrideChanged" || row.hasBefore == row.hasAfter) {
        return false;
    }
    std::string key;
    if (row.component.empty()) {
        key = row.field;
    } else if (row.field == "+") {
        key = "+" + row.component;
    } else if (row.field == "-") {
        key = "-" + row.component;
    } else {
        key = row.component + "." + row.field;
    }
    const json value = json::parse(row.hasAfter ? row.afterValue : row.beforeValue, nullptr, false);
    return value.is_string() && value.get<std::string>() == key;
}

// ---- 計画の組み立て ----

class Builder {
public:
    Builder(const SceneRestoreInput& input, const SceneReadView& view) : input_(input), view_(view) {}

    SceneRestorePlan Run();

private:
    const json* Live(uint64_t fileId);
    bool Exists(uint64_t fileId) { return Live(fileId) != nullptr || removedIds_.count(fileId) != 0; }
    void Set(size_t i, Verdict verdict, Issue issue, const std::string& detail = std::string());
    bool LoadSides(size_t i, Sides& sides);

    void EvalField(size_t i);
    void EvalEntityKey(size_t i);
    void EvalHierarchy(size_t i);
    void EvalComponentAdded(size_t i);
    void EvalComponentRemoved(size_t i);
    void EvalEntitySets();
    bool HasCycle(uint64_t child, uint64_t newParent);
    // 値が参照するエンティティがいるか (EntityRef フィールドの値のとき)。いなければ false と、いない fileId
    bool RefsExist(const std::string& component, const json& object, uint64_t& missing);
    void Collect(SceneRestorePlan& plan);

    const SceneRestoreInput& input_;
    const SceneReadView& view_;
    std::map<uint64_t, std::optional<json>> cache_;
    std::set<uint64_t> removedIds_;      // EntityRemoved の行の fileId (同じ変更で作られるので、参照の先として許す)
    std::vector<SceneRestoreRowResult> rows_;
    std::vector<SceneRestoreOp> ops_;
};

const json* Builder::Live(uint64_t fileId)
{
    auto found = cache_.find(fileId);
    if (found == cache_.end()) {
        json entity;
        std::optional<json> value;
        if (fileId != 0 && view_.Entity(fileId, entity) && entity.is_object()) {
            value = std::move(entity);
        }
        found = cache_.emplace(fileId, std::move(value)).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

void Builder::Set(size_t i, Verdict verdict, Issue issue, const std::string& detail)
{
    rows_[i].verdict = verdict;
    rows_[i].issue = issue;
    rows_[i].detail = detail;
}

bool Builder::LoadSides(size_t i, Sides& sides)
{
    const TrackSceneChange& row = input_.rows[i];
    if (row.oversize) {
        Set(i, Verdict::NotRestorable, Issue::ValueOversize);
        return false;
    }
    sides = ReadSides(row);
    if (!sides.ok) {
        Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
        return false;
    }
    return true;
}

bool Builder::RefsExist(const std::string& component, const json& object, uint64_t& missing)
{
    const ComponentDesc* desc = FindDesc(component);
    if (desc == nullptr || !object.is_object() || view_.IsManagedComponent(component)) {
        return true;
    }
    for (const FieldDesc& field : desc->fields) {
        if (field.type != FieldType::EntityRef || (field.flags & kFieldNoSerialize) != 0 || !object.contains(field.name)) {
            continue;
        }
        const json& value = object[field.name];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            continue;
        }
        const uint64_t target = value.get<uint64_t>();
        if (target != 0 && !Exists(target)) {
            missing = target;
            return false;
        }
    }
    return true;
}

bool Builder::HasCycle(uint64_t child, uint64_t newParent)
{
    uint64_t current = newParent;
    for (int guard = 0; current != 0 && guard < 100000; ++guard) {
        if (current == child) {
            return true;
        }
        const json* entity = Live(current);
        current = entity != nullptr ? entity->value("parent", 0ull) : 0;
    }
    return false;
}

void Builder::EvalField(size_t i)
{
    const TrackSceneChange& row = input_.rows[i];
    Sides sides;
    if (!LoadSides(i, sides)) {
        return;
    }
    const uint64_t fileId = static_cast<uint64_t>(row.fileId);
    const bool managed = view_.IsManagedComponent(row.component);
    const ComponentDesc* desc = FindDesc(row.component);
    if (desc == nullptr) {
        Set(i, Verdict::NotRestorable, Issue::UnknownComponent);
        return;
    }
    if (!managed && FindField(*desc, row.field) == nullptr) {
        Set(i, Verdict::NotRestorable, Issue::UnknownField);
        return;
    }
    if (managed && (!sides.hasBefore || !sides.hasAfter)) {
        Set(i, Verdict::NotRestorable, Issue::NoBefore); // C# のフィールドの既定値は分からない
        return;
    }
    json before;
    json after;
    if (sides.hasBefore) {
        if (CanonicalFieldJson(row.component, row.field, sides.before, managed, before) != CanonStatus::Ok) {
            Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
            return;
        }
    } else {
        DefaultFieldJson(row.component, row.field, managed, before);
    }
    bool afterReadable = true;
    if (sides.hasAfter) {
        afterReadable = CanonicalFieldJson(row.component, row.field, sides.after, managed, after) == CanonStatus::Ok;
    } else {
        DefaultFieldJson(row.component, row.field, managed, after);
    }
    const json* live = Live(fileId);
    if (live == nullptr) {
        Set(i, Verdict::Conflict, Issue::EntityMissing);
        return;
    }
    const json& components = (*live)["components"];
    if (!components.is_object() || !components.contains(row.component)) {
        Set(i, Verdict::Conflict, Issue::ComponentMissing);
        return;
    }
    const json& component = components[row.component];
    const json current = component.is_object() && component.contains(row.field) ? component[row.field] : json();
    if (current == before) {
        Set(i, Verdict::AlreadyRestored, Issue::None);
        return;
    }
    if (!afterReadable || !(current == after)) {
        Set(i, Verdict::Conflict, afterReadable ? Issue::ValueChanged : Issue::ValueUnreadable);
        return;
    }
    if (const FieldDesc* field = managed ? nullptr : FindField(*desc, row.field); field != nullptr && field->type == FieldType::EntityRef && before.is_number_unsigned()) {
        const uint64_t target = before.get<uint64_t>();
        if (target != 0 && !Exists(target)) {
            Set(i, Verdict::Conflict, Issue::ReferenceMissing, std::to_string(target));
            return;
        }
    }
    Set(i, Verdict::Ready, Issue::None);
    SceneRestoreOp op;
    op.kind = SceneRestoreOp::Kind::SetField;
    op.fileId = fileId;
    op.component = row.component;
    op.field = row.field;
    op.value = before;
    op.current = current;
    op.sceneChangeIds = { row.sceneChangeId };
    op.entityName = live->value("name", std::string());
    ops_.push_back(std::move(op));
}

void Builder::EvalEntityKey(size_t i)
{
    const TrackSceneChange& row = input_.rows[i];
    Sides sides;
    if (!LoadSides(i, sides)) {
        return;
    }
    const uint64_t fileId = static_cast<uint64_t>(row.fileId);
    json before;
    json after;
    json current;
    const json* live = Live(fileId);
    if (row.field == "name") {
        if (!sides.hasBefore || !sides.hasAfter || !sides.before.is_string() || !sides.after.is_string()) {
            Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
            return;
        }
        before = sides.before;
        after = sides.after;
        if (live != nullptr) {
            current = live->value("name", std::string());
        }
    } else {
        // tagMask: 無いキーは 0
        const bool beforeOk = !sides.hasBefore || sides.before.is_number_unsigned();
        const bool afterOk = !sides.hasAfter || sides.after.is_number_unsigned();
        if (!beforeOk) {
            Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
            return;
        }
        before = sides.hasBefore ? sides.before.get<uint64_t>() : 0ull;
        after = sides.hasAfter && afterOk ? sides.after.get<uint64_t>() : 0ull;
        if (live != nullptr) {
            current = live->value("tagMask", 0ull);
        }
        if (!afterOk) {
            Set(i, Verdict::Conflict, Issue::ValueUnreadable);
            return;
        }
    }
    if (live == nullptr) {
        Set(i, Verdict::Conflict, Issue::EntityMissing);
        return;
    }
    if (current == before) {
        Set(i, Verdict::AlreadyRestored, Issue::None);
        return;
    }
    if (!(current == after)) {
        Set(i, Verdict::Conflict, Issue::ValueChanged);
        return;
    }
    Set(i, Verdict::Ready, Issue::None);
    SceneRestoreOp op;
    op.kind = SceneRestoreOp::Kind::SetEntityKey;
    op.fileId = fileId;
    op.field = row.field;
    op.value = before;
    op.current = current;
    op.sceneChangeIds = { row.sceneChangeId };
    op.entityName = live->value("name", std::string());
    ops_.push_back(std::move(op));
}

void Builder::EvalHierarchy(size_t i)
{
    const TrackSceneChange& row = input_.rows[i];
    Sides sides;
    if (!LoadSides(i, sides)) {
        return;
    }
    const uint64_t fileId = static_cast<uint64_t>(row.fileId);
    const bool isParent = row.field == "parent";
    // parent は無い = ルート (0)。childIndex は両側が要る
    if ((!isParent && (!sides.hasBefore || !sides.hasAfter)) || (sides.hasBefore && !sides.before.is_number_unsigned())) {
        Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
        return;
    }
    const uint64_t before = sides.hasBefore ? sides.before.get<uint64_t>() : 0;
    const bool afterReadable = !sides.hasAfter || sides.after.is_number_unsigned();
    const uint64_t after = sides.hasAfter && afterReadable ? sides.after.get<uint64_t>() : 0;
    const json* live = Live(fileId);
    if (live == nullptr) {
        Set(i, Verdict::Conflict, Issue::EntityMissing);
        return;
    }
    const uint64_t current = live->value(isParent ? "parent" : "childIndex", 0ull);
    if (current == before) {
        Set(i, Verdict::AlreadyRestored, Issue::None);
        return;
    }
    if (!afterReadable || current != after) {
        Set(i, Verdict::Conflict, afterReadable ? Issue::ValueChanged : Issue::ValueUnreadable);
        return;
    }
    if (isParent && before != 0) {
        if (!Exists(before)) {
            Set(i, Verdict::Conflict, Issue::ParentMissing, std::to_string(before));
            return;
        }
        if (HasCycle(fileId, before)) {
            Set(i, Verdict::Conflict, Issue::ParentCycle, std::to_string(before));
            return;
        }
    }
    Set(i, Verdict::Ready, Issue::None);
    SceneRestoreOp op;
    op.kind = SceneRestoreOp::Kind::SetHierarchy;
    op.fileId = fileId;
    op.field = row.field;
    op.value = before;
    op.current = current;
    op.sceneChangeIds = { row.sceneChangeId };
    op.entityName = live->value("name", std::string());
    ops_.push_back(std::move(op));
}

void Builder::EvalComponentAdded(size_t i)
{
    const TrackSceneChange& row = input_.rows[i];
    Sides sides;
    if (!LoadSides(i, sides)) {
        return;
    }
    const ComponentDesc* desc = FindDesc(row.component);
    if (desc == nullptr) {
        Set(i, Verdict::NotRestorable, Issue::UnknownComponent);
        return;
    }
    if (IsProtected(row.component, *desc)) {
        Set(i, Verdict::NotRestorable, Issue::ProtectedComponent);
        return;
    }
    if (!sides.hasAfter || !sides.after.is_object()) {
        Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
        return;
    }
    const uint64_t fileId = static_cast<uint64_t>(row.fileId);
    const json* live = Live(fileId);
    if (live == nullptr) {
        Set(i, Verdict::Conflict, Issue::EntityMissing);
        return;
    }
    const json& components = (*live)["components"];
    if (!components.is_object() || !components.contains(row.component)) {
        Set(i, Verdict::AlreadyRestored, Issue::None); // もう無い
        return;
    }
    if (!ComponentMatches(row.component, sides.after, components[row.component], view_.IsManagedComponent(row.component))) {
        Set(i, Verdict::Conflict, Issue::ComponentChanged);
        return;
    }
    Set(i, Verdict::Ready, Issue::None);
    SceneRestoreOp op;
    op.kind = SceneRestoreOp::Kind::RemoveComponent;
    op.fileId = fileId;
    op.component = row.component;
    op.current = components[row.component];
    op.sceneChangeIds = { row.sceneChangeId };
    op.entityName = live->value("name", std::string());
    ops_.push_back(std::move(op));
}

void Builder::EvalComponentRemoved(size_t i)
{
    const TrackSceneChange& row = input_.rows[i];
    Sides sides;
    if (!LoadSides(i, sides)) {
        return;
    }
    const ComponentDesc* desc = FindDesc(row.component);
    if (desc == nullptr) {
        Set(i, Verdict::NotRestorable, Issue::UnknownComponent);
        return;
    }
    if (IsProtected(row.component, *desc)) {
        Set(i, Verdict::NotRestorable, Issue::ProtectedComponent);
        return;
    }
    if (!sides.hasBefore || !sides.before.is_object()) {
        Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
        return;
    }
    const uint64_t fileId = static_cast<uint64_t>(row.fileId);
    const json* live = Live(fileId);
    if (live == nullptr) {
        Set(i, Verdict::Conflict, Issue::EntityMissing);
        return;
    }
    const json& components = (*live)["components"];
    if (components.is_object() && components.contains(row.component)) {
        const bool same = ComponentMatches(row.component, sides.before, components[row.component], view_.IsManagedComponent(row.component));
        Set(i, same ? Verdict::AlreadyRestored : Verdict::Conflict, same ? Issue::None : Issue::ComponentExists);
        return;
    }
    uint64_t missing = 0;
    if (!RefsExist(row.component, sides.before, missing)) {
        Set(i, Verdict::Conflict, Issue::ReferenceMissing, std::to_string(missing));
        return;
    }
    Set(i, Verdict::Ready, Issue::None);
    SceneRestoreOp op;
    op.kind = SceneRestoreOp::Kind::AddComponent;
    op.fileId = fileId;
    op.component = row.component;
    op.value = sides.before;
    op.sceneChangeIds = { row.sceneChangeId };
    op.entityName = live->value("name", std::string());
    ops_.push_back(std::move(op));
}

// EntityAdded (消す) と EntityRemoved (作る) は、同じ変更の行の集合として見る (子孫は同じ集合に入る)
void Builder::EvalEntitySets()
{
    struct Item {
        size_t index = 0;
        uint64_t fileId = 0;
        json expected;
    };
    std::map<uint64_t, Item> added;
    std::map<uint64_t, Item> removed;
    for (size_t i = 0; i < input_.rows.size(); ++i) {
        const SceneRowKind kind = rows_[i].kind;
        if ((kind != SceneRowKind::EntityAdded && kind != SceneRowKind::EntityRemoved) || rows_[i].verdict != Verdict::Ready) {
            continue;
        }
        const TrackSceneChange& row = input_.rows[i];
        Sides sides;
        if (!LoadSides(i, sides)) {
            continue;
        }
        const bool isAdded = kind == SceneRowKind::EntityAdded;
        const json& expected = isAdded ? sides.after : sides.before;
        if (!(isAdded ? sides.hasAfter : sides.hasBefore) || !expected.is_object()
            || expected.value("fileId", 0ull) != static_cast<uint64_t>(row.fileId)) {
            Set(i, Verdict::NotRestorable, Issue::ValueUnreadable);
            continue;
        }
        (isAdded ? added : removed)[static_cast<uint64_t>(row.fileId)] = Item{ i, static_cast<uint64_t>(row.fileId), expected };
    }

    // 追加の取り消し: エンティティ (と子孫) が After のままで、After に無い子孫が居ないときだけ、根ごと消す
    std::set<uint64_t> destroyRoots;
    for (auto& [fileId, item] : added) {
        const json* live = Live(fileId);
        if (live == nullptr) {
            Set(item.index, Verdict::AlreadyRestored, Issue::None);
            continue;
        }
        if (!EntityMatches(item.expected, *live, view_)) {
            Set(item.index, Verdict::Conflict, Issue::EntityChanged);
            continue;
        }
        bool extra = false;
        std::vector<uint64_t> stack = { fileId };
        while (!stack.empty() && !extra) {
            const uint64_t current = stack.back();
            stack.pop_back();
            for (uint64_t child : view_.Children(current)) {
                if (child == 0 || added.count(child) == 0) {
                    Set(item.index, Verdict::Conflict, Issue::ExtraDescendant, std::to_string(child));
                    extra = true;
                    break;
                }
                stack.push_back(child);
            }
        }
        if (extra) {
            continue;
        }
        Set(item.index, Verdict::Ready, Issue::None);
        if (!added.count(live->value("parent", 0ull))) {
            destroyRoots.insert(fileId);
        }
    }
    for (uint64_t root : destroyRoots) {
        SceneRestoreOp op;
        op.kind = SceneRestoreOp::Kind::DestroyEntities;
        op.fileId = root;
        op.entityName = Live(root)->value("name", std::string());
        // 消える fileId (深さ優先)。対応する行の ID も集める
        std::vector<uint64_t> stack = { root };
        while (!stack.empty()) {
            const uint64_t current = stack.back();
            stack.pop_back();
            op.subtree.push_back(current);
            if (auto it = added.find(current); it != added.end()) {
                op.sceneChangeIds.push_back(input_.rows[it->second.index].sceneChangeId);
            }
            std::vector<uint64_t> children = view_.Children(current);
            std::sort(children.begin(), children.end(), std::greater<uint64_t>());
            for (uint64_t child : children) {
                stack.push_back(child);
            }
        }
        std::sort(op.sceneChangeIds.begin(), op.sceneChangeIds.end());
        ops_.push_back(std::move(op));
    }

    // 削除の取り消し: その fileId が使われておらず、親と参照の先がいるときだけ、根ごと作る
    std::set<uint64_t> toCreate;
    for (auto& [fileId, item] : removed) {
        const json* live = Live(fileId);
        if (live != nullptr) {
            const bool same = EntityMatches(item.expected, *live, view_);
            Set(item.index, same ? Verdict::AlreadyRestored : Verdict::Conflict, same ? Issue::None : Issue::EntityExists);
            continue;
        }
        const uint64_t parent = item.expected.value("parent", 0ull);
        if (parent != 0 && !Exists(parent)) {
            Set(item.index, Verdict::Conflict, Issue::ParentMissing, std::to_string(parent));
            continue;
        }
        bool refsOk = true;
        if (item.expected.contains("components") && item.expected["components"].is_object()) {
            for (auto it = item.expected["components"].begin(); it != item.expected["components"].end() && refsOk; ++it) {
                uint64_t missing = 0;
                if (!RefsExist(it.key(), it.value(), missing)) {
                    Set(item.index, Verdict::Conflict, Issue::ReferenceMissing, std::to_string(missing));
                    refsOk = false;
                }
            }
        }
        if (refsOk) {
            Set(item.index, Verdict::Ready, Issue::None);
            toCreate.insert(fileId);
        }
    }
    std::map<uint64_t, std::vector<uint64_t>> children;
    for (uint64_t fileId : toCreate) {
        children[removed[fileId].expected.value("parent", 0ull)].push_back(fileId);
    }
    for (auto& [parent, list] : children) {
        std::sort(list.begin(), list.end(), [&](uint64_t a, uint64_t b) {
            return std::make_pair(removed[a].expected.value("childIndex", 0ull), a) < std::make_pair(removed[b].expected.value("childIndex", 0ull), b);
        });
    }
    for (uint64_t root : toCreate) {
        if (toCreate.count(removed[root].expected.value("parent", 0ull)) != 0) {
            continue;
        }
        SceneRestoreOp op;
        op.kind = SceneRestoreOp::Kind::CreateEntities;
        op.fileId = root;
        op.entityName = removed[root].expected.value("name", std::string());
        op.value = json::array();
        std::vector<uint64_t> stack = { root };
        while (!stack.empty()) {
            const uint64_t current = stack.back();
            stack.pop_back();
            op.value.push_back(removed[current].expected);
            op.sceneChangeIds.push_back(input_.rows[removed[current].index].sceneChangeId);
            const std::vector<uint64_t>& kids = children[current];
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                stack.push_back(*it);
            }
        }
        std::sort(op.sceneChangeIds.begin(), op.sceneChangeIds.end());
        ops_.push_back(std::move(op));
    }
}

SceneRestorePlan Builder::Run()
{
    SceneRestorePlan plan;
    const size_t count = input_.rows.size();
    rows_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const TrackSceneChange& row = input_.rows[i];
        rows_[i].sceneChangeId = row.sceneChangeId;
        rows_[i].fileId = row.hasFileId ? row.fileId : 0;
        rows_[i].component = row.component;
        rows_[i].field = row.field;
        rows_[i].kind = ClassifySceneRow(row);
        rows_[i].derived = rows_[i].kind == SceneRowKind::OverrideKey;
        if (rows_[i].kind == SceneRowKind::EntityRemoved) {
            removedIds_.insert(static_cast<uint64_t>(row.fileId));
        }
    }
    if (count == 0 || count > kSceneRestoreMaxRows || !IsRestorableActor(input_.actor)) {
        plan.verdict = Verdict::NotRestorable;
        plan.issue = count == 0 ? Issue::NoRows : (count > kSceneRestoreMaxRows ? Issue::TooManyRows : Issue::ActorNotRestorable);
        plan.rows = std::move(rows_);
        return plan;
    }
    for (size_t i = 0; i < count; ++i) {
        const TrackSceneChange& row = input_.rows[i];
        const SceneRowKind kind = rows_[i].kind;
        // 既定は Ready。各行の判定で書き換える
        rows_[i].verdict = Verdict::Ready;
        rows_[i].issue = Issue::None;
        const std::string document = row.resolvedDocumentId.empty() ? row.documentId : row.resolvedDocumentId;
        if (input_.openDocumentId.empty() || document != input_.openDocumentId) {
            Set(i, Verdict::NotRestorable, Issue::NotOpenDocument);
            continue;
        }
        if (kind == SceneRowKind::Unsupported || !row.hasFileId) {
            Set(i, Verdict::NotRestorable, Issue::RowUnsupported);
            continue;
        }
        if (row.state == "restored") {
            Set(i, Verdict::AlreadyRestored, Issue::None);
            continue;
        }
        switch (kind) {
        case SceneRowKind::Field: EvalField(i); break;
        case SceneRowKind::EntityKey: EvalEntityKey(i); break;
        case SceneRowKind::Hierarchy: EvalHierarchy(i); break;
        case SceneRowKind::ComponentAdded: EvalComponentAdded(i); break;
        case SceneRowKind::ComponentRemoved: EvalComponentRemoved(i); break;
        case SceneRowKind::EntityAdded:
        case SceneRowKind::EntityRemoved:
        case SceneRowKind::OverrideKey:
        case SceneRowKind::Unsupported: break; // 集合の判定へ / 連動 / 上で処理済み
        }
    }
    EvalEntitySets();
    Collect(plan);
    plan.rows = std::move(rows_);
    return plan;
}

// 行ごとの判定を、計画全体の判定にまとめる。1 つでも戻せない・競合があれば、何も当てない
void Builder::Collect(SceneRestorePlan& plan)
{
    const Verdict kPriority[] = { Verdict::NotRestorable, Verdict::Conflict };
    for (const Verdict wanted : kPriority) {
        for (const SceneRestoreRowResult& row : rows_) {
            if (!row.derived && row.verdict == wanted) {
                plan.verdict = wanted;
                plan.issue = row.issue;
                return;
            }
        }
    }
    const bool anyReady = std::any_of(rows_.begin(), rows_.end(), [](const SceneRestoreRowResult& r) { return !r.derived && r.verdict == Verdict::Ready; });
    const bool anyRestored = std::any_of(rows_.begin(), rows_.end(), [](const SceneRestoreRowResult& r) { return !r.derived && r.verdict == Verdict::AlreadyRestored; });
    if (anyReady) {
        plan.verdict = Verdict::Ready;
    } else if (anyRestored) {
        plan.verdict = Verdict::AlreadyRestored;
    } else {
        plan.verdict = Verdict::NotRestorable; // 上書きの印だけ
        plan.issue = Issue::OverrideKeyOnly;
        return;
    }
    // 操作は、作る → 書き換える → 消す の順。同じ段の中は fileId・種類・名前の順 (走査順に頼らない)
    auto phase = [](const SceneRestoreOp& op) {
        if (op.kind == SceneRestoreOp::Kind::CreateEntities) {
            return 0;
        }
        return op.kind == SceneRestoreOp::Kind::DestroyEntities ? 2 : 1;
    };
    std::stable_sort(ops_.begin(), ops_.end(), [&](const SceneRestoreOp& a, const SceneRestoreOp& b) {
        return std::make_tuple(phase(a), a.fileId, static_cast<int>(a.kind), a.component, a.field)
               < std::make_tuple(phase(b), b.fileId, static_cast<int>(b.kind), b.component, b.field);
    });
    plan.ops = std::move(ops_);
}

} // namespace

SceneRowKind ClassifySceneRow(const TrackSceneChange& row)
{
    const std::string& type = row.changeType;
    if (type == "EntityAdded") {
        return SceneRowKind::EntityAdded;
    }
    if (type == "EntityRemoved") {
        return SceneRowKind::EntityRemoved;
    }
    if (type == "ComponentAdded") {
        return row.component.empty() ? SceneRowKind::Unsupported : SceneRowKind::ComponentAdded;
    }
    if (type == "ComponentRemoved") {
        return row.component.empty() ? SceneRowKind::Unsupported : SceneRowKind::ComponentRemoved;
    }
    if (type == "HierarchyChanged") {
        return row.field == "parent" || row.field == "childIndex" ? SceneRowKind::Hierarchy : SceneRowKind::Unsupported;
    }
    const bool entityKey = row.component.empty() && (row.field == "name" || row.field == "tagMask");
    if (type == "FieldModified") {
        return !row.component.empty() && !row.field.empty() ? SceneRowKind::Field : SceneRowKind::Unsupported;
    }
    if (type == "EntityModified") {
        return entityKey ? SceneRowKind::EntityKey : SceneRowKind::Unsupported;
    }
    if (type == "PrefabOverrideChanged") {
        if (IsOverrideKeyRow(row)) {
            return SceneRowKind::OverrideKey;
        }
        if (entityKey) {
            return SceneRowKind::EntityKey;
        }
        return !row.component.empty() && !row.field.empty() ? SceneRowKind::Field : SceneRowKind::Unsupported;
    }
    return SceneRowKind::Unsupported; // PrefabInstanceChanged / PrefabAssetChanged / PrefabPropagation / unparsed
}

SceneRestorePlan BuildSceneRestorePlan(const SceneRestoreInput& input, const SceneReadView& view)
{
    return Builder(input, view).Run();
}

bool SceneRestoreScopeAvailable(const TrackSceneChange& anchor, SceneRestoreScope scope)
{
    switch (scope) {
    case SceneRestoreScope::Field: return anchor.hasFileId && !anchor.field.empty();
    case SceneRestoreScope::Component: return anchor.hasFileId && !anchor.component.empty();
    case SceneRestoreScope::Entity: return anchor.hasFileId;
    case SceneRestoreScope::Revision: return true;
    }
    return false;
}

std::vector<TrackSceneChange> SelectSceneRows(const std::vector<TrackSceneChange>& all, const TrackSceneChange& anchor, SceneRestoreScope scope)
{
    if (scope == SceneRestoreScope::Revision) {
        return all;
    }
    if (!SceneRestoreScopeAvailable(anchor, scope)) {
        return { anchor };
    }
    std::vector<TrackSceneChange> out;
    // 追加・削除されたエンティティを選んだときは、同じ変更で追加・削除された子孫の行も範囲に入れる
    // (入れないと「After に無い子孫」で止まる、または子孫を残して親だけ作ってしまう)
    std::set<int64_t> subtree;
    const std::string kind = anchor.changeType;
    if (scope == SceneRestoreScope::Entity && (kind == "EntityAdded" || kind == "EntityRemoved")) {
        std::map<int64_t, int64_t> parentOf; // 同じ種類の行の fileId → 親の fileId
        for (const TrackSceneChange& row : all) {
            if (row.changeType != kind || !row.hasFileId) {
                continue;
            }
            const json entity = json::parse(kind == "EntityAdded" ? row.afterValue : row.beforeValue, nullptr, false);
            parentOf[row.fileId] = entity.is_object() ? static_cast<int64_t>(entity.value("parent", 0ull)) : 0;
        }
        subtree.insert(anchor.fileId);
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [fileId, parent] : parentOf) {
                if (subtree.count(fileId) == 0 && subtree.count(parent) != 0) {
                    subtree.insert(fileId);
                    grew = true;
                }
            }
        }
    }
    for (const TrackSceneChange& row : all) {
        if (!row.hasFileId) {
            continue;
        }
        if (!subtree.empty() && row.changeType == kind && subtree.count(row.fileId) != 0) {
            out.push_back(row);
            continue;
        }
        if (row.fileId != anchor.fileId) {
            continue;
        }
        if (scope != SceneRestoreScope::Entity && row.component != anchor.component) {
            continue;
        }
        if (scope == SceneRestoreScope::Field && row.field != anchor.field) {
            continue;
        }
        out.push_back(row);
    }
    return out;
}

CanonStatus CanonicalFieldJson(const std::string& component, const std::string& field, const json& raw, bool managed, json& canon)
{
    const ComponentDesc* desc = FindDesc(component);
    if (desc == nullptr) {
        return CanonStatus::UnknownComponent;
    }
    if (managed) {
        canon = raw;
        return CanonStatus::Ok;
    }
    const FieldDesc* found = FindField(*desc, field);
    return found == nullptr ? CanonStatus::UnknownField : CanonicalNative(*found, raw, canon);
}

bool DefaultFieldJson(const std::string& component, const std::string& field, bool managed, json& out)
{
    const ComponentDesc* desc = FindDesc(component);
    if (desc == nullptr || managed) {
        return false;
    }
    const FieldDesc* found = FindField(*desc, field);
    return found != nullptr && DefaultNative(*desc, *found, out);
}

} // namespace mye
