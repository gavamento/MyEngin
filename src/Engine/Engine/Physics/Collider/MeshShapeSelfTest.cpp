//====================================================================================
//                          MeshShapeSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/19/2026
//                                          スキン付きメッシュの形 (コライダー・音) の回帰テストの実装
//====================================================================================
#include "Engine/Engine/Physics/Collider/MeshShapeSelfTest.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/FbxLoader.h"
#include "Engine/Engine/Asset/ModelCook.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Engine/Modal/ModalSoundLibrary.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/ConvexHull.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/MeshShape.h"
#include "Engine/Renderer/Mesh/SkinBounds.h"
#include "Engine/Renderer/Mesh/Skeleton.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kCmToM = 0.01f;
constexpr float kNearEps = 1e-4f;
constexpr float kCubeHalfCm = 50.0f;   // 1m の立方体
constexpr float kChildOffsetCm = 100.0f; // 子ジョイントの位置 (根から 1m 上)

// FBX の cm 単位のリグと同じ形: 根のジョイントに 0.01、子は 100cm 上、inverseBind = 0.01 × バインドの逆
// (ufbx の geometry_to_bone。バインドポーズのパレット inverseBind × jointGlobal が 0.01 そのものになる)
SkinnedModel MakeCmRig()
{
    SkinnedModel m;
    m.joints.resize(2);
    m.joints[0].bindS = { kCmToM, kCmToM, kCmToM };
    m.joints[1].parent = 0;
    m.joints[1].bindT = { 0.0f, kChildOffsetCm, 0.0f };
    std::vector<XMMATRIX> locals;
    ComputeJointLocals(m, -1, 0.0f, locals);
    for (int32_t j = 0; j < 2; ++j) {
        const XMMATRIX g = JointGlobalFromLocals(m, locals, j);
        XMStoreFloat4x4(&m.joints[static_cast<size_t>(j)].inverseBind,
                        XMMatrixMultiply(XMMatrixScaling(kCmToM, kCmToM, kCmToM), XMMatrixInverse(nullptr, g)));
    }
    return m;
}

// 中心 (0, 100cm, 0)・辺 100cm の立方体。全頂点を子ジョイント (bone) に重み 1 で結ぶ。
// unit はメートルに直した参照用 (重み無し) を作るときの倍率
void MakeCube(float unit, bool skinned, std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    verts.clear();
    for (int c = 0; c < 8; ++c) {
        MeshVertex v;
        v.position = { ((c & 1) ? kCubeHalfCm : -kCubeHalfCm) * unit,
                       (kChildOffsetCm + ((c & 2) ? kCubeHalfCm : -kCubeHalfCm)) * unit,
                       ((c & 4) ? kCubeHalfCm : -kCubeHalfCm) * unit };
        if (skinned) {
            v.boneIndices[0] = 1;
            v.boneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
        }
        verts.push_back(v);
    }
    // 6 面 × 2 三角形 (巻き順は凸包・ボクセル化に効かないので揃えるだけ)
    indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
}

bool Near(float a, float b)
{
    return std::fabs(a - b) <= kNearEps;
}

bool NearBox(const XMFLOAT3& lo, const XMFLOAT3& hi, const XMFLOAT3& elo, const XMFLOAT3& ehi)
{
    return Near(lo.x, elo.x) && Near(lo.y, elo.y) && Near(lo.z, elo.z) && Near(hi.x, ehi.x) && Near(hi.y, ehi.y)
        && Near(hi.z, ehi.z);
}

void BoxOf(const std::vector<XMFLOAT3>& pts, XMFLOAT3& lo, XMFLOAT3& hi)
{
    lo = pts.empty() ? XMFLOAT3{} : pts[0];
    hi = lo;
    for (const XMFLOAT3& p : pts) {
        lo = { std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z) };
        hi = { std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z) };
    }
}

} // namespace

bool RunMeshShapeSelfTest()
{
    MYE_LOG_INFO("==== Mesh shape self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  [PASS] %s", what);
        } else {
            MYE_LOG_ERROR("  [FAIL] %s", what);
            ++failCount;
        }
    };

    const SkinnedModel rig = MakeCmRig();
    std::vector<MeshVertex> cmVerts;
    std::vector<uint32_t> cubeIdx;
    MakeCube(1.0f, true, cmVerts, cubeIdx);
    std::vector<MeshVertex> mVerts;
    std::vector<uint32_t> mIdx;
    MakeCube(kCmToM, false, mVerts, mIdx);
    // 期待値: 中心 (0, 1, 0)・辺 1m
    const XMFLOAT3 expectLo = { -0.5f, 0.5f, -0.5f };
    const XMFLOAT3 expectHi = { 0.5f, 1.5f, 0.5f };

    // ---- 頂点の変換そのもの ----
    {
        std::vector<XMFLOAT3> pos;
        std::vector<MeshSkinVertex> skin;
        for (const MeshVertex& v : cmVerts) {
            pos.push_back(v.position);
            MeshSkinVertex s;
            std::memcpy(s.boneIndices, v.boneIndices, sizeof(s.boneIndices));
            s.boneWeights = v.boneWeights;
            skin.push_back(s);
        }
        // 重み 0 の頂点はシェーダと同じく恒等 (cm のまま)
        pos.push_back({ 7.0f, 8.0f, 9.0f });
        skin.push_back(MeshSkinVertex{});
        std::vector<XMFLOAT3> out;
        bool allNear = ComputeBindPosePositions(rig, pos, skin, out) && out.size() == pos.size();
        for (size_t i = 0; allNear && i < cmVerts.size(); ++i) {
            allNear = Near(out[i].x, mVerts[i].position.x) && Near(out[i].y, mVerts[i].position.y)
                && Near(out[i].z, mVerts[i].position.z);
        }
        check(allNear, "bind pose: a cm rig moves every weighted vertex to 1/100 (the size the renderer draws)");
        check(!out.empty() && out.back().x == 7.0f && out.back().y == 8.0f && out.back().z == 9.0f,
              "bind pose: a vertex without weights stays where it is (same as the shader)");
        std::vector<MeshSkinVertex> shortSkin(skin.begin(), skin.end() - 1);
        check(!ComputeBindPosePositions(rig, pos, shortSkin, out) && out.empty(),
              "bind pose: a skin array that does not match the vertex count is rejected");
    }

    RenderResources res; // Init 無し = CPU 側だけ (selftest 用のヘッドレス登録)
    const AssetID cmMesh = res.meshes.Register("meshshape#cm", cmVerts, cubeIdx);
    const AssetID rawMesh = res.meshes.Register("meshshape#raw", cmVerts, cubeIdx); // スキン未結合 (従来どおり)
    const AssetID meterMesh = res.meshes.Register("meshshape#meter", mVerts, mIdx);
    const AssetID skinId = res.skinnedModels.Register("meshshape#skin", rig);
    res.meshes.SetSkinModel(cmMesh, skinId);

    // ---- 凸包 (shape=5) ----
    {
        ConvexColliderLibrary conv;
        conv.Init(&res);
        const ConvexHullData* hull = conv.Get(cmMesh);
        check(hull != nullptr && NearBox(hull->aabbMin, hull->aabbMax, expectLo, expectHi),
              "convex: a hull of a cm skinned mesh has the rendered size (1m cube, not 100m)");
        const ConvexHullData* raw = conv.Get(rawMesh);
        check(raw != nullptr && Near(raw->aabbMax.y, kChildOffsetCm + kCubeHalfCm),
              "convex: a mesh without a skin still uses its vertices as they are");

        ConvexColliderLibrary again;
        again.Init(&res);
        const ConvexHullData* hull2 = again.Get(cmMesh);
        check(hull != nullptr && hull2 != nullptr && hull->verts.size() == hull2->verts.size()
                  && std::memcmp(hull->verts.data(), hull2->verts.data(), hull->verts.size() * sizeof(XMFLOAT3)) == 0,
              "convex: building the skinned hull twice gives the same bits");
    }

    // ---- スキンが後から登録される: 先に引いても 100 倍の形をキャッシュしない ----
    {
        const AssetID orphan = res.meshes.Register("meshshape#orphan", cmVerts, cubeIdx);
        const char* lateSkinKey = "meshshape#lateskin";
        res.meshes.SetSkinModel(orphan, AssetID{ HashStr(lateSkinKey) });
        ConvexColliderLibrary conv;
        conv.Init(&res);
        MeshColliderLibrary meshLib;
        meshLib.Init(&res);
        check(conv.Get(orphan) == nullptr && meshLib.Get(orphan) == nullptr,
              "late skin: while the skin is not registered no shape is built (and none is cached)");
        res.skinnedModels.Register(lateSkinKey, rig);
        const ConvexHullData* hull = conv.Get(orphan);
        check(hull != nullptr && NearBox(hull->aabbMin, hull->aabbMax, expectLo, expectHi),
              "late skin: once the skin is registered the hull has the rendered size");
    }

    // ---- メッシュコライダー (shape=3) ----
    {
        MeshColliderLibrary meshLib;
        meshLib.Init(&res);
        const MeshColliderData* md = meshLib.Get(cmMesh);
        XMFLOAT3 lo = {};
        XMFLOAT3 hi = {};
        if (md != nullptr) {
            BoxOf(md->positions, lo, hi);
        }
        check(md != nullptr && md->TriCount() == 12 && NearBox(lo, hi, expectLo, expectHi),
              "mesh collider: the triangles of a cm skinned mesh have the rendered size");
    }

    // ---- ModalSound: 焼く形 (ボクセル化の枠) が描画の大きさ ----
    {
        ModalSoundLibrary lib;
        lib.Init(&res);
        const bool ready = lib.SetBackendByName("cpu")
            && lib.LoadModel(FindEngineRepoRoot() + L"\\tests\\deepmodal\\fixture.dmnet");
        check(ready, "modal: the fixture model is loaded");
        if (ready) {
            const bool bakedSkinned = lib.BakeSync(cmMesh);
            const bool bakedMeter = lib.BakeSync(meterMesh);
            const ModalFeatureMap* skinnedMap = lib.Get(cmMesh);
            const ModalFeatureMap* meterMap = lib.Get(meterMesh);
            bool sameFrame = bakedSkinned && bakedMeter && skinnedMap != nullptr && meterMap != nullptr;
            for (int a = 0; sameFrame && a < 3; ++a) {
                sameFrame = Near(skinnedMap->frame.aabbMin[a], meterMap->frame.aabbMin[a])
                    && Near(skinnedMap->frame.aabbMax[a], meterMap->frame.aabbMax[a]);
            }
            check(sameFrame, "modal: a cm skinned mesh is baked in the same frame as the same cube in meters");
        }
        lib.Shutdown();
    }

    // ---- クックの往復: メッシュとスキンの結び付きが残る ----
    {
        ModelCook::ModelCookData cook;
        cook.AddMesh("meshshape#cm", cmVerts, cubeIdx);
        cook.AddMesh("meshshape#meter", mVerts, mIdx);
        cook.AddSkin("meshshape#skin", rig);
        cook.SetMeshSkin("meshshape#cm", "meshshape#skin");
        std::vector<uint8_t> blob;
        ModelCook::Serialize(cook, blob);
        ModelCook::ModelCookData back;
        const bool parsed = ModelCook::Deserialize(blob, back);
        check(parsed && back.meshes.size() == 2 && back.meshes[0].skinKey == "meshshape#skin"
                  && back.meshes[1].skinKey.empty(),
              "cook: the skin key of each mesh survives Serialize / Deserialize");
        RenderResources replayed;
        ShaderManager shaders;
        if (parsed) {
            ModelCook::Replay(replayed, shaders, back);
        }
        const Mesh* linked = replayed.meshes.Get(cmMesh);
        const Mesh* plain = replayed.meshes.Get(meterMesh);
        check(linked != nullptr && linked->skinModel == skinId && plain != nullptr && plain->skinModel.IsNull(),
              "cook: Replay links the mesh to its skin the same way the fresh load does");
    }

    // ---- 実アセット: FBX はスキン付きの部品がすべて自分のスキンに結ばれ、凸包が描画の箱に収まる ----
    {
        const std::wstring path =
            std::filesystem::absolute(L"assets\\models\\player_fp_v01\\Player_Researcher_FP.fbx").wstring();
        RegisterBuiltinComponents(); // sTypeId 解決 (冪等)
        Scene scene;
        RenderResources fbxRes;
        ShaderManager shaders;
        FbxLoader::Load(scene, fbxRes, shaders, path);
        World& world = scene.GetWorld();
        world.ApplyStructuralChanges();
        int parts = 0;
        int linkedParts = 0;
        int hullInside = 0;
        ConvexColliderLibrary conv;
        conv.Init(&fbxRes);
        const ComponentTypeId req[] = { SkinnedMeshComponent::sTypeId, MeshRendererComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(SkinnedMeshComponent::sTypeId);
            const int mi = arch.FindTypeIndex(MeshRendererComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* sm = static_cast<const SkinnedMeshComponent*>(arch.GetPtr(si, row));
                const auto* mr = static_cast<const MeshRendererComponent*>(arch.GetPtr(mi, row));
                const Mesh* mesh = fbxRes.meshes.Get(mr->mesh);
                const SkinnedModel* model = fbxRes.skinnedModels.Get(sm->model);
                if (mesh == nullptr || model == nullptr) {
                    continue;
                }
                ++parts;
                if (mesh->skinModel == sm->model) {
                    ++linkedParts;
                }
                // 描画のバインドポーズの箱 (エディタの選択枠と同じ計算) に凸包が収まる
                std::vector<XMMATRIX> bindLocals;
                ComputeJointLocals(*model, -1, 0.0f, bindLocals);
                SkinnedLocalAabb box;
                const ConvexHullData* hull = conv.Get(mr->mesh);
                if (hull != nullptr && ComputeSkinnedPoseAabb(*model, mesh->positions, mesh->skin, bindLocals, box)
                    && hull->aabbMin.x >= box.min.x - kNearEps && hull->aabbMin.y >= box.min.y - kNearEps
                    && hull->aabbMin.z >= box.min.z - kNearEps && hull->aabbMax.x <= box.max.x + kNearEps
                    && hull->aabbMax.y <= box.max.y + kNearEps && hull->aabbMax.z <= box.max.z + kNearEps) {
                    ++hullInside;
                }
            }
        });
        MYE_LOG_INFO("  [info] fbx: %d skinned parts, %d linked, %d hulls inside the bind-pose box", parts,
                     linkedParts, hullInside);
        check(parts > 0 && linkedParts == parts,
              "fbx: every skinned part of Player_Researcher_FP.fbx is linked to the skin its entity plays");
        check(parts > 0 && hullInside == parts,
              "fbx: every skinned part's hull fits the box the renderer draws in the bind pose");
    }

    // ---- 実アセット: glTF は結ばない (非ジョイントの祖先をエンティティが持つ。従来どおり) ----
    {
        const std::wstring path = std::filesystem::absolute(L"assets\\models\\CesiumMan.glb").wstring();
        RenderResources gltfRes;
        ShaderManager shaders;
        const bool loaded = ModelLoader::RegisterAssets(gltfRes, shaders, path, false);
        int skinnedMeshes = 0;
        int linked = 0;
        for (const AssetEntry& e : gltfRes.meshes.Enumerate()) {
            const Mesh* m = gltfRes.meshes.Get(e.id);
            if (m != nullptr && !m->skin.empty()) {
                ++skinnedMeshes;
                linked += m->skinModel.IsNull() ? 0 : 1;
            }
        }
        check(loaded && skinnedMeshes > 0 && linked == 0, "gltf: skinned meshes are not linked (shape is unchanged)");
    }

    MYE_LOG_INFO("Mesh shape self test: %s (%d failures)", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
