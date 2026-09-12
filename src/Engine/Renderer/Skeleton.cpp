#include "Engine/Renderer/Skeleton.h"

#include <algorithm>

#include "Engine/Core/Hash.h"

using namespace DirectX;

namespace mye {

AssetID SkinnedModelLibrary::Register(std::string_view name, SkinnedModel model)
{
    const AssetID id{ HashStr(name) };
    models_[id.value] = std::move(model);
    names_[id.value] = std::string(name);
    return id;
}

const SkinnedModel* SkinnedModelLibrary::Get(AssetID id) const
{
    auto it = models_.find(id.value);
    return (it != models_.end()) ? &it->second : nullptr;
}

// GpuResources.cpp の EnumerateNames と同じ形 (名前順で固定する — unordered_map の
// 走査順は不定で、そのまま返すとピッカーを開くたびに並びが変わる)
std::vector<SkinnedModelEntry> SkinnedModelLibrary::Enumerate() const
{
    std::vector<SkinnedModelEntry> out;
    out.reserve(names_.size());
    for (const auto& [hash, name] : names_) {
        out.push_back({ hash, name });
    }
    std::sort(out.begin(), out.end(), [](const SkinnedModelEntry& a, const SkinnedModelEntry& b) {
        return a.name < b.name;
    });
    return out;
}

namespace {

// times 昇順配列で t を挟む区間 [i0,i1] と補間係数 f を求める (範囲外はクランプ)
void FindSpan(const std::vector<float>& times, float t, size_t& i0, size_t& i1, float& f)
{
    if (times.size() <= 1 || t <= times.front()) {
        i0 = i1 = 0;
        f = 0.0f;
        return;
    }
    if (t >= times.back()) {
        i0 = i1 = times.size() - 1;
        f = 0.0f;
        return;
    }
    // 線形探索 (キー数は小さい)。times[i0] <= t < times[i0+1]
    size_t i = 0;
    while (i + 1 < times.size() && times[i + 1] <= t) {
        ++i;
    }
    i0 = i;
    i1 = i + 1;
    const float span = times[i1] - times[i0];
    f = (span > 1e-8f) ? (t - times[i0]) / span : 0.0f;
}

XMFLOAT3 SampleVec3(const std::vector<float>& times, const std::vector<XMFLOAT3>& vals,
                    float t, const XMFLOAT3& fallback)
{
    if (times.empty() || vals.empty()) {
        return fallback;
    }
    size_t i0, i1;
    float f;
    FindSpan(times, t, i0, i1, f);
    const XMVECTOR a = XMLoadFloat3(&vals[i0]);
    const XMVECTOR b = XMLoadFloat3(&vals[i1]);
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVectorLerp(a, b, f));
    return out;
}

XMFLOAT4 SampleQuat(const std::vector<float>& times, const std::vector<XMFLOAT4>& vals,
                    float t, const XMFLOAT4& fallback)
{
    if (times.empty() || vals.empty()) {
        return fallback;
    }
    size_t i0, i1;
    float f;
    FindSpan(times, t, i0, i1, f);
    const XMVECTOR a = XMLoadFloat4(&vals[i0]);
    const XMVECTOR b = XMLoadFloat4(&vals[i1]);
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionSlerp(a, b, f));
    return out;
}

// clip (範囲外は null = バインドポーズ) を timeSec でサンプルした 1 ジョイントの TRS。
// 手順は ComputeJointLocals のループ本体と同じ (トラックが無いチャネルはバインド値)
void SampleJointTrs(const SkeletalClip* c, const SkeletonJoint& jt, size_t j, float timeSec,
                    XMFLOAT3& t, XMFLOAT4& r, XMFLOAT3& s)
{
    t = jt.bindT;
    r = jt.bindR;
    s = jt.bindS;
    if (c && j < c->tracks.size()) {
        const JointTrack& tr = c->tracks[j];
        t = SampleVec3(tr.tTimes, tr.tVals, timeSec, t);
        r = SampleQuat(tr.rTimes, tr.rVals, timeSec, r);
        s = SampleVec3(tr.sTimes, tr.sVals, timeSec, s);
    }
}

const SkeletalClip* ClipOrNull(const SkinnedModel& model, int clip)
{
    return (clip >= 0 && clip < static_cast<int>(model.clips.size())) ? &model.clips[clip] : nullptr;
}

} // namespace

// clip を timeSec でサンプルして全ジョイントのローカル行列を作る (palette / jointGlobal 共用)。
// 式・評価順は M18 の ComputeBonePalette 前半ループそのまま (ビット不変が selftest 対象、M48a)
void ComputeJointLocals(const SkinnedModel& model, int clip, float timeSec,
                        std::vector<XMMATRIX>& local)
{
    const size_t n = model.joints.size();
    const SkeletalClip* c =
        (clip >= 0 && clip < static_cast<int>(model.clips.size())) ? &model.clips[clip] : nullptr;

    local.resize(n);
    for (size_t j = 0; j < n; ++j) {
        const SkeletonJoint& jt = model.joints[j];
        XMFLOAT3 t = jt.bindT;
        XMFLOAT4 r = jt.bindR;
        XMFLOAT3 s = jt.bindS;
        if (c && j < c->tracks.size()) {
            const JointTrack& tr = c->tracks[j];
            t = SampleVec3(tr.tTimes, tr.tVals, timeSec, t);
            r = SampleQuat(tr.rTimes, tr.rVals, timeSec, r);
            s = SampleVec3(tr.sTimes, tr.sVals, timeSec, s);
        }
        // 行ベクトル規約: local = S * R * T (スケール→回転→平行移動の順)
        local[j] = XMMatrixScaling(s.x, s.y, s.z) *
                   XMMatrixRotationQuaternion(XMLoadFloat4(&r)) *
                   XMMatrixTranslation(t.x, t.y, t.z);
    }
}

void ComputeJointLocalsBlended(const SkinnedModel& model, int clipA, float timeSecA, int clipB,
                               float timeSecB, float weightB, std::vector<XMMATRIX>& local)
{
    const size_t n = model.joints.size();
    const SkeletalClip* ca = ClipOrNull(model, clipA);
    const SkeletalClip* cb = ClipOrNull(model, clipB);
    const float w = std::clamp(weightB, 0.0f, 1.0f);

    local.resize(n);
    for (size_t j = 0; j < n; ++j) {
        const SkeletonJoint& jt = model.joints[j];
        XMFLOAT3 ta, sa, tb, sb;
        XMFLOAT4 ra, rb;
        SampleJointTrs(ca, jt, j, timeSecA, ta, ra, sa);
        SampleJointTrs(cb, jt, j, timeSecB, tb, rb, sb);
        // XMQuaternionSlerp は内積が負なら片側を反転する (= 常に短い弧を通る)。
        // ベイク済みキーは隣り合うクリップ間で符号が揃っている保証が無いので、これが要る
        const XMVECTOR t = XMVectorLerp(XMLoadFloat3(&ta), XMLoadFloat3(&tb), w);
        const XMVECTOR r = XMQuaternionSlerp(XMLoadFloat4(&ra), XMLoadFloat4(&rb), w);
        const XMVECTOR s = XMVectorLerp(XMLoadFloat3(&sa), XMLoadFloat3(&sb), w);
        // 行ベクトル規約: local = S * R * T (ComputeJointLocals と同じ積順)
        local[j] = XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) *
                   XMMatrixTranslationFromVector(t);
    }
}

// グローバル = local[j] * local[parent] * ... (親チェーンを上へ、順序非依存)
XMMATRIX JointGlobalFromLocals(const SkinnedModel& model, const std::vector<XMMATRIX>& local,
                               int32_t jointIndex)
{
    if (jointIndex < 0 || static_cast<size_t>(jointIndex) >= local.size()) {
        return XMMatrixIdentity();
    }
    const size_t j = static_cast<size_t>(jointIndex);
    XMMATRIX global = local[j];
    int p = model.joints[j].parent;
    while (p >= 0) {
        global = XMMatrixMultiply(global, local[static_cast<size_t>(p)]);
        p = model.joints[static_cast<size_t>(p)].parent;
    }
    return global;
}

int32_t SkinnedModel::FindJointByName(std::string_view name) const
{
    if (name.empty()) {
        return -1;
    }
    for (size_t j = 0; j < joints.size(); ++j) {
        if (joints[j].name == name) {
            return static_cast<int32_t>(j);
        }
    }
    return -1;
}

void ComputeBonePalette(const SkinnedModel& model, int clip, float timeSec,
                        std::vector<XMFLOAT4X4>& out)
{
    const size_t n = model.joints.size();
    std::vector<XMMATRIX> local;
    ComputeJointLocals(model, clip, timeSec, local);

    out.resize(n);
    for (size_t j = 0; j < n; ++j) {
        const XMMATRIX global = JointGlobalFromLocals(model, local, static_cast<int32_t>(j));
        // skin = inverseBind * jointGlobal (行ベクトル: 頂点 * IB * global)。転置してアップロード
        const XMMATRIX ib = XMLoadFloat4x4(&model.joints[j].inverseBind);
        XMStoreFloat4x4(&out[j], XMMatrixTranspose(XMMatrixMultiply(ib, global)));
    }
}

void ComputeBonePaletteWithOverrides(const SkinnedModel& model, const std::vector<XMMATRIX>& local,
                                     const std::vector<uint8_t>& hasOverride,
                                     const std::vector<XMMATRIX>& overrides,
                                     std::vector<XMFLOAT4X4>& out)
{
    const size_t n = model.joints.size();
    // 壊れた入力 (長さ違い) は override 無しとして扱う — 黙って別のポーズを作らない
    const bool useOv = (hasOverride.size() == n && overrides.size() == n);

    out.resize(n);
    for (size_t j = 0; j < n; ++j) {
        XMMATRIX global;
        if (useOv && hasOverride[j] != 0) {
            global = overrides[j];
        } else {
            // ★JointGlobalFromLocals と同じ「親チェーンを上へ」の積列。override 済みの
            //   祖先に当たったらそこで打ち切る。useOv が false ならこのループは
            //   JointGlobalFromLocals と 1 命令も変わらない (= ビット一致)
            global = local[j];
            int p = model.joints[j].parent;
            while (p >= 0) {
                const size_t pi = static_cast<size_t>(p);
                if (useOv && hasOverride[pi] != 0) {
                    global = XMMatrixMultiply(global, overrides[pi]);
                    break;
                }
                global = XMMatrixMultiply(global, local[pi]);
                p = model.joints[pi].parent;
            }
        }
        const XMMATRIX ib = XMLoadFloat4x4(&model.joints[j].inverseBind);
        XMStoreFloat4x4(&out[j], XMMatrixTranspose(XMMatrixMultiply(ib, global)));
    }
}

XMMATRIX ComputeJointGlobal(const SkinnedModel& model, int clip, float timeSec, int32_t jointIndex)
{
    if (jointIndex < 0 || static_cast<size_t>(jointIndex) >= model.joints.size()) {
        return XMMatrixIdentity();
    }
    std::vector<XMMATRIX> local;
    ComputeJointLocals(model, clip, timeSec, local);
    return JointGlobalFromLocals(model, local, jointIndex);
}

} // namespace mye
