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
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/FractureAsset.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Scene.h"

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
        const bool wroteFile
            = fs::exists(root / L"Fracture" / L"EditorSelfTestBox_7_4.mfrac", ec);
        check(wroteFile, "commit: the .mfrac lands at assets\\Fracture\\<name>_<seed>_<pieceCount>.mfrac");

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

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Fracture editor self test: PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Fracture editor self test: FAIL (%d) ====", failCount);
    return false;
}

} // namespace mye
