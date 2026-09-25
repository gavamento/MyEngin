//====================================================================================
//                          FractureSkinSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          スキンメッシュの破壊 (M80j) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Physics/FractureSkinSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <process.h> // M80j round 3: _getpid (キャッシュ置き場をプロセスごとに一意にする)
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/CookedCache.h" // M80j round 2: 実アセットの .mmdl を書く/読む
#include "Engine/Engine/Asset/FractureAsset.h"
#include "Engine/Engine/Asset/ModelCook.h" // M80j round 2: TryLoadCookedMeshVertices の実測
#include "Engine/Engine/FbxLoader.h"       // M80j round 2: skinned_beam.fbx
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/FractureSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/ModelLoader.h" // M80j round 2: CesiumMan.glb
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Physics/FractureSkinBake.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

// ---- テスト用「腕」: 半径 (hx,hy,hz) の箱を Bone0 (下半分) / Bone1 (上半分、Bone0 の子) の
//      2 骨で覆う。下 4 頂点 = Bone0 のウェイト 1、上 4 頂点 = Bone1 のウェイト 1 (境界の
//      50/50 は sub-10 の本題ではないので単純化する)。CCW 外向き (FractureSelfTest.cpp の
//      MakeBox と同じ巻き順)
struct SkinBoxSource {
    std::vector<MeshVertex> verts; // 重み込み (root の MeshRenderer.mesh 用)
    std::vector<uint32_t> indices;
    std::vector<FractureSkinVertex> skinVerts; // 骨割り当て入力 (verts と同じ並び)
};

SkinBoxSource MakeSkinBox(float hx, float hy, float hz)
{
    struct Corner {
        float x, y, z, nx, ny, nz;
    };
    const Corner c[8] = {
        { -hx, -hy, -hz, -1, -1, -1 }, { hx, -hy, -hz, 1, -1, -1 }, { hx, hy, -hz, 1, 1, -1 },
        { -hx, hy, -hz, -1, 1, -1 },   { -hx, -hy, hz, -1, -1, 1 }, { hx, -hy, hz, 1, -1, 1 },
        { hx, hy, hz, 1, 1, 1 },       { -hx, hy, hz, -1, 1, 1 },
    };
    SkinBoxSource r;
    r.verts.resize(8);
    r.skinVerts.resize(8);
    for (int i = 0; i < 8; ++i) {
        MeshVertex mv;
        mv.position = { c[i].x, c[i].y, c[i].z };
        mv.normal = { c[i].nx, c[i].ny, c[i].nz };
        mv.uv = { 0.0f, 0.0f };
        mv.boneIndices[0] = (c[i].y > 0.0f) ? 1 : 0;
        mv.boneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
        r.verts[i] = mv;
        r.skinVerts[i].position = mv.position;
        r.skinVerts[i].boneIndices[0] = mv.boneIndices[0];
        r.skinVerts[i].boneWeights = mv.boneWeights;
    }
    auto quad = [&](uint32_t a, uint32_t b, uint32_t cc, uint32_t d) {
        r.indices.push_back(a);
        r.indices.push_back(b);
        r.indices.push_back(cc);
        r.indices.push_back(a);
        r.indices.push_back(cc);
        r.indices.push_back(d);
    };
    quad(0, 3, 2, 1); // -Z
    quad(4, 5, 6, 7); // +Z
    quad(0, 1, 5, 4); // -Y
    quad(3, 7, 6, 2); // +Y
    quad(0, 4, 7, 3); // -X
    quad(1, 2, 6, 5); // +X
    return r;
}

FractureMesh ToFractureMesh(const std::vector<MeshVertex>& verts, const std::vector<uint32_t>& indices)
{
    FractureMesh m;
    m.verts.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        m.verts[i].position = verts[i].position;
        m.verts[i].normal = verts[i].normal;
        m.verts[i].uv = verts[i].uv;
    }
    m.indices.assign(indices.begin(), indices.end());
    return m;
}

// Bone0 (ルート、下半分の原点 (0,-hy,0)) -> Bone1 (Bone0 の子、上半分の原点 (0,+hy,0)、
// 局所バインドは親からの相対 (0,2hy,0))。Bend クリップは Bone1 だけを Z 軸まわりに
// 0 → bendRadians 回転させる (Bone0 はチャネル無し = バインド値のまま)
SkinnedModel MakeArmSkinModel(float hy, float bendRadians)
{
    SkinnedModel model;
    model.joints.resize(2);
    model.joints[0].name = "Bone0";
    model.joints[0].parent = -1;
    model.joints[0].bindT = { 0.0f, -hy, 0.0f };
    model.joints[0].bindR = { 0.0f, 0.0f, 0.0f, 1.0f };
    model.joints[0].bindS = { 1.0f, 1.0f, 1.0f };
    model.joints[0].inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, hy, 0, 1 };

    model.joints[1].name = "Bone1";
    model.joints[1].parent = 0;
    model.joints[1].bindT = { 0.0f, 2.0f * hy, 0.0f };
    model.joints[1].bindR = { 0.0f, 0.0f, 0.0f, 1.0f };
    model.joints[1].bindS = { 1.0f, 1.0f, 1.0f };
    model.joints[1].inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -hy, 0, 1 };

    SkeletalClip clip;
    clip.name = "Bend";
    clip.duration = 1.0f;
    clip.tracks.resize(2); // tracks[0] (Bone0) は空のまま = バインド値
    JointTrack& t1 = clip.tracks[1];
    t1.rTimes = { 0.0f, 1.0f };
    t1.rVals = { XMFLOAT4{ 0, 0, 0, 1 },
                XMFLOAT4{ 0, 0, std::sin(bendRadians * 0.5f), std::cos(bendRadians * 0.5f) } };
    model.clips.push_back(std::move(clip));
    return model;
}

// クォータニオンでベクトルを回転する (FractureSystem.cpp の QuatRotate と同じ式)。
// 球の狙い先を計算するだけの用途でも XMVector3Rotate は使わない — このコードベースは
// XMMatrixInverse/XMMatrixDecompose 同様、構成間 (Debug/Release) でビットが割れ得る
// DirectXMath の組み込み関数を避け、四則だけの式に固定する流儀を貫いている
void QuatRotateVec(float qx, float qy, float qz, float qw, float vx, float vy, float vz, float& ox,
                   float& oy, float& oz)
{
    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);
    ox = vx + qw * tx + (qy * tz - qz * ty);
    oy = vy + qw * ty + (qz * tx - qx * tz);
    oz = vz + qw * tz + (qx * ty - qy * tx);
}

// root の直子から FracturePiece.index==index のものを探す (FractureSelfTest.cpp の
// FindPieceChild と同じアルゴリズム。非公開ヘルパはファイルごとに複製する慣例に従う)
EntityID FindPieceChild(World& world, EntityID root, int32_t index)
{
    const auto* rh = world.GetComponent<HierarchyComponent>(root);
    for (EntityID c = rh ? rh->firstChild : kNullEntity; !c.IsNull();) {
        if (const auto* fp = world.GetComponent<FracturePieceComponent>(c)) {
            if (fp->index == index) {
                return c;
            }
        }
        const auto* ch = world.GetComponent<HierarchyComponent>(c);
        c = ch ? ch->nextSibling : kNullEntity;
    }
    return kNullEntity;
}

// ---- round 2 (planner VERDICT): 実アセット (CesiumMan.glb / skinned_beam.fbx) で
// TryLoadCookedMeshVertices・焼き (拒否 or ボクセル化)・骨割り当て・バインドポーズの
// 一致を確かめる。PartSelfTest.cpp:596-650 と同じヘッドレスロード ----

// srcPath の .mmdl から meshKey に一致する CookedMesh の頂点数・index 数を生で読む
// (TryLoadCookedMeshVertices は頂点しか返さないため、index 数の照合はここで別途行う。
// CookedCache::ReadValidated / ModelCook::Deserialize はどちらも公開 API — 本番コードは変えない)
bool ReadCookedMeshCounts(const std::wstring& srcPath, const std::string& meshKey, size_t& outVertexCount,
                          size_t& outIndexCount)
{
    std::vector<uint8_t> payload;
    if (!CookedCache::ReadValidated(srcPath, ModelCook::kModelExt, payload)) {
        return false;
    }
    ModelCook::ModelCookData d;
    if (!ModelCook::Deserialize(payload, d)) {
        return false;
    }
    for (const ModelCook::CookedMesh& m : d.meshes) {
        if (m.key == meshKey) {
            outVertexCount = m.vertices.size();
            outIndexCount = m.indices.size();
            return true;
        }
    }
    return false;
}

// 実アセット 1 個の検証。isFbx で FbxLoader/ModelLoader を切り替える
void CheckRealSkinnedAsset(int& failCount, const std::wstring& path, const char* label, bool isFbx)
{
    auto check = [&](bool cond, const std::string& what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what.c_str());
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what.c_str());
            ++failCount;
        }
    };
    char buf[320];

    RenderResources resources;
    ShaderManager shaders;
    // ★.mmdl を書くのは RegisterAssets (起動時の一括ヘッドレス登録) だけ — Load はエンティティを
    //   作る側の別経路で、フレッシュパースするだけでクックキャッシュを読み書きしない
    //   (ModelLoader.cpp: TryReplayFromCache/SaveToCache は RegisterAssets の中にしか無い)。
    //   Load とバイト同一のキーで登録する契約 (M50a) を使い、ここで先に RegisterAssets を
    //   1 回呼んで .mmdl を書かせてから Load でエンティティを作る (同じ resources へ両方登録
    //   しても、同名の再登録は差し替えなので安全)
    const bool registered = isFbx ? FbxLoader::RegisterAssets(resources, shaders, path, true)
                                  : ModelLoader::RegisterAssets(resources, shaders, path, true);
    std::snprintf(buf, sizeof(buf), "%s: RegisterAssets parses without error", label);
    check(registered, buf);

    // round 3: 「書かれていない」と「(書けているが) 読めない」を区別する — CookedCache::Write は
    // 戻り値を検査されない fire-and-forget なので、RegisterAssets の成功だけでは .mmdl が
    // 実際にディスクへ出たとは言えない。TryLoadCookedMeshVertices と同じ CookedCache::PathFor
    // で期待パスを求め、存在だけを見る (中身の妥当性は後段の TryLoadCookedMeshVertices が見る)
    const std::wstring expectedMmdlPath = CookedCache::PathFor(path, ModelCook::kModelExt);
    std::error_code existsEc;
    const bool mmdlWritten =
        !expectedMmdlPath.empty() && std::filesystem::exists(expectedMmdlPath, existsEc);
    std::snprintf(buf, sizeof(buf), "%s: RegisterAssets actually wrote a .mmdl file to disk", label);
    check(mmdlWritten, buf);

    Scene scene;
    GameObject root = isFbx ? FbxLoader::Load(scene, resources, shaders, path)
                            : ModelLoader::Load(scene, resources, shaders, path);
    World& w = scene.GetWorld();
    w.ApplyStructuralChanges();
    std::snprintf(buf, sizeof(buf), "%s: loads without error", label);
    check(static_cast<bool>(root), buf);
    if (!root) {
        return;
    }

    EntityID skinned = kNullEntity;
    {
        const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (skinned.IsNull()) {
                    skinned = arch.EntityAt(row);
                }
            }
        });
    }
    std::snprintf(buf, sizeof(buf), "%s: has a SkinnedMeshComponent entity", label);
    check(!skinned.IsNull(), buf);
    if (skinned.IsNull()) {
        return;
    }

    auto* sm = w.GetComponent<SkinnedMeshComponent>(skinned);
    auto* mr = w.GetComponent<MeshRendererComponent>(skinned);
    std::snprintf(buf, sizeof(buf), "%s: skinned entity has SkinnedMeshComponent + MeshRendererComponent",
                 label);
    check(sm != nullptr && mr != nullptr, buf);
    if (sm == nullptr || mr == nullptr) {
        return;
    }

    const SkinnedModel* model = resources.skinnedModels.Get(sm->model);
    std::snprintf(buf, sizeof(buf), "%s: skeleton is registered with at least 1 joint", label);
    check(model != nullptr && !model->joints.empty(), buf);
    if (model == nullptr || model->joints.empty()) {
        return;
    }

    Mesh* mesh = resources.meshes.Get(mr->mesh);
    std::snprintf(buf, sizeof(buf), "%s: mesh is registered with CPU vertices", label);
    check(mesh != nullptr && !mesh->positions.empty(), buf);
    if (mesh == nullptr || mesh->positions.empty()) {
        return;
    }

    const std::string* meshName = resources.meshes.NameOf(mr->mesh);
    std::snprintf(buf, sizeof(buf), "%s: mesh has a registered name", label);
    check(meshName != nullptr, buf);
    if (meshName == nullptr) {
        return;
    }
    // ヘッドレス selftest には AssetDatabase (guid → 現在パスの逆引き) が無いので
    // assetkey::SourcePathForSubAssetKey は「resolver 未設定」で空を返す (関数自身のコメント
    // どおりの既定動作 — ConvexCookSourcePath / .mcvx と同じ前提)。Inspector の本番経路は
    // AssetDatabase が resolver を Install しているので実際には空にならない。ここでは
    // ロードに使った path をそのまま srcPath とする — 見るのは
    // 「正しい srcPath を渡したとき TryLoadCookedMeshVertices が .mmdl を正しく読めるか」
    // (この関数自体の正しさ) であって、Inspector 側のパス解決は範囲外
    const std::wstring& srcPath = path;

    // ---- (a) TryLoadCookedMeshVertices: 頂点数/index数が CPU メッシュと一致、ウェイト和 ~1、
    //      骨 index が範囲内 ----
    std::vector<MeshVertex> weighted;
    const bool gotWeighted =
        !srcPath.empty() && ModelCook::TryLoadCookedMeshVertices(srcPath, *meshName, weighted);
    std::snprintf(buf, sizeof(buf), "%s: TryLoadCookedMeshVertices reads the .mmdl cache", label);
    check(gotWeighted, buf);

    std::vector<FractureSkinVertex> skinVerts;
    if (gotWeighted) {
        std::snprintf(buf, sizeof(buf), "%s: vertex count matches the CPU mesh (%zu vs %zu)", label,
                     weighted.size(), mesh->positions.size());
        check(weighted.size() == mesh->positions.size(), buf);

        size_t rawVertexCount = 0, rawIndexCount = 0;
        const bool gotCounts = ReadCookedMeshCounts(srcPath, *meshName, rawVertexCount, rawIndexCount);
        std::snprintf(buf, sizeof(buf), "%s: index count matches the CPU mesh (%zu vs %zu)", label,
                     rawIndexCount, mesh->indices.size());
        check(gotCounts && rawIndexCount == mesh->indices.size(), buf);

        int32_t badWeightCount = 0, badBoneCount = 0;
        for (const MeshVertex& v : weighted) {
            const float sum = v.boneWeights.x + v.boneWeights.y + v.boneWeights.z + v.boneWeights.w;
            if (std::fabs(sum - 1.0f) > 0.02f) {
                ++badWeightCount;
            }
            const float weightsArr[4] = { v.boneWeights.x, v.boneWeights.y, v.boneWeights.z,
                                          v.boneWeights.w };
            for (int k = 0; k < 4; ++k) {
                if (weightsArr[k] > 0.0f && static_cast<size_t>(v.boneIndices[k]) >= model->joints.size()) {
                    ++badBoneCount;
                }
            }
        }
        std::snprintf(buf, sizeof(buf), "%s: bone weight sums are ~1 (%d/%zu vertices off by >0.02)", label,
                     badWeightCount, weighted.size());
        check(badWeightCount == 0, buf);
        std::snprintf(buf, sizeof(buf), "%s: bone indices are within range (%d out-of-range slots)", label,
                     badBoneCount);
        check(badBoneCount == 0, buf);

        skinVerts.resize(weighted.size());
        for (size_t i = 0; i < weighted.size(); ++i) {
            skinVerts[i].position = weighted[i].position;
            std::memcpy(skinVerts[i].boneIndices, weighted[i].boneIndices, sizeof(skinVerts[i].boneIndices));
            skinVerts[i].boneWeights = weighted[i].boneWeights;
        }
    }

    // ---- (b) openMeshMode=0 で焼く。成功/拒否のどちらでもよい。理由と件数を観測して記録 ----
    FractureMesh src;
    src.verts.resize(mesh->positions.size());
    for (size_t i = 0; i < mesh->positions.size(); ++i) {
        src.verts[i].position = mesh->positions[i];
        src.verts[i].normal = i < mesh->normals.size() ? mesh->normals[i] : XMFLOAT3{ 0, 1, 0 };
        src.verts[i].uv = i < mesh->uvs.size() ? mesh->uvs[i] : XMFLOAT2{ 0, 0 };
    }
    src.indices.assign(mesh->indices.begin(), mesh->indices.end());

    FractureBakeInput in0;
    in0.sourceMesh = src;
    in0.seed = 1;
    in0.pieceCount = 2; // 実アセットは高ポリなので最小 (ボクセル化の解像度は指示どおり 32 を使う)
    in0.openMeshMode = 0;
    FractureBakeResult bake0;
    const bool baked0 = BakeFracture(in0, bake0);
    if (bake0.rejectedOpenMesh) {
        MYE_LOG_INFO("  %s: openMeshMode=0 REJECTED (boundary=%d non-manifold=%d orientation=%d)", label,
                    bake0.boundaryEdges, bake0.nonManifoldEdges, bake0.orientationMismatches);
    } else if (baked0 && bake0.success) {
        MYE_LOG_INFO("  %s: openMeshMode=0 SUCCEEDED (%zu pieces)", label, bake0.pieces.size());
    } else {
        MYE_LOG_INFO("  %s: openMeshMode=0 failed for a reason other than an open mesh: %s", label,
                    bake0.failReason.c_str());
    }

    int32_t usedOpenMeshMode = 0;
    FractureBakeResult finalBake;
    bool finalOk = false;
    if (baked0 && bake0.success) {
        finalBake = std::move(bake0);
        finalOk = true;
    } else {
        // ---- (c) 拒否/失敗されたら openMeshMode=1 (解像度 32) で焼く。成功し、全破片に
        //      有効な骨が割り当たることを確かめる ----
        FractureBakeInput in1 = in0;
        in1.openMeshMode = 1;
        in1.voxelResolution = 32;
        FractureBakeResult bake1;
        const bool baked1 = BakeFracture(in1, bake1);
        std::snprintf(buf, sizeof(buf), "%s: openMeshMode=1 (res=32) bake succeeds after rejection", label);
        check(baked1 && bake1.success, buf);
        if (baked1 && bake1.success) {
            finalBake = std::move(bake1);
            finalOk = true;
            usedOpenMeshMode = 1;
        }
    }
    if (!finalOk) {
        return;
    }

    if (!gotWeighted || skinVerts.empty()) {
        return; // ウェイトが取れていなければ骨割り当て以降は検査できない (上の check() で既に失敗計上済み)
    }

    // ---- 骨割り当て: 全破片に有効な骨が割り当たる ----
    std::vector<FractureSkinJoint> joints;
    joints.reserve(model->joints.size());
    for (const SkeletonJoint& j : model->joints) {
        joints.push_back({ j.name, j.inverseBind });
    }
    const FractureBakeResult bakeBeforeTransform = finalBake; // (d) の比較用 (変換前のコピー)
    const std::vector<std::string> boneNames =
        AssignFractureBonesAndTransform(finalBake, skinVerts, joints);
    int32_t invalidBoneCount = 0;
    for (const std::string& n : boneNames) {
        if (n.empty() || model->FindJointByName(n) < 0) {
            ++invalidBoneCount;
        }
    }
    std::snprintf(buf, sizeof(buf), "%s: every piece is assigned a valid bone (%zu pieces, %d invalid)",
                 label, boneNames.size(), invalidBoneCount);
    check(!boneNames.empty() && invalidBoneCount == 0, buf);

    // ---- (d) バインドポーズで骨に追従させた破片の外側面のワールド位置が、元のスキンメッシュの
    //      バインド位置と一致する (受け入れ条件 2)。骨空間変換で「破片の原点」は骨自身の原点へ
    //      再定義される (体積重心ではない — round 1 で確定済みの設計、変えない) ので、
    //      比較先は origin フィールドではなく頂点そのもの: 骨空間の頂点を、割り当てた骨の
    //      jointGlobal(バインド) で戻すと変換前の (ソース空間の) 絶対位置に一致するはず
    //      (このファイル前半の手続き生成テスト (2) と同じ式。あちらは自作の 2 骨、
    //      こちらは実アセットの骨階層で同じ式を検算する)
    {
        float maxErr = 0.0f;
        float maxScale = 1.0f; // 相対誤差の分母
        int32_t checkedCount = 0;
        for (size_t i = 0; i < finalBake.pieces.size() && i < boneNames.size(); ++i) {
            const int32_t jointIndex = model->FindJointByName(boneNames[i]);
            if (jointIndex < 0) {
                continue; // 上の check() で既に failure 計上済み
            }
            const XMMATRIX bindGlobal = ComputeJointGlobal(*model, /*clip=*/-1, 0.0f, jointIndex);
            const XMFLOAT3& beforeOrigin = bakeBeforeTransform.pieces[i].origin;
            auto checkMesh = [&](const FractureMesh& after, const FractureMesh& before) {
                for (size_t k = 0; k < after.verts.size() && k < before.verts.size(); ++k) {
                    const XMVECTOR afterPos = XMLoadFloat3(&after.verts[k].position);
                    XMFLOAT3 got;
                    XMStoreFloat3(&got, XMVector3TransformCoord(afterPos, bindGlobal));
                    const XMFLOAT3 want = { beforeOrigin.x + before.verts[k].position.x,
                                            beforeOrigin.y + before.verts[k].position.y,
                                            beforeOrigin.z + before.verts[k].position.z };
                    maxErr = (std::max)(maxErr, std::fabs(got.x - want.x));
                    maxErr = (std::max)(maxErr, std::fabs(got.y - want.y));
                    maxErr = (std::max)(maxErr, std::fabs(got.z - want.z));
                    maxScale =
                        (std::max)({ maxScale, std::fabs(want.x), std::fabs(want.y), std::fabs(want.z) });
                    ++checkedCount;
                }
            };
            checkMesh(finalBake.pieces[i].outer, bakeBeforeTransform.pieces[i].outer);
            checkMesh(finalBake.pieces[i].cap, bakeBeforeTransform.pieces[i].cap);
        }
        std::snprintf(buf, sizeof(buf),
                     "%s: bind-pose piece vertices match the pre-transform bake position (checked=%d "
                     "verts, max err=%.8f, tol=%.8f)",
                     label, checkedCount, static_cast<double>(maxErr),
                     static_cast<double>(1e-5f * maxScale));
        check(checkedCount > 0 && maxErr <= 1e-5f * maxScale, buf);
    }

    // ---- おまけ: 実アセットでも BuildFracturePieces が落ちずに正しい数の子を組む
    //      (kinematic root / PartComponent の配線自体は手続き生成の腕のテストで検証済み) ----
    {
        ConvexColliderLibrary colliders;
        FractureLibrary lib;
        colliders.Init(&resources);
        lib.Init(&resources, &colliders);
        fracturelib::Install(&lib);
        convexcol::Install(&colliders);

        const std::string prefix = std::string("fracture-selftest://realasset_") + label;
        const FractureAssetHandle* handle = lib.RegisterBaked(
            prefix, finalBake, HashStr(*meshName), in0.seed, in0.pieceCount, usedOpenMeshMode, 32,
            boneNames);
        if (handle != nullptr) {
            auto* destructible = w.GetComponent<DestructibleComponent>(skinned);
            if (destructible == nullptr) {
                destructible = w.AddComponent<DestructibleComponent>(skinned);
            }
            destructible->fractureAsset = AssetID{ HashStr(prefix) };
            const int built = BuildFracturePieces(w, skinned, *handle);
            std::snprintf(buf, sizeof(buf), "%s: BuildFracturePieces builds all %zu pieces without error",
                         label, handle->pieces.size());
            check(static_cast<size_t>(built) == handle->pieces.size(), buf);
        } else {
            std::snprintf(buf, sizeof(buf), "%s: RegisterBaked succeeds", label);
            check(false, buf);
        }

        fracturelib::Install(nullptr);
        convexcol::Install(nullptr);
    }
}

} // namespace

bool RunFractureSkinSelfTest()
{
    MYE_LOG_INFO("==== Fracture skin (M80j) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    constexpr float kHx = 0.5f, kHy = 1.0f, kHz = 0.5f;
    const SkinBoxSource box = MakeSkinBox(kHx, kHy, kHz);
    const SkinnedModel model = MakeArmSkinModel(kHy, XM_PIDIV4);
    const std::vector<XMFLOAT3> seeds = { { 0.0f, -kHy * 0.5f, 0.0f }, { 0.0f, kHy * 0.5f, 0.0f } };

    // ---- 1. 骨割り当て + 骨空間への変換 + 決定論 (受け入れ条件 1 相当・5) ----
    std::vector<std::string> boneNames;
    FractureBakeResult bake;
    {
        const FractureMesh src = ToFractureMesh(box.verts, box.indices);
        const bool baked = BakeFractureWithSeeds(src, seeds, 0.1f, bake);
        check(baked && bake.success && bake.pieces.size() == 2,
              "bake: the arm box splits into exactly 2 geometrically-closed pieces "
              "(BakeFractureCore rejects unclosed pieces, so success==true already covers it)");
        if (!baked || !bake.success || bake.pieces.size() != 2) {
            return false; // 以降は前提が崩れるので打ち切る
        }

        std::vector<FractureSkinJoint> joints;
        for (const SkeletonJoint& j : model.joints) {
            joints.push_back({ j.name, j.inverseBind });
        }
        // 変換前のコピー (絶対位置の比較用、下の (2))
        const FractureBakeResult bakeBeforeTransform = bake;

        boneNames = AssignFractureBonesAndTransform(bake, box.skinVerts, joints);
        check(boneNames.size() == 2 && boneNames[0] == "Bone0" && boneNames[1] == "Bone1",
              "bone assignment: the bottom seed's piece gets Bone0, the top seed's piece gets Bone1");
        check(bake.pieces[0].origin.x == 0.0f && bake.pieces[0].origin.y == 0.0f
                  && bake.pieces[0].origin.z == 0.0f && bake.pieces[1].origin.x == 0.0f
                  && bake.pieces[1].origin.y == 0.0f && bake.pieces[1].origin.z == 0.0f,
              "bone space: both pieces are re-based to their joint's own origin (0,0,0)");

        // ---- 2. バインドポーズの絶対位置が一致する (受け入れ条件 2) ----
        float maxErr = 0.0f;
        for (size_t p = 0; p < bake.pieces.size(); ++p) {
            const int32_t jointIndex = model.FindJointByName(boneNames[p]);
            check(jointIndex >= 0, "bone assignment: the assigned bone name resolves in the SkinnedModel");
            const XMMATRIX bindGlobal = ComputeJointGlobal(model, /*clip=*/-1, 0.0f, jointIndex);
            const XMFLOAT3& beforeOrigin = bakeBeforeTransform.pieces[p].origin;
            auto checkMesh = [&](const FractureMesh& after, const FractureMesh& before) {
                for (size_t k = 0; k < after.verts.size() && k < before.verts.size(); ++k) {
                    const XMVECTOR afterPos = XMLoadFloat3(&after.verts[k].position);
                    XMFLOAT3 got;
                    XMStoreFloat3(&got, XMVector3TransformCoord(afterPos, bindGlobal));
                    const XMFLOAT3 want = { beforeOrigin.x + before.verts[k].position.x,
                                            beforeOrigin.y + before.verts[k].position.y,
                                            beforeOrigin.z + before.verts[k].position.z };
                    maxErr = (std::max)(maxErr, std::fabs(got.x - want.x));
                    maxErr = (std::max)(maxErr, std::fabs(got.y - want.y));
                    maxErr = (std::max)(maxErr, std::fabs(got.z - want.z));
                }
            };
            checkMesh(bake.pieces[p].outer, bakeBeforeTransform.pieces[p].outer);
            checkMesh(bake.pieces[p].cap, bakeBeforeTransform.pieces[p].cap);
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "bind pose: bone-space vertex * jointGlobal(bind) recovers the pre-transform "
                      "source-space position (max err %.8f <= 1e-5)",
                      static_cast<double>(maxErr));
        check(maxErr <= 1e-5f, buf);
    }

    // ---- 3. 決定論: 同じ入力を 2 回焼いて骨を割り当てても .mfrac 相当のバイト列が一致する ----
    {
        const FractureMesh src = ToFractureMesh(box.verts, box.indices);
        std::vector<FractureSkinJoint> joints;
        for (const SkeletonJoint& j : model.joints) {
            joints.push_back({ j.name, j.inverseBind });
        }
        FractureBakeResult bakeA, bakeB;
        BakeFractureWithSeeds(src, seeds, 0.1f, bakeA);
        BakeFractureWithSeeds(src, seeds, 0.1f, bakeB);
        const std::vector<std::string> namesA = AssignFractureBonesAndTransform(bakeA, box.skinVerts, joints);
        const std::vector<std::string> namesB = AssignFractureBonesAndTransform(bakeB, box.skinVerts, joints);
        const FractureAsset::FractureData dataA
            = BuildFractureAssetData(bakeA, HashStr("selftest://skin_arm"), 1, 2, 0, 32, namesA);
        const FractureAsset::FractureData dataB
            = BuildFractureAssetData(bakeB, HashStr("selftest://skin_arm"), 1, 2, 0, 32, namesB);
        std::vector<uint8_t> bytesA, bytesB;
        FractureAsset::Serialize(dataA, bytesA);
        FractureAsset::Serialize(dataB, bytesB);
        check(!bytesA.empty() && bytesA == bytesB,
              "determinism: two independent bake+assign runs on the same input produce a "
              "byte-identical .mfrac payload (bone names included)");
    }

    // ---- 4/5. BuildFracturePieces (kinematic root + PartComponent) と、アニメ中の接触・分離 ----
    {
        RenderResources resources;
        ConvexColliderLibrary colliders;
        FractureLibrary lib;
        colliders.Init(&resources);
        lib.Init(&resources, &colliders);
        fracturelib::Install(&lib);
        convexcol::Install(&colliders);

        const AssetID meshId = resources.meshes.Register("skin_arm_mesh", box.verts, box.indices);
        const AssetID skinId = resources.skinnedModels.Register("skin_arm_model", model);

        const char* prefix = "fracture-selftest://skin_arm";
        const FractureAssetHandle* handle
            = lib.RegisterBaked(prefix, bake, HashStr("skin_arm_mesh"), 1, 2, 0, 32, boneNames);
        check(handle != nullptr && handle->pieces.size() == 2,
              "setup: the skin-baked asset registers with 2 pieces");

        Scene s;
        World& w = s.GetWorld();
        GameObject root = s.CreateGameObject("Arm");
        auto* mr = root.AddComponent<MeshRendererComponent>();
        mr->mesh = meshId;
        auto* sm = root.AddComponent<SkinnedMeshComponent>();
        sm->model = skinId;
        sm->clip = 0;
        sm->playing = false; // このテストでは timeTicks を手動で固定する
        auto* d = root.AddComponent<DestructibleComponent>();
        d->strength = 200.0f;
        d->fractureAsset = AssetID{ HashStr(prefix) };
        if (handle != nullptr) {
            BuildFracturePieces(w, root.Id(), *handle);
        }
        w.ApplyStructuralChanges();

        auto* rootRb = w.GetComponent<RigidbodyComponent>(root.Id());
        check(rootRb != nullptr && rootRb->isKinematic && rootRb->compoundColliders,
              "BuildFracturePieces: a SkinnedMesh root gets a kinematic compound Rigidbody");
        check(w.GetComponent<ColliderComponent>(root.Id()) == nullptr,
              "BuildFracturePieces: the root's own Collider is removed (pieces replace it)");

        const EntityID frag0 = FindPieceChild(w, root.Id(), 0);
        const EntityID frag1 = FindPieceChild(w, root.Id(), 1);
        check(!frag0.IsNull() && !frag1.IsNull(), "BuildFracturePieces: both fracture pieces exist");
        const auto* pc0 = w.GetComponent<PartComponent>(frag0);
        const auto* pc1 = w.GetComponent<PartComponent>(frag1);
        check(pc0 != nullptr && std::string(pc0->joint) == "Bone0" && pc0->source.IsNull(),
              "BuildFracturePieces: piece 0 gets PartComponent(joint=Bone0, source=null)");
        check(pc1 != nullptr && std::string(pc1->joint) == "Bone1" && pc1->source.IsNull(),
              "BuildFracturePieces: piece 1 gets PartComponent(joint=Bone1, source=null)");

        // ---- アニメ中 (非バインド姿勢) の追従を確認する ----
        // ★sm はこの後の AddComponent (Destructible / BuildFracturePieces 内の Rigidbody) で
        //   root のアーキタイプが動くたびに死ぬ。ここで取り直す
        //   (FractureBuilder.cpp 自身が守っている規約と同じ理由)
        auto* smNow = w.GetComponent<SkinnedMeshComponent>(root.Id());
        PartFollowSystem follow;
        smNow->timeTicks = 60; // クリップ長 1.0s ぶん = Bend の終端キー (45 度)
        follow.Update(w, resources);
        const auto* lt1AfterBend = w.GetComponent<LocalTransform>(frag1);
        const XMFLOAT4 kIdentityQuat{ 0, 0, 0, 1 };
        check(lt1AfterBend != nullptr
                  && std::memcmp(&lt1AfterBend->rotation, &kIdentityQuat, sizeof(XMFLOAT4)) != 0,
              "part follow: mid-animation, the bone-following piece's rotation differs from bind");

        // 現在の (曲がった) 姿勢での frag1 の凸包おおよその中心を、今の回転で運んだ先へ球を当てる
        // (アニメ再生中の当たり判定、受け入れ条件 3)。Bone1 のバインド位置 (0,+hy,0、mesh 空間)
        // は箱の上端そのものなので、Bone1 のピースの幾何 (元 mesh 空間で y∈[0,+hy]) は骨空間で
        // y∈[-hy,0] (骨原点の**下**) に来る — 中心はおよそ (0,-hy/2,0)
        XMFLOAT3 target{};
        {
            const XMFLOAT4& q = lt1AfterBend->rotation;
            float rx, ry, rz;
            QuatRotateVec(q.x, q.y, q.z, q.w, 0.0f, -kHy * 0.5f, 0.0f, rx, ry, rz);
            target = { rx + lt1AfterBend->position.x, ry + lt1AfterBend->position.y,
                      rz + lt1AfterBend->position.z };
        }

        // 半径は狙いのわずかなずれ (Debug/Release の下位ビット差) を吸収できるよう大きめにする —
        // 狙い先はピースの中心の目安に過ぎず、ピンポイントで当てる必要はない
        GameObject ball = s.CreateGameObjectTracked("Ball");
        ball.SetLocalPosition(target.x, target.y, target.z - 15.0f);
        auto* bcol = ball.AddComponent<ColliderComponent>();
        bcol->shape = collidershape::kSphere;
        bcol->radius = 1.0f;
        auto* brb = ball.AddComponent<RigidbodyComponent>();
        brb->mass = 20.0f;
        brb->gravityScale = 0.0f;
        brb->velocity = { 0.0f, 0.0f, 30.0f };
        w.ApplyStructuralChanges();

        PhysicsSystem phys;
        FractureSystem fsys;
        std::vector<ShapeImpulse> impulses;
        constexpr float kDt = 1.0f / 60.0f;
        for (int32_t tick = 0; tick < 90; ++tick) {
            follow.Update(w, resources); // 姿勢を固定したまま毎 tick 書き直す (実運用と同じ呼び順)
            phys.Update(w, kDt, nullptr, nullptr, &impulses);
            fsys.Update(w, kDt, impulses);
            w.ApplyStructuralChanges();
        }

        const auto* dAfter = w.GetComponent<DestructibleComponent>(root.Id());
        check(dAfter != nullptr && dAfter->detachedCount > 0,
              "fracture system: the ball hit knocks a piece off the animated skinned arm");

        // どちらが分かれたかは体積の同値タイブレークに依存し得るので、結果から判定する:
        // 分かれた方は Rigidbody を持ち PartComponent を失う。残った方は逆
        const bool frag0Detached = w.IsAlive(frag0) && w.GetComponent<RigidbodyComponent>(frag0) != nullptr;
        const bool frag1Detached = w.IsAlive(frag1) && w.GetComponent<RigidbodyComponent>(frag1) != nullptr;
        check(frag0Detached != frag1Detached, "fracture system: exactly one of the two pieces detaches");
        const EntityID detached = frag0Detached ? frag0 : frag1;
        const EntityID remaining = frag0Detached ? frag1 : frag0;
        check(w.GetComponent<PartComponent>(detached) == nullptr,
              "fracture system: the detached (now rigid) piece loses its PartComponent");
        if (w.IsAlive(remaining)) {
            check(w.GetComponent<PartComponent>(remaining) != nullptr,
                  "fracture system: the piece that stayed with the root keeps following its bone");
        }

        fracturelib::Install(nullptr);
        convexcol::Install(nullptr);
    }

    // ---- round 2 (planner VERDICT): 実アセットで TryLoadCookedMeshVertices・焼き・骨割り当て・
    //      バインドポーズの一致を確かめる。CookedCache を一時ディレクトリへ向けてから読み込む —
    //      .mmdl を書く/読むには CookedCache::Enabled() が要る (CookedCacheSelfTest.cpp と同じ流儀) ----
    // round 3 (planner VERDICT): 置き場を「固定名 + 開始時に remove_all」にしていたため、
    // Debug と Release の --selftest を同時に走らせると、速い側 (Release) がこの節へ先に入って
    // 同じディレクトリを remove_all し、遅い側 (Debug) が .mmdl を書いてから読み戻すまでの間に
    // 消してしまうことがあった (症状: Debug だけ・断続的)。置き場をプロセス ID 入りの名前にして
    // プロセス間で衝突しないようにする
    {
        MYE_LOG_INFO("---- real asset checks (CesiumMan.glb / skinned_beam.fbx) ----");
        wchar_t cookDirName[64];
        swprintf_s(cookDirName, L"mye_fractureskin_realasset_selftest_%d", _getpid());
        const std::filesystem::path cookDir = std::filesystem::temp_directory_path() / cookDirName;
        std::error_code ec;
        std::filesystem::remove_all(cookDir, ec); // 前回この PID が異常終了して残った分だけを掃除
        CookedCache::Configure(cookDir.wstring(), true);

        const std::wstring glbPath = std::filesystem::absolute(L"assets\\models\\CesiumMan.glb").wstring();
        const std::wstring fbxPath = std::filesystem::absolute(L"assets\\models\\skinned_beam.fbx").wstring();
        CheckRealSkinnedAsset(failCount, glbPath, "CesiumMan.glb", /*isFbx=*/false);
        CheckRealSkinnedAsset(failCount, fbxPath, "skinned_beam.fbx", /*isFbx=*/true);

        CookedCache::Configure(L"", false); // 他スイートへ漏らさない (CookedCacheSelfTest と同じ後始末)
        std::filesystem::remove_all(cookDir, ec); // 後始末 (プロセス固有名なので次回の remove_all に頼らず自分で消す)
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Fracture skin self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Fracture skin self test: %d FAILURE(S) ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
