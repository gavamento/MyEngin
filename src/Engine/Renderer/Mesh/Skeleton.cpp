#include "Engine/Renderer/Mesh/Skeleton.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Util/Hash.h"

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

bool ExpandToLinearKeys(KeyInterpolation interp, const std::vector<float>& times, const std::vector<float>& raw,
                        int32_t comps, bool normalize, float sampleHz, std::vector<float>& outTimes,
                        std::vector<float>& outVals)
{
    outTimes.clear();
    outVals.clear();
    const size_t n = times.size();
    const size_t c = static_cast<size_t>(comps);
    const size_t perKey = interp == KeyInterpolation::CubicSpline ? 3 * c : c;
    if (comps <= 0 || raw.size() < n * perKey) {
        return false;
    }
    const auto push = [&](float t, const float* v) {
        outTimes.push_back(t);
        outVals.insert(outVals.end(), v, v + c);
    };
    switch (interp) {
    case KeyInterpolation::Linear:
        outTimes = times;
        outVals.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(n * c));
        return true;
    case KeyInterpolation::Step:
        for (size_t k = 0; k < n; ++k) {
            if (k > 0) {
                push(times[k], &raw[(k - 1) * c]);
            }
            push(times[k], &raw[k * c]);
        }
        return true;
    case KeyInterpolation::CubicSpline:
        break;
    }
    // CubicSpline: キー k の要素は [入り接線 a_k, 値 v_k, 出接線 b_k]。区間 [t0, t1] で s = (t - t0) / dt として
    // p(s) = (2s³-3s²+1) v0 + (s³-2s²+s) dt b0 + (-2s³+3s²) v1 + (s³-s²) dt a1 (glTF 2.0 仕様 Appendix C)
    std::vector<float> p(c);
    const auto emit = [&](float t) {
        if (normalize) {
            float len2 = 0.0f;
            for (size_t i = 0; i < c; ++i) {
                len2 += p[i] * p[i];
            }
            if (len2 > 1e-12f) {
                const float inv = 1.0f / std::sqrt(len2);
                for (size_t i = 0; i < c; ++i) {
                    p[i] *= inv;
                }
            }
        }
        push(t, p.data());
    };
    for (size_t k = 0; k < n; ++k) {
        const float* v0 = &raw[k * perKey + c];
        std::copy(v0, v0 + c, p.begin());
        emit(times[k]);
        if (k + 1 == n) {
            break;
        }
        const float* b0 = &raw[k * perKey + 2 * c];
        const float* a1 = &raw[(k + 1) * perKey];
        const float* v1 = &raw[(k + 1) * perKey + c];
        const float dt = times[k + 1] - times[k];
        // 刻みは sampleHz 以下 (区間の中だけ。両端のキーは別に出す)。dt が 0 以下の区間は刻まない
        const int32_t steps = dt > 0.0f ? std::max(static_cast<int32_t>(std::ceil(dt * sampleHz)), 1) : 1;
        for (int32_t i = 1; i < steps; ++i) {
            const float s = static_cast<float>(i) / static_cast<float>(steps);
            const float s2 = s * s;
            const float s3 = s2 * s;
            const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
            const float h10 = s3 - 2.0f * s2 + s;
            const float h01 = -2.0f * s3 + 3.0f * s2;
            const float h11 = s3 - s2;
            for (size_t j = 0; j < c; ++j) {
                p[j] = h00 * v0[j] + h10 * dt * b0[j] + h01 * v1[j] + h11 * dt * a1[j];
            }
            emit(times[k] + dt * s);
        }
    }
    return true;
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

void ComputeJointLocalsLayered(const SkinnedModel& model, const SkeletalLayer* layers,
                               int32_t layerCount, std::vector<XMMATRIX>& local)
{
    // 重み 0 の層は畳む前に落とす (残りが 1 枚なら旧経路と同じ関数へ委ねられるように)
    const SkeletalLayer* live[kMaxSkeletalLayers] = {};
    int32_t liveCount = 0;
    for (int32_t i = 0; i < layerCount && liveCount < kMaxSkeletalLayers; ++i) {
        if (layers[i].weight > 0) {
            live[liveCount++] = &layers[i];
        }
    }
    if (liveCount == 0) {
        ComputeJointLocals(model, -1, 0.0f, local);
        return;
    }
    if (liveCount == 1) {
        ComputeJointLocals(model, live[0]->clip, live[0]->timeSec, local);
        return;
    }

    // 層 i を混ぜる比 = w_i / (w_0 + ... + w_i)。和は int64 で持ち、float へは 1 回だけ変換する
    float ratio[kMaxSkeletalLayers] = {};
    int64_t accumulated = 0;
    for (int32_t i = 0; i < liveCount; ++i) {
        accumulated += live[i]->weight;
        ratio[i] = static_cast<float>(static_cast<double>(live[i]->weight)
                                      / static_cast<double>(accumulated));
    }

    const size_t n = model.joints.size();
    local.resize(n);
    for (size_t j = 0; j < n; ++j) {
        const SkeletonJoint& jt = model.joints[j];
        XMFLOAT3 t0, s0;
        XMFLOAT4 r0;
        SampleJointTrs(ClipOrNull(model, live[0]->clip), jt, j, live[0]->timeSec, t0, r0, s0);
        XMVECTOR t = XMLoadFloat3(&t0);
        XMVECTOR r = XMLoadFloat4(&r0);
        XMVECTOR s = XMLoadFloat3(&s0);
        for (int32_t i = 1; i < liveCount; ++i) {
            XMFLOAT3 ti, si;
            XMFLOAT4 ri;
            SampleJointTrs(ClipOrNull(model, live[i]->clip), jt, j, live[i]->timeSec, ti, ri, si);
            // 短い弧を通す理由は ComputeJointLocalsBlended と同じ (クリップ間でキーの符号が揃わない)
            t = XMVectorLerp(t, XMLoadFloat3(&ti), ratio[i]);
            r = XMQuaternionSlerp(r, XMLoadFloat4(&ri), ratio[i]);
            s = XMVectorLerp(s, XMLoadFloat3(&si), ratio[i]);
        }
        // 行ベクトル規約: local = S * R * T (ComputeJointLocals と同じ積順)
        local[j] = XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) *
                   XMMatrixTranslationFromVector(t);
    }
}

XMFLOAT3 SampleJointTranslation(const SkinnedModel& model, int clip, int32_t jointIndex, float timeSec)
{
    if (jointIndex < 0 || static_cast<size_t>(jointIndex) >= model.joints.size()) {
        return { 0.0f, 0.0f, 0.0f };
    }
    const size_t j = static_cast<size_t>(jointIndex);
    const SkeletalClip* c = ClipOrNull(model, clip);
    const XMFLOAT3& bindT = model.joints[j].bindT;
    if (c == nullptr || j >= c->tracks.size()) {
        return bindT;
    }
    const JointTrack& tr = c->tracks[j];
    return SampleVec3(tr.tTimes, tr.tVals, timeSec, bindT);
}

int32_t FindRootJoint(const SkinnedModel& model)
{
    for (size_t j = 0; j < model.joints.size(); ++j) {
        if (model.joints[j].parent < 0) {
            return static_cast<int32_t>(j);
        }
    }
    return -1;
}

XMFLOAT2 SampleJointYaw(const SkinnedModel& model, int clip, int32_t jointIndex, float timeSec, const float (&up)[3])
{
    if (jointIndex < 0 || static_cast<size_t>(jointIndex) >= model.joints.size()) {
        return { 1.0f, 0.0f };
    }
    const size_t j = static_cast<size_t>(jointIndex);
    const SkeletalClip* c = ClipOrNull(model, clip);
    if (c == nullptr || j >= c->tracks.size()) {
        return { 1.0f, 0.0f };
    }
    const JointTrack& tr = c->tracks[j];
    const XMFLOAT4& bindR = model.joints[j].bindR;
    const XMFLOAT4 a = SampleQuat(tr.rTimes, tr.rVals, timeSec, bindR);
    const XMFLOAT4 h = SampleQuat(tr.rTimes, tr.rVals, 0.0f, bindR);
    // q = a · conj(h) (Hamilton 積)。虚部は「同じ 2 積の差」の組で括る: a == h なら各組が 0 ちょうどになる
    const float qw = a.w * h.w + a.x * h.x + a.y * h.y + a.z * h.z;
    const float qx = (a.x * h.w - a.w * h.x) + (a.z * h.y - a.y * h.z);
    const float qy = (a.y * h.w - a.w * h.y) + (a.x * h.z - a.z * h.x);
    const float qz = (a.z * h.w - a.w * h.z) + (a.y * h.x - a.x * h.y);
    const float along = qx * up[0] + qy * up[1] + qz * up[2];
    if (along == 0.0f) {
        return { 1.0f, 0.0f };
    }
    // twist = (along·up, qw) を正規化。up に垂直な 180 度の振りだけのときは along == 0 で上で返っている
    const float len = std::sqrt(qw * qw + along * along);
    const float sign = qw < 0.0f ? -1.0f : 1.0f;
    return { sign * qw / len, sign * along / len };
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

namespace {

// ---- 2 ボーン IK の小道具 (M89l) ----
// 長さがこれ未満の骨・距離は向きが決まらないので扱わない
constexpr float kIkEpsilon = 1e-6f;

XMVECTOR Normalize3OrZero(XMVECTOR v)
{
    const float len = XMVectorGetX(XMVector3Length(v));
    return len > kIkEpsilon ? XMVectorScale(v, 1.0f / len) : XMVectorZero();
}

// v の dir (単位) に垂直な成分を正規化したもの。垂直な成分が無ければ 0
XMVECTOR PerpendicularTo(XMVECTOR v, XMVECTOR dir)
{
    return Normalize3OrZero(XMVectorSubtract(v, XMVectorScale(dir, XMVectorGetX(XMVector3Dot(v, dir)))));
}

// u の向きを v の向きへ回す最短の弧の四元数。半角公式 (u×v, 1 + u·v) を正規化するので acos を使わない。
// ちょうど逆向きは u に垂直な軸で 180 度
XMVECTOR ShortestArc(XMVECTOR u, XMVECTOR v)
{
    u = Normalize3OrZero(u);
    v = Normalize3OrZero(v);
    const float d = XMVectorGetX(XMVector3Dot(u, v));
    if (d < -0.999999f) {
        XMVECTOR axis = XMVector3Cross(u, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
        if (XMVectorGetX(XMVector3LengthSq(axis)) < kIkEpsilon) {
            axis = XMVector3Cross(u, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        }
        return XMVectorSetW(Normalize3OrZero(axis), 0.0f);
    }
    return XMQuaternionNormalize(XMVectorSetW(XMVector3Cross(u, v), 1.0f + d));
}

// グローバル行列 g を、自分の原点 (平行移動の行) を中心に q だけ回す (エンティティ空間で後から掛ける)
XMMATRIX RotateAboutOwnOrigin(const XMMATRIX& g, XMVECTOR q)
{
    XMMATRIX out = XMMatrixMultiply(g, XMMatrixRotationQuaternion(q));
    out.r[3] = g.r[3];
    return out;
}

// 親のグローバル行列の逆 (ルートは恒等)。新しいグローバルを局所へ戻すのに使う
XMMATRIX InverseParentGlobal(const SkinnedModel& model, const std::vector<XMMATRIX>& locals, int32_t joint)
{
    const int32_t parent = model.joints[static_cast<size_t>(joint)].parent;
    if (parent < 0) {
        return XMMatrixIdentity();
    }
    return XMMatrixInverse(nullptr, JointGlobalFromLocals(model, locals, parent));
}

} // namespace

void SolveTwoBoneIk(const SkinnedModel& model, const TwoBoneIkGoal& goal, std::vector<XMMATRIX>& locals)
{
    // NaN の重みも何もしない側へ倒す
    if (!(goal.weight > 0.0f) || locals.size() != model.joints.size()) {
        return;
    }
    const float w = std::min(goal.weight, 1.0f);
    const int32_t c = goal.endJoint;
    if (c < 0 || static_cast<size_t>(c) >= locals.size()) {
        return;
    }
    const int32_t b = model.joints[static_cast<size_t>(c)].parent;
    if (b < 0) {
        return;
    }
    const int32_t a = model.joints[static_cast<size_t>(b)].parent;
    if (a < 0) {
        return;
    }
    const XMMATRIX ga = JointGlobalFromLocals(model, locals, a);
    const XMMATRIX gb = JointGlobalFromLocals(model, locals, b);
    const XMMATRIX gc = JointGlobalFromLocals(model, locals, c);
    const XMVECTOR pa = ga.r[3];
    const XMVECTOR pb = gb.r[3];
    const XMVECTOR pc = gc.r[3];
    const float upperLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(pb, pa)));
    const float lowerLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(pc, pb)));
    if (upperLen < kIkEpsilon || lowerLen < kIkEpsilon) {
        return;
    }

    // 目標は今の先端から weight だけ寄せる
    const XMVECTOR target = XMVectorLerp(pc, XMVectorSetW(XMLoadFloat3(&goal.target), 1.0f), w);
    const XMVECTOR toTarget = XMVectorSubtract(target, pa);
    const float dist = XMVectorGetX(XMVector3Length(toTarget));
    if (dist < kIkEpsilon) {
        return;
    }
    const XMVECTOR dir = XMVectorScale(toTarget, 1.0f / dist);
    // 届かない距離は伸び切り / 縮み切りで止める
    const float reach = std::clamp(dist, std::fabs(upperLen - lowerLen), upperLen + lowerLen);
    // 根 → 中間の、目標方向の成分と垂直な成分 (余弦定理を角度にせず長さのまま解く)
    const float along = (upperLen * upperLen - lowerLen * lowerLen + reach * reach) / (2.0f * reach);
    const float across = std::sqrt(std::max(upperLen * upperLen - along * along, 0.0f));

    // 曲げる向き: 今の中間の側。pole があれば weight だけ pole の側へ寄せる
    XMVECTOR bend = PerpendicularTo(XMVectorSubtract(pb, pa), dir);
    if (goal.hasPole) {
        const XMVECTOR poleSide =
            PerpendicularTo(XMVectorSubtract(XMVectorSetW(XMLoadFloat3(&goal.pole), 1.0f), pa), dir);
        bend = Normalize3OrZero(XMVectorLerp(bend, poleSide, w));
    }
    if (XMVectorGetX(XMVector3LengthSq(bend)) == 0.0f) {
        // 伸び切った腕で pole も無い: 曲げる向きは決まらないので、目標方向に垂直な軸を 1 つ選ぶ
        bend = PerpendicularTo(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), dir);
        if (XMVectorGetX(XMVector3LengthSq(bend)) == 0.0f) {
            bend = PerpendicularTo(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), dir);
        }
    }
    const XMVECTOR midGoal = XMVectorAdd(pa, XMVectorAdd(XMVectorScale(dir, along), XMVectorScale(bend, across)));
    const XMVECTOR endGoal = XMVectorAdd(pa, XMVectorScale(dir, reach));

    // 根: 中間が midGoal に来るように回す
    const XMMATRIX gaNew = RotateAboutOwnOrigin(ga, ShortestArc(XMVectorSubtract(pb, pa), XMVectorSubtract(midGoal, pa)));
    locals[static_cast<size_t>(a)] = XMMatrixMultiply(gaNew, InverseParentGlobal(model, locals, a));
    // 中間: 回った後の先端が endGoal に来るように回す
    const XMMATRIX gbMoved = XMMatrixMultiply(locals[static_cast<size_t>(b)], gaNew);
    const XMMATRIX gcMoved = XMMatrixMultiply(locals[static_cast<size_t>(c)], gbMoved);
    const XMMATRIX gbNew = RotateAboutOwnOrigin(
        gbMoved, ShortestArc(XMVectorSubtract(gcMoved.r[3], gbMoved.r[3]), XMVectorSubtract(endGoal, gbMoved.r[3])));
    locals[static_cast<size_t>(b)] = XMMatrixMultiply(gbNew, XMMatrixInverse(nullptr, gaNew));

    // 先端の回転 (位置は中間が運ぶ。拡大はそのまま)
    if (goal.useRotation) {
        const XMMATRIX gcNew = XMMatrixMultiply(locals[static_cast<size_t>(c)], gbNew);
        XMVECTOR scale, rotation, translation;
        if (XMMatrixDecompose(&scale, &rotation, &translation, gcNew)) {
            const XMVECTOR wanted =
                XMQuaternionSlerp(rotation, XMQuaternionNormalize(XMLoadFloat4(&goal.targetRotation)), w);
            const XMMATRIX gcWanted = XMMatrixScalingFromVector(scale) * XMMatrixRotationQuaternion(wanted)
                                      * XMMatrixTranslationFromVector(translation);
            locals[static_cast<size_t>(c)] = XMMatrixMultiply(gcWanted, XMMatrixInverse(nullptr, gbNew));
        }
    }
}

void OffsetJointGlobal(const SkinnedModel& model, int32_t joint, const XMFLOAT3& offset, std::vector<XMMATRIX>& locals)
{
    if (joint < 0 || static_cast<size_t>(joint) >= locals.size() || locals.size() != model.joints.size()) {
        return;
    }
    XMMATRIX g = JointGlobalFromLocals(model, locals, joint);
    g.r[3] = XMVectorAdd(g.r[3], XMVectorSet(offset.x, offset.y, offset.z, 0.0f));
    locals[static_cast<size_t>(joint)] = XMMatrixMultiply(g, InverseParentGlobal(model, locals, joint));
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

int32_t SkinnedModel::FindClipByHash(uint64_t nameHash) const
{
    // クリップは高々十数本なので毎回ハッシュする。表を持たないので、Register を通らずに組んだ
    // モデル (selftest の手組み) でも、クック済みから戻したモデルでも同じ答えになる
    for (size_t c = 0; c < clips.size(); ++c) {
        if (!clips[c].name.empty() && HashStr(clips[c].name) == nameHash) {
            return static_cast<int32_t>(c);
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
