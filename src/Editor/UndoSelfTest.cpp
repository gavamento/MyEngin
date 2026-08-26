#include "Editor/UndoSelfTest.h"

#include "Editor/ComponentClipboard.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {
namespace {

uint64_t FidOf(Scene& scene, EntityID e)
{
    auto* f = scene.GetWorld().GetComponent<FileIdComponent>(e);
    return f ? f->value : 0;
}

// 1 エンティティに対する modify op を 1 Undo エントリとして記録するヘルパ
template <typename MutateFn>
void RecordModify(UndoStack& undo, Scene& scene, Selection& sel, const char* label, uint64_t fid,
                  MutateFn&& mutate)
{
    undo.BeginRecord(label, sel);
    undo.CaptureBefore(scene, fid);
    mutate();
    scene.GetWorld().ApplyStructuralChanges();
    undo.CaptureAfter(scene, fid);
    undo.EndRecord(sel);
}

} // namespace

bool RunUndoSelfTest()
{
    MYE_LOG_INFO("==== Undo self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ============ Phase 1: 生成/破棄を含まない op の完全可逆性 (WorldHash) ============
    {
        Scene scene;
        World& world = scene.GetWorld();
        UndoStack undo;
        Selection sel;

        GameObject a = scene.CreateGameObjectTracked("A");
        GameObject b = scene.CreateGameObjectTracked("B");
        GameObject c = scene.CreateGameObjectTracked("C");
        a.SetLocalPosition(1.0f, 2.0f, 3.0f);
        b.SetLocalPosition(-4.0f, 5.0f, 6.0f);
        c.SetLocalPosition(7.0f, 8.0f, 9.0f);
        world.ApplyStructuralChanges();

        const uint64_t aFid = FidOf(scene, a.Id());
        const uint64_t bFid = FidOf(scene, b.Id());
        const uint64_t cFid = FidOf(scene, c.Id());
        const uint64_t initialHash = HashWorld(world);

        // op1: A の位置を変更
        RecordModify(undo, scene, sel, "move A", aFid, [&] {
            scene.FindByFileId(aFid).GetComponent<LocalTransform>()->position.x = 100.0f;
        });
        // op2: B に Collider を追加
        RecordModify(undo, scene, sel, "add collider", bFid, [&] {
            world.AddComponentRaw(scene.FindByFileId(bFid).Id(), ColliderComponent::sTypeId);
        });
        // op3: C を A の子にする (親リンク変更 = ハッシュ対象)
        RecordModify(undo, scene, sel, "reparent C", cFid,
                     [&] { world.SetParent(scene.FindByFileId(cFid).Id(),
                                           scene.FindByFileId(aFid).Id()); });
        // op4: A に Camera を追加してフィールド編集
        RecordModify(undo, scene, sel, "add camera", aFid, [&] {
            auto* cam = static_cast<CameraComponent*>(
                world.AddComponentRaw(scene.FindByFileId(aFid).Id(), CameraComponent::sTypeId));
            cam->fovYDeg = 33.0f;
        });

        const uint64_t editedHash = HashWorld(world);
        check(editedHash != initialHash, "edits changed the world hash");

        int undoCount = 0;
        while (undo.CanUndo()) {
            undo.Undo(scene, sel);
            ++undoCount;
        }
        check(undoCount == 4, "4 ops undone");
        check(HashWorld(world) == initialHash, "undo-all restores initial world hash");

        int redoCount = 0;
        while (undo.CanRedo()) {
            undo.Redo(scene, sel);
            ++redoCount;
        }
        check(redoCount == 4, "4 ops redone");
        check(HashWorld(world) == editedHash, "redo-all restores edited world hash");
    }

    // ============ Phase 2: 生成 / 破棄の undo/redo (構造・値で検証) ============
    {
        Scene scene;
        World& world = scene.GetWorld();
        UndoStack undo;
        Selection sel;

        GameObject keep = scene.CreateGameObjectTracked("Keep");
        keep.SetLocalPosition(1.0f, 1.0f, 1.0f);
        world.ApplyStructuralChanges();
        const uint32_t baseCount = world.AliveCount();

        // ---- create ----
        undo.BeginRecord("create", sel);
        GameObject created = scene.CreateGameObjectTracked("Created");
        created.SetLocalPosition(3.0f, 4.0f, 5.0f);
        created.AddComponent<MeshRendererComponent>()->mesh = AssetID{ 0x77ull };
        world.ApplyStructuralChanges();
        const uint64_t createdFid = FidOf(scene, created.Id());
        undo.CaptureAfter(scene, createdFid);
        undo.EndRecord(sel);
        check(world.AliveCount() == baseCount + 1, "create: entity added");

        undo.Undo(scene, sel);
        check(!static_cast<bool>(scene.FindByFileId(createdFid)), "undo create: entity removed");
        check(world.AliveCount() == baseCount, "undo create: alive count restored");

        undo.Redo(scene, sel);
        GameObject recreated = scene.FindByFileId(createdFid);
        bool recreatedOk = static_cast<bool>(recreated);
        if (recreatedOk) {
            auto* lt = recreated.GetComponent<LocalTransform>();
            auto* mr = recreated.GetComponent<MeshRendererComponent>();
            recreatedOk = lt && lt->position.y == 4.0f && mr && mr->mesh.value == 0x77ull;
        }
        check(recreatedOk, "redo create: entity + components restored");

        // ---- destroy (サブツリー: Keep に子を付けてから Keep を破棄) ----
        undo.BeginRecord("attach child", sel);
        undo.CaptureBefore(scene, FidOf(scene, keep.Id()));
        GameObject child = scene.CreateGameObjectTracked("KeepChild");
        child.SetParent(keep);
        world.ApplyStructuralChanges();
        undo.CaptureAfter(scene, FidOf(scene, keep.Id()));
        undo.EndRecord(sel);

        const uint64_t keepFid = FidOf(scene, keep.Id());
        const uint64_t childFid = FidOf(scene, child.Id());

        undo.BeginRecord("destroy Keep", sel);
        undo.CaptureBefore(scene, keepFid); // サブツリー丸ごと
        world.DestroyEntity(keep.Id());
        world.ApplyStructuralChanges();
        undo.EndRecord(sel);
        check(!static_cast<bool>(scene.FindByFileId(keepFid)), "destroy: subtree removed");
        check(!static_cast<bool>(scene.FindByFileId(childFid)), "destroy: child removed");

        undo.Undo(scene, sel);
        GameObject keepBack = scene.FindByFileId(keepFid);
        GameObject childBack = scene.FindByFileId(childFid);
        bool restoreOk = static_cast<bool>(keepBack) && static_cast<bool>(childBack);
        if (restoreOk) {
            auto* lt = keepBack.GetComponent<LocalTransform>();
            restoreOk = lt && lt->position.x == 1.0f
                && world.GetParent(childBack.Id()) == keepBack.Id();
        }
        check(restoreOk, "undo destroy: subtree + parent link + values restored");
    }

    // ============ Phase 4: StateSerial (ダーティ判定、M27b) ============
    {
        Scene scene;
        UndoStack undo;
        Selection sel;
        GameObject a = scene.CreateGameObjectTracked("A");
        scene.GetWorld().ApplyStructuralChanges();
        const uint64_t fid = FidOf(scene, a.Id());

        const uint64_t savedAt = undo.StateSerial(); // 「保存した」時点
        RecordModify(undo, scene, sel, "Move", fid,
                     [&] { a.SetLocalPosition(9.0f, 0.0f, 0.0f); });
        check(undo.StateSerial() != savedAt, "StateSerial: edit makes state differ (dirty)");

        undo.Undo(scene, sel);
        check(undo.StateSerial() == savedAt, "StateSerial: undo back to saved state (clean)");

        undo.Redo(scene, sel);
        check(undo.StateSerial() != savedAt, "StateSerial: redo makes state differ again");

        const uint64_t beforeClear = undo.StateSerial();
        undo.ClearAll();
        check(undo.StateSerial() != beforeClear && undo.StateSerial() != savedAt,
              "StateSerial: ClearAll starts a fresh base (never reuses old serials)");
    }

    // ============ Phase 4 (M40a): マルチ選択バッチ編集 = 1 Undo エントリ ============
    {
        Scene scene;
        World& world = scene.GetWorld();
        UndoStack undo;
        Selection sel;

        GameObject a = scene.CreateGameObjectTracked("A");
        GameObject b = scene.CreateGameObjectTracked("B");
        a.SetLocalPosition(1.0f, 0.0f, 0.0f);
        b.SetLocalPosition(2.0f, 0.0f, 0.0f);
        world.ApplyStructuralChanges();
        const uint64_t aFid = FidOf(scene, a.Id());
        const uint64_t bFid = FidOf(scene, b.Id());
        const uint64_t initialHash = HashWorld(world);

        // Inspector のバッチ編集と同じ手順: 全対象 CaptureBefore → 編集 → 全対象 CaptureAfter
        undo.BeginRecord("Batch Modify", sel);
        undo.CaptureBefore(scene, aFid);
        undo.CaptureBefore(scene, bFid);
        scene.FindByFileId(aFid).GetComponent<LocalTransform>()->position.y = 5.0f;
        scene.FindByFileId(bFid).GetComponent<LocalTransform>()->position.y = 5.0f;
        undo.CaptureAfter(scene, aFid);
        undo.CaptureAfter(scene, bFid);
        undo.EndRecord(sel);
        const uint64_t editedHash = HashWorld(world);
        check(editedHash != initialHash, "batch: edits changed the world hash");

        undo.Undo(scene, sel);
        check(HashWorld(world) == initialHash,
              "batch: single undo restores BOTH entities (one entry)");
        check(!undo.CanUndo(), "batch: exactly one undo entry was recorded");

        undo.Redo(scene, sel);
        check(HashWorld(world) == editedHash, "batch: redo re-applies both");
        check(scene.FindByFileId(aFid).GetComponent<LocalTransform>()->position.y == 5.0f
                  && scene.FindByFileId(bFid).GetComponent<LocalTransform>()->position.y == 5.0f,
              "batch: redo values on both entities");
    }

    // ============ Phase 5 (M40a): コンポーネント copy/paste round-trip ============
    {
        const ComponentRegistry& reg = ComponentRegistry::Get();
        const ComponentDesc& desc = reg.Desc(ColliderComponent::sTypeId);

        ColliderComponent src{};
        src.shape = 1;
        src.radius = 3.5f;
        src.layer = 4;
        src.mask = 0x0000000Fu;
        const nlohmann::json j = ComponentFieldsToJson(desc, &src);

        ColliderComponent dst{};
        ComponentFieldsFromJson(desc, &dst, j);
        check(dst.shape == 1 && dst.radius == 3.5f && dst.layer == 4 && dst.mask == 0x0000000Fu,
              "clipboard: component fields round-trip");

        // 欠落キーは現在値維持 (旧クリップボード/型違いに寛容)
        ColliderComponent partialDst{};
        partialDst.radius = 9.0f;
        nlohmann::json partial = nlohmann::json::object();
        partial["layer"] = 7;
        ComponentFieldsFromJson(desc, &partialDst, partial);
        check(partialDst.layer == 7 && partialDst.radius == 9.0f,
              "clipboard: missing keys keep current values");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Undo self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Undo self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
