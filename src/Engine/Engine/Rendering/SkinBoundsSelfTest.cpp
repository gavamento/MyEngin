//====================================================================================
//                          SkinBoundsSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          スキンの保守的 AABB の回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/SkinBoundsSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Asset/ModelLoader.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/SkinBounds.h"
#include "Engine/Renderer/Mesh/Skeleton.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kInsideEpsilon = 1e-4f;

XMFLOAT4 Quat(float x, float y, float z, float angleRad)
{
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationAxis(XMVectorSet(x, y, z, 0.0f), angleRad));
    return q;
}

// 3 ジョイントの鎖 (根 → 中 → 先、間隔 1m)。バインドは直立
SkinnedModel MakeChainModel()
{
    SkinnedModel m;
    m.joints.resize(3);
    for (int j = 0; j < 3; ++j) {
        SkeletonJoint& jt = m.joints[static_cast<size_t>(j)];
        jt.parent = j - 1;
        jt.bindT = { 0.0f, j == 0 ? 0.0f : 1.0f, 0.0f };
        const XMMATRIX bindGlobal = XMMatrixTranslation(0.0f, static_cast<float>(j), 0.0f);
        XMStoreFloat4x4(&jt.inverseBind, XMMatrixInverse(nullptr, bindGlobal));
    }
    // 振り: 中が大きく曲がって戻り、根が横へ動く
    SkeletalClip swing;
    swing.name = "swing";
    swing.duration = 2.0f;
    swing.tracks.resize(3);
    swing.tracks[1].rTimes = { 0.0f, 1.0f, 2.0f };
    swing.tracks[1].rVals = { Quat(0, 0, 1, 0.0f), Quat(0, 0, 1, XMConvertToRadians(120.0f)), Quat(0, 0, 1, 0.0f) };
    swing.tracks[0].tTimes = { 0.0f, 2.0f };
    swing.tracks[0].tVals = { { 0.0f, 0.0f, 0.0f }, { 5.0f, 0.0f, 0.0f } };
    // 回し: 先がキー 2 つの間で 170 度回る (弧の途中が両端より外へ出る)
    SkeletalClip spin;
    spin.name = "spin";
    spin.duration = 1.0f;
    spin.tracks.resize(3);
    spin.tracks[1].rTimes = { 0.0f, 1.0f };
    spin.tracks[1].rVals = { Quat(1, 0, 0, 0.0f), Quat(1, 0, 0, XMConvertToRadians(170.0f)) };
    spin.tracks[2].rTimes = { 0.0f, 1.0f };
    spin.tracks[2].rVals = { Quat(0, 1, 0, 0.0f), Quat(0, 1, 0, XMConvertToRadians(170.0f)) };
    m.clips.push_back(swing);
    m.clips.push_back(spin);
    return m;
}

// 各ジョイントの周りの箱 + ジョイント間の頂点 (重みを半々) + 重みの無い頂点 1 個
void MakeChainMesh(std::vector<MeshVertex>& out)
{
    out.clear();
    const auto add = [&](float x, float y, float z, uint8_t b0, float w0, uint8_t b1, float w1) {
        MeshVertex v;
        v.position = { x, y, z };
        v.boneIndices[0] = b0;
        v.boneIndices[1] = b1;
        v.boneWeights = { w0, w1, 0.0f, 0.0f };
        out.push_back(v);
    };
    for (uint8_t b = 0; b < 3; ++b) {
        for (int c = 0; c < 8; ++c) {
            add((c & 1) ? 0.3f : -0.3f, static_cast<float>(b) + ((c & 2) ? 0.3f : -0.3f), (c & 4) ? 0.3f : -0.3f, b, 1.0f,
                0, 0.0f);
        }
    }
    add(0.2f, 0.5f, 0.0f, 0, 0.5f, 1, 0.5f);
    add(-0.2f, 1.5f, 0.1f, 1, 0.5f, 2, 0.5f);
    add(0.0f, 2.8f, 0.0f, 0, 0.0f, 0, 0.0f); // 重み 0 = 恒等で描かれる
}

void Split(const std::vector<MeshVertex>& verts, std::vector<XMFLOAT3>& pos, std::vector<MeshSkinVertex>& skin)
{
    pos.clear();
    skin.clear();
    for (const MeshVertex& v : verts) {
        pos.push_back(v.position);
        MeshSkinVertex s;
        std::memcpy(s.boneIndices, v.boneIndices, sizeof(s.boneIndices));
        s.boneWeights = v.boneWeights;
        skin.push_back(s);
    }
}

// palette (転置済み) で全頂点をスキニングし、箱の外へ出た頂点数を返す。シェーダと同じ式 (重み和 1e-4 以下は恒等)
int CountOutside(const std::vector<XMFLOAT3>& pos, const std::vector<MeshSkinVertex>& skin,
                 const std::vector<XMFLOAT4X4>& palette, const SkinnedLocalAabb& box)
{
    int outside = 0;
    for (size_t i = 0; i < pos.size(); ++i) {
        const MeshSkinVertex& s = skin[i];
        const float w[4] = { s.boneWeights.x, s.boneWeights.y, s.boneWeights.z, s.boneWeights.w };
        XMVECTOR p = XMLoadFloat3(&pos[i]);
        if (w[0] + w[1] + w[2] + w[3] > 1e-4f) {
            XMVECTOR acc = XMVectorZero();
            for (int k = 0; k < 4; ++k) {
                if (s.boneIndices[k] >= palette.size()) {
                    continue;
                }
                const XMMATRIX m = XMMatrixTranspose(XMLoadFloat4x4(&palette[s.boneIndices[k]]));
                acc = XMVectorAdd(acc, XMVectorScale(XMVector3TransformCoord(p, m), w[k]));
            }
            p = acc;
        }
        XMFLOAT3 q;
        XMStoreFloat3(&q, p);
        if (q.x < box.min.x - kInsideEpsilon || q.y < box.min.y - kInsideEpsilon || q.z < box.min.z - kInsideEpsilon
            || q.x > box.max.x + kInsideEpsilon || q.y > box.max.y + kInsideEpsilon
            || q.z > box.max.z + kInsideEpsilon) {
            ++outside;
        }
    }
    return outside;
}

// 全クリップを 1 tick (1/60 秒) ごとに末尾まで評価して、箱の外へ出た頂点の延べ数と評価した姿勢数を返す
void SweepClips(const SkinnedModel& model, const std::vector<XMFLOAT3>& pos, const std::vector<MeshSkinVertex>& skin,
                const SkinnedLocalAabb& box, int& outsideTotal, int& poses)
{
    outsideTotal = 0;
    poses = 0;
    std::vector<XMFLOAT4X4> palette;
    for (size_t c = 0; c < model.clips.size(); ++c) {
        const int ticks = static_cast<int>(std::ceil(model.clips[c].duration * 60.0f)) + 1;
        for (int k = 0; k <= ticks; ++k) {
            ComputeBonePalette(model, static_cast<int>(c), static_cast<float>(k) / 60.0f, palette);
            outsideTotal += CountOutside(pos, skin, palette, box);
            ++poses;
        }
    }
}

bool SameBox(const SkinnedLocalAabb& a, const SkinnedLocalAabb& b)
{
    return std::memcmp(&a, &b, sizeof(SkinnedLocalAabb)) == 0;
}

} // namespace

bool RunSkinBoundsSelfTest()
{
    MYE_LOG_INFO("==== Skin bounds self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  [PASS] %s", what);
        } else {
            MYE_LOG_ERROR("  [FAIL] %s", what);
            ++failCount;
        }
    };

    // ---- 手組みの鎖: 全クリップの全 tick + クリップ間のブレンドで全頂点が箱の中 ----
    const SkinnedModel model = MakeChainModel();
    std::vector<MeshVertex> verts;
    MakeChainMesh(verts);
    std::vector<XMFLOAT3> pos;
    std::vector<MeshSkinVertex> skin;
    Split(verts, pos, skin);

    SkinnedLocalAabb box;
    check(ComputeSkinnedLocalAabb(model, pos, skin, box), "chain: the bounds are computed");
    int outside = -1;
    int poses = 0;
    SweepClips(model, pos, skin, box, outside, poses);
    check(poses > 100 && outside == 0, "chain: every vertex of every tick of every clip is inside the box");

    // 動かない箱 (バインドポーズだけの AABB) は、この動きでは頂点を取りこぼす = テストが締まっていること
    {
        SkinnedLocalAabb bindOnly;
        bindOnly.min = bindOnly.max = pos[0];
        for (const XMFLOAT3& p : pos) {
            bindOnly.min = { std::min(bindOnly.min.x, p.x), std::min(bindOnly.min.y, p.y), std::min(bindOnly.min.z, p.z) };
            bindOnly.max = { std::max(bindOnly.max.x, p.x), std::max(bindOnly.max.y, p.y), std::max(bindOnly.max.z, p.z) };
        }
        int bindOutside = 0;
        int ignored = 0;
        SweepClips(model, pos, skin, bindOnly, bindOutside, ignored);
        check(bindOutside > 0, "chain: the bind-pose box alone would miss vertices (the sweep is not vacuous)");
    }

    // クリップ A の途中とクリップ B の途中の 50% ブレンド (キーフレームに無い姿勢) も余白に収まる
    {
        int blendOutside = 0;
        std::vector<XMMATRIX> locals;
        std::vector<XMFLOAT4X4> palette;
        for (int a = 0; a <= 120; a += 4) {
            for (int b = 0; b <= 60; b += 4) {
                ComputeJointLocalsBlended(model, 0, static_cast<float>(a) / 60.0f, 1, static_cast<float>(b) / 60.0f,
                                          0.5f, locals);
                ComputeBonePaletteWithOverrides(model, locals, {}, {}, palette);
                blendOutside += CountOutside(pos, skin, palette, box);
            }
        }
        check(blendOutside == 0, "chain: cross-fade poses between two clips stay inside the margin");
    }

    // 決定的: 同じ入力から同じバイト列
    {
        SkinnedLocalAabb again;
        check(ComputeSkinnedLocalAabb(model, pos, skin, again) && SameBox(box, again),
              "chain: the same input gives the same box byte for byte");
    }

    // 重みの無い頂点はバインド位置のまま含まれる (箱を縮めて頂点を落とさない)
    check(box.max.y >= 2.8f, "chain: a vertex with no weight is kept at its bind position");

    // ---- 重みを持たないメッシュ = 頂点の AABB (余白なし) ----
    {
        std::vector<MeshSkinVertex> none;
        SkinnedLocalAabb rigidBox;
        const bool ok = ComputeSkinnedLocalAabb(model, pos, none, rigidBox);
        check(ok && std::fabs(rigidBox.max.y - 2.8f) < 1e-6f && std::fabs(rigidBox.min.y - (-0.3f)) < 1e-6f
                  && std::fabs(rigidBox.max.x - 0.3f) < 1e-6f,
              "rigid: a mesh without skin weights is just its vertex AABB");
    }

    // ---- 壊れた入力 ----
    {
        std::vector<MeshSkinVertex> shortSkin(skin.begin(), skin.begin() + 3);
        SkinnedLocalAabb ignored;
        check(!ComputeSkinnedLocalAabb(model, pos, shortSkin, ignored),
              "invalid: a skin attribute list of the wrong length is refused");
        check(!ComputeSkinnedLocalAabb(model, {}, {}, ignored), "invalid: an empty mesh is refused");
        // 存在しないボーンへの影響は無視する (落ちない・その分の頂点は箱に入らないだけ)
        std::vector<MeshSkinVertex> wild = skin;
        wild[0].boneIndices[0] = 200;
        check(ComputeSkinnedLocalAabb(model, pos, wild, ignored), "invalid: a bone index past the skeleton is ignored");
    }

    // ---- キャッシュ: 組ごとに 1 回だけ計算し、再登録で計算し直す ----
    {
        RenderResources res; // デバイス無し (CPU 側だけの登録)
        const AssetID skinId = res.skinnedModels.Register("sb_chain", model);
        const AssetID meshId = res.meshes.Register("sb_chain_mesh", verts, std::vector<uint32_t>{ 0, 1, 2 });
        SkinBoundsCache cache;
        const SkinnedLocalAabb* first = cache.Get(res.skinnedModels, skinId, res.meshes, meshId);
        check(first != nullptr && SameBox(*first, box), "cache: the box of a registered pair equals the direct result");
        const SkinnedLocalAabb firstCopy = first != nullptr ? *first : SkinnedLocalAabb{};
        cache.Get(res.skinnedModels, skinId, res.meshes, meshId);
        check(cache.ComputedCount() == 1, "cache: a second lookup does not recompute");

        std::vector<MeshVertex> farther = verts;
        MeshVertex v;
        v.position = { 0.0f, 0.0f, 9.0f };
        v.boneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
        farther.push_back(v);
        res.meshes.Register("sb_chain_mesh", farther, std::vector<uint32_t>{ 0, 1, 2 }); // 同名の再登録 = ホットリロード
        const SkinnedLocalAabb* second = cache.Get(res.skinnedModels, skinId, res.meshes, meshId);
        check(second != nullptr && cache.ComputedCount() == 2 && second->max.z > firstCopy.max.z + 1.0f,
              "cache: re-registering the mesh recomputes the box");

        res.skinnedModels.Register("sb_chain", model);
        cache.Get(res.skinnedModels, skinId, res.meshes, meshId);
        check(cache.ComputedCount() == 3, "cache: re-registering the skeleton recomputes the box");

        check(cache.Get(res.skinnedModels, AssetID{ 12345 }, res.meshes, meshId) == nullptr
                  && cache.Get(res.skinnedModels, skinId, res.meshes, AssetID{ 54321 }) == nullptr,
              "cache: an unregistered model or mesh has no box (the caller keeps it always visible)");
    }

    // ---- 実アセット: 全クリップの全 tick で全頂点が箱の中 ----
    for (const wchar_t* file : { L"assets\\models\\anim_test.glb", L"assets\\models\\CesiumMan.glb" }) {
        const std::wstring path = std::filesystem::absolute(file).wstring();
        const std::string label = std::filesystem::path(file).filename().string();
        RenderResources res;
        ShaderManager shaders;
        if (!ModelLoader::RegisterAssets(res, shaders, path, false)) {
            check(false, (label + ": the asset is registered headlessly").c_str());
            continue;
        }
        const std::vector<SkinnedModelEntry> skins = res.skinnedModels.Enumerate();
        AssetID skinnedMesh = {};
        for (const AssetEntry& e : res.meshes.Enumerate()) {
            const Mesh* m = res.meshes.Get(e.id);
            if (m != nullptr && !m->skin.empty()) {
                skinnedMesh = e.id;
                break;
            }
        }
        check(skins.size() == 1 && !skinnedMesh.IsNull(), (label + ": one skeleton and one skinned mesh are registered").c_str());
        if (skins.size() != 1 || skinnedMesh.IsNull()) {
            continue;
        }
        SkinBoundsCache cache;
        const SkinnedModel* sm = res.skinnedModels.Get(AssetID{ skins[0].hash });
        const SkinnedLocalAabb* real = cache.Get(res.skinnedModels, AssetID{ skins[0].hash }, res.meshes, skinnedMesh);
        check(real != nullptr, (label + ": the box is computed").c_str());
        if (real == nullptr || sm == nullptr) {
            continue;
        }
        const Mesh* mesh = res.meshes.Get(skinnedMesh);
        int realOutside = -1;
        int realPoses = 0;
        SweepClips(*sm, mesh->positions, mesh->skin, *real, realOutside, realPoses);
        check(realPoses > 0 && realOutside == 0,
              (label + ": every vertex of every tick of every clip is inside the box").c_str());
        const float bindExtent = mesh->aabbMax.y - mesh->aabbMin.y;
        const float boxExtent = real->max.y - real->min.y;
        MYE_LOG_INFO("  [info] %s: %d clips, %d poses, bind height %.3f, box height %.3f (%.2fx)", label.c_str(),
                     static_cast<int>(sm->clips.size()), realPoses, bindExtent, boxExtent,
                     bindExtent > 0.0f ? boxExtent / bindExtent : 0.0f);
    }

    MYE_LOG_INFO("Skin bounds self test: %s (%d failures)", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
