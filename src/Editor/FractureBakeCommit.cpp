//====================================================================================
//                          FractureBakeCommit.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          焼き成功結果の確定の実装 (M80i)
//====================================================================================
#include "Editor/FractureBakeCommit.h"

#include <cstdio>

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
#include "Engine/Engine/Physics/FractureBake.h" // kFractureBakeVersion
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// 保存名を焼きの入力 (ソースメッシュ・分割パラメータ・焼き方式の版) から決める 16hex。
// 入力が同じなら同じ値、違えば別の値になる (名前が同じ別のエンティティでの衝突を防ぐ)
uint64_t ComputeFractureBakeInputHash(const FractureBakeRequest& request)
{
    uint64_t h = kFnvOffset;
    h = HashBytes(request.sourceMesh.verts.data(),
                 request.sourceMesh.verts.size() * sizeof(FractureVertex), h);
    h = HashBytes(request.sourceMesh.indices.data(),
                 request.sourceMesh.indices.size() * sizeof(int32_t), h);
    h = HashCombine(h, request.seed);
    h = HashCombine(h, static_cast<uint64_t>(request.pieceCount));
    h = HashCombine(h, static_cast<uint64_t>(request.openMeshMode));
    h = HashCombine(h, static_cast<uint64_t>(request.voxelResolution));
    h = HashCombine(h, static_cast<uint64_t>(kFractureBakeVersion));
    return h;
}

} // namespace

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

    // 保存先: assets\Fracture\<エンティティ名>_<入力の16hex>.mfrac。入力が同じなら同じ
    // ファイルを指してよい
    const std::string safeName = SanitizeFileName(world.GetName(root), "Fracture");
    char hashHex[17];
    std::snprintf(hashHex, sizeof(hashHex), "%016llx",
                 static_cast<unsigned long long>(ComputeFractureBakeInputHash(request)));
    const std::wstring path = ctx.assetsRoot + L"\\Fracture\\" + Utf8ToWide(safeName) + L"_"
                             + Utf8ToWide(hashHex) + FractureAsset::kFractureExt;
    const FractureAsset::FractureData data
        = BuildFractureAssetData(result, request.sourceMeshHash, request.seed, request.pieceCount,
                                request.openMeshMode, request.voxelResolution, pieceBoneNames);
    if (!FractureAsset::Save(path, data)) {
        MYE_LOG_ERROR("[fracture] failed to write %s", WideToUtf8(path).c_str());
        return false;
    }
    // .meta を確定させてから登録する (先に確定させないと登録名が path-hash に落ちて、
    // ファイルを移動しただけで Destructible.fractureAsset の参照が壊れる)。GUID の値は
    // 他の AssetRef (Collider.physMaterial 等) と同じ表現でそのまま欄へ書く
    const uint64_t guid = ctx.assetDb != nullptr ? ctx.assetDb->GuidForPath(path, /*createIfMissing=*/true)
                                                  : AssetDatabase::EnsureMeta(path);
    scmhint::Changed(path);

    // 書いた直後は必ず読み直す: 同じ保存名を以前のセッションや別のエンティティの焼きで
    // 既に読み込んでいた場合でも、今書いた中身で登録し直す
    FractureLibrary* lib = fracturelib::Library();
    const FractureAssetHandle* handle = lib != nullptr ? lib->ReloadFromFile(path) : nullptr;
    if (handle == nullptr) {
        MYE_LOG_ERROR("[fracture] failed to register %s right after writing it",
                     WideToUtf8(path).c_str());
        return false;
    }

    undo.BeginRecord("Generate Fracture Pieces", selection);
    undo.CaptureBefore(*ctx.scene, fid);
    comp->fractureAsset = AssetID{ guid };
    BuildFracturePieces(world, root, *handle); // 内部で ApplyStructuralChanges 済み
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    return true;
}

} // namespace mye
