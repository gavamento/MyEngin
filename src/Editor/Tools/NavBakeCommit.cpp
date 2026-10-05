//====================================================================================
//                          NavBakeCommit.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ベイク結果の確定の実装 (M82b)
//====================================================================================
#include "Editor/Tools/NavBakeCommit.h"

#include <cstdio>
#include <vector>

#include "Editor/Asset/AssetOps.h" // SanitizeFileName
#include "Editor/Project/NavAgentTypes.h"
#include "Editor/Scene/Selection.h"
#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Loop/EngineLoop.h" // EngineContext
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// surface のグループ (同じ agentTypeId の有効な Surface、M84b) の全 Surface。グループに入らない (無効な) Surface は自分だけ
std::vector<EntityID> GroupMembers(World& world, EntityID surface)
{
    NavSurfaceGroup group;
    if (!NavFindSurfaceGroup(world, surface, group)) {
        return { surface };
    }
    return group.members;
}

} // namespace

std::wstring NavBakeAssetPath(const std::wstring& assetsRoot, const std::string& surfaceName, uint64_t inputHash)
{
    char hashHex[17];
    std::snprintf(hashHex, sizeof(hashHex), "%016llx", static_cast<unsigned long long>(inputHash));
    return assetsRoot + L"\\NavMesh\\" + Utf8ToWide(SanitizeFileName(surfaceName, "NavMesh")) + L"_"
        + Utf8ToWide(hashHex) + NavMeshAsset::kNavExt;
}

bool CommitNavBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID surface, uint64_t fid,
                   const NavBakeOutput& output)
{
    World& world = ctx.scene->GetWorld();
    // BeginRecord は進行中の記録を捨てる。ドラッグ中は何も書かず、呼び出し側が次のフレームで再試行する
    if (undo.IsRecording()) {
        return false;
    }
    if (output.status != NavBakeStatus::Ok || !world.IsAlive(surface)) {
        return false;
    }
    auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface);
    if (comp == nullptr) {
        return false; // ベイク中に Surface が外された
    }
    // ファイル名は Agent Type の名前 (グループ全体の .mnav なので、押した Surface の名前より分かりやすい)
    NavAgentTypes& types = NavAgentTypes::Get();
    types.Load(ctx.assetsRoot);
    const NavAgentType* type = types.Find(comp->agentTypeId);
    const std::wstring path = NavBakeAssetPath(ctx.assetsRoot, type != nullptr ? type->name : world.GetName(surface),
                                               output.data.inputHash);
    if (!NavMeshAsset::Save(path, output.data)) {
        return false;
    }
    // .meta を確定させてから GUID を引く (先に確定させないと path-hash に落ち、ファイルを移動しただけで参照が壊れる)
    const uint64_t guid = ctx.assetDb != nullptr ? ctx.assetDb->GuidForPath(path, /*createIfMissing=*/true)
                                                  : AssetDatabase::EnsureMeta(path);
    scmhint::Changed(path);

    // グループの全 Surface が同じ .mnav を指す (1 Undo)
    const std::vector<EntityID> members = GroupMembers(world, surface);
    std::vector<uint64_t> fids;
    for (const EntityID member : members) {
        fids.push_back(member == surface ? fid : ctx.scene->EnsureFileId(member));
    }
    undo.Record("Bake NavMesh", *ctx.scene, selection, fids, UndoStack::StructuralChanges::None, [&] {
        for (const EntityID member : members) {
            world.GetComponent<NavMeshSurfaceComponent>(member)->navAsset = AssetID{ guid };
        }
    });
    MYE_LOG_INFO("[nav] baked '%s': %zu layer(s) -> %s", world.GetName(surface), output.data.layers.size(),
                 WideToUtf8(path).c_str());
    return true;
}

bool ClearNavBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID surface, uint64_t fid)
{
    World& world = ctx.scene->GetWorld();
    if (undo.IsRecording() || !world.IsAlive(surface)) { // 記録中は進行中の記録を壊すので何もしない
        return false;
    }
    if (world.GetComponent<NavMeshSurfaceComponent>(surface) == nullptr) {
        return false;
    }
    // グループの全 Surface の参照を外す (1 Undo)
    const std::vector<EntityID> members = GroupMembers(world, surface);
    bool any = false;
    std::vector<uint64_t> fids;
    for (const EntityID member : members) {
        any = any || !world.GetComponent<NavMeshSurfaceComponent>(member)->navAsset.IsNull();
        fids.push_back(member == surface ? fid : ctx.scene->EnsureFileId(member));
    }
    if (!any) {
        return false;
    }
    undo.Record("Clear NavMesh", *ctx.scene, selection, fids, UndoStack::StructuralChanges::None, [&] {
        for (const EntityID member : members) {
            world.GetComponent<NavMeshSurfaceComponent>(member)->navAsset = AssetID{};
        }
    });
    return true;
}

} // namespace mye
