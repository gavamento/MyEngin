//====================================================================================
//                          SkinBounds.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          スキンメッシュの保守的 AABB の計算とキャッシュ
//====================================================================================
#include "Engine/Renderer/Mesh/SkinBounds.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace mye {
namespace {

// キーの間をこの刻みより粗く空けない (回転補間の弧が両端より外へ膨らむ分を拾う)
constexpr float kMaxSampleStepSec = 1.0f / 30.0f;
// 1 クリップのサンプル数の上限 (これを超える極端に長いクリップは間引く。余白で吸収する)
constexpr size_t kMaxSamplesPerClip = 8192;
constexpr float kMarginRatio = 0.5f;
constexpr float kMinMarginM = 0.05f;
// shadow_depth_skinned.hlsl / forward_skinned.hlsl と同じ「スキニングする頂点」の閾値
constexpr float kWeightSum = 1e-4f;

struct Box {
    XMFLOAT3 lo = { FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool Empty() const { return lo.x > hi.x; }
    void Extend(const XMFLOAT3& p)
    {
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
    }
};

bool IsFinite(const XMFLOAT3& p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// 親を持つ前提でジョイントのグローバル行列を 1 回の走査で作る (親が子より後ろに居てもよい)
void ComputeGlobals(const SkinnedModel& model, const std::vector<XMMATRIX>& locals,
                    std::vector<XMMATRIX>& globals, std::vector<uint8_t>& done, std::vector<size_t>& chain)
{
    const size_t n = model.joints.size();
    globals.resize(n);
    done.assign(n, 0);
    for (size_t j = 0; j < n; ++j) {
        chain.clear();
        int32_t p = static_cast<int32_t>(j);
        while (p >= 0 && static_cast<size_t>(p) < n && done[static_cast<size_t>(p)] == 0 && chain.size() <= n) {
            chain.push_back(static_cast<size_t>(p));
            p = model.joints[static_cast<size_t>(p)].parent;
        }
        for (size_t k = chain.size(); k-- > 0;) {
            const size_t idx = chain[k];
            const int32_t parent = model.joints[idx].parent;
            const bool hasParent = parent >= 0 && static_cast<size_t>(parent) < n && done[static_cast<size_t>(parent)] != 0;
            globals[idx] = hasParent ? XMMatrixMultiply(locals[idx], globals[static_cast<size_t>(parent)]) : locals[idx];
            done[idx] = 1;
        }
    }
}

// クリップ 1 本のサンプル時刻: 全トラックのキー時刻 + 0 + 末尾、キーの間は kMaxSampleStepSec 以下に刻む
void CollectSampleTimes(const SkeletalClip& clip, std::vector<float>& times)
{
    times.clear();
    times.push_back(0.0f);
    times.push_back(clip.duration);
    for (const JointTrack& tr : clip.tracks) {
        times.insert(times.end(), tr.tTimes.begin(), tr.tTimes.end());
        times.insert(times.end(), tr.rTimes.begin(), tr.rTimes.end());
        times.insert(times.end(), tr.sTimes.begin(), tr.sTimes.end());
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    std::vector<float> dense;
    dense.reserve(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        dense.push_back(times[i]);
        if (i + 1 == times.size()) {
            break;
        }
        const float gap = times[i + 1] - times[i];
        if (gap > kMaxSampleStepSec) {
            const int32_t steps = static_cast<int32_t>(std::ceil(gap / kMaxSampleStepSec));
            for (int32_t s = 1; s < steps; ++s) {
                dense.push_back(times[i] + gap * static_cast<float>(s) / static_cast<float>(steps));
            }
        }
    }
    if (dense.size() > kMaxSamplesPerClip) {
        const size_t stride = (dense.size() + kMaxSamplesPerClip - 1) / kMaxSamplesPerClip;
        times.clear();
        for (size_t i = 0; i < dense.size(); i += stride) {
            times.push_back(dense[i]);
        }
        if (times.back() != dense.back()) {
            times.push_back(dense.back());
        }
    } else {
        times.swap(dense);
    }
}

} // namespace

bool ComputeSkinnedLocalAabb(const SkinnedModel& model, const std::vector<XMFLOAT3>& positions,
                             const std::vector<MeshSkinVertex>& skin, SkinnedLocalAabb& out)
{
    if (positions.empty()) {
        return false;
    }
    if (!skin.empty() && skin.size() != positions.size()) {
        return false;
    }

    const size_t jointCount = model.joints.size();
    std::vector<Box> envelope(jointCount); // ボーンごとの頂点の箱 (バインド空間)
    Box rigid;                             // どのボーンにも結ばれない頂点 (恒等で描かれる)
    if (skin.empty()) {
        for (const XMFLOAT3& p : positions) {
            rigid.Extend(p);
        }
    } else {
        for (size_t i = 0; i < positions.size(); ++i) {
            const MeshSkinVertex& sv = skin[i];
            const float w[4] = { sv.boneWeights.x, sv.boneWeights.y, sv.boneWeights.z, sv.boneWeights.w };
            if (w[0] + w[1] + w[2] + w[3] <= kWeightSum) {
                rigid.Extend(positions[i]);
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                // 存在しないボーンの影響はパレットに無いので境界に入れられない (描画側の上限切り捨てと同じ扱い)
                if (w[k] > 0.0f && sv.boneIndices[k] < jointCount) {
                    envelope[sv.boneIndices[k]].Extend(positions[i]);
                }
            }
        }
    }

    Box total = rigid;
    bool anyBone = false;
    for (const Box& e : envelope) {
        anyBone = anyBone || !e.Empty();
    }
    if (anyBone) {
        std::vector<XMMATRIX> locals;
        std::vector<XMMATRIX> globals;
        std::vector<uint8_t> done;
        std::vector<size_t> chain;
        std::vector<float> times;
        const auto samplePose = [&](int32_t clip, float timeSec) {
            ComputeJointLocals(model, clip, timeSec, locals);
            ComputeGlobals(model, locals, globals, done, chain);
            for (size_t b = 0; b < jointCount; ++b) {
                if (envelope[b].Empty()) {
                    continue;
                }
                // 頂点 * inverseBind * jointGlobal = パレット (転置前) の行列。箱の 8 隅を送れば箱の像を包める
                const XMMATRIX m = XMMatrixMultiply(XMLoadFloat4x4(&model.joints[b].inverseBind), globals[b]);
                for (int c = 0; c < 8; ++c) {
                    const XMVECTOR corner = XMVectorSet((c & 1) ? envelope[b].hi.x : envelope[b].lo.x,
                                                        (c & 2) ? envelope[b].hi.y : envelope[b].lo.y,
                                                        (c & 4) ? envelope[b].hi.z : envelope[b].lo.z, 1.0f);
                    XMFLOAT3 moved;
                    XMStoreFloat3(&moved, XMVector3TransformCoord(corner, m));
                    total.Extend(moved);
                }
            }
        };
        samplePose(-1, 0.0f); // クリップ無し = バインドポーズ
        for (size_t c = 0; c < model.clips.size(); ++c) {
            CollectSampleTimes(model.clips[c], times);
            for (const float t : times) {
                samplePose(static_cast<int32_t>(c), t);
            }
        }
    }

    if (total.Empty() || !IsFinite(total.lo) || !IsFinite(total.hi)) {
        return false;
    }
    float pad = 0.0f;
    if (anyBone) {
        const float extent = std::max({ total.hi.x - total.lo.x, total.hi.y - total.lo.y, total.hi.z - total.lo.z });
        pad = std::max(extent * kMarginRatio, kMinMarginM);
    }
    out.min = { total.lo.x - pad, total.lo.y - pad, total.lo.z - pad };
    out.max = { total.hi.x + pad, total.hi.y + pad, total.hi.z + pad };
    return true;
}

const SkinnedLocalAabb* SkinBoundsCache::Get(const SkinnedModelLibrary& models, AssetID model,
                                             MeshLibrary& meshes, AssetID mesh)
{
    const SkinnedModel* sm = models.Get(model);
    const Mesh* me = meshes.Get(mesh);
    if (sm == nullptr || me == nullptr) {
        return nullptr;
    }
    Entry& e = entries_[{ model.value, mesh.value }];
    if (e.modelRevision != sm->revision || e.meshRevision != me->revision) {
        e.modelRevision = sm->revision;
        e.meshRevision = me->revision;
        e.valid = ComputeSkinnedLocalAabb(*sm, me->positions, me->skin, e.box);
        ++computed_;
    }
    return e.valid ? &e.box : nullptr;
}

} // namespace mye
