//====================================================================================
//                          FractureEditorSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          Inspector 焼き回りのヘッドレス回帰テスト実装
//====================================================================================
#include "Editor/FractureEditorSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <DirectXMath.h>

#include "Editor/FractureBakeCommit.h"
#include "Editor/FractureBakeService.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/InspectorWindow.h" // M80p: GetCachedSkinWeights (friend) の検算
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/CookedCache.h" // M80p: 検算用にキャッシュ済み .mmdl を直接操作する
#include "Engine/Engine/Asset/FractureAsset.h"
#include "Engine/Engine/Asset/ModelCook.h" // M80p: ModelCook::SaveToCache で実アセット無しに .mmdl を作る
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/FractureSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Physics/PhysicsSystem.h" // ShapeImpulse (FractureSystem::Update の引数)
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Renderer/GpuResources.h"

using namespace DirectX;
namespace fs = std::filesystem;

namespace mye {
namespace {

int32_t AddVert(FractureMesh& m, float x, float y, float z, float nx, float ny, float nz, float u,
                float v)
{
    FractureVertex vert;
    vert.position = { x, y, z };
    vert.normal = { nx, ny, nz };
    vert.uv = { u, v };
    m.verts.push_back(vert);
    return static_cast<int32_t>(m.verts.size()) - 1;
}

void Tri(FractureMesh& m, int32_t a, int32_t b, int32_t c)
{
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
}

// 共有 8 頂点の箱 (半径 hx,hy,hz)。CCW 外向き。閉じ判定/切断の SelfTest (FractureSelfTest.cpp)
// と同じジオメトリを複製したもの — Editor 側は焼きの配線だけを見たいので、
// 分割コアが「閉じている」と保証済みの形をそのまま使う
FractureMesh MakeBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, -1, -1, -1, 0, 0), AddVert(m, hx, -hy, -hz, 1, -1, -1, 0, 0),
        AddVert(m, hx, hy, -hz, 1, 1, -1, 0, 0),      AddVert(m, -hx, hy, -hz, -1, 1, -1, 0, 0),
        AddVert(m, -hx, -hy, hz, -1, -1, 1, 0, 0),    AddVert(m, hx, -hy, hz, 1, -1, 1, 0, 0),
        AddVert(m, hx, hy, hz, 1, 1, 1, 0, 0),         AddVert(m, -hx, hy, hz, -1, 1, 1, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    quad(p[4], p[5], p[6], p[7]); // +Z
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 蓋のない箱 (+Z 面を欠く。境界辺 4 本)。ボクセル化経路の確認用
FractureMesh MakeOpenBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, 0, 0, 0, 0, 0), AddVert(m, hx, -hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, -hz, 0, 0, 0, 0, 0),   AddVert(m, -hx, hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, -hx, -hy, hz, 0, 0, 0, 0, 0),  AddVert(m, hx, -hy, hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, hz, 0, 0, 0, 0, 0),     AddVert(m, -hx, hy, hz, 0, 0, 0, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    // +Z を作らない (穴)
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 1 枚の平面 (quad)。境界辺 4 本 (拒否理由の件数を確かめる最小入力)
FractureMesh MakePlaneQuad(float half)
{
    FractureMesh m;
    const int32_t a = AddVert(m, -half, 0, -half, 0, 1, 0, 0, 0);
    const int32_t b = AddVert(m, half, 0, -half, 0, 1, 0, 1, 0);
    const int32_t c = AddVert(m, half, 0, half, 0, 1, 0, 1, 1);
    const int32_t d = AddVert(m, -half, 0, half, 0, 1, 0, 0, 1);
    Tri(m, a, b, c);
    Tri(m, a, c, d);
    return m;
}

std::vector<char> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 1 セッション分のエンジン側資産状態。「セッションを閉じて開き直す」を模すテストが、
// 前のセッションの登録が一切残っていない状態を作るために使う
struct FractureSessionEnv {
    RenderResources resources;
    ConvexColliderLibrary colliders;
    FractureLibrary library;
    FractureSessionEnv()
    {
        colliders.Init(&resources);
        library.Init(&resources, &colliders);
        fracturelib::Install(&library);
    }
    ~FractureSessionEnv() { fracturelib::Install(nullptr); }
};

} // namespace

bool RunFractureEditorSelfTest()
{
    MYE_LOG_INFO("==== Fracture editor self test ====");
    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_fracture_editor_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- (1) 拒否理由が構造化データとして返る (開いた平面、openMeshMode=0) ----
    {
        FractureBakeInput in;
        in.sourceMesh = MakePlaneQuad(1.0f);
        in.seed = 1;
        in.pieceCount = 4;
        in.openMeshMode = 0;
        FractureBakeResult r;
        const bool ok = BakeFracture(in, r);
        check(!ok && !r.success && r.rejectedOpenMesh && r.boundaryEdges == 4
                  && r.nonManifoldEdges == 0 && r.orientationMismatches == 0,
              "reject: an open plane (openMeshMode=0) is rejected with the exact boundary-edge "
              "count in the structured result (not just a Japanese failReason string)");
    }

    // ---- (2) 焼きの進行段階コールバック ----
    {
        std::vector<FractureBakeStage> stages;
        struct CbCtx {
            std::vector<FractureBakeStage>* out;
        } cbCtx{ &stages };
        const FractureBakeProgressFn cb = [](FractureBakeStage stage, void* user) {
            static_cast<CbCtx*>(user)->out->push_back(stage);
        };

        FractureBakeInput closedIn;
        closedIn.sourceMesh = MakeBox(1, 1, 1);
        closedIn.seed = 3;
        closedIn.pieceCount = 3;
        closedIn.progress = cb;
        closedIn.progressUserData = &cbCtx;
        FractureBakeResult closedResult;
        const bool closedOk = BakeFracture(closedIn, closedResult);
        check(closedOk && stages.size() == 3 && stages[0] == FractureBakeStage::ClosedCheck
                  && stages[1] == FractureBakeStage::Split && stages[2] == FractureBakeStage::Hull,
              "progress: a closed mesh reports ClosedCheck -> Split -> Hull and never Voxelize");

        stages.clear();
        FractureBakeInput openIn;
        openIn.sourceMesh = MakeOpenBox(1, 1, 1);
        openIn.seed = 5;
        openIn.pieceCount = 1; // 分割自体の速さは sub-04/14 で検証済み。ここは配線だけ見る
        openIn.openMeshMode = 1;
        openIn.voxelResolution = 16; // 最小解像度 (焼き時間を最短に)
        openIn.progress = cb;
        openIn.progressUserData = &cbCtx;
        FractureBakeResult openResult;
        const bool openOk = BakeFracture(openIn, openResult);
        check(openOk && stages.size() == 4 && stages[0] == FractureBakeStage::ClosedCheck
                  && stages[1] == FractureBakeStage::Voxelize && stages[2] == FractureBakeStage::Split
                  && stages[3] == FractureBakeStage::Hull,
              "progress: an open mesh allowed to voxelize reports all four stages in order");
    }

    // ---- (3) 同じ入力を焼いた 2 つの .mfrac がバイト一致 ----
    {
        FractureBakeInput in;
        in.sourceMesh = MakeBox(1, 1, 1);
        in.seed = 42;
        in.pieceCount = 6;
        FractureBakeResult r1, r2;
        const bool ok1 = BakeFracture(in, r1);
        const bool ok2 = BakeFracture(in, r2);
        check(ok1 && ok2 && r1.pieces.size() == r2.pieces.size() && !r1.pieces.empty(),
              "determinism: baking the same input twice succeeds with the same piece count");

        const FractureAsset::FractureData d1
            = BuildFractureAssetData(r1, 123, 42, 6, 0, 32);
        const FractureAsset::FractureData d2
            = BuildFractureAssetData(r2, 123, 42, 6, 0, 32);
        const std::wstring p1 = (root / L"a.mfrac").wstring();
        const std::wstring p2 = (root / L"b.mfrac").wstring();
        const bool saved = FractureAsset::Save(p1, d1) && FractureAsset::Save(p2, d2);
        check(saved, "determinism: both copies save without error");
        const std::vector<char> b1 = ReadFileBytes(p1);
        const std::vector<char> b2 = ReadFileBytes(p2);
        check(!b1.empty() && b1 == b2,
              "determinism: two bakes of the same input write byte-identical .mfrac files");
    }

    // ---- (4) 非同期ワーカー: Request -> Pump -> TakeResult が同期焼きと同じ結果を返す ----
    {
        FractureBakeService svc;
        FractureBakeRequest req;
        req.sourceMesh = MakeBox(1, 1, 1);
        req.seed = 9;
        req.pieceCount = 4;
        svc.Request(1, req);
        check(svc.GetState(1) == FractureBakeJobState::Baking,
              "async: a fresh request is immediately visible as Baking");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (svc.GetState(1) != FractureBakeJobState::Ready
               && std::chrono::steady_clock::now() < deadline) {
            svc.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FractureBakeRequest reqOut;
        FractureBakeResult resultOut;
        std::vector<std::string> boneNamesOut;
        const bool took = svc.TakeResult(1, reqOut, resultOut, boneNamesOut);
        check(took, "async: the worker finishes and Pump/TakeResult deliver the result");
        check(svc.GetState(1) == FractureBakeJobState::None,
              "async: TakeResult consumes the entry (no double-commit on the next frame)");

        FractureBakeInput syncIn;
        syncIn.sourceMesh = MakeBox(1, 1, 1);
        syncIn.seed = 9;
        syncIn.pieceCount = 4;
        FractureBakeResult syncResult;
        BakeFracture(syncIn, syncResult);
        check(took && resultOut.success && FractureBakeDigest(resultOut) == FractureBakeDigest(syncResult),
              "async: the worker's output digest matches a synchronous BakeFracture with the same "
              "input (the thread boundary changes nothing about the bake itself)");
        svc.Shutdown();
    }

    // ---- (4b) 取り消し (M80p): 焼き始めた後の取り消しが有限時間で
    //      Ready(cancelled=true) になる。まだキュー待ちの取り消しは即座に None へ戻る ----
    {
        FractureBakeService svc;

        // 焼き始めた後の取り消し: 256 破片 (数百 ms 以上かかる、bench.md 参照) を投げ、
        // 実際に Split 段階へ入ったのを確認してから Cancel する (セル切断ループの途中を
        // 確実に捉える。固定時間の sleep だけに頼らない)
        FractureBakeRequest req;
        req.sourceMesh = MakeBox(1, 1, 1);
        req.seed = 5;
        req.pieceCount = 256;
        svc.Request(1, req);
        const auto splitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (svc.GetStage(1) != FractureBakeStage::Split
               && std::chrono::steady_clock::now() < splitDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        svc.Cancel(1);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (svc.GetState(1) != FractureBakeJobState::Ready
               && std::chrono::steady_clock::now() < deadline) {
            svc.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FractureBakeRequest reqOut;
        FractureBakeResult resultOut;
        std::vector<std::string> boneNamesOut;
        const bool took = svc.TakeResult(1, reqOut, resultOut, boneNamesOut);
        check(took, "cancel: a bake that was cancelled mid-flight still reaches Ready in finite time");
        check(took && !resultOut.success && resultOut.cancelled,
              "cancel: the result reports cancelled (not just a generic failure)");
        svc.Shutdown();
    }

    // ---- (4b2) Shutdown が焼きの途中でも速やかに戻る (M80p)。Cancel を挟まず
    //      Shutdown だけを呼び、同じ入力の同期焼き (フルの所要時間) と比べて有意に短いことを
    //      見る (絶対時間ではなく相対比較にして、Debug/Release の速度差に依存しない検算にする) ----
    {
        FractureBakeInput fullIn;
        fullIn.sourceMesh = MakeBox(1, 1, 1);
        fullIn.seed = 5;
        fullIn.pieceCount = 256;
        FractureBakeResult fullResult;
        const auto fullStart = std::chrono::steady_clock::now();
        BakeFracture(fullIn, fullResult);
        const double fullBakeMs
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fullStart).count();

        FractureBakeService svc;
        FractureBakeRequest req;
        req.sourceMesh = fullIn.sourceMesh;
        req.seed = fullIn.seed;
        req.pieceCount = fullIn.pieceCount;
        svc.Request(1, req);
        const auto splitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (svc.GetStage(1) != FractureBakeStage::Split
               && std::chrono::steady_clock::now() < splitDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto shutdownStart = std::chrono::steady_clock::now();
        svc.Shutdown();
        const double shutdownMs
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shutdownStart)
                  .count();
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "shutdown: Shutdown() mid-bake returns in %.1f ms, well under a full synchronous "
                      "bake of the same input (%.1f ms) — it did not wait for the bake to finish",
                      shutdownMs, fullBakeMs);
        check(shutdownMs < fullBakeMs * 0.5, buf);
    }

    // ---- (4c) スキンウェイト照会のキャッシュ (M80p): Inspector 側は同じ
    //      (fid, srcPath, meshKey) の 2 回目以降で .mmdl を読み直さない。呼び出し回数を
    //      数える代わりに、キャッシュ後に .mmdl を消しても同じ結果を返すことで検算する
    //      (観測可能な挙動そのものを見る) ----
    {
        CookedCache::Configure((root / L"weight_cache_cooked").wstring(), true);
        const std::wstring srcPath = (root / L"weight_cache_src.glb").wstring();
        { std::ofstream(srcPath, std::ios::binary) << "x"; } // ReadValidated が mtime を見るので実在させる
        ModelCook::ModelCookData data;
        const std::vector<MeshVertex> verts(3); // 中身は問わない (件数だけ見る)
        data.AddMesh("meshA", verts, { 0, 1, 2 });
        ModelCook::SaveToCache(srcPath, data);

        InspectorWindow win;
        EngineContext cacheCtx;
        std::vector<MeshVertex> out1;
        const bool got1 = win.GetCachedSkinWeights(cacheCtx, 1, srcPath, "meshA", out1);
        check(got1 && out1.size() == verts.size(),
              "weight cache: the first call reads the freshly written .mmdl");

        // .mmdl を退避する (キャッシュが効いていなければ次の呼び出しは失敗するはず)
        const std::wstring mmdlPath = CookedCache::PathFor(srcPath, ModelCook::kModelExt);
        const std::wstring movedPath = mmdlPath + L".movedaway";
        std::error_code moveEc;
        fs::rename(mmdlPath, movedPath, moveEc);
        check(!moveEc, "weight cache: setup can move the .mmdl aside to prove caching");

        std::vector<MeshVertex> out2;
        const bool got2 = win.GetCachedSkinWeights(cacheCtx, 1, srcPath, "meshA", out2);
        check(got2 && out2.size() == verts.size(),
              "weight cache: the same (fid, srcPath, meshKey) reuses the cached result after the "
              ".mmdl is gone (no re-read)");

        // 別の fid (別エンティティ扱い) はキャッシュを共有しない。.mmdl は既に無いので、
        // 素直に読みに行けば失敗するはず (キャッシュが fid をまたいで誤爆していないことも確認)
        std::vector<MeshVertex> out3;
        const bool got3 = win.GetCachedSkinWeights(cacheCtx, 2, srcPath, "meshA", out3);
        check(!got3,
              "weight cache: a different fid is unaffected by another entity's cache (a fresh "
              "lookup correctly fails once the .mmdl is gone)");

        fs::rename(movedPath, mmdlPath, moveEc);
        CookedCache::Configure(L"", false); // 他スイートへ漏らさない
    }

    // ---- (5) CommitFractureBake: .mfrac 保存・登録・BuildFracturePieces・Undo/Redo ----
    {
        FractureLibrary fractureLib;
        fractureLib.Init(nullptr, nullptr); // メッシュ/凸包の実登録は不要 (配線だけ見る)
        fracturelib::Install(&fractureLib);

        Scene s;
        EngineContext ctx;
        ctx.scene = &s;
        ctx.assetsRoot = root.wstring(); // assetDb は null -> AssetDatabase::EnsureMeta へフォールバック

        GameObject box = s.CreateGameObjectTracked("EditorSelfTestBox");
        auto* destructible = box.AddComponent<DestructibleComponent>();
        destructible->seed = 7;
        destructible->pieceCount = 4;
        const uint64_t fid = s.EnsureFileId(box.Id());

        FractureBakeInput in;
        in.sourceMesh = MakeBox(1, 1, 1);
        in.seed = destructible->seed;
        in.pieceCount = destructible->pieceCount;
        FractureBakeResult bakeResult;
        const bool baked = BakeFracture(in, bakeResult) && bakeResult.success;
        check(baked && bakeResult.pieces.size() > 1,
              "setup: a small box bakes into more than one piece for the commit test");

        FractureBakeRequest req;
        req.seed = destructible->seed;
        req.pieceCount = destructible->pieceCount;
        req.openMeshMode = destructible->openMeshMode;
        req.voxelResolution = destructible->voxelResolution;
        req.sourceMeshHash = HashStr("test://box");

        UndoStack undo;
        Selection sel;
        const bool committed = CommitFractureBake(ctx, sel, undo, box.Id(), fid, req, bakeResult);
        check(committed, "commit: writes the .mfrac and registers it without error");
        check(!destructible->fractureAsset.IsNull(),
              "commit: Destructible.fractureAsset is set to the newly registered asset");
        const int childrenAfterGenerate = CountFracturePieceChildren(s.GetWorld(), box.Id());
        check(childrenAfterGenerate == static_cast<int>(bakeResult.pieces.size()),
              "commit: BuildFracturePieces creates exactly the baked piece count as children");
        bool wroteFile = false;
        for (const auto& entry : fs::directory_iterator(root / L"Fracture", ec)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"EditorSelfTestBox_", 0) == 0 && entry.path().extension() == L".mfrac") {
                wroteFile = true;
                break;
            }
        }
        check(wroteFile, "commit: the .mfrac lands at assets\\Fracture\\<name>_<input hash>.mfrac");

        undo.Undo(s, sel);
        s.GetWorld().ApplyStructuralChanges();
        {
            const auto* d = s.GetWorld().GetComponent<DestructibleComponent>(box.Id());
            check(d != nullptr && d->fractureAsset.IsNull()
                      && CountFracturePieceChildren(s.GetWorld(), box.Id()) == 0,
                  "undo: one Undo removes every generated piece and resets fractureAsset to null");
        }

        undo.Redo(s, sel);
        s.GetWorld().ApplyStructuralChanges();
        {
            const auto* d = s.GetWorld().GetComponent<DestructibleComponent>(box.Id());
            check(d != nullptr && !d->fractureAsset.IsNull()
                      && CountFracturePieceChildren(s.GetWorld(), box.Id())
                             == static_cast<int>(bakeResult.pieces.size()),
                  "redo: restores the fracture pieces and the fractureAsset reference "
                  "(re-baking after Undo would point at the same .mfrac since the input is unchanged)");
        }

        fracturelib::Install(nullptr);
    }

    // ---- (6) セッションをまたぐ解決: 焼いて保存したセッションを閉じ、新しい
    //          AssetDatabase・FractureLibrary・World で開き直しても割れる ----
    {
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_fracture_editor_selftest_session";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        nlohmann::json saved;

        // セッション 1: 実際の AssetDatabase で焼き、シーンを保存する
        {
            AssetDatabase db;
            db.ScanAndSync(dir.wstring());
            db.InstallAsKeyResolver();
            FractureSessionEnv env;

            Scene s;
            EngineContext ctx;
            ctx.scene = &s;
            ctx.assetsRoot = dir.wstring();
            ctx.assetDb = &db;

            GameObject crate = s.CreateGameObjectTracked("SessionCrate");
            auto* destructible = crate.AddComponent<DestructibleComponent>();
            destructible->seed = 3;
            destructible->pieceCount = 6;

            FractureBakeInput in;
            in.sourceMesh = MakeBox(1, 1, 1);
            in.seed = destructible->seed;
            in.pieceCount = destructible->pieceCount;
            FractureBakeResult bakeResult;
            check(BakeFracture(in, bakeResult) && bakeResult.success,
                  "session: source bake succeeds for the cross-session test");

            FractureBakeRequest req;
            req.sourceMesh = in.sourceMesh;
            req.seed = destructible->seed;
            req.pieceCount = destructible->pieceCount;

            UndoStack undo;
            Selection sel;
            check(CommitFractureBake(ctx, sel, undo, crate.Id(), s.EnsureFileId(crate.Id()), req, bakeResult),
                  "session: commit succeeds with a real AssetDatabase");

            saved = SceneSerializer::SaveToJson(s);
            AssetDatabase::UninstallKeyResolver();
        }

        // セッション 2: 新しい AssetDatabase・FractureLibrary・World で開き直す
        {
            AssetDatabase db;
            db.ScanAndSync(dir.wstring());
            db.InstallAsKeyResolver();
            FractureSessionEnv env;

            Scene s;
            check(SceneSerializer::LoadFromJson(s, saved), "session: the saved scene loads back");
            World& w = s.GetWorld();

            // シーンロード経路が必ず行う先読み。最初の物理 tick より前に呼ぶ契約
            PreloadFractureAssets(w);

            GameObject crateGo = s.Find("SessionCrate");
            const auto* dc = crateGo ? w.GetComponent<DestructibleComponent>(crateGo.Id()) : nullptr;
            check(dc != nullptr && !dc->fractureAsset.IsNull(),
                  "session: Destructible.fractureAsset survives the round trip");

            GameObject frag0 = s.Find("Frag0");
            const auto* col0 = frag0 ? w.GetComponent<ColliderComponent>(frag0.Id()) : nullptr;
            check(col0 != nullptr && env.colliders.Get(col0->meshAsset) != nullptr,
                  "session: the piece's convex hull is registered before the first physics tick");

            const FractureAssetHandle* handle = dc ? ResolveFractureAsset(dc->fractureAsset) : nullptr;
            check(handle != nullptr, "session: the fracture asset resolves in a brand-new session");
            check(handle != nullptr && crateGo
                      && DestructiblePiecesMatchAsset(w, crateGo.Id(), *handle, dc->broken),
                  "session: the Inspector-style query reports a match right after loading");

            if (frag0) {
                ApplyFractureDamage(w, frag0.Id(), { 0, 0, 0 }, 0.0f, 1.0e6f);
            }
            FractureSystem fsys;
            for (int i = 0; i < 3; ++i) {
                fsys.Update(w, 1.0f / 60.0f, {});
                w.ApplyStructuralChanges();
                fsys.ApplyDeferredLocals(w);
            }
            const auto* dcAfter = crateGo ? w.GetComponent<DestructibleComponent>(crateGo.Id()) : nullptr;
            check(dcAfter != nullptr && dcAfter->broken,
                  "session: the destructible breaks after loading in a brand-new session");
            check(handle != nullptr && dcAfter != nullptr && crateGo
                      && DestructiblePiecesMatchAsset(w, crateGo.Id(), *handle, dcAfter->broken),
                  "session: the Inspector-style query still reports a match after breaking");

            AssetDatabase::UninstallKeyResolver();
        }
        fs::remove_all(dir, ec);
    }

    // ---- (7) 保存名は内容から決まる: 同名の別エンティティは中身が違えば別ファイル、
    //          同じ中身の再焼きは同じファイルを指す ----
    {
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_fracture_editor_selftest_names";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        AssetDatabase db;
        db.ScanAndSync(dir.wstring());
        db.InstallAsKeyResolver();
        FractureSessionEnv env;

        Scene s;
        EngineContext ctx;
        ctx.scene = &s;
        ctx.assetsRoot = dir.wstring();
        ctx.assetDb = &db;
        UndoStack undo;
        Selection sel;

        GameObject a = s.CreateGameObjectTracked("DupName");
        a.AddComponent<DestructibleComponent>();
        GameObject shelf = s.CreateGameObjectTracked("Shelf");
        GameObject b = s.CreateGameObjectTracked("DupName");
        s.GetWorld().SetParent(b.Id(), shelf.Id());
        s.GetWorld().ApplyStructuralChanges();
        b.AddComponent<DestructibleComponent>();

        FractureBakeInput boxIn;
        boxIn.sourceMesh = MakeBox(0.5f, 0.5f, 0.5f);
        boxIn.seed = 1;
        boxIn.pieceCount = 6;
        FractureBakeResult boxBake;
        check(BakeFracture(boxIn, boxBake) && boxBake.success, "duplicate names: box bakes");

        FractureBakeInput plankIn;
        plankIn.sourceMesh = MakeBox(2.0f, 0.1f, 0.1f); // 同名の別物 (細長い板)
        plankIn.seed = 1;
        plankIn.pieceCount = 6;
        FractureBakeResult plankBake;
        check(BakeFracture(plankIn, plankBake) && plankBake.success, "duplicate names: plank bakes");

        FractureBakeRequest reqA;
        reqA.sourceMesh = boxIn.sourceMesh;
        reqA.seed = 1;
        reqA.pieceCount = 6;
        check(CommitFractureBake(ctx, sel, undo, a.Id(), s.EnsureFileId(a.Id()), reqA, boxBake),
              "duplicate names: committing A (box) succeeds");

        FractureBakeRequest reqB;
        reqB.sourceMesh = plankIn.sourceMesh;
        reqB.seed = 1;
        reqB.pieceCount = 6;
        check(CommitFractureBake(ctx, sel, undo, b.Id(), s.EnsureFileId(b.Id()), reqB, plankBake),
              "duplicate names: committing B (plank, same name/seed/pieceCount) succeeds");

        const auto* dcA = s.GetWorld().GetComponent<DestructibleComponent>(a.Id());
        const auto* dcB = s.GetWorld().GetComponent<DestructibleComponent>(b.Id());
        check(dcA != nullptr && dcB != nullptr && dcA->fractureAsset != dcB->fractureAsset,
              "duplicate names: different content under the same entity name gets a different asset");

        size_t mfracFiles = 0;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            mfracFiles += entry.path().extension() == L".mfrac" ? 1 : 0;
        }
        check(mfracFiles == 2, "duplicate names: two separate .mfrac files are written");

        const FractureAssetHandle* handleA = dcA ? ResolveFractureAsset(dcA->fractureAsset) : nullptr;
        const FractureAssetHandle* handleB = dcB ? ResolveFractureAsset(dcB->fractureAsset) : nullptr;
        float maxAbsXA = 0.0f, maxAbsXB = 0.0f;
        for (const auto& p : handleA ? handleA->pieces : std::vector<FracturePieceRef>{}) {
            maxAbsXA = (std::max)(maxAbsXA, std::fabs(p.origin.x));
        }
        for (const auto& p : handleB ? handleB->pieces : std::vector<FracturePieceRef>{}) {
            maxAbsXB = (std::max)(maxAbsXB, std::fabs(p.origin.x));
        }
        check(handleA != nullptr && handleB != nullptr && maxAbsXB > maxAbsXA,
              "duplicate names: each asset keeps its own geometry (the plank's pieces span farther "
              "in x than the box's)");

        // 同じ中身をもう一度焼くと同じファイル (= 同じ fractureAsset) を指す。CommitFractureBake は
        // 構造変更を伴うため、比較対象の値は呼ぶ前に取り出しておく (dcA は再委託後に無効たり得る)
        const AssetID assetBeforeRebake = dcA != nullptr ? dcA->fractureAsset : AssetID{};
        FractureBakeResult boxBake2;
        check(BakeFracture(boxIn, boxBake2) && boxBake2.success,
              "duplicate names: re-baking the same box succeeds");
        check(CommitFractureBake(ctx, sel, undo, a.Id(), s.EnsureFileId(a.Id()), reqA, boxBake2),
              "duplicate names: re-committing the same input succeeds");
        const auto* dcA2 = s.GetWorld().GetComponent<DestructibleComponent>(a.Id());
        check(dcA2 != nullptr && dcA2->fractureAsset == assetBeforeRebake,
              "duplicate names: re-baking identical input reuses the same .mfrac");

        AssetDatabase::UninstallKeyResolver();
        fs::remove_all(dir, ec);
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Fracture editor self test: PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Fracture editor self test: FAIL (%d) ====", failCount);
    return false;
}

} // namespace mye
