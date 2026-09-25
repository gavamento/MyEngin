//====================================================================================
//                          FractureBakeCommit.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          焼き成功結果の確定の実装 (M80i)
//====================================================================================
#include "Editor/FractureBakeCommit.h"

#include "Editor/AssetOps.h"       // SanitizeFileName
#include "Editor/Selection.h"
#include "Editor/SourceControl/ScmHint.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h" // DestructibleComponent
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EngineLoop.h" // EngineContext
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

bool CommitFractureBake(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID root,
                        uint64_t fid, const FractureBakeRequest& request,
                        const FractureBakeResult& result,
                        const std::vector<std::string>& pieceBoneNames)
{
    World& world = ctx.scene->GetWorld();
    if (!world.IsAlive(root)) {
        return false;
    }
    auto* comp = world.GetComponent<DestructibleComponent>(root);
    if (comp == nullptr) {
        return false; // 焼き中に Destructible が外された (稀)
    }

    // 保存先 (spec §4.3): assets\Fracture\<エンティティ名>_<seed>_<pieceCount>.mfrac。
    // 同じ設定 (同じ名前・seed・pieceCount) の再生成は上書き確認なしで上書きする
    const std::string safeName = SanitizeFileName(world.GetName(root), "Fracture");
    const std::wstring path = ctx.assetsRoot + L"\\Fracture\\" + Utf8ToWide(safeName) + L"_"
                             + std::to_wstring(request.seed) + L"_"
                             + std::to_wstring(request.pieceCount) + FractureAsset::kFractureExt;
    const FractureAsset::FractureData data
        = BuildFractureAssetData(result, request.sourceMeshHash, request.seed, request.pieceCount,
                                request.openMeshMode, request.voxelResolution, pieceBoneNames);
    if (!FractureAsset::Save(path, data)) {
        MYE_LOG_ERROR("[fracture] failed to write %s", WideToUtf8(path).c_str());
        return false;
    }
    // .meta を確定させてから登録する (先に確定させないと登録名が path-hash に落ちて、
    // ファイルを移動しただけで Destructible.fractureAsset の参照が壊れる)
    if (ctx.assetDb != nullptr) {
        ctx.assetDb->GuidForPath(path, /*createIfMissing=*/true);
    } else {
        AssetDatabase::EnsureMeta(path);
    }
    scmhint::Changed(path);

    FractureLibrary* lib = fracturelib::Library();
    const FractureAssetHandle* handle = lib != nullptr ? lib->LoadFromFile(path) : nullptr;
    if (handle == nullptr) {
        MYE_LOG_ERROR("[fracture] failed to register %s right after writing it",
                     WideToUtf8(path).c_str());
        return false;
    }

    undo.BeginRecord("Generate Fracture Pieces", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    comp->fractureAsset = AssetID{ HashStr(handle->namePrefix) };
    BuildFracturePieces(world, root, *handle); // 内部で ApplyStructuralChanges 済み
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    return true;
}

} // namespace mye
