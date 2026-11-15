//====================================================================================
//                          SceneRestoreRunner.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の実行 (Undo に積んで World を書き、読み直して確かめる)
//====================================================================================
#include "Editor/ChangeTracking/SceneRestoreRunner.h"

#include <algorithm>
#include <set>

#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/JsonUtil.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"

#include "Editor/ChangeTracking/RestoreJournal.h"

namespace mye {

using nlohmann::json;

// ---- ライブのシーンの読み取り ----

bool LiveSceneReadView::Entity(uint64_t fileId, json& out) const
{
    if (env_.scene == nullptr || fileId == 0) {
        return false;
    }
    GameObject object = env_.scene->FindByFileId(fileId);
    if (!object) {
        return false;
    }
    out = SceneSerializer::EntityToJson(*env_.scene, object.Id()); // fileId のあるエンティティなので、採番は起きない
    return out.is_object() && !out.empty();
}

std::vector<uint64_t> LiveSceneReadView::Children(uint64_t fileId) const
{
    std::vector<uint64_t> children;
    if (env_.scene == nullptr) {
        return children;
    }
    GameObject object = env_.scene->FindByFileId(fileId);
    if (!object) {
        return children;
    }
    World& world = env_.scene->GetWorld();
    const HierarchyComponent* hierarchy = world.GetComponent<HierarchyComponent>(object.Id());
    EntityID child = hierarchy != nullptr ? hierarchy->firstChild : kNullEntity;
    while (!child.IsNull() && world.IsAlive(child)) {
        const FileIdComponent* id = world.GetComponent<FileIdComponent>(child);
        children.push_back(id != nullptr ? id->value : 0);
        const HierarchyComponent* childHierarchy = world.GetComponent<HierarchyComponent>(child);
        child = childHierarchy != nullptr ? childHierarchy->nextSibling : kNullEntity;
    }
    return children;
}

bool LiveSceneReadView::IsManagedComponent(const std::string& componentName) const
{
    if (env_.managed == nullptr) {
        return false;
    }
    const ComponentTypeId type = ComponentRegistry::Get().FindByName(componentName);
    return type != kInvalidComponentType && env_.managed->IsManagedComponent(type);
}

namespace {

// 計画の ops が触るエンティティ (UndoStack::Record の fids。作る・消すものは根)
std::vector<uint64_t> TouchedFileIds(const std::vector<SceneRestoreOp>& ops)
{
    std::set<uint64_t> ids;
    for (const SceneRestoreOp& op : ops) {
        ids.insert(op.fileId);
    }
    return std::vector<uint64_t>(ids.begin(), ids.end());
}

// フィールド 1 つを書く。通常のコンポーネントは FieldFromJson (EntityRef は fileId から解決)、C# は DeserializeComponent へ 1 キーだけの JSON
bool WriteField(const SceneRestoreEnv& env, const SceneRestoreOp& op)
{
    Scene& scene = *env.scene;
    World& world = scene.GetWorld();
    GameObject object = scene.FindByFileId(op.fileId);
    const ComponentRegistry& registry = ComponentRegistry::Get();
    const ComponentTypeId type = registry.FindByName(op.component);
    if (!object || type == kInvalidComponentType) {
        return false;
    }
    void* component = world.GetComponentRaw(object.Id(), type);
    if (component == nullptr) {
        return false;
    }
    if (env.managed != nullptr && env.managed->IsManagedComponent(type)) {
        json one = json::object();
        one[op.field] = op.value;
        env.managed->DeserializeComponent(type, object.Id(), component, one.dump());
        return true;
    }
    for (const FieldDesc& field : registry.Desc(type).fields) {
        if ((field.flags & kFieldNoSerialize) != 0 || op.field != field.name) {
            continue;
        }
        if (field.type == FieldType::EntityRef) {
            const uint64_t target = op.value.is_number_unsigned() ? op.value.get<uint64_t>() : 0;
            EntityID resolved = kNullEntity;
            if (target != 0) {
                GameObject other = scene.FindByFileId(target);
                if (!other) {
                    return false;
                }
                resolved = other.Id();
            }
            *reinterpret_cast<EntityID*>(static_cast<uint8_t*>(component) + field.offset) = resolved;
            return true;
        }
        return FieldFromJson(component, field, op.value);
    }
    return false;
}

// 計画の操作を World へ書く。作る → フィールド → エンティティの JSON を直して ApplyPartial → 消す の順
bool WriteOps(const SceneRestoreEnv& env, const std::vector<SceneRestoreOp>& ops)
{
    Scene& scene = *env.scene;
    World& world = scene.GetWorld();
    for (const SceneRestoreOp& op : ops) {
        if (op.kind != SceneRestoreOp::Kind::CreateEntities) {
            continue;
        }
        if (!SceneSerializer::ApplyPartial(scene, op.value, false)) {
            return false;
        }
        for (const json& item : op.value) {
            // 古い fileId を持つエンティティを作ったあとも、新しい採番が衝突しない
            const uint64_t fileId = item.value("fileId", 0ull);
            if (fileId >= scene.PeekNextFileId()) {
                scene.SetNextFileId(fileId + 1);
            }
        }
    }
    world.ApplyStructuralChanges();
    for (const SceneRestoreOp& op : ops) {
        if (op.kind == SceneRestoreOp::Kind::SetField && !WriteField(env, op)) {
            return false;
        }
    }
    // name / tagMask / 親子 / コンポーネントの増減は、エンティティの JSON を直して ApplyPartial (Undo の復元と同じ経路)
    std::map<uint64_t, std::vector<const SceneRestoreOp*>> byEntity;
    for (const SceneRestoreOp& op : ops) {
        switch (op.kind) {
        case SceneRestoreOp::Kind::SetEntityKey:
        case SceneRestoreOp::Kind::SetHierarchy:
        case SceneRestoreOp::Kind::AddComponent:
        case SceneRestoreOp::Kind::RemoveComponent: byEntity[op.fileId].push_back(&op); break;
        case SceneRestoreOp::Kind::SetField:
        case SceneRestoreOp::Kind::CreateEntities:
        case SceneRestoreOp::Kind::DestroyEntities: break;
        }
    }
    json patched = json::array();
    for (const auto& [fileId, list] : byEntity) {
        GameObject object = scene.FindByFileId(fileId);
        if (!object) {
            return false;
        }
        json entity = SceneSerializer::EntityToJson(scene, object.Id());
        for (const SceneRestoreOp* op : list) {
            if (op->kind == SceneRestoreOp::Kind::SetEntityKey) {
                if (op->field == "tagMask" && op->value == 0) {
                    entity.erase("tagMask");
                } else {
                    entity[op->field] = op->value;
                }
            } else if (op->kind == SceneRestoreOp::Kind::SetHierarchy) {
                if (op->field == "parent" && op->value == 0) {
                    entity.erase("parent");
                } else {
                    entity[op->field] = op->value;
                }
            } else if (op->kind == SceneRestoreOp::Kind::AddComponent) {
                entity["components"][op->component] = op->value;
            } else {
                entity["components"].erase(op->component);
            }
        }
        patched.push_back(std::move(entity));
    }
    if (!patched.empty() && !SceneSerializer::ApplyPartial(scene, patched, false)) {
        return false;
    }
    for (const SceneRestoreOp& op : ops) {
        if (op.kind != SceneRestoreOp::Kind::DestroyEntities) {
            continue;
        }
        GameObject object = scene.FindByFileId(op.fileId);
        if (!object) {
            return false;
        }
        world.DestroyEntity(object.Id()); // 子孫も破棄 (確定は次の ApplyStructuralChanges)
    }
    world.ApplyStructuralChanges();
    return true;
}

} // namespace

std::vector<int64_t> RestoredSceneChangeIds(const SceneRestorePlan& plan)
{
    std::vector<int64_t> ids;
    for (const SceneRestoreRowResult& row : plan.rows) {
        if (row.verdict == SceneRestoreVerdict::Ready || row.verdict == SceneRestoreVerdict::AlreadyRestored) {
            ids.push_back(row.sceneChangeId);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

json BuildSceneRestoreRecord(const std::string& restoreId, const std::string& state, int64_t revisionId, const std::vector<int64_t>& sceneChangeIds,
                             const std::string& scenePath, const std::string& error)
{
    json paths = json::array();
    if (!scenePath.empty()) {
        paths.push_back(scenePath);
    }
    json target = { { "revisionId", revisionId }, { "scope", "scene" }, { "sceneChangeIds", sceneChangeIds }, { "paths", paths } };
    json args = { { "restoreId", restoreId }, { "state", state }, { "targets", json::array({ target }) } };
    if (!error.empty()) {
        args["error"] = error.size() > 900 ? error.substr(0, 900) : error;
    }
    return args;
}

SceneRestoreResult ApplySceneRestore(const SceneRestoreRequest& request)
{
    SceneRestoreResult result;
    const SceneRestoreEnv& env = request.env;
    if (env.scene == nullptr || env.undo == nullptr || env.selection == nullptr) {
        result.outcome = SceneRestoreOutcome::NoScene;
        return result;
    }
    // 1. 実行の直前に、もう一度ゲートを判定する
    if (request.blocks) {
        result.blocks = request.blocks();
        if (!result.blocks.empty()) {
            result.outcome = SceneRestoreOutcome::Blocked;
            return result;
        }
    }
    // 2. 計画を判定し直す。プレビューのときと違えば書かない
    SceneRestoreInput input;
    input.rows = request.rows;
    input.actor = request.actor;
    input.openDocumentId = request.openDocumentId;
    {
        LiveSceneReadView view(env);
        result.plan = BuildSceneRestorePlan(input, view);
    }
    if (result.plan.verdict == SceneRestoreVerdict::AlreadyRestored) {
        result.outcome = SceneRestoreOutcome::AlreadyDone;
        return result;
    }
    if (result.plan.verdict != SceneRestoreVerdict::Ready) {
        result.outcome = SceneRestoreOutcome::NotReady;
        return result;
    }
    if (result.plan.ops != request.previewOps) {
        result.outcome = SceneRestoreOutcome::PlanChanged;
        return result;
    }
    result.restoreId = NewRestoreId();
    const std::vector<SceneRestoreOp>& ops = result.plan.ops;
    const std::vector<uint64_t> fileIds = TouchedFileIds(ops);
    std::vector<uint64_t> destroyedIds;
    for (const SceneRestoreOp& op : ops) {
        destroyedIds.insert(destroyedIds.end(), op.subtree.begin(), op.subtree.end());
    }
    // 3. Undo の 1 エントリの中で書く (CaptureAfter がプレハブの上書きの記録を取り直す)。構造を変えるので Apply
    bool verifyFailed = false;
    env.undo->Record(request.undoLabel.c_str(), *env.scene, *env.selection, std::span<const uint64_t>(fileIds), UndoStack::StructuralChanges::Apply, [&] {
        // 失敗したときの戻し先: 書く前の、触るエンティティのサブツリー
        json snapshot = json::array();
        for (uint64_t fileId : fileIds) {
            GameObject object = env.scene->FindByFileId(fileId);
            if (object) {
                for (json& item : SceneSerializer::SubtreeToJson(*env.scene, object.Id())) {
                    snapshot.push_back(std::move(item));
                }
            }
        }
        bool ok = WriteOps(env, ops);
        if (ok) {
            // 4. 読み直して、全部が Before と同じになったことを確かめる
            LiveSceneReadView view(env);
            const SceneRestorePlan after = BuildSceneRestorePlan(input, view);
            ok = after.verdict == SceneRestoreVerdict::AlreadyRestored;
            if (!ok) {
                for (const SceneRestoreRowResult& row : after.rows) {
                    if (!row.derived && row.verdict != SceneRestoreVerdict::AlreadyRestored) {
                        result.detail = std::to_string(row.sceneChangeId);
                        break;
                    }
                }
            }
        }
        if (!ok) {
            // 自分で書いた分を戻す: 作ったものを消し、書く前のサブツリーを当て直す。Before と After が同じになるので、Undo には積まれない
            verifyFailed = true;
            World& world = env.scene->GetWorld();
            for (const SceneRestoreOp& op : ops) {
                if (op.kind == SceneRestoreOp::Kind::CreateEntities) {
                    for (const json& item : op.value) {
                        GameObject made = env.scene->FindByFileId(item.value("fileId", 0ull));
                        if (made) {
                            world.DestroyEntity(made.Id());
                        }
                    }
                }
            }
            world.ApplyStructuralChanges();
            SceneSerializer::ApplyPartial(*env.scene, snapshot, /*removeHiddenMissing=*/true);
            world.ApplyStructuralChanges();
            return;
        }
        for (uint64_t fileId : destroyedIds) {
            env.selection->Remove(fileId); // 消えたエンティティを選択に残さない (EndRecord が書き込む選択へ反映される)
        }
    });
    result.outcome = verifyFailed ? SceneRestoreOutcome::VerifyFailed : SceneRestoreOutcome::Committed;
    result.appliedOps = verifyFailed ? 0 : static_cast<int>(ops.size());
    return result;
}

} // namespace mye
