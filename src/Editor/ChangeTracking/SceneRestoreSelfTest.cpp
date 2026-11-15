//====================================================================================
//                          SceneRestoreSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元 (照合・Undo・ゲート・窓・C#・プレハブの上書き) の回帰テスト
//====================================================================================
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <process.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/AssetRegistrySync.h"
#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/RestoreFormat.h"
#include "Editor/ChangeTracking/RestoreRunFormat.h"
#include "Editor/ChangeTracking/SceneRestoreFormat.h"
#include "Editor/ChangeTracking/SceneRestorePlan.h"
#include "Editor/ChangeTracking/SceneRestoreRunner.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Editor/Windows/Project/SceneRestoreWindow.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/EntityNaming.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

template <typename F>
void InBothLanguages(F&& body)
{
    const Lang previous = CurrentLanguage();
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        SetLanguage(lang);
        body(lang);
    }
    SetLanguage(previous);
}

// ---- ライブのシーンの道具 ----

struct SceneRig {
    Scene scene;
    UndoStack undo;
    Selection selection;

    SceneRestoreEnv Env(ManagedHost* managed = nullptr)
    {
        SceneRestoreEnv env;
        env.scene = &scene;
        env.undo = &undo;
        env.selection = &selection;
        env.managed = managed;
        return env;
    }
    uint64_t Make(const char* name, uint64_t parentFid = 0)
    {
        GameObject object = scene.CreateGameObjectTracked(name);
        if (parentFid != 0) {
            object.SetParent(scene.FindByFileId(parentFid));
        }
        scene.GetWorld().ApplyStructuralChanges();
        return scene.EnsureFileId(object.Id());
    }
    EntityID Id(uint64_t fid) { return scene.FindByFileId(fid).Id(); }
    json Entity(uint64_t fid)
    {
        GameObject object = scene.FindByFileId(fid);
        return object ? SceneSerializer::EntityToJson(scene, object.Id()) : json();
    }
    json Comp(uint64_t fid, const char* name)
    {
        const json entity = Entity(fid);
        return entity.is_object() && entity["components"].contains(name) ? entity["components"][name] : json();
    }
    bool Has(uint64_t fid) { return static_cast<bool>(scene.FindByFileId(fid)); }
    CameraComponent* Camera(uint64_t fid) { return scene.GetWorld().GetComponent<CameraComponent>(Id(fid)); }
    LocalTransform* Transform(uint64_t fid) { return scene.GetWorld().GetComponent<LocalTransform>(Id(fid)); }
    uint64_t Hash() { return HashWorld(scene.GetWorld()); }
    size_t Archetypes() { return scene.GetWorld().Archetypes().size(); }
};

constexpr const char* kDoc = "docA";

TrackSceneChange Row(int64_t id, const char* type, int64_t fileId, const char* component, const char* field, const char* before, const char* after)
{
    TrackSceneChange row;
    row.sceneChangeId = id;
    row.documentId = kDoc;
    row.resolvedDocumentId = kDoc;
    row.hasFileId = fileId != 0;
    row.fileId = fileId;
    row.changeType = type;
    row.component = component != nullptr ? component : "";
    row.field = field != nullptr ? field : "";
    row.hasBefore = before != nullptr;
    row.hasAfter = after != nullptr;
    row.beforeValue = before != nullptr ? before : "";
    row.afterValue = after != nullptr ? after : "";
    row.state = "active";
    return row;
}

SceneRestoreInput Input(std::vector<TrackSceneChange> rows)
{
    SceneRestoreInput input;
    input.rows = std::move(rows);
    input.actor = "ai";
    input.openDocumentId = kDoc;
    return input;
}

SceneRestorePlan PlanOf(SceneRig& rig, const std::vector<TrackSceneChange>& rows, const char* actor = "ai", const char* doc = kDoc, ManagedHost* managed = nullptr)
{
    SceneRestoreInput input = Input(rows);
    input.actor = actor;
    input.openDocumentId = doc;
    LiveSceneReadView view(rig.Env(managed));
    return BuildSceneRestorePlan(input, view);
}

SceneRestoreResult Apply(SceneRig& rig, const std::vector<TrackSceneChange>& rows, const SceneRestorePlan& preview, ManagedHost* managed = nullptr,
                         std::function<std::vector<RestoreBlock>()> blocks = {})
{
    SceneRestoreRequest request;
    request.env = rig.Env(managed);
    request.revisionId = 5;
    request.actor = "ai";
    request.openDocumentId = kDoc;
    request.scenePath = "assets/a.scene.json";
    request.rows = rows;
    request.previewOps = preview.ops;
    request.blocks = std::move(blocks);
    request.undoLabel = "Restore AI change";
    return ApplySceneRestore(request);
}

// 偽の読み取り口 (純粋な判定の試験用)
class FakeView final : public SceneReadView {
public:
    std::map<uint64_t, json> entities;
    std::map<uint64_t, std::vector<uint64_t>> children;
    bool Entity(uint64_t fileId, json& out) const override
    {
        const auto it = entities.find(fileId);
        if (it == entities.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    std::vector<uint64_t> Children(uint64_t fileId) const override
    {
        const auto it = children.find(fileId);
        return it == children.end() ? std::vector<uint64_t>() : it->second;
    }
    bool IsManagedComponent(const std::string&) const override { return false; }
};

bool HasIssue(const SceneRestorePlan& plan, SceneRestoreIssue issue)
{
    return std::any_of(plan.rows.begin(), plan.rows.end(), [&](const SceneRestoreRowResult& r) { return r.issue == issue; });
}

// ---- 純粋な判定 (正規形・種類・範囲) ----

void RunPureTests(const Check& check)
{
    // 正規形: 字句ではなく、フィールドの型で読んだ値で比べる
    json canonA;
    json canonB;
    json canonC;
    check(CanonicalFieldJson("Camera", "nearZ", json::parse("1.0"), false, canonA) == CanonStatus::Ok
              && CanonicalFieldJson("Camera", "nearZ", json::parse("1"), false, canonB) == CanonStatus::Ok
              && CanonicalFieldJson("Camera", "nearZ", json::parse("1e0"), false, canonC) == CanonStatus::Ok && canonA == canonB && canonB == canonC,
          "canon: 1.0 / 1 / 1e0 are the same float after reading it as the field's type");
    check(CanonicalFieldJson("Camera", "nearZ", json::parse("\"abc\""), false, canonA) == CanonStatus::Unreadable
              && CanonicalFieldJson("Camera", "isPrimary", json::parse("[1]"), false, canonA) == CanonStatus::Unreadable,
          "canon: a value of the wrong type is unreadable (a string for a float, an array for a bool)");
    check(CanonicalFieldJson("NoSuchComponent", "x", json(1), false, canonA) == CanonStatus::UnknownComponent
              && CanonicalFieldJson("Camera", "noSuchField", json(1), false, canonA) == CanonStatus::UnknownField,
          "canon: an unknown component / field is reported as such");
    check(CanonicalFieldJson("Camera", "isPrimary", json::parse("1"), false, canonA) == CanonStatus::Ok && canonA == json(true), "canon: a legacy 0/1 bool reads as true");
    json defaultValue;
    check(DefaultFieldJson("Camera", "fovYDeg", false, defaultValue) && defaultValue == json(60.0f), "canon: the default of a missing key is the component's constructed value");
    // EntityRef は fileId の整数で比べる
    ComponentTypeId springType = ComponentRegistry::Get().FindByName("SpringJoint");
    check(springType != kInvalidComponentType && CanonicalFieldJson("SpringJoint", "connectedEntity", json::parse("12"), false, canonA) == CanonStatus::Ok && canonA == json(12u)
              && CanonicalFieldJson("SpringJoint", "connectedEntity", json::parse("\"x\""), false, canonA) == CanonStatus::Unreadable,
          "canon: an EntityRef is compared as the integer fileId");

    // 行の種類
    auto kind = [](const TrackSceneChange& r) { return ClassifySceneRow(r); };
    check(kind(Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1")) == SceneRowKind::Field && kind(Row(1, "EntityAdded", 1, nullptr, nullptr, nullptr, "{}")) == SceneRowKind::EntityAdded
              && kind(Row(1, "ComponentRemoved", 1, "Camera", nullptr, "{}", nullptr)) == SceneRowKind::ComponentRemoved
              && kind(Row(1, "HierarchyChanged", 1, nullptr, "parent", "1", "2")) == SceneRowKind::Hierarchy && kind(Row(1, "EntityModified", 1, nullptr, "name", "\"a\"", "\"b\"")) == SceneRowKind::EntityKey
              && kind(Row(1, "PrefabInstanceChanged", 1, "PrefabInstance", "prefabHash", "1", "2")) == SceneRowKind::Unsupported
              && kind(Row(1, "PrefabAssetChanged", 0, nullptr, nullptr, nullptr, nullptr)) == SceneRowKind::Unsupported,
          "kind: scene change types map to the restore kinds");
    check(kind(Row(1, "PrefabOverrideChanged", 1, "Camera", "nearZ", nullptr, "\"Camera.nearZ\"")) == SceneRowKind::OverrideKey
              && kind(Row(1, "PrefabOverrideChanged", 1, "Camera", "+", nullptr, "\"+Camera\"")) == SceneRowKind::OverrideKey
              && kind(Row(1, "PrefabOverrideChanged", 1, nullptr, "name", nullptr, "\"name\"")) == SceneRowKind::OverrideKey
              && kind(Row(1, "PrefabOverrideChanged", 1, "Camera", "nearZ", "0.1", "1")) == SceneRowKind::Field
              && kind(Row(1, "PrefabOverrideChanged", 1, nullptr, "name", "\"a\"", "\"b\"")) == SceneRowKind::EntityKey,
          "kind: an override key row (the set element only) is told apart from an overridden value row");

    // 範囲の選び方
    std::vector<TrackSceneChange> all = { Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1"), Row(2, "FieldModified", 1, "Camera", "farZ", "1", "2"),
                                          Row(3, "FieldModified", 1, "LocalTransform", "position", "[0,0,0]", "[1,1,1]"), Row(4, "FieldModified", 2, "Camera", "nearZ", "0.1", "1") };
    check(SelectSceneRows(all, all[0], SceneRestoreScope::Field).size() == 1 && SelectSceneRows(all, all[0], SceneRestoreScope::Component).size() == 2
              && SelectSceneRows(all, all[0], SceneRestoreScope::Entity).size() == 3 && SelectSceneRows(all, all[0], SceneRestoreScope::Revision).size() == 4,
          "scope: field / component / entity / revision select the right rows");
    {
        // 追加されたエンティティの範囲には、同じ変更で追加された子孫の行も入る (入れないと「After に無い子孫」で止まる)
        std::vector<TrackSceneChange> added = { Row(1, "EntityAdded", 10, nullptr, nullptr, nullptr, "{\"fileId\":10,\"name\":\"P\"}"),
                                                Row(2, "EntityAdded", 11, nullptr, nullptr, nullptr, "{\"fileId\":11,\"name\":\"C\",\"parent\":10}"),
                                                Row(3, "EntityAdded", 12, nullptr, nullptr, nullptr, "{\"fileId\":12,\"name\":\"G\",\"parent\":11}"),
                                                Row(4, "EntityAdded", 13, nullptr, nullptr, nullptr, "{\"fileId\":13,\"name\":\"Other\"}"),
                                                Row(5, "FieldModified", 14, "Camera", "nearZ", "0.1", "1") };
        check(SelectSceneRows(added, added[0], SceneRestoreScope::Entity).size() == 3 && SelectSceneRows(added, added[1], SceneRestoreScope::Entity).size() == 2
                  && SelectSceneRows(added, added[3], SceneRestoreScope::Entity).size() == 1,
              "scope: an added entity's scope includes the descendants added by the same change");
        std::vector<TrackSceneChange> removed = { Row(1, "EntityRemoved", 10, nullptr, nullptr, "{\"fileId\":10,\"name\":\"P\"}", nullptr),
                                                  Row(2, "EntityRemoved", 11, nullptr, nullptr, "{\"fileId\":11,\"name\":\"C\",\"parent\":10}", nullptr) };
        check(SelectSceneRows(removed, removed[0], SceneRestoreScope::Entity).size() == 2 && SelectSceneRows(removed, removed[1], SceneRestoreScope::Entity).size() == 1,
              "scope: a removed entity's scope includes the descendants removed by the same change");
    }
    check(!SceneRestoreScopeAvailable(Row(1, "EntityAdded", 1, nullptr, nullptr, nullptr, "{}"), SceneRestoreScope::Field)
              && SceneRestoreScopeAvailable(Row(1, "EntityAdded", 1, nullptr, nullptr, nullptr, "{}"), SceneRestoreScope::Entity),
          "scope: an entity-level row has no field scope");

    // 偽の読み取り口: Before のブロブが無くても、字句の違い (1.0 と 1) で競合しない / 型が合わない After は競合
    {
        FakeView view;
        view.entities[1] = json::parse(R"({"fileId":1,"name":"A","childIndex":0,"components":{"Camera":{"fovYDeg":60.0,"nearZ":1.0,"farZ":1000.0,"isPrimary":true,"orthographic":false,"orthoSize":5.0}}})");
        SceneRestoreInput input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1") });
        SceneRestorePlan plan = BuildSceneRestorePlan(input, view);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.ops[0].kind == SceneRestoreOp::Kind::SetField,
              "plan: After spelled 1 matches the live float 1.0 (canonical forms are equal)");
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "\"one\"") });
        plan = BuildSceneRestorePlan(input, view);
        check(plan.verdict == SceneRestoreVerdict::Conflict && plan.ops.empty(), "plan: an After of the wrong type is a conflict");
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "1.0", "1") });
        plan = BuildSceneRestorePlan(input, view);
        check(plan.verdict == SceneRestoreVerdict::AlreadyRestored && plan.ops.empty(), "plan: a value already equal to Before is already restored");
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "2") });
        check(BuildSceneRestorePlan(input, view).verdict == SceneRestoreVerdict::Conflict, "plan: a value that is neither Before nor After is a conflict");
        // 欄が足りない・行為者・開いていない文書・上限
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1") });
        input.actor = "editor_user";
        check(BuildSceneRestorePlan(input, view).verdict == SceneRestoreVerdict::NotRestorable && BuildSceneRestorePlan(input, view).issue == SceneRestoreIssue::ActorNotRestorable,
              "plan: only an AI / unknown actor's change is restorable");
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1") });
        input.openDocumentId.clear();
        check(BuildSceneRestorePlan(input, view).issue == SceneRestoreIssue::NotOpenDocument, "plan: with no open scene nothing is restorable item by item");
        input = Input({});
        check(BuildSceneRestorePlan(input, view).issue == SceneRestoreIssue::NoRows, "plan: an empty scope is reported");
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1") });
        input.rows[0].oversize = true;
        check(BuildSceneRestorePlan(input, view).issue == SceneRestoreIssue::ValueOversize, "plan: an oversize value cannot be restored");
        input = Input({ Row(1, "PrefabOverrideChanged", 1, "Camera", "nearZ", nullptr, "\"Camera.nearZ\"") });
        check(BuildSceneRestorePlan(input, view).issue == SceneRestoreIssue::OverrideKeyOnly, "plan: an override key without its value row cannot be restored alone");
        // 復元済みの状態の行 (サービスが restored にした行) は何もしない
        input = Input({ Row(1, "FieldModified", 1, "Camera", "nearZ", "0.1", "1") });
        input.rows[0].state = "restored";
        check(BuildSceneRestorePlan(input, view).verdict == SceneRestoreVerdict::AlreadyRestored, "plan: a row the service already marked restored is skipped");
    }

    // 全ての理由と、画面の文面が英日で埋まっている
    InBothLanguages([&](Lang) {
        bool allText = true;
        for (int i = 1; i <= static_cast<int>(SceneRestoreIssue::ParentCycle); ++i) {
            allText = allText && !SceneRestoreIssueText(static_cast<SceneRestoreIssue>(i), "7").empty();
        }
        check(allText, "format: every issue has a text in this language");
    });
    check(SceneRestoreIssueText(SceneRestoreIssue::ParentMissing, "42").find("42") != std::string::npos, "format: the fileId is filled into the issue text");
}

// ---- ライブのシーンへの適用 ----

void RunApplyTests(const Check& check)
{
    // (1) Field を戻す → Before、Undo → After、Redo → Before。何も戻らない Undo のエントリができない
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 90.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> rows = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "90") };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1, "field: the plan is ready when the live value is the recorded After");
        const uint64_t hashBefore = rig.Hash();
        const uint64_t serialBefore = rig.undo.StateSerial();
        const SceneRestoreResult result = Apply(rig, rows, plan);
        check(result.outcome == SceneRestoreOutcome::Committed && rig.Camera(fid)->fovYDeg == 60.0f, "field: restoring one field writes the Before value");
        check(rig.undo.CanUndo() && std::string(rig.undo.NextUndoLabel()) == "Restore AI change" && rig.undo.StateSerial() != serialBefore,
              "field: the restore is one Undo entry (the scene becomes dirty)");
        const uint64_t hashRestored = rig.Hash();
        rig.undo.Undo(rig.scene, rig.selection);
        check(rig.Camera(fid)->fovYDeg == 90.0f && rig.Hash() == hashBefore, "field: Undo puts the After value back (the world hash returns to the start)");
        rig.undo.Redo(rig.scene, rig.selection);
        check(rig.Camera(fid)->fovYDeg == 60.0f && rig.Hash() == hashRestored, "field: Redo restores the Before value again");
        // 復元のあとに同じ行を判定し直すと「復元済み」
        check(PlanOf(rig, rows).verdict == SceneRestoreVerdict::AlreadyRestored, "field: planning again after the restore says already restored");
    }

    // (2) 字句の違い: After が 1.0 でライブが 1 の float → 一致して戻せる。型が合わない After → 競合で World が変わらない
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->nearZ = 1.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> good = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "nearZ", "0.1", "1.0") };
        check(PlanOf(rig, good).verdict == SceneRestoreVerdict::Ready, "spelling: After 1.0 matches the live 1 (live float)");
        const std::vector<TrackSceneChange> bad = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "nearZ", "0.1", "[1]") };
        const SceneRestorePlan badPlan = PlanOf(rig, bad);
        const uint64_t hash = rig.Hash();
        const SceneRestoreResult result = Apply(rig, bad, badPlan);
        check(badPlan.verdict == SceneRestoreVerdict::Conflict && result.outcome == SceneRestoreOutcome::NotReady && rig.Hash() == hash && rig.Camera(fid)->nearZ == 1.0f,
              "spelling: an After of the wrong type is a conflict and the world is untouched");
    }

    // (3) 今の値が After と違う Field は競合。World (ハッシュ・エンティティ数・アーキタイプ数) も Undo の山も変わらない
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 75.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> rows = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "90.0") };
        SceneRestorePlan ready = PlanOf(rig, { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "75.0") });
        const SceneRestorePlan plan = PlanOf(rig, rows);
        const uint64_t hash = rig.Hash();
        const uint32_t alive = rig.scene.GetWorld().AliveCount();
        const size_t archetypes = rig.Archetypes();
        const uint64_t serial = rig.undo.StateSerial();
        const SceneRestoreResult result = Apply(rig, rows, ready); // プレビューは別の計画 (別の After)
        check(plan.verdict == SceneRestoreVerdict::Conflict && HasIssue(plan, SceneRestoreIssue::ValueChanged), "conflict: a value that is not the recorded After is a conflict");
        check(result.outcome == SceneRestoreOutcome::NotReady && rig.Hash() == hash && rig.scene.GetWorld().AliveCount() == alive && rig.Archetypes() == archetypes
                  && rig.undo.StateSerial() == serial && !rig.undo.CanUndo(),
              "conflict: nothing is written (world hash, entity count, archetype count, Undo stack unchanged)");
        // 実行の直前にシーンが変わると、プレビューの計画と違うので書かない
        SceneRestorePlan stale = PlanOf(rig, { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "75.0") });
        rig.Camera(fid)->fovYDeg = 80.0f; // プレビューのあとで編集された
        const SceneRestoreResult changed = Apply(rig, { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "75.0") }, stale);
        check(changed.outcome == SceneRestoreOutcome::NotReady && rig.Camera(fid)->fovYDeg == 80.0f, "conflict: a value edited after the preview is caught when the plan is built again");
    }

    // (3b) 実行の直前のゲートが止めたら、何も書かない (確認のあとに Play を始めた)
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 90.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> rows = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "90.0") };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        const uint64_t hash = rig.Hash();
        const SceneRestoreResult result = Apply(rig, rows, plan, nullptr, [] {
            RestoreBlock block;
            block.gate = GateBlocker::Playing;
            return std::vector<RestoreBlock>{ block };
        });
        check(result.outcome == SceneRestoreOutcome::Blocked && result.blocks.size() == 1 && rig.Hash() == hash && !rig.undo.CanUndo() && rig.Camera(fid)->fovYDeg == 90.0f,
              "gate: a block that appears just before the write stops it (nothing is written)");
    }

    // (4) 別のシーンを開いているとき、同じ fileId のエンティティに当たらない
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 90.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> rows = { Row(1, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "90.0") };
        const SceneRestorePlan plan = PlanOf(rig, rows, "ai", "docB"); // 開いているのは別のシーン (同じ fileId のエンティティがいる)
        const uint64_t hash = rig.Hash();
        SceneRestoreRequest request;
        request.env = rig.Env();
        request.actor = "ai";
        request.openDocumentId = "docB";
        request.rows = rows;
        request.previewOps = plan.ops;
        request.undoLabel = "x";
        const SceneRestoreResult result = ApplySceneRestore(request);
        check(plan.verdict == SceneRestoreVerdict::NotRestorable && plan.issue == SceneRestoreIssue::NotOpenDocument && result.outcome == SceneRestoreOutcome::NotReady
                  && rig.Hash() == hash && rig.Camera(fid)->fovYDeg == 90.0f,
              "document: a change of another scene is not applied to the entity with the same fileId");
    }

    // (5a) Component の追加の取り消し (外す) と Undo / Redo。After と違うコンポーネントは競合
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 70.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::string after = rig.Comp(fid, "Camera").dump();
        const std::vector<TrackSceneChange> rows = { Row(1, "ComponentAdded", static_cast<int64_t>(fid), "Camera", nullptr, nullptr, after.c_str()) };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.ops[0].kind == SceneRestoreOp::Kind::RemoveComponent, "component: undoing an added component plans to remove it");
        check(Apply(rig, rows, plan).outcome == SceneRestoreOutcome::Committed && rig.Camera(fid) == nullptr, "component: the added component is removed");
        rig.undo.Undo(rig.scene, rig.selection);
        check(rig.Camera(fid) != nullptr && rig.Camera(fid)->fovYDeg == 70.0f, "component: Undo brings the component back with its value (the entry is not empty)");
        rig.undo.Redo(rig.scene, rig.selection);
        check(rig.Camera(fid) == nullptr, "component: Redo removes it again");
        rig.undo.Undo(rig.scene, rig.selection);
        rig.Camera(fid)->fovYDeg = 20.0f; // After と違うコンポーネント
        check(PlanOf(rig, rows).verdict == SceneRestoreVerdict::Conflict && HasIssue(PlanOf(rig, rows), SceneRestoreIssue::ComponentChanged),
              "component: a component edited after the change is a conflict");
        // 必須のコンポーネントは外せない
        check(PlanOf(rig, { Row(1, "ComponentAdded", static_cast<int64_t>(fid), "LocalTransform", nullptr, nullptr, "{}") }).issue == SceneRestoreIssue::ProtectedComponent,
              "component: LocalTransform cannot be removed");
    }

    // (5b) Component の削除の取り消し (付ける)。同じコンポーネントがあれば競合
    {
        SceneRig rig;
        const uint64_t fid = rig.Make("A");
        const std::string before = R"({"fovYDeg":80.0,"nearZ":0.5,"farZ":900.0,"isPrimary":true,"orthographic":false,"orthoSize":5.0})";
        const std::vector<TrackSceneChange> rows = { Row(1, "ComponentRemoved", static_cast<int64_t>(fid), "Camera", nullptr, before.c_str(), nullptr) };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.ops[0].kind == SceneRestoreOp::Kind::AddComponent, "component: undoing a removal plans to add it");
        const SceneRestoreResult result = Apply(rig, rows, plan);
        check(result.outcome == SceneRestoreOutcome::Committed && rig.Camera(fid) != nullptr && rig.Camera(fid)->fovYDeg == 80.0f && rig.Camera(fid)->nearZ == 0.5f,
              "component: the removed component is added back with the recorded values");
        rig.undo.Undo(rig.scene, rig.selection);
        check(rig.Camera(fid) == nullptr, "component: Undo removes it again (the entry is not empty)");
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 33.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        check(PlanOf(rig, rows).verdict == SceneRestoreVerdict::Conflict && HasIssue(PlanOf(rig, rows), SceneRestoreIssue::ComponentExists), "component: a different component already there is a conflict");
        // 参照の先がいない
        const std::vector<TrackSceneChange> refRows = { Row(2, "ComponentRemoved", static_cast<int64_t>(fid), "SpringJoint", nullptr, R"({"connectedEntity":9999})", nullptr) };
        const SceneRestorePlan refPlan = PlanOf(rig, refRows);
        check(refPlan.verdict == SceneRestoreVerdict::Conflict && HasIssue(refPlan, SceneRestoreIssue::ReferenceMissing) && refPlan.ops.empty(),
              "component: a removed component that referenced a missing entity stops as a missing dependency");
    }

    // (5c) Entity の追加の取り消し (子孫つき) と、After に無い子孫が居る場合
    {
        SceneRig rig;
        const uint64_t keep = rig.Make("Keep");
        const uint64_t parent = rig.Make("P", keep);
        const uint64_t child = rig.Make("C", parent);
        const std::string pJson = rig.Entity(parent).dump();
        const std::string cJson = rig.Entity(child).dump();
        const std::vector<TrackSceneChange> rows = { Row(1, "EntityAdded", static_cast<int64_t>(parent), nullptr, nullptr, nullptr, pJson.c_str()),
                                                     Row(2, "EntityAdded", static_cast<int64_t>(child), nullptr, nullptr, nullptr, cJson.c_str()) };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.ops[0].kind == SceneRestoreOp::Kind::DestroyEntities && plan.ops[0].subtree.size() == 2
                  && plan.ops[0].sceneChangeIds.size() == 2,
              "entity: undoing an added entity with a child is one delete of the whole subtree");
        rig.selection.SelectOnly(child);
        check(Apply(rig, rows, plan).outcome == SceneRestoreOutcome::Committed && !rig.Has(parent) && !rig.Has(child) && rig.Has(keep), "entity: the added subtree is deleted and the rest stays");
        check(!rig.selection.Contains(child), "entity: a deleted entity is dropped from the selection");
        rig.undo.Undo(rig.scene, rig.selection);
        check(rig.Has(parent) && rig.Has(child) && rig.Entity(child)["parent"] == json(parent), "entity: Undo brings the subtree back with its parent link (the entry is not empty)");
        rig.undo.Redo(rig.scene, rig.selection);
        check(!rig.Has(parent) && !rig.Has(child), "entity: Redo deletes it again");
        rig.undo.Undo(rig.scene, rig.selection);
        const uint64_t extra = rig.Make("Extra", parent); // After に無い子孫
        const SceneRestorePlan blocked = PlanOf(rig, rows);
        check(blocked.verdict == SceneRestoreVerdict::Conflict && HasIssue(blocked, SceneRestoreIssue::ExtraDescendant) && blocked.ops.empty(),
              "entity: a descendant the change did not add stops the delete");
        (void)extra;
    }

    // (5d) Entity の削除の取り消し (子孫つき)。親がいない・参照の先がいない・fileId が使われている
    {
        SceneRig rig;
        const uint64_t keep = rig.Make("Keep");
        const uint64_t parent = rig.Make("P", keep);
        const uint64_t child = rig.Make("C", parent);
        rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(child))->fovYDeg = 44.0f;
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::string pJson = rig.Entity(parent).dump();
        const std::string cJson = rig.Entity(child).dump();
        // AI が P (と子) を消した状態を作る
        rig.scene.GetWorld().DestroyEntity(rig.Id(parent));
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> rows = { Row(1, "EntityRemoved", static_cast<int64_t>(parent), nullptr, nullptr, pJson.c_str(), nullptr),
                                                     Row(2, "EntityRemoved", static_cast<int64_t>(child), nullptr, nullptr, cJson.c_str(), nullptr) };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.ops[0].kind == SceneRestoreOp::Kind::CreateEntities && plan.ops[0].value.size() == 2,
              "entity: undoing a deletion re-creates the entity and the descendants deleted by the same change");
        check(Apply(rig, rows, plan).outcome == SceneRestoreOutcome::Committed && rig.Has(parent) && rig.Has(child) && rig.Entity(child)["parent"] == json(parent)
                  && rig.Camera(child) != nullptr && rig.Camera(child)->fovYDeg == 44.0f && rig.Entity(parent)["parent"] == json(keep),
              "entity: the re-created entities have their parents, components and values");
        check(rig.scene.PeekNextFileId() > child && rig.scene.PeekNextFileId() > parent, "entity: new fileIds do not collide with the restored ones");
        rig.undo.Undo(rig.scene, rig.selection);
        check(!rig.Has(parent) && !rig.Has(child), "entity: Undo deletes them again (the entry is not empty)");
        rig.undo.Redo(rig.scene, rig.selection);
        check(rig.Has(parent) && rig.Has(child), "entity: Redo creates them again");
        // 使われている fileId
        const std::string reuse = "{\"fileId\":" + std::to_string(keep) + ",\"name\":\"other\"}";
        const SceneRestorePlan inUse = PlanOf(rig, { Row(3, "EntityRemoved", static_cast<int64_t>(keep), nullptr, nullptr, reuse.c_str(), nullptr) });
        check(inUse.verdict == SceneRestoreVerdict::Conflict && HasIssue(inUse, SceneRestoreIssue::EntityExists) && inUse.ops.empty(), "entity: a fileId in use is not re-created");
        // 親がいない
        rig.scene.GetWorld().DestroyEntity(rig.Id(parent));
        rig.scene.GetWorld().DestroyEntity(rig.Id(keep));
        rig.scene.GetWorld().ApplyStructuralChanges();
        const SceneRestorePlan orphan = PlanOf(rig, { rows[1] });
        check(orphan.verdict == SceneRestoreVerdict::Conflict && HasIssue(orphan, SceneRestoreIssue::ParentMissing) && orphan.ops.empty(), "entity: a deleted entity whose parent is gone stops as a missing dependency");
        // 参照の先がいない
        const std::string refJson = std::string("{\"fileId\":777,\"name\":\"R\",\"childIndex\":0,\"components\":{\"SpringJoint\":{\"connectedEntity\":888}}}");
        const SceneRestorePlan ref = PlanOf(rig, { Row(4, "EntityRemoved", 777, nullptr, nullptr, refJson.c_str(), nullptr) });
        check(ref.verdict == SceneRestoreVerdict::Conflict && HasIssue(ref, SceneRestoreIssue::ReferenceMissing) && ref.ops.empty(), "entity: an entity referring to a missing entity stops as a missing dependency");
        // 参照の先が同じ変更で消えたエンティティなら通る
        const std::string refJson2 = std::string("{\"fileId\":778,\"name\":\"R2\",\"childIndex\":0,\"components\":{\"SpringJoint\":{\"connectedEntity\":779}}}");
        const std::string targetJson = std::string("{\"fileId\":779,\"name\":\"T\",\"childIndex\":1,\"components\":{}}");
        const SceneRestorePlan pair = PlanOf(rig, { Row(5, "EntityRemoved", 778, nullptr, nullptr, refJson2.c_str(), nullptr), Row(6, "EntityRemoved", 779, nullptr, nullptr, targetJson.c_str(), nullptr) });
        check(pair.verdict == SceneRestoreVerdict::Ready && pair.ops.size() == 2, "entity: a reference to an entity deleted by the same change is allowed");
    }

    // (5e) name / tagMask / 親子
    {
        SceneRig rig;
        const uint64_t a = rig.Make("Before");
        const uint64_t b = rig.Make("B");
        SetEntityName(rig.scene.GetWorld(), rig.Id(a), "After");
        const std::vector<TrackSceneChange> nameRows = { Row(1, "EntityModified", static_cast<int64_t>(a), nullptr, "name", "\"Before\"", "\"After\"") };
        const SceneRestorePlan namePlan = PlanOf(rig, nameRows);
        check(namePlan.verdict == SceneRestoreVerdict::Ready && Apply(rig, nameRows, namePlan).outcome == SceneRestoreOutcome::Committed
                  && std::string(rig.scene.GetWorld().GetName(rig.Id(a))) == "Before",
              "key: the entity name is restored");
        rig.undo.Undo(rig.scene, rig.selection);
        check(std::string(rig.scene.GetWorld().GetName(rig.Id(a))) == "After", "key: Undo puts the name back");
        // 親子: a を b の子にした変更を戻す
        rig.scene.GetWorld().SetParent(rig.Id(a), rig.Id(b));
        rig.scene.GetWorld().ApplyStructuralChanges();
        const std::vector<TrackSceneChange> parentRows = { Row(2, "HierarchyChanged", static_cast<int64_t>(a), nullptr, "parent", nullptr, std::to_string(b).c_str()) };
        const SceneRestorePlan parentPlan = PlanOf(rig, parentRows);
        check(parentPlan.verdict == SceneRestoreVerdict::Ready && Apply(rig, parentRows, parentPlan).outcome == SceneRestoreOutcome::Committed && rig.Entity(a)["parent"].is_null(),
              "hierarchy: a re-parent is undone (back to the root)");
        rig.undo.Undo(rig.scene, rig.selection);
        check(rig.Entity(a)["parent"] == json(b), "hierarchy: Undo restores the new parent");
        // 輪になる戻し: b の親を a に戻すと、a が b の子のまま輪になる
        const std::vector<TrackSceneChange> cycleRows = { Row(3, "HierarchyChanged", static_cast<int64_t>(b), nullptr, "parent", std::to_string(a).c_str(), nullptr) };
        const SceneRestorePlan cycle = PlanOf(rig, cycleRows);
        check(cycle.verdict == SceneRestoreVerdict::Conflict && HasIssue(cycle, SceneRestoreIssue::ParentCycle), "hierarchy: restoring a parent that is a descendant is refused");
    }

    // (5f) 書いた後の読み直しが合わなければ、自分で書いた分を戻し、Undo には何も積まない。
    //      兄弟の位置は範囲に丸められるので、兄弟の数より大きい Before の位置は、書いても読み直しで合わない
    {
        SceneRig rig;
        const uint64_t a = rig.Make("A");
        const uint64_t b = rig.Make("B");
        const std::vector<TrackSceneChange> rows = { Row(1, "HierarchyChanged", static_cast<int64_t>(a), nullptr, "childIndex", "5", "0") };
        const SceneRestorePlan plan = PlanOf(rig, rows);
        const uint64_t hash = rig.Hash();
        const uint64_t serial = rig.undo.StateSerial();
        const SceneRestoreResult result = Apply(rig, rows, plan);
        check(plan.verdict == SceneRestoreVerdict::Ready && result.outcome == SceneRestoreOutcome::VerifyFailed, "verify: a restore that does not read back as the Before is reported as failed");
        check(rig.Entity(a)["childIndex"] == json(0) && rig.Entity(b)["childIndex"] == json(1) && rig.Hash() == hash, "verify: what was written is put back (the world is as it was)");
        check(!rig.undo.CanUndo() && rig.undo.StateSerial() == serial, "verify: a failed restore leaves nothing on the Undo stack");
    }
}

// ---- プレハブの上書きの記録 ----

void RunPrefabTests(const Check& check)
{
    PrefabLibrary lib;
    const std::wstring path = (fs::temp_directory_path() / (L"mye_scene_restore_" + std::to_wstring(_getpid()) + L".actor.json")).wstring();
    Scene source;
    GameObject root = source.CreateGameObjectTracked("Root");
    root.SetLocalPosition(1.0f, 2.0f, 3.0f);
    source.GetWorld().ApplyStructuralChanges();
    const uint64_t hash = Prefab::CreateAsset(source, lib, path, root.Id());

    SceneRig rig;
    rig.undo.SetPrefabLibrary(&lib);
    const uint64_t rootFid = Prefab::Instantiate(rig.scene, lib, hash, 0);
    rig.scene.GetWorld().ApplyStructuralChanges();
    const std::string key = "LocalTransform.position";
    // AI が位置を上書きした状態を、エディタの編集経路 (Undo の記録) で作る
    rig.undo.Record("edit", rig.scene, rig.selection, rootFid, UndoStack::StructuralChanges::None, [&] { rig.Transform(rootFid)->position.x = 99.0f; });
    check(rig.scene.GetOverrides(rootFid) != nullptr && *rig.scene.GetOverrides(rootFid) == std::set<std::string>{ key }, "prefab: the field edit recorded exactly the position override");
    const std::vector<TrackSceneChange> rows = { Row(1, "PrefabOverrideChanged", static_cast<int64_t>(rootFid), "LocalTransform", "position", "[1.0,2.0,3.0]", "[99.0,2.0,3.0]"),
                                                 Row(2, "PrefabOverrideChanged", static_cast<int64_t>(rootFid), "LocalTransform", "position", nullptr, "\"LocalTransform.position\"") };
    const SceneRestorePlan plan = PlanOf(rig, rows);
    check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1 && plan.rows[1].derived, "prefab: the value row is restored and the override key row follows it (derived)");
    const SceneRestoreResult result = Apply(rig, rows, plan);
    check(result.outcome == SceneRestoreOutcome::Committed && rig.Transform(rootFid)->position.x == 1.0f, "prefab: the overridden field goes back to the recorded Before");
    check(rig.scene.HasOverrideRecord(rootFid) && rig.scene.GetOverrides(rootFid) != nullptr && rig.scene.GetOverrides(rootFid)->empty(),
          "prefab: the override record was re-taken (the override is gone, the record is not lost)");
    const json saved = SceneSerializer::SaveToJson(rig.scene);
    bool wroteEmpty = false;
    for (const json& entity : saved["entities"]) {
        if (entity.value("fileId", 0ull) == rootFid) {
            wroteEmpty = entity.contains("overrides") && entity["overrides"] == json::array();
        }
    }
    Scene reloaded;
    SceneSerializer::LoadFromJson(reloaded, saved);
    check(wroteEmpty && reloaded.GetOverrides(rootFid) != nullptr && reloaded.GetOverrides(rootFid)->empty(), "prefab: saving and reading back keeps the same (empty) override list");
    rig.undo.Undo(rig.scene, rig.selection);
    check(rig.Transform(rootFid)->position.x == 99.0f && rig.scene.GetOverrides(rootFid) != nullptr && *rig.scene.GetOverrides(rootFid) == std::set<std::string>{ key },
          "prefab: Undo brings the value and the override record back together");
    std::error_code ec;
    fs::remove(path, ec);
}

// ---- C# コンポーネント ----

constexpr const char* kCsProbeSource = R"CS(
using MyeScripting;

public class RestoreProbe : MyeScript
{
    public float speed = 1.5f;
    public int count = 3;
    public bool flag = true;
    public MyeVec3 dir = new MyeVec3(1.0f, 2.0f, 3.0f);
}
)CS";

void RunManagedTests(const Check& check)
{
    const std::wstring exeDir = GetExecutableDir();
    if (!fs::exists(fs::path(exeDir) / L"MyeScripting.dll")) {
        MYE_LOG_WARN("  SKIP: MyeScripting.dll is not next to the executable (tools\\build_managed.bat was not run). The C# component restore is NOT verified");
        return;
    }
    const fs::path root = fs::temp_directory_path() / (L"mye_scene_restore_cs_" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"assets" / L"scripts", ec);
    std::ofstream(root / L"assets" / L"scripts" / L"RestoreProbe.cs", std::ios::binary) << kCsProbeSource;

    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine SceneRestoreSelfTest";
    setup.saveDirOverride = (fs::temp_directory_path() / (L"mye_selftest_save_p" + std::to_wstring(_getpid()))).wstring();
    setup.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    HeadlessSim sim;
    if (setup.scene.showcase == nullptr || !sim.Init(setup)) {
        check(false, "managed: the headless sim starts (run from the repository root)");
        fs::remove_all(root, ec);
        return;
    }
    ManagedHost& host = sim.Managed();
    Scene* scene = sim.Refs().scene;
    if (!host.Init(exeDir, scene)) {
        MYE_LOG_WARN("  SKIP: CoreCLR could not start (no .NET runtime?). The C# component restore is NOT verified");
        fs::remove_all(root, ec);
        return;
    }
    if (!host.CompileScripts((root / L"assets" / L"scripts").wstring())) {
        check(false, "managed: the probe script compiles");
        host.Shutdown();
        fs::remove_all(root, ec);
        return;
    }
    const ComponentTypeId type = ComponentRegistry::Get().FindByName("RestoreProbe");
    check(type != kInvalidComponentType && host.IsManagedComponent(type), "managed: the probe script is registered as a C# component");
    if (type == kInvalidComponentType) {
        host.Shutdown();
        fs::remove_all(root, ec);
        return;
    }

    UndoStack undo;
    Selection selection;
    SceneRestoreEnv env;
    env.scene = scene;
    env.undo = &undo;
    env.selection = &selection;
    env.managed = &host;
    World& world = scene->GetWorld();
    GameObject go = scene->CreateGameObjectTracked("CsProbe");
    world.AddComponentRaw(go.Id(), type);
    world.ApplyStructuralChanges();
    const uint64_t fid = scene->EnsureFileId(go.Id());
    auto comp = [&]() { return SceneSerializer::EntityToJson(*scene, scene->FindByFileId(fid).Id())["components"]["RestoreProbe"]; };
    auto setJson = [&](const char* text) { host.DeserializeComponent(type, go.Id(), world.GetComponentRaw(go.Id(), type), text); };

    const json initial = comp();
    check(initial.is_object() && initial.contains("speed") && initial.contains("dir"), "managed: the live C# component serializes its public fields");
    // (a) 1 キーの JSON で、他のフィールドが変わらない
    setJson(R"({"speed":9.0})");
    json now = comp();
    check(now["speed"] == json(9.0) && now["count"] == initial["count"] && now["flag"] == initial["flag"] && now["dir"] == initial["dir"],
          "managed (a): DeserializeComponent with one key changes only that field");
    // 復元の計画と適用: speed を Before (1.5) へ。C# は managed の出力どうしで比べる
    const std::string initialSpeed = initial["speed"].dump();
    const std::vector<TrackSceneChange> rows = { Row(1, "FieldModified", static_cast<int64_t>(fid), "RestoreProbe", "speed", initialSpeed.c_str(), "9.0") };
    SceneRestoreInput input = Input(rows);
    LiveSceneReadView view(env);
    const SceneRestorePlan plan = BuildSceneRestorePlan(input, view);
    check(plan.verdict == SceneRestoreVerdict::Ready && plan.ops.size() == 1, "managed: a C# field is planned from the managed serializer's output");
    SceneRestoreRequest request;
    request.env = env;
    request.actor = "ai";
    request.openDocumentId = kDoc;
    request.rows = rows;
    request.previewOps = plan.ops;
    request.undoLabel = "Restore AI change";
    const SceneRestoreResult result = ApplySceneRestore(request);
    now = comp();
    check(result.outcome == SceneRestoreOutcome::Committed && now["speed"] == initial["speed"] && now["count"] == initial["count"] && now["dir"] == initial["dir"],
          "managed (a): restoring one C# field writes it and leaves the others");
    // (b) Undo で戻り、Redo でもう一度当たる
    undo.Undo(*scene, selection);
    now = comp();
    check(now["speed"] == json(9.0) && now["count"] == initial["count"], "managed (b): Undo brings the C# value back");
    undo.Redo(*scene, selection);
    now = comp();
    check(now["speed"] == initial["speed"], "managed (b): Redo restores it again");
    // 競合: 今の値が After と違う
    setJson(R"({"speed":4.0})");
    check(BuildSceneRestorePlan(input, view).verdict == SceneRestoreVerdict::Conflict, "managed: a C# value that is not the recorded After is a conflict");
    // 型が合わない
    check(BuildSceneRestorePlan(Input({ Row(1, "FieldModified", static_cast<int64_t>(fid), "RestoreProbe", "speed", "1.5", "4.0") }), view).verdict == SceneRestoreVerdict::Ready
              && BuildSceneRestorePlan(Input({ Row(1, "FieldModified", static_cast<int64_t>(fid), "RestoreProbe", "speed", nullptr, "4.0") }), view).issue == SceneRestoreIssue::NoBefore,
          "managed: a C# field with no known Before cannot be restored");
    setJson(R"({"speed":9.0})");

    // (c) プレハブのインスタンスの上書きの記録が、C# コンポーネントのフィールドで保たれる
    {
        PrefabLibrary lib;
        const std::wstring prefabPath = (root / L"cs.actor.json").wstring();
        const uint64_t prefabHash = Prefab::CreateAsset(*scene, lib, prefabPath, go.Id());
        undo.SetPrefabLibrary(&lib);
        const uint64_t instFid = Prefab::Instantiate(*scene, lib, prefabHash, 0);
        world.ApplyStructuralChanges();
        GameObject inst = scene->FindByFileId(instFid);
        const bool made = static_cast<bool>(inst) && world.GetComponentRaw(inst.Id(), type) != nullptr;
        check(made, "managed (c): a prefab instance with the C# component is created");
        if (made) {
            auto instComp = [&]() { return SceneSerializer::EntityToJson(*scene, scene->FindByFileId(instFid).Id())["components"]["RestoreProbe"]; };
            const json baseComp = instComp();
            undo.Record("edit", *scene, selection, instFid, UndoStack::StructuralChanges::None, [&] {
                host.DeserializeComponent(type, inst.Id(), world.GetComponentRaw(inst.Id(), type), R"({"speed":7.0})");
            });
            const std::set<std::string> overridesAfterEdit = scene->GetOverrides(instFid) != nullptr ? *scene->GetOverrides(instFid) : std::set<std::string>();
            SceneRestoreEnv instEnv = env;
            const std::string baseSpeed = baseComp["speed"].dump();
            const std::vector<TrackSceneChange> instRows = { Row(1, "FieldModified", static_cast<int64_t>(instFid), "RestoreProbe", "speed", baseSpeed.c_str(), "7.0") };
            LiveSceneReadView instView(instEnv);
            SceneRestoreInput instInput = Input(instRows);
            const SceneRestorePlan instPlan = BuildSceneRestorePlan(instInput, instView);
            SceneRestoreRequest instRequest;
            instRequest.env = instEnv;
            instRequest.actor = "ai";
            instRequest.openDocumentId = kDoc;
            instRequest.rows = instRows;
            instRequest.previewOps = instPlan.ops;
            instRequest.undoLabel = "Restore AI change";
            const SceneRestoreResult instResult = ApplySceneRestore(instRequest);
            const bool recorded = scene->HasOverrideRecord(instFid);
            const json saved = SceneSerializer::SaveToJson(*scene);
            Scene reloaded;
            SceneSerializer::LoadFromJson(reloaded, saved);
            check(instResult.outcome == SceneRestoreOutcome::Committed && instComp()["speed"] == baseComp["speed"] && recorded
                      && reloaded.HasOverrideRecord(instFid)
                      && (reloaded.GetOverrides(instFid) != nullptr ? *reloaded.GetOverrides(instFid) : std::set<std::string>()) == (scene->GetOverrides(instFid) != nullptr ? *scene->GetOverrides(instFid) : std::set<std::string>()),
                  "managed (c): restoring a C# field on a prefab instance keeps the override record, and it survives save and reload");
            MYE_LOG_INFO("  info: override keys after the C# edit = %zu (managed fields have no FieldDesc, so the live diff does not record them)", overridesAfterEdit.size());
        }
        undo.SetPrefabLibrary(nullptr);
    }
    host.Shutdown();
    fs::remove_all(root, ec);
}

// ---- ゲート ----

void RunGateTests(const Check& check)
{
    auto blocksFor = [](GateInputs gate, bool tracked = true, bool journal = false, bool external = false) {
        RestoreGateInput in;
        in.target = RestoreGateTarget::LiveScene;
        in.gate = gate;
        in.documents.openScene = "assets/a.scene.json";
        in.documents.actor.known = true;
        in.targetPaths = { "assets/a.scene.json" };
        in.trackConnected = tracked;
        in.journalPending = journal;
        in.externalChangePending = external;
        return EvaluateRestoreGate(in);
    };
    auto has = [](const std::vector<RestoreBlock>& blocks, GateBlocker gate) {
        return std::any_of(blocks.begin(), blocks.end(), [&](const RestoreBlock& b) { return b.kind == RestoreBlockKind::Gate && b.gate == gate; });
    };
    auto hasKind = [](const std::vector<RestoreBlock>& blocks, RestoreBlockKind kind) {
        return std::any_of(blocks.begin(), blocks.end(), [&](const RestoreBlock& b) { return b.kind == kind; });
    };
    GateInputs idle;
    check(blocksFor(idle).empty(), "gate: nothing blocks an idle editor");
    GateInputs playing;
    playing.playing = true;
    GateInputs net;
    net.netActive = true;
    GateInputs actor;
    actor.actorEdit = true;
    check(has(blocksFor(playing), GateBlocker::Playing) && has(blocksFor(net), GateBlocker::NetActive) && has(blocksFor(actor), GateBlocker::ActorEdit),
          "gate: Play, a net session and mini-scene editing block a live scene restore");
    GateInputs benign;
    benign.buildRunning = true;
    benign.scriptBuildRunning = true;
    benign.sceneDirty = true;
    benign.opInFlight = true;
    benign.mergeInProgress = true;
    benign.animationDirty = true;
    benign.controllerDirty = true;
    check(blocksFor(benign).empty(), "gate: a build, an unsaved scene, git writes, a merge and other windows' unsaved documents do not block (no file is written)");
    check(hasKind(blocksFor(idle, false), RestoreBlockKind::TrackUnavailable) && hasKind(blocksFor(idle, true, true), RestoreBlockKind::JournalPending)
              && hasKind(blocksFor(idle, true, false, true), RestoreBlockKind::ExternalChangePending),
          "gate: a missing service, a pending restore and a pending external change block");
    GateInputs many = playing;
    many.netActive = true;
    many.actorEdit = true;
    const std::vector<RestoreBlock> all = blocksFor(many, false, true, true);
    check(all.size() == 6, "gate: every reason is listed together (Play, net, mini-scene, service, journal, external change)");
    // 文面
    InBothLanguages([&](Lang) {
        bool texts = true;
        for (const RestoreBlock& b : all) {
            texts = texts && !RestoreBlockText(b).empty();
        }
        RestoreBlock noScene;
        noScene.kind = RestoreBlockKind::NoScene;
        check(texts && !RestoreBlockText(noScene).empty(), "gate: every block has a text in this language");
    });
}

// ---- ダイアログ (偽のサービス) ----

struct DialogRig {
    fs::path root;
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    SceneRig scene;
    SceneRestoreDialog dialog;
    AiChangesHost host;
    GateInputs gate;
    bool externalPending = false;
    size_t answered = 0;
    std::vector<json> records;
    json detail;
    json items = json::array();
    bool answerRecords = true;
    int64_t clockMs = 1'000'000;

    explicit DialogRig(const std::string& name)
    {
        root = fs::temp_directory_path() / (L"mye_scene_restore_" + Utf8ToWide(name) + L"_" + std::to_wstring(_getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / L"assets", ec);
        std::ofstream(root / L"assets" / L"a.scene.json", std::ios::binary) << "{}";
        session.StartWithIo(std::make_unique<FakeIo>(s), root.wstring(), true);
        session.Poll();
        FeedJson(*s, OkReply(LastHelloId(*s), { { "proto", 1 }, { "projectId", ProjectId(root.wstring()) }, { "role", "editor" } }));
        session.Poll();
        session.Restore().SetClock([this]() { return clockMs; });
        host.allowTracking = true;
        host.enabled = true;
        host.projectRoot = root.wstring();
        host.gateInputs = [this]() { return gate; };
        host.editedDocuments = [this]() {
            EditedDocuments docs;
            docs.openScene = "assets/a.scene.json";
            docs.actor.known = true;
            return docs;
        };
        host.sceneRestoreEnv = [this]() { return scene.Env(); };
        host.restoreJournalPending = []() { return false; };
        detail = { { "revisionId", 5 }, { "path", "assets/a.scene.json" }, { "changeType", "modified" }, { "actor", "ai" }, { "attribution", "claimed" }, { "completeness", "complete" },
                   { "diffMode", "lines" }, { "changeState", "active" }, { "reviewable", true }, { "contentKind", "scene" },
                   { "sceneSummary", { { "documentId", kDoc }, { "resolvedDocumentId", kDoc }, { "counts", { { "FieldModified", 1 } } } } } };
    }
    ~DialogRig()
    {
        session.Shutdown();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void SetItems(const std::vector<TrackSceneChange>& rows)
    {
        items = json::array();
        for (const TrackSceneChange& r : rows) {
            json item = { { "sceneChangeId", r.sceneChangeId }, { "documentId", r.documentId }, { "resolvedDocumentId", r.resolvedDocumentId }, { "changeType", r.changeType }, { "oversize", false }, { "state", r.state },
                          { "review", "pending" } };
            item["fileId"] = r.hasFileId ? json(r.fileId) : json();
            item["component"] = r.component.empty() ? json() : json(r.component);
            item["field"] = r.field.empty() ? json() : json(r.field);
            item["beforeValue"] = r.hasBefore ? json(r.beforeValue) : json();
            item["afterValue"] = r.hasAfter ? json(r.afterValue) : json();
            items.push_back(item);
        }
    }

    void AnswerAll()
    {
        std::string batch;
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello) {
                continue;
            }
            json result = json::object();
            if (op == trackop::kDescribe) {
                result = detail;
            } else if (op == trackop::kSceneDiff) {
                result = { { "items", items }, { "nextCursor", nullptr } };
            } else if (op == trackop::kDocumentForPath) {
                result = { { "documentId", kDoc }, { "idSource", "meta" }, { "kind", "scene" }, { "known", true }, { "path", "assets/a.scene.json" } };
            } else if (op == trackop::kSceneOverlay) {
                result = { { "entities", json::array() }, { "documentChanges", json::array() }, { "latestRevisionId", 0 }, { "nextCursor", nullptr }, { "incomplete", false }, { "truncated", false },
                           { "approvedHidden", 0 }, { "unparsedRevisionIds", json::array() } };
            } else if (op == trackop::kRestoreRecord) {
                records.push_back(req["args"]);
                if (!answerRecords) {
                    batch += ErrReply(id, "internal").dump() + "\n";
                    continue;
                }
            } else if (op == trackop::kList) {
                result = { { "items", json::array() }, { "nextCursor", nullptr } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }
    void Round()
    {
        s->nowMs += 600;
        clockMs += 600;
        session.Poll();
        AnswerAll();
        session.Poll();
    }
    // 詳細・構造の変更・文書の ID が揃うまで
    bool Load(int64_t revisionId)
    {
        session.Overlay().SetDocumentPath((root / L"assets" / L"a.scene.json").wstring());
        session.Model().SetInterest(kTrackAllParts);
        session.Model().SelectRevision(revisionId);
        for (int i = 0; i < 60; ++i) {
            Round();
            if (session.Model().SceneChangesComplete() && session.Model().Detail().is_object() && !session.Overlay().DocumentId().empty()) {
                return true;
            }
        }
        return false;
    }
};

std::map<std::string, std::string> TreeHashes(const fs::path& root)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string key = WideToUtf8(fs::relative(it->path(), root, ec).generic_wstring());
        if (it->is_regular_file(ec)) {
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            out[key] = Sha256Hex(text.str());
        } else {
            out[key] = "<dir>";
        }
    }
    return out;
}

void RunDialogTests(const Check& check)
{
    DialogRig rig("dialog");
    SceneRig& scene = rig.scene;
    const uint64_t fid = scene.Make("Hero");
    scene.scene.GetWorld().AddComponent<CameraComponent>(scene.Id(fid))->fovYDeg = 90.0f;
    scene.scene.GetWorld().ApplyStructuralChanges();
    const std::vector<TrackSceneChange> rows = { Row(11, "FieldModified", static_cast<int64_t>(fid), "Camera", "fovYDeg", "60.0", "90.0"),
                                                 Row(12, "FieldModified", static_cast<int64_t>(fid), "Camera", "farZ", "1000.0", "500.0") };
    scene.Camera(fid)->farZ = 500.0f;
    rig.SetItems(rows);
    check(rig.Load(5), "dialog: the model got the detail, the scene changes and the open document");
    const TrackSceneChange anchor = rig.session.Model().SceneChanges().empty() ? TrackSceneChange() : rig.session.Model().SceneChanges()[0];
    check(rig.session.Model().SceneChanges().size() == 2 && anchor.state == "active", "dialog: scene_diff rows carry their state");

    const auto before = TreeHashes(rig.root);
    check(rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, anchor) && rig.dialog.IsOpen() && rig.dialog.RowCount() == 1, "dialog: the preview opens for one field");
    const SceneRestoreScreen& screen = rig.dialog.LastScreen();
    check(screen.hasPlan && screen.verdict == SceneRestoreVerdict::Ready && screen.canApply && screen.ops.size() == 1, "dialog: the preview is ready and can be applied");
    InBothLanguages([&](Lang) {
        rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, anchor);
        const SceneRestoreScreen& s2 = rig.dialog.LastScreen();
        bool notes = s2.notes.size() >= 2 && s2.notes[0] == Tr(StrId::AiChg_SrNoteUndo) && s2.notes[1] == Tr(StrId::AiChg_SrNoteUnsaved);
        check(notes && s2.ops.size() == 1 && s2.ops[0].find("Hero") != std::string::npos && s2.ops[0].find("fovYDeg") != std::string::npos && s2.ops[0].find("90") != std::string::npos
                  && s2.ops[0].find("60") != std::string::npos,
              "dialog: the preview says which entity's value changes from what to what, with the Undo and unsaved notes (both languages)");
    });
    rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Entity, anchor);
    check(rig.dialog.RowCount() == 2 && rig.dialog.LastScreen().ops.size() == 2, "dialog: the entity scope restores both fields");
    rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, anchor);

    // 確認のあとに Play を始めたら、実行の直前で止まり、何も書かない
    rig.gate.playing = true;
    const uint64_t hash = scene.Hash();
    rig.dialog.RefreshScreen(rig.session, rig.host);
    check(!rig.dialog.Blocks(rig.session, rig.host).empty() && !rig.dialog.LastScreen().canApply && !rig.dialog.LastScreen().blocks.empty(), "dialog: Play starting after the preview shows up as a block");
    check(!rig.dialog.Apply(rig.session, rig.host) && scene.Hash() == hash && !scene.undo.CanUndo() && rig.records.empty(), "dialog: applying while Play runs writes nothing and sends nothing");
    rig.gate.playing = false;
    // ミニシーンの編集・外部の変更の保留中・追跡の切断
    rig.gate.actorEdit = true;
    check(!rig.dialog.Blocks(rig.session, rig.host).empty(), "dialog: mini-scene editing blocks");
    rig.gate.actorEdit = false;
    rig.host.externalChangePending = true;
    check(!rig.dialog.Blocks(rig.session, rig.host).empty(), "dialog: a pending external change blocks");
    rig.host.externalChangePending = false;
    // ビルド中・未保存でも止まらない
    rig.gate.buildRunning = true;
    rig.gate.sceneDirty = true;
    check(rig.dialog.Blocks(rig.session, rig.host).empty(), "dialog: a build and an unsaved scene do not block");
    rig.gate.buildRunning = false;
    rig.gate.sceneDirty = false;

    // 実行: World は書き換わり、ファイルは書かない。ジャーナルは作らない。記録をサービスへ送る
    const int overlayFetchesBefore = rig.session.Overlay().FetchCount();
    check(rig.dialog.Apply(rig.session, rig.host) && rig.dialog.HasResult() && rig.dialog.Result().outcome == SceneRestoreOutcome::Committed && scene.Camera(fid)->fovYDeg == 60.0f && scene.Camera(fid)->farZ == 500.0f,
          "dialog: applying restores only the chosen field in the live scene");
    check(TreeHashes(rig.root) == before && !fs::exists(rig.root / L".mye" / L"ai_changes" / L"recovery"), "dialog: no file in the project changed and no recovery journal was made");
    check(scene.undo.CanUndo() && scene.undo.StateSerial() != 0, "dialog: the scene is dirty through the Undo stack");
    rig.Round();
    rig.Round();
    check(rig.records.size() == 1 && rig.records[0].value("state", std::string()) == "committed" && rig.records[0]["targets"][0].value("scope", std::string()) == "scene"
              && rig.records[0]["targets"][0]["sceneChangeIds"] == json::array({ 11 }) && rig.records[0]["targets"][0]["paths"] == json::array({ "assets/a.scene.json" })
              && rig.records[0]["targets"][0].value("revisionId", 0) == 5,
          "dialog: restore_record is sent with scope scene, the restored row ids and the scene path");
    check(rig.session.Overlay().DocumentFetchCount() >= 1 && rig.session.Overlay().FetchCount() > overlayFetchesBefore,
          "dialog: the overlay is fetched again when the record is accepted (the restored row drops out of the highlight)");
    InBothLanguages([&](Lang) {
        const SceneRestoreResultView view = BuildSceneRestoreResultView(rig.dialog.Result(), false);
        check(view.success && view.headline.find("Ctrl+Z") != std::string::npos, "dialog: the result says the restore can be undone");
    });
    // 履歴の行にシーンの復元が出る
    {
        InBothLanguages([&](Lang) {
            const std::vector<RestoreHistoryRow> history = BuildHistoryRows({ json{ { "restoreId", "s1" }, { "state", "committed" }, { "startedAt", 1700000000000ll },
                                                                                  { "targets", json::array({ { { "scope", "scene" }, { "sceneChangeIds", json::array({ 1, 2 }) }, { "paths", json::array({ "assets/a.scene.json" }) } } }) } } });
            check(history.size() == 1 && history[0].target.find("assets/a.scene.json") != std::string::npos && history[0].target.find("2") != std::string::npos,
                  "history: a scene restore shows its scene path and how many items it restored");
        });
    }
    // Undo すると値が戻る (記録は送らない)
    const size_t recordsBefore = rig.records.size();
    scene.undo.Undo(scene.scene, scene.selection);
    rig.Round();
    check(scene.Camera(fid)->fovYDeg == 90.0f && rig.records.size() == recordsBefore, "dialog: Undo brings the value back and sends no restore record");
    scene.undo.Redo(scene.scene, scene.selection);

    // 切断中は記録を溜め、つながったら送り直す
    rig.dialog.AcknowledgeResult();
    scene.Camera(fid)->fovYDeg = 90.0f;
    rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, anchor);
    rig.answerRecords = false;
    rig.dialog.Apply(rig.session, rig.host);
    rig.Round();
    rig.Round();
    check(rig.session.Restore().PendingRecordCount() >= 1, "dialog: a record the service rejected transiently stays queued");
    rig.answerRecords = true;
    rig.s->nowMs += 60000;
    rig.clockMs += 60000;
    for (int i = 0; i < 8; ++i) {
        rig.Round();
    }
    check(rig.session.Restore().PendingRecordCount() == 0, "dialog: the queued record is sent again and accepted");
    rig.dialog.AcknowledgeResult();

    // 開いていないシーンの行 (ほかの文書)
    {
        TrackSceneChange other = anchor;
        other.documentId = "docZ";
        other.resolvedDocumentId = "docZ";
        check(rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, other) && rig.dialog.NotOpen() && rig.dialog.LastScreen().notOpen && !rig.dialog.LastScreen().canApply,
              "dialog: a change of a scene that is not open cannot be applied item by item");
        InBothLanguages([&](Lang) {
            rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Field, other);
            check(rig.dialog.LastScreen().notOpenText.find(Lang::Ja == CurrentLanguage() ? "Hunk" : "hunk") != std::string::npos, "dialog: the not-open text points at the file / hunk restore (both languages)");
        });
        rig.dialog.Close();
    }
    // ヘッドレスの ImGui でプレビューと結果を描いても落ちない
    rig.dialog.Open(rig.session, rig.host, SceneRestoreScope::Entity, anchor);
    WithHeadlessImGuiFrame([&] { rig.dialog.OnImGui(rig.session, rig.host); });
    rig.dialog.Close();
    check(true, "dialog: the preview and result draw in a headless ImGui frame");
}

// ---- アセットの登録の後始末 ----

void RunAssetRegistryTests(const Check& check)
{
    const fs::path root = fs::temp_directory_path() / (L"mye_scene_restore_assets_" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"assets", ec);
    const fs::path scene = root / L"assets" / L"t.scene.json";
    std::ofstream(scene, std::ios::binary) << "{}";
    AssetDatabase db;
    db.ScanAndSync((root / L"assets").wstring());
    const size_t registered = db.Count();
    check(registered >= 1, "asset registry: the scene file is registered after a scan");
    fs::remove(scene, ec);
    fs::remove(root / L"assets" / L"t.scene.json.meta", ec);
    std::vector<BatchChange> deleted(1);
    deleted[0].path = scene.wstring();
    deleted[0].kind = BatchChange::Kind::Deleted;
    const int syncedDeleted = SyncAssetRegistry(db, root.wstring(), deleted);
    MYE_LOG_INFO("  info: asset registry synced=%d count=%zu registered=%zu root=%s path=%s", syncedDeleted, db.Count(), registered, WideToUtf8(NormalizePathKey(root.wstring())).c_str(),
                 WideToUtf8(NormalizePathKey(scene.wstring())).c_str());
    check(syncedDeleted == 1 && db.Count() == registered - 1, "asset registry: a deleted file is removed from the registration (the restore of an added file)");
    std::ofstream(scene, std::ios::binary) << "{}";
    std::vector<BatchChange> added(1);
    added[0].path = scene.wstring();
    added[0].kind = BatchChange::Kind::Added;
    check(SyncAssetRegistry(db, root.wstring(), added) == 1 && db.Count() == registered, "asset registry: a re-created file is registered again");
    std::vector<BatchChange> outside(1);
    outside[0].path = (root / L"other.txt").wstring();
    outside[0].kind = BatchChange::Kind::Deleted;
    check(SyncAssetRegistry(db, root.wstring(), outside) == 0, "asset registry: a file outside assets\\ is not touched");
    fs::remove_all(root, ec);
}

void RunModelTests(const Check& check)
{
    const json items = { { "items", json::array({ { { "sceneChangeId", 3 }, { "documentId", "d" }, { "changeType", "FieldModified" }, { "state", "restored" }, { "oversize", false } },
                                                   { { "sceneChangeId", 4 }, { "documentId", "d" }, { "changeType", "FieldModified" }, { "oversize", false } } }) } };
    const std::vector<TrackSceneChange> parsed = ChangeModel::ParseSceneChanges(items);
    check(parsed.size() == 2 && parsed[0].state == "restored" && parsed[1].state.empty(), "model: scene_diff rows carry the state the service reports");
}

// ---- 実サービスとの結合 (MYE_TRACK_PROBE=1 のとき ChangeTrackingSelfTest の probe から呼ばれる) ----

} // namespace

void RunSceneRestoreProbe(ChangeTrackingSession& session, const std::wstring& root, const std::function<bool(const std::function<bool()>&, int)>& pump, const Check& check)
{
    const fs::path scenePath = fs::path(root) / L"assets" / L"restore.scene.json";
    std::error_code ec;
    fs::create_directories(scenePath.parent_path(), ec);
    auto text = [](double fov) {
        std::ostringstream s;
        s << "{\"engine\":\"MyEngine\",\"entities\":[{\"childIndex\":0,\"components\":{\"Camera\":{\"fovYDeg\":" << fov
          << ",\"nearZ\":0.1,\"farZ\":1000.0,\"isPrimary\":true,\"orthographic\":false,\"orthoSize\":5.0}},\"fileId\":1,\"name\":\"Hero\"}],\"nextFileId\":2,\"sceneName\":\"s\",\"version\":4}";
        return s.str();
    };
    std::ofstream(scenePath, std::ios::binary) << text(60.0);
    session.Overlay().SetDocumentPath(scenePath.wstring());
    const bool created = pump([&] {
        const auto& rows = session.Model().Changes();
        return std::any_of(rows.begin(), rows.end(), [](const TrackChangeRow& r) { return r.path == "assets/restore.scene.json"; });
    }, 30000);
    check(created, "probe (scene restore): the scene file is recorded");
    // 外から書き換える (AI の編集に当たる: 行為者は不明)
    std::ofstream(scenePath, std::ios::binary | std::ios::trunc) << text(90.0);
    int64_t revisionId = 0;
    const bool modified = pump([&] {
        for (const TrackChangeRow& r : session.Model().Changes()) {
            if (r.path == "assets/restore.scene.json" && r.changeType == "modified") {
                revisionId = r.revisionId;
            }
        }
        return revisionId != 0;
    }, 30000);
    check(modified, "probe (scene restore): the external rewrite of the scene is recorded as a modified revision");
    if (!modified) {
        return;
    }
    session.Model().SelectRevision(revisionId);
    const bool loaded = pump([&] { return session.Model().SceneChangesComplete() && session.Model().SceneChanges().size() == 1 && !session.Overlay().DocumentId().empty(); }, 30000);
    check(loaded, "probe (scene restore): scene_diff and the document id reached the model");
    if (!loaded) {
        return;
    }
    // ライブのシーン: 書き換え後のファイルを読み込んだ状態
    SceneRig rig;
    const uint64_t fid = rig.Make("Hero");
    rig.scene.GetWorld().AddComponent<CameraComponent>(rig.Id(fid))->fovYDeg = 90.0f;
    rig.scene.GetWorld().ApplyStructuralChanges();
    SceneRestoreDialog dialog;
    AiChangesHost host;
    host.allowTracking = true;
    host.enabled = true;
    host.projectRoot = root;
    host.editedDocuments = [] {
        EditedDocuments docs;
        docs.openScene = "assets/restore.scene.json";
        docs.actor.known = true;
        return docs;
    };
    host.sceneRestoreEnv = [&]() { return rig.Env(); };
    host.restoreJournalPending = []() { return false; };
    const auto filesBefore = TreeHashes(fs::path(root) / L"assets");
    const TrackSceneChange anchor = session.Model().SceneChanges()[0];
    const bool opened = dialog.Open(session, host, SceneRestoreScope::Field, anchor);
    check(opened && dialog.LastScreen().verdict == SceneRestoreVerdict::Ready && dialog.LastScreen().canApply, "probe (scene restore): the live service's scene change is ready to restore in the open scene");
    const bool applied = opened && dialog.Apply(session, host);
    check(applied && dialog.Result().outcome == SceneRestoreOutcome::Committed && rig.Camera(fid)->fovYDeg == 60.0f, "probe (scene restore): the field went back to the recorded Before in the live scene");
    check(TreeHashes(fs::path(root) / L"assets") == filesBefore, "probe (scene restore): no project file was written by the scene restore");
    const bool restoredRow = pump([&] {
        const auto& rows = session.Model().SceneChanges();
        session.Model().Refresh();
        return !rows.empty() && rows[0].state == "restored";
    }, 30000);
    check(restoredRow && session.Restore().PendingRecordCount() == 0, "probe (scene restore): scene_changes.state became restored in the live service and the record was accepted");
    const bool overlayCleared = pump([&] { return session.Overlay().Counts().Total() == 0 && session.Overlay().Loaded(); }, 30000);
    check(overlayCleared, "probe (scene restore): the restored row no longer appears in scene_overlay (the highlight is gone)");
    rig.undo.Undo(rig.scene, rig.selection);
    check(rig.Camera(fid)->fovYDeg == 90.0f, "probe (scene restore): Undo puts the value back to the After");
    // 履歴には先に行ったファイルの復元もある。シーンの復元 (scope = scene) が出るまで取り直す
    const bool history = pump([&] {
        session.Restore().RequestHistory();
        for (const json& item : session.Restore().History()) {
            if (item.contains("targets") && item["targets"].is_array() && !item["targets"].empty() && item["targets"][0].value("scope", std::string()) == "scene"
                && item["targets"][0].contains("sceneChangeIds") && item.value("state", std::string()) == "committed") {
                return true;
            }
        }
        return false;
    }, 15000);
    check(history, "probe (scene restore): restore_list returns the scene restore (committed, with its sceneChangeIds)");
    session.Overlay().SetDocumentPath(std::wstring());
}

void RunSceneRestoreSelfTests(const std::function<void(bool, const char*)>& check)
{
    RunPureTests(check);
    RunApplyTests(check);
    RunPrefabTests(check);
    RunGateTests(check);
    RunDialogTests(check);
    RunAssetRegistryTests(check);
    RunModelTests(check);
    RunManagedTests(check);
}

} // namespace mye
