//====================================================================================
//                          NavEditorSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh Surface のエディタ側の回帰テスト実装
//====================================================================================
#include "Editor/Tools/NavEditorSelfTest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Editor/Scene/ComponentDependencies.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Tools/NavBakeCommit.h"
#include "Editor/Tools/NavBakeService.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Widgets/CreateMenu.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Navigation/NavBakeInput.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// ワーカーが終わるのを待つ (上限つき)。待ち切れなければ false
bool WaitReady(NavBakeService& service, uint64_t id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (service.GetState(id) != NavBakeJobState::Ready) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    std::vector<uint8_t> bytes;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return bytes;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
        return bytes;
    }
    bytes.resize(static_cast<size_t>(size));
    const size_t got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);
    return bytes;
}

} // namespace

bool RunNavEditorSelfTest()
{
    MYE_LOG_INFO("==== NavEditor (Create / Bake / Clear) self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_naveditor_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    Scene scene;
    EngineContext ctx;
    ctx.scene = &scene;
    ctx.assetsRoot = root.wstring(); // assetDb は null -> AssetDatabase::EnsureMeta へフォールバック
    UndoStack undo;
    Selection selection;
    World& world = scene.GetWorld();

    // ---- 1. Create -> 3D Object -> NavMesh Surface (Undo / Redo) ----
    {
        const GameObject created = RecordCreate(ctx, selection, undo, "Create NavMesh Surface",
                                                [&] { return CreateNavMeshSurface(ctx, "NavMesh Surface"); });
        world.ApplyStructuralChanges();
        const uint64_t fid = scene.EnsureFileId(created.Id());
        check(world.GetComponent<NavMeshSurfaceComponent>(created.Id()) != nullptr,
              "Create: the entity has a NavMeshSurface component");
        const NavMeshSurfaceComponent* c = world.GetComponent<NavMeshSurfaceComponent>(created.Id());
        check(c != nullptr && c->size.x == 20.0f && c->size.y == 10.0f && c->size.z == 20.0f && c->navAsset.IsNull(),
              "Create: the default range is 20 x 10 x 20 and it is not baked");
        undo.Undo(scene, selection);
        world.ApplyStructuralChanges();
        check(!scene.FindByFileId(fid), "Undo: the created Surface is removed");
        undo.Redo(scene, selection);
        world.ApplyStructuralChanges();
        {
            GameObject back = scene.FindByFileId(fid);
            check(static_cast<bool>(back) && world.GetComponent<NavMeshSurfaceComponent>(back.Id()) != nullptr,
                  "Redo: the Surface and its component come back");
            if (back) {
                back.Destroy();
                world.ApplyStructuralChanges();
            }
        }
        undo.ClearAll();
    }

    // ---- 1a. Create -> 3D Object -> NavMesh Obstacle (Undo / Redo、M82f) ----
    {
        const GameObject created = RecordCreate(ctx, selection, undo, "Create NavMesh Obstacle",
                                                [&] { return CreateNavMeshObstacle(ctx, "NavMesh Obstacle"); });
        world.ApplyStructuralChanges();
        const uint64_t fid = scene.EnsureFileId(created.Id());
        const NavMeshObstacleComponent* c = world.GetComponent<NavMeshObstacleComponent>(created.Id());
        check(c != nullptr && c->carve && c->shape == navobstacleshape::kBox && c->size.x == 1.0f,
              "Create: the entity has a NavMeshObstacle component (a 1 m box that carves)");
        undo.Undo(scene, selection);
        world.ApplyStructuralChanges();
        check(!scene.FindByFileId(fid), "Undo: the created Obstacle is removed");
        undo.Redo(scene, selection);
        world.ApplyStructuralChanges();
        {
            GameObject back = scene.FindByFileId(fid);
            check(static_cast<bool>(back) && world.GetComponent<NavMeshObstacleComponent>(back.Id()) != nullptr,
                  "Redo: the Obstacle and its component come back");
            if (back) {
                back.Destroy();
                world.ApplyStructuralChanges();
            }
        }
        undo.ClearAll();
    }

    // ---- 1b. Add Component: NavMeshAgent を足すと CharacterController も同じ 1 Undo で付く (M82c) ----
    {
        GameObject target = scene.CreateGameObjectTracked("AgentTarget");
        world.ApplyStructuralChanges();
        const uint64_t targetFid = scene.EnsureFileId(target.Id());
        undo.BeginRecord("Add Component", selection);
        undo.CaptureBefore(scene, targetFid);
        AddComponentWithRequirements(world, target.Id(), NavMeshAgentComponent::sTypeId);
        world.ApplyStructuralChanges();
        undo.CaptureAfter(scene, targetFid);
        undo.EndRecord(selection);
        check(world.GetComponent<NavMeshAgentComponent>(target.Id()) != nullptr
                  && world.GetComponent<CharacterControllerComponent>(target.Id()) != nullptr,
              "Add Component: NavMeshAgent also adds the CharacterController it needs");
        undo.Undo(scene, selection);
        world.ApplyStructuralChanges();
        check(world.GetComponent<NavMeshAgentComponent>(target.Id()) == nullptr
                  && world.GetComponent<CharacterControllerComponent>(target.Id()) == nullptr,
              "Undo: one Undo removes both the agent and the controller");
        undo.Redo(scene, selection);
        world.ApplyStructuralChanges();
        check(world.GetComponent<NavMeshAgentComponent>(target.Id()) != nullptr
                  && world.GetComponent<CharacterControllerComponent>(target.Id()) != nullptr,
              "Redo: both come back");
        // 既に CC を持つエンティティの CC は重複させず、そのまま残す
        GameObject withCc = scene.CreateGameObjectTracked("AgentWithController");
        withCc.AddComponent<CharacterControllerComponent>()->radius = 0.45f;
        world.ApplyStructuralChanges();
        AddComponentWithRequirements(world, withCc.Id(), NavMeshAgentComponent::sTypeId);
        world.ApplyStructuralChanges();
        const auto* keptCc = world.GetComponent<CharacterControllerComponent>(withCc.Id());
        check(keptCc != nullptr && keptCc->radius == 0.45f && world.GetComponent<NavMeshAgentComponent>(withCc.Id()) != nullptr,
              "Add Component: an existing CharacterController is kept as is");
        target.Destroy();
        withCc.Destroy();
        world.ApplyStructuralChanges();
        undo.ClearAll();
    }

    // ---- 1b. インスペクタの警告の判定 (M82d): 段差と坂の設定が効かない組み合わせ ----
    {
        CharacterControllerComponent cc;
        NavMeshSurfaceComponent sf;
        check(!NavAgentStepBelowClimb(cc, 1.0f, sf), "warning: default CharacterController.stepOffset (0.3) covers the default Max Climb (0.3)");
        cc.stepOffset = 0.1f;
        check(NavAgentStepBelowClimb(cc, 1.0f, sf), "warning: stepOffset below Max Climb is reported");
        check(!NavAgentStepBelowClimb(cc, 3.0f, sf), "warning: the effective step is stepOffset x scale.y (0.1 x 3 covers 0.3)");
        cc.stepOffset = 0.3f;
        check(NavAgentStepBelowClimb(cc, 0.5f, sf), "warning: a half-height character cannot climb 0.3 (effective 0.15)");
        cc.stepOffset = 0.0f;
        check(NavAgentStepBelowClimb(cc, 1.0f, sf), "warning: stepOffset 0 is reported");
        sf.maxClimb = 0.0f;
        check(!NavAgentStepBelowClimb(cc, 1.0f, sf), "warning: Max Climb 0 needs no step");

        NavMeshSurfaceComponent slope;
        check(!NavSurfaceSlopeUnreachable(slope), "warning: default Surface settings reach their own max slope");
        slope.maxSlopeDeg = 60.0f;
        check(!NavSurfaceSlopeUnreachable(slope), "warning: auto cell size follows a steeper Max Slope");
        slope.autoCellSize = false;
        check(NavSurfaceSlopeUnreachable(slope), "warning: manual cell size 0.3 cannot reach 60 degrees");
        slope.cellSize = 0.05f;
        check(!NavSurfaceSlopeUnreachable(slope), "warning: a fine manual cell size reaches 60 degrees again");
        slope.autoCellSize = true;
        slope.maxSlopeDeg = 85.0f;
        slope.maxClimb = 0.1f;
        check(NavSurfaceSlopeUnreachable(slope) && NavResolveCellSize(slope).clampedToMinimum,
              "warning: a cell size clamped to the minimum cannot reach 85 degrees");
    }

    // ---- 2. Bake -> Commit -> Undo / Redo -> Clear ----
    GameObject ground = scene.CreateGameObjectTracked("Ground");
    ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
    {
        auto* col = ground.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 9.5f, 0.5f, 9.5f };
    }
    GameObject block = scene.CreateGameObjectTracked("Block");
    block.SetLocalPosition(3.0f, 1.0f, 0.0f);
    {
        auto* col = block.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 1.0f, 1.0f, 1.0f };
    }
    GameObject surface = scene.CreateGameObjectTracked("Level Surface");
    {
        auto* s = surface.AddComponent<NavMeshSurfaceComponent>();
        s->center = { 0.0f, 1.0f, 0.0f };
        s->size = { 20.0f, 6.0f, 20.0f };
        s->autoCellSize = false; // バイト一致の検査が目的なので軽い粗いセルのまま
        s->cellSize = 0.5f;
        s->cellHeight = 0.2f;
    }
    world.ApplyStructuralChanges();
    TransformSystem transforms;
    transforms.Update(world);
    const uint64_t fid = scene.EnsureFileId(surface.Id());

    NavBakeInputs inputs;
    check(NavPrepareBakeInputs(world, surface.Id(), inputs), "inputs: collected on the main thread");
    NavBakeOutput direct = NavBakeAsset(inputs.config, inputs.soup, nullptr);
    std::vector<uint8_t> directBytes;
    NavMeshAsset::Serialize(direct.data, directBytes);

    NavBakeService service;
    NavBakeInputs inputsForWorker;
    NavPrepareBakeInputs(world, surface.Id(), inputsForWorker);
    service.Request(fid, std::move(inputsForWorker));
    check(service.GetState(fid) != NavBakeJobState::None, "service: a request is accepted");
    check(WaitReady(service, fid), "service: the worker finishes");
    NavBakeResult result;
    check(service.TakeResult(fid, result) && service.GetState(fid) == NavBakeJobState::None,
          "service: TakeResult hands over the result and the job returns to None");
    check(result.output.status == NavBakeStatus::Ok, "service: the bake succeeds");
    std::vector<uint8_t> workerBytes;
    NavMeshAsset::Serialize(result.output.data, workerBytes);
    check(workerBytes == directBytes, "service: the worker's bytes equal calling the Engine bake function directly");

    // 取り消し: 打ち切るか、間に合って完走するか。どちらでも Ready になり、取り出せる
    {
        NavBakeInputs again;
        NavPrepareBakeInputs(world, surface.Id(), again);
        service.Request(fid, std::move(again));
        service.Cancel(fid);
        check(WaitReady(service, fid), "cancel: the worker still reaches Ready in finite time");
        NavBakeResult cancelled;
        check(service.TakeResult(fid, cancelled)
                  && (cancelled.output.status == NavBakeStatus::Cancelled || cancelled.output.status == NavBakeStatus::Ok),
              "cancel: the result is Cancelled (or Ok if the bake was already done)");
    }

    const bool committed = CommitNavBake(ctx, selection, undo, surface.Id(), fid, result.output);
    check(committed, "commit: writes the .mnav and sets the reference");
    {
        const auto* s = world.GetComponent<NavMeshSurfaceComponent>(surface.Id());
        check(s != nullptr && !s->navAsset.IsNull(), "commit: Surface.navAsset is set");
        const std::wstring path = NavBakeAssetPath(ctx.assetsRoot, "Level Surface", result.output.data.inputHash);
        check(fs::exists(path), "commit: the file lands at assets\\NavMesh\\<name>_<input hash>.mnav");
        check(ReadFileBytes(path) == workerBytes, "commit: the file is the bake's bytes as is");
        NavMeshAsset::Data loaded;
        check(NavMeshAsset::Load(path, loaded), "commit: the saved .mnav loads");
        check(fs::exists(path + L".meta"), "commit: a .meta is written beside it (GUID reference)");
    }
    undo.Undo(scene, selection);
    {
        const auto* s = world.GetComponent<NavMeshSurfaceComponent>(surface.Id());
        check(s != nullptr && s->navAsset.IsNull(), "undo: one Undo resets navAsset to null");
    }
    undo.Redo(scene, selection);
    {
        const auto* s = world.GetComponent<NavMeshSurfaceComponent>(surface.Id());
        check(s != nullptr && !s->navAsset.IsNull(), "redo: restores the reference");
    }
    const std::wstring savedPath = NavBakeAssetPath(ctx.assetsRoot, "Level Surface", result.output.data.inputHash);
    check(ClearNavBake(ctx, selection, undo, surface.Id(), fid), "clear: drops the reference");
    {
        const auto* s = world.GetComponent<NavMeshSurfaceComponent>(surface.Id());
        check(s != nullptr && s->navAsset.IsNull(), "clear: navAsset is null");
        check(fs::exists(savedPath), "clear: the .mnav file itself is not deleted");
    }
    undo.Undo(scene, selection);
    {
        const auto* s = world.GetComponent<NavMeshSurfaceComponent>(surface.Id());
        check(s != nullptr && !s->navAsset.IsNull(), "clear: one Undo brings the reference back");
    }
    check(!ClearNavBake(ctx, selection, undo, kNullEntity, 0), "clear: an invalid entity is refused");
    {
        NavBakeOutput failed;
        failed.status = NavBakeStatus::Failed;
        check(!CommitNavBake(ctx, selection, undo, surface.Id(), fid, failed), "commit: a failed bake is refused");
    }

    service.Shutdown();
    fs::remove_all(root, ec);
    if (failCount == 0) {
        MYE_LOG_INFO("NavEditor self test: ALL PASS");
    } else {
        MYE_LOG_ERROR("NavEditor self test: %d FAILED", failCount);
    }
    return failCount == 0;
}

} // namespace mye
