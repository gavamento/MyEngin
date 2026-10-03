//====================================================================================
//                          NavBakeCommit.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ベイク結果の確定の実装 (M82b)
//====================================================================================
#include "Editor/Tools/NavBakeCommit.h"

#include <cstdio>

#include "Editor/Asset/AssetOps.h" // SanitizeFileName
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
    if (output.status != NavBakeStatus::Ok || !world.IsAlive(surface)) {
        return false;
    }
    auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface);
    if (comp == nullptr) {
        return false; // ベイク中に Surface が外された
    }
    const std::wstring path = NavBakeAssetPath(ctx.assetsRoot, world.GetName(surface), output.data.inputHash);
    if (!NavMeshAsset::Save(path, output.data)) {
        return false;
    }
    // .meta を確定させてから GUID を引く (先に確定させないと path-hash に落ち、ファイルを移動しただけで参照が壊れる)
    const uint64_t guid = ctx.assetDb != nullptr ? ctx.assetDb->GuidForPath(path, /*createIfMissing=*/true)
                                                  : AssetDatabase::EnsureMeta(path);
    scmhint::Changed(path);

    undo.BeginRecord("Bake NavMesh", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    comp->navAsset = AssetID{ guid };
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    MYE_LOG_INFO("[nav] baked '%s': %zu layer(s) -> %s", world.GetName(surface), output.data.layers.size(),
                 WideToUtf8(path).c_str());
    return true;
}

bool ClearNavBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID surface, uint64_t fid)
{
    World& world = ctx.scene->GetWorld();
    if (!world.IsAlive(surface)) {
        return false;
    }
    auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface);
    if (comp == nullptr || comp->navAsset.IsNull()) {
        return false;
    }
    undo.BeginRecord("Clear NavMesh", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    comp->navAsset = AssetID{};
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    return true;
}

} // namespace mye
