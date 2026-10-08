#include "Engine/Engine/Animation/AnimatorController.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/HierarchyWalk.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.controller.json" → "X.controller"
    const std::string suf = ".controller";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

// アニメイベントの kind (M89i)
const char* ClipEventKindToStr(ClipEventKind kind)
{
    switch (kind) {
    case ClipEventKind::Script: return "script";
    case ClipEventKind::Sound: return "sound";
    case ClipEventKind::Effect: return "effect";
    case ClipEventKind::Noise: return "noise";
    }
    return "script";
}

bool ClipEventKindFromStr(const std::string& s, ClipEventKind& out)
{
    const ClipEventKind kinds[] = { ClipEventKind::Script, ClipEventKind::Sound, ClipEventKind::Effect,
                                    ClipEventKind::Noise };
    for (ClipEventKind k : kinds) {
        if (s == ClipEventKindToStr(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

const char* OpToStr(CondOp op)
{
    switch (op) {
    case CondOp::Gt: return "gt";
    case CondOp::Ge: return "ge";
    case CondOp::Lt: return "lt";
    case CondOp::Le: return "le";
    case CondOp::Eq: return "eq";
    case CondOp::Ne: return "ne";
    }
    return "gt";
}

CondOp StrToOp(const std::string& s)
{
    if (s == "ge") return CondOp::Ge;
    if (s == "lt") return CondOp::Lt;
    if (s == "le") return CondOp::Le;
    if (s == "eq") return CondOp::Eq;
    if (s == "ne") return CondOp::Ne;
    return CondOp::Gt;
}

const char* ParamTypeToStr(ControllerParamType type)
{
    switch (type) {
    case ControllerParamType::Int: return "int";
    case ControllerParamType::Float: return "float";
    case ControllerParamType::Bool: return "bool";
    case ControllerParamType::Trigger: return "trigger";
    }
    return "int";
}

ControllerParamType StrToParamType(const std::string& s)
{
    if (s == "float") return ControllerParamType::Float;
    if (s == "bool") return ControllerParamType::Bool;
    if (s == "trigger") return ControllerParamType::Trigger;
    return ControllerParamType::Int;
}

template <typename T>
bool EvalCond(CondOp op, T a, T b)
{
    switch (op) {
    case CondOp::Gt: return a > b;
    case CondOp::Ge: return a >= b;
    case CondOp::Lt: return a < b;
    case CondOp::Le: return a <= b;
    case CondOp::Eq: return a == b;
    case CondOp::Ne: return a != b;
    }
    return false;
}

bool ConditionMet(const ControllerAsset& ctrl, const ControllerCondition& c, const int32_t* params)
{
    if (c.param < 0 || c.param >= AnimatorControllerComponent::kMaxParams) {
        return false;
    }
    const int32_t raw = params[c.param];
    switch (ControllerParamTypeAt(ctrl, c.param)) {
    case ControllerParamType::Int: return EvalCond(c.op, raw, c.value);
    case ControllerParamType::Float: return EvalCond(c.op, std::bit_cast<float>(raw), c.floatValue);
    case ControllerParamType::Bool: return EvalCond(c.op, raw != 0 ? 1 : 0, c.value != 0 ? 1 : 0);
    case ControllerParamType::Trigger: return raw != 0;
    }
    return false;
}

bool AllConditionsMet(const ControllerAsset& ctrl, const ControllerTransition& t, const int32_t* params)
{
    for (const ControllerCondition& c : t.conditions) {
        if (!ConditionMet(ctrl, c, params)) {
            return false;
        }
    }
    return true;
}

// 採用した遷移が条件に使った Trigger を下ろす (同じ Trigger を 2 回参照していても 0 にするだけ)
void ConsumeTriggers(const ControllerAsset& ctrl, const ControllerTransition& t, int32_t* params)
{
    for (const ControllerCondition& c : t.conditions) {
        if (ControllerParamTypeAt(ctrl, c.param) == ControllerParamType::Trigger) {
            params[c.param] = 0; // ConditionMet が範囲を確かめ済み (採用 = 全条件が真)
        }
    }
}

// time を speed 分進める (loop で巻き戻し / 非 loop で末尾停止)
void AdvanceStateTime(int32_t& time, int32_t speed, int32_t loop, int32_t length)
{
    if (length <= 0) {
        return;
    }
    time += speed;
    if (time >= length) {
        time = loop ? (time % length) : length;
    } else if (time < 0) {
        time = loop ? (((time % length) + length) % length) : 0;
    }
}

bool HasSkeletalStates(const ControllerAsset& controller)
{
    for (const ControllerState& st : controller.states) {
        if (StateDrivesSkeleton(st)) {
            return true;
        }
    }
    return false;
}

const char* BlendTypeToStr(ControllerBlendType type)
{
    switch (type) {
    case ControllerBlendType::Blend1D: return "blend1d";
    case ControllerBlendType::Blend2D: return "blend2d";
    case ControllerBlendType::None: break;
    }
    return "";
}

constexpr int64_t kWeightOne = SkinnedMeshComponent::kPoseWeightOne;
constexpr uint64_t kPhaseCycle = uint64_t(1) << 32; // 位相の 1 周
// speed·2^48 が int64 に収まる上限。これを超える speed は丸める (1 tick に 3 万周を超える再生は意味を持たない)
constexpr int32_t kMaxBlendSpeed = 32767;

// float の重み (和 ≈ 1) を Q16 へ 1 回だけ切り捨て、端数を最大重みの要素 (同値なら先の要素) に足す = 和はちょうど kWeightOne
void QuantizeWeights(const float* w, int32_t n, int32_t* q)
{
    int64_t sum = 0;
    int32_t largest = 0;
    for (int32_t i = 0; i < n; ++i) {
        const float clamped = std::clamp(w[i], 0.0f, 1.0f);
        q[i] = static_cast<int32_t>(clamped * static_cast<float>(kWeightOne));
        sum += q[i];
        if (w[i] > w[largest]) {
            largest = i;
        }
    }
    q[largest] += static_cast<int32_t>(kWeightOne - sum);
}

// ステートの今のパラメータの値での子の重み (y は Blend2D だけが読む)
int32_t StateBlendWeights(const ControllerAsset& ctrl, const ControllerState& st, const int32_t* params,
                          BlendChildWeight (&out)[kMaxBlendLayers])
{
    const float x = ControllerParamAsFloat(ctrl, st.blendParam, params);
    const float y = st.blendType == ControllerBlendType::Blend2D ? ControllerParamAsFloat(ctrl, st.blendParamY, params) : 0.0f;
    return ComputeBlendWeights(st, x, y, out);
}

int32_t ComputeBlend1DWeights(const ControllerState& state, float x, BlendChildWeight (&out)[kMaxBlendLayers])
{
    const int32_t n = static_cast<int32_t>(state.blendChildren.size());
    const auto threshold = [&](int32_t i) { return state.blendChildren[static_cast<size_t>(i)].threshold; };
    // lo = x 以下で最大の閾値 / hi = x より大きい最小の閾値 (同じ閾値なら先の子)。NaN はどの比較も偽なので hi 側に入る
    int32_t lo = -1;
    int32_t hi = -1;
    for (int32_t i = 0; i < n; ++i) {
        if (threshold(i) <= x) {
            if (lo < 0 || threshold(i) > threshold(lo)) {
                lo = i;
            }
        } else if (hi < 0 || threshold(i) < threshold(hi)) {
            hi = i;
        }
    }
    if (lo < 0 || hi < 0) {
        out[0] = { lo >= 0 ? lo : hi, static_cast<int32_t>(kWeightOne) };
        return 1;
    }
    // threshold(lo) <= x < threshold(hi) なので分母は正、t は [0, 1)
    const float t = (x - threshold(lo)) / (threshold(hi) - threshold(lo));
    const int32_t first = std::min(lo, hi);
    const int32_t second = std::max(lo, hi);
    const float w[2] = { first == lo ? 1.0f - t : t, first == lo ? t : 1.0f - t };
    int32_t q[2] = {};
    QuantizeWeights(w, 2, q);
    int32_t count = 0;
    if (q[0] > 0) {
        out[count++] = { first, q[0] };
    }
    if (q[1] > 0) {
        out[count++] = { second, q[1] };
    }
    return count;
}

// Freeform Cartesian (gradient band、Johansen 2009 / Unity の 2D Freeform Cartesian と同じ式)。
// 全体の Σh で正規化してから上位を選び直すのと、上位だけの比を取るのは同じ値になるので、上位 4 本の h だけを持つ
// (子の数によらず作業領域が固定で、tick ごとの確保が無い)
int32_t ComputeBlend2DWeights(const ControllerState& state, float x, float y, BlendChildWeight (&out)[kMaxBlendLayers])
{
    const int32_t n = static_cast<int32_t>(state.blendChildren.size());
    if (!std::isfinite(x) || !std::isfinite(y)) {
        out[0] = { 0, static_cast<int32_t>(kWeightOne) };
        return 1;
    }
    int32_t topChild[kMaxBlendLayers] = {};
    float topH[kMaxBlendLayers] = {};
    int32_t topCount = 0;
    for (int32_t i = 0; i < n; ++i) {
        const ControllerBlendChild& ci = state.blendChildren[static_cast<size_t>(i)];
        float h = 1.0f;
        bool duplicate = false;
        for (int32_t j = 0; j < n && !duplicate; ++j) {
            if (j == i) {
                continue;
            }
            const ControllerBlendChild& cj = state.blendChildren[static_cast<size_t>(j)];
            const float dx = cj.posX - ci.posX;
            const float dy = cj.posY - ci.posY;
            const float lengthSq = dx * dx + dy * dy;
            if (lengthSq == 0.0f) {
                duplicate = j < i; // 同じ位置の子は先の子だけを使う (後の子は線分を作らない)
                continue;
            }
            const float proj = ((x - ci.posX) * dx + (y - ci.posY) * dy) / lengthSq;
            h = std::min(h, 1.0f - proj);
        }
        // 巨大な値であふれた NaN もここで 0 に落ちる
        if (duplicate || !(h > 0.0f)) {
            continue;
        }
        // h の降順 (同値なら先に見た = index の小さい子が前) に挿し、上位 kMaxBlendLayers 本だけ残す
        int32_t at = topCount;
        while (at > 0 && h > topH[at - 1]) {
            --at;
        }
        if (at >= kMaxBlendLayers) {
            continue;
        }
        const int32_t last = std::min(topCount, kMaxBlendLayers - 1);
        for (int32_t k = last; k > at; --k) {
            topChild[k] = topChild[k - 1];
            topH[k] = topH[k - 1];
        }
        topChild[at] = i;
        topH[at] = h;
        topCount = std::min(topCount + 1, kMaxBlendLayers);
    }
    if (topCount == 0) {
        // 有限の入力では最寄りの子の h が 1/2 以上になるので来ない (あふれた入力の保険)
        out[0] = { 0, static_cast<int32_t>(kWeightOne) };
        return 1;
    }
    // 出力は子の index の昇順 (端数の行き先「同値なら index の小さい子」もこの順で決まる)
    for (int32_t a = 1; a < topCount; ++a) {
        for (int32_t b = a; b > 0 && topChild[b] < topChild[b - 1]; --b) {
            std::swap(topChild[b], topChild[b - 1]);
            std::swap(topH[b], topH[b - 1]);
        }
    }
    float sum = 0.0f;
    for (int32_t k = 0; k < topCount; ++k) {
        sum += topH[k];
    }
    float w[kMaxBlendLayers] = {};
    for (int32_t k = 0; k < topCount; ++k) {
        w[k] = topH[k] / sum;
    }
    int32_t q[kMaxBlendLayers] = {};
    QuantizeWeights(w, topCount, q);
    int32_t count = 0;
    for (int32_t k = 0; k < topCount; ++k) {
        if (q[k] > 0) {
            out[count++] = { topChild[k], q[k] };
        }
    }
    return count;
}

// 子のクリップの長さ (tick) を model で引く。モデルが無い・名前のクリップが無ければ 0
int32_t BlendChildTicks(const SkinnedModel* model, const ControllerBlendChild& child)
{
    if (model == nullptr) {
        return 0;
    }
    const int32_t clip = model->FindClipByHash(child.clipHash);
    return clip >= 0 ? SkeletalClipTicks(model->clips[static_cast<size_t>(clip)]) : 0;
}

// Σ(wQ_i·L_i)。重みが Q16 なので、65536 で割ると混ぜた 1 周の長さ (tick) になる
int64_t BlendWeightedTicks(const ControllerState& st, const BlendChildWeight* w, int32_t n, const SkinnedModel* model)
{
    int64_t sum = 0;
    for (int32_t i = 0; i < n; ++i) {
        sum += static_cast<int64_t>(w[i].weightQ) * BlendChildTicks(model, st.blendChildren[static_cast<size_t>(w[i].child)]);
    }
    return sum;
}

// ブレンドツリーのステートの 1 tick の位相の進み (今のパラメータの重みで)。長さが引けなければ 0
int64_t BlendPhaseDelta(const ControllerAsset& ctrl, const ControllerState& st, const int32_t* params,
                        const SkinnedModel* mainModel)
{
    BlendChildWeight w[kMaxBlendLayers];
    const int32_t n = StateBlendWeights(ctrl, st, params, w);
    const int64_t weightedTicks = BlendWeightedTicks(st, w, n, mainModel);
    if (weightedTicks <= 0) {
        return 0;
    }
    // 2^32 (1 周) × 65536 (重みの満杯) / Σ(wQ·L) = 1 tick の進み。割り算は 0 方向への切り捨て
    const int64_t speed = std::clamp(st.speed, -kMaxBlendSpeed, kMaxBlendSpeed);
    return speed * (int64_t(1) << 48) / weightedTicks;
}

uint32_t AdvanceBlendPhase(uint32_t phase, int64_t delta, int32_t loop)
{
    if (loop) {
        // 2^32 を法とする加算 = 折り返し (逆再生も同じ式)
        return static_cast<uint32_t>(static_cast<uint64_t>(phase) + static_cast<uint64_t>(delta));
    }
    // 非ループは端に張り付く。1 周より大きい進みは結果を変えないので丸めて int64 のあふれを防ぐ
    const int64_t step = std::clamp<int64_t>(delta, -int64_t(kPhaseCycle), int64_t(kPhaseCycle));
    return static_cast<uint32_t>(std::clamp<int64_t>(int64_t(phase) + step, 0, int64_t(UINT32_MAX)));
}

// hasExitTime: 次の進みで位相が 1 周に達する = 今が周の最後の tick (単一クリップの time >= len - 1 に当たる)
bool BlendExitReached(uint32_t phase, int64_t delta)
{
    return delta > 0 && static_cast<uint64_t>(phase) + static_cast<uint64_t>(delta) >= kPhaseCycle;
}

// 1 ステートの時計の、この tick の動き (M89f の描画補間用)。prev は進める前、step は折り返す前の進み。
// ★step は sim では使わない (描画が前 tick の姿勢から滑らかに繋ぐためだけの値)
struct StateClock {
    int32_t time = 0;
    int32_t prevTime = 0;
    int32_t timeStep = 0;
    uint32_t phase = 0;
    uint32_t prevPhase = 0;
    int64_t phaseStep = 0;
};

// ポーズプログラムの元になる 1 層 (骨クリップと再生位置と重み)
struct SkeletalSource {
    uint64_t clipHash = 0;
    bool usesPhase = false; // true = ブレンドツリーの子 (位相をメッシュごとの長さで時刻にする)
    StateClock clock;       // 単一クリップは time 系、ブレンドツリーの子は phase 系を使う
    int32_t loop = 1;
    int32_t weightQ = 0;
};
// 遷移の元と先がどちらもブレンドツリーでも収まる
constexpr int32_t kMaxSkeletalSources = 2 * kMaxBlendLayers;
static_assert(kMaxSkeletalSources <= SkinnedMeshComponent::kMaxPoseLayers,
              "a transition between two blend trees must fit in one pose program");

// 1 ステートぶんの層を out へ足す。stateWeightQ をステート内の子の重みで割り振る (端数は呼び出し側で直す)
int32_t AppendStateSources(const ControllerAsset& ctrl, const ControllerState& st, const int32_t* params,
                           const StateClock& clock, int32_t stateWeightQ, SkeletalSource* out)
{
    if (st.blendType == ControllerBlendType::None) {
        out[0] = { st.skelClipHash, false, clock, st.loop, stateWeightQ };
        return 1;
    }
    BlendChildWeight w[kMaxBlendLayers];
    const int32_t n = StateBlendWeights(ctrl, st, params, w);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t weightQ = static_cast<int32_t>(int64_t(stateWeightQ) * w[i].weightQ / kWeightOne);
        out[i] = { st.blendChildren[static_cast<size_t>(w[i].child)].clipHash, true, clock, st.loop, weightQ };
    }
    return n;
}

// 今の tick のプログラムの元を組む (時刻を進めた後の値で)。返り値 = 層数 (0..kMaxSkeletalSources)。
// 遷移中は元 → 先の順に並べる (層を先頭から畳むので順序も入力の一部)。ステートの重みは
// transitionTick / transitionDuration を Q16 に 1 回だけ切り捨て、残りを元へ回す。ブレンドの子へ割り振った
// 切り捨ての端数は最大重みの層 (同値なら先の層) に足す = 和はちょうど kPoseWeightOne
// fromClock / toClock は currentState / transitionTo の時計 (この tick に進めた後の値と、その動き)
int32_t BuildSkeletalSources(const ControllerAsset& ctrl, const AnimatorControllerComponent& c,
                             const StateClock& fromClock, const StateClock& toClock,
                             SkeletalSource (&out)[kMaxSkeletalSources])
{
    const ControllerState& from = ctrl.states[static_cast<size_t>(c.currentState)];
    const bool fromSkel = StateDrivesSkeleton(from);
    int32_t count = 0;
    const auto appendFrom = [&](int64_t weightQ) {
        count += AppendStateSources(ctrl, from, c.params, fromClock, static_cast<int32_t>(weightQ), out + count);
    };
    if (c.transitionTo < 0) {
        if (!fromSkel) {
            return 0;
        }
        appendFrom(kWeightOne);
    } else {
        const ControllerState& to = ctrl.states[static_cast<size_t>(c.transitionTo)];
        const bool toSkel = StateDrivesSkeleton(to);
        const auto appendTo = [&](int64_t weightQ) {
            count += AppendStateSources(ctrl, to, c.params, toClock, static_cast<int32_t>(weightQ), out + count);
        };
        if (!fromSkel && !toSkel) {
            return 0;
        }
        if (!toSkel) {
            appendFrom(kWeightOne);
        } else if (!fromSkel) {
            appendTo(kWeightOne);
        } else {
            const int32_t duration = c.transitionDuration > 0 ? c.transitionDuration : 1;
            const int64_t tick = std::clamp<int64_t>(c.transitionTick, 0, duration);
            const int64_t toWeight = tick * kWeightOne / duration;
            appendFrom(kWeightOne - toWeight);
            appendTo(toWeight);
        }
    }
    int64_t sum = 0;
    int32_t largest = 0;
    for (int32_t i = 0; i < count; ++i) {
        sum += out[i].weightQ;
        if (out[i].weightQ > out[largest].weightQ) {
            largest = i;
        }
    }
    out[largest].weightQ += static_cast<int32_t>(kWeightOne - sum);
    return count;
}

// 長さ lengthTicks のクリップの 1 周を timeQ で表した値。timeQ (int32) に収まる長さに丸める
// (約 38 時間。実際のクリップは届かない)
int64_t BlendSpanQ(int32_t lengthTicks)
{
    return static_cast<int64_t>(std::min(lengthTicks, INT32_MAX / SkinnedMeshComponent::kPoseTimeQPerTick))
           * SkinnedMeshComponent::kPoseTimeQPerTick;
}

int32_t ClampToInt32(int64_t v)
{
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}

void WriteSkeletalProgram(SkinnedMeshComponent& sm, const SkinnedModel* model, const SkeletalSource* sources,
                          int32_t count)
{
    constexpr int32_t kQ = SkinnedMeshComponent::kPoseTimeQPerTick;
    sm.poseLayerCount = count;
    for (int32_t i = 0; i < count; ++i) {
        const SkeletalSource& src = sources[i];
        const StateClock& clock = src.clock;
        SkinnedMeshComponent::PoseLayer& layer = sm.poseLayers[i];
        // モデルが未登録・名前のクリップが無いモデルは -1 = バインドポーズ (層の数と重みは他のメッシュとそろえる)
        layer.clip = model != nullptr ? model->FindClipByHash(src.clipHash) : -1;
        if (src.usesPhase) {
            // 位相はメッシュ自身のモデルでの長さで時刻にする (長さの違うモデルでも周がそろう)
            const int32_t ticks =
                layer.clip >= 0 ? SkeletalClipTicks(model->clips[static_cast<size_t>(layer.clip)]) : 0;
            layer.timeQ = BlendPhaseToTimeQ(clock.phase, ticks, src.loop);
            layer.prevTimeQ = BlendPhaseToTimeQ(clock.prevPhase, ticks, src.loop);
            // 進みは時刻の差から取り (alpha = 1 の手前で timeQ にちょうど届く)、ループの折り返しだけ
            // 位相の進む向きで 1 周ぶん足し戻す。1 tick で 1 周以上進む速さは描画の補間でも追わない
            int64_t step = int64_t(layer.timeQ) - layer.prevTimeQ;
            if (clock.phaseStep == 0) {
                step = 0;
            } else if (src.loop) {
                const int64_t spanQ = BlendSpanQ(ticks);
                if (clock.phaseStep > 0 && step < 0) {
                    step += spanQ;
                } else if (clock.phaseStep < 0 && step > 0) {
                    step -= spanQ;
                }
            }
            layer.stepQ = ClampToInt32(step);
        } else {
            layer.timeQ = clock.time * kQ;
            layer.prevTimeQ = clock.prevTime * kQ;
            layer.stepQ = ClampToInt32(int64_t(clock.timeStep) * kQ);
        }
        layer.weightQ = src.weightQ;
    }
    // 使っていない層は既定値に戻す (snapshot の生バイトを入力だけで決まる形にしておく)
    for (int32_t i = count; i < SkinnedMeshComponent::kMaxPoseLayers; ++i) {
        sm.poseLayers[i] = {};
    }
    sm.poseClaim = 1;
}

// ---- ルートモーション (M89j) ----
// 固定 tick の周波数 (1 tick の移動 × これ = m/s)。SkeletalClipTicks の 60 と同じ前提
constexpr float kTicksPerSecond = 60.0f;
// timeQ (1/256 tick) を秒にする。SampleSkinnedLocals と同じ式 (同じ時刻なら同じ値をサンプルする)
constexpr float kTimeQPerSecond = kTicksPerSecond * static_cast<float>(SkinnedMeshComponent::kPoseTimeQPerTick);

// 0 方向ではなく負の無限大方向へ丸める割り算 (逆再生の折り返しの回数)
int64_t FloorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// 単位クォータニオンでベクトルを回す (物理の QuatRotate と同じ式。DirectXMath の SIMD 経路を通さない)
void RotateByQuat(const float (&q)[4], const float (&v)[3], float (&out)[3])
{
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// entity から根までの LocalTransform の回転と拡大の合成。拡大は軸ごとの積で、回転との順序を無視する
// (物理の ComposeParentFrame と同じ近似。シアーは扱わない)。WorldMatrix は前 tick の値なので使わない
struct ChainRotScale {
    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float s[3] = { 1.0f, 1.0f, 1.0f };
};
ChainRotScale ComposeChain(World& world, EntityID from)
{
    ChainRotScale f;
    for (EntityID cur = from; !cur.IsNull(); cur = world.GetParent(cur)) {
        const LocalTransform* lt = world.GetComponent<LocalTransform>(cur);
        if (lt == nullptr) {
            break;
        }
        // q' = q_lt * q (子の回転を先に当てる)
        const float ax = lt->rotation.x, ay = lt->rotation.y, az = lt->rotation.z, aw = lt->rotation.w;
        const float bx = f.q[0], by = f.q[1], bz = f.q[2], bw = f.q[3];
        f.q[0] = aw * bx + ax * bw + ay * bz - az * by;
        f.q[1] = aw * by - ax * bz + ay * bw + az * bx;
        f.q[2] = aw * bz + ax * by - ay * bx + az * bw;
        f.q[3] = aw * bw - ax * bx - ay * by - az * bz;
        f.s[0] *= lt->scale.x;
        f.s[1] *= lt->scale.y;
        f.s[2] *= lt->scale.z;
    }
    return f;
}

// ワールドの上 (0, 1, 0) を entity の空間へ戻した単位ベクトル (連鎖の回転の共役で回す。逆行列は使わない)。
// Z-up の glTF は非ジョイントの祖先 (Z_UP ノード) がエンティティ側に載るので、ここで初めて上が分かる
void WorldUpInEntitySpace(World& world, EntityID entity, float (&out)[3])
{
    const ChainRotScale f = ComposeChain(world, entity);
    const float conj[4] = { -f.q[0], -f.q[1], -f.q[2], f.q[3] };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    RotateByQuat(conj, up, out);
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 0.0f) {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    } else {
        out[0] = 0.0f;
        out[1] = 1.0f;
        out[2] = 0.0f;
    }
}

// ---- ルートモーションのヨー (M89k) ----
// 上まわりのひねり (cos(θ/2), sin(θ/2)) = 四元数 (s·up, c)。同じ軸まわりなので積は複素数の積 (可換)
struct Yaw {
    float c = 1.0f;
    float s = 0.0f;
};
Yaw YawFrom(const DirectX::XMFLOAT2& v)
{
    return { v.x, v.y };
}
Yaw YawMul(const Yaw& a, const Yaw& b)
{
    return { a.c * b.c - a.s * b.s, a.c * b.s + a.s * b.c };
}
Yaw YawConj(const Yaw& a)
{
    return { a.c, -a.s };
}

// v を up まわりに yaw だけ回す。回らない (s == 0) なら v のまま (M89j の結果をビットも変えない)
void RotateByYaw(const Yaw& yaw, const float (&up)[3], const float (&v)[3], float (&out)[3])
{
    if (yaw.s == 0.0f) {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        return;
    }
    const float q[4] = { yaw.s * up[0], yaw.s * up[1], yaw.s * up[2], yaw.c };
    RotateByQuat(q, v, out);
}

// 1 周の移動 c と 1 周のヨー z から、n 周 (n >= 0) の移動 Σ_{k<n} z^k·c と z^n を倍々で求める
// (S(a+b) = S(a) + z^a·S(b))。1 tick で何周しても反復は高々 64 回
void RepeatCycle(const Yaw& z, const float (&up)[3], const float (&c)[3], uint64_t n, float (&sum)[3], Yaw& power)
{
    sum[0] = sum[1] = sum[2] = 0.0f;
    power = {};
    float blockSum[3] = { c[0], c[1], c[2] };
    Yaw blockPower = z;
    for (; n != 0; n >>= 1) {
        if ((n & 1u) != 0) {
            float r[3];
            RotateByYaw(power, up, blockSum, r);
            for (int k = 0; k < 3; ++k) {
                sum[k] += r[k];
            }
            power = YawMul(power, blockPower);
        }
        float r[3];
        RotateByYaw(blockPower, up, blockSum, r);
        for (int k = 0; k < 3; ++k) {
            blockSum[k] += r[k];
        }
        blockPower = YawMul(blockPower, blockPower);
    }
}

// 1 層のこの tick のルートジョイントの動き (ルートの親空間)
struct RootLayerMotion {
    float delta[3] = {};
    Yaw yaw;
};

// 区間 start → end の動きに、ループの折り返し wraps 回ぶんの 1 周 (移動は末尾 − 先頭、ヨーは末尾のひねり) を足す。
// followYaw (エンティティがヨーを受け取って回る) なら、クリップの座標での移動を区間の頭までに回ったぶん戻して
// 今のエンティティの向きで表す。周をまたぐと、エンティティは 1 周ぶんのヨーだけ回った向きから次の周を始める
RootLayerMotion RootLayerDelta(const SkinnedModel& model, int32_t clip, int32_t joint, const float (&up)[3],
                               bool followYaw, int32_t startQ, int32_t endQ, int64_t wraps, int32_t lengthTicks)
{
    const float startSec = static_cast<float>(startQ) / kTimeQPerSecond;
    const float endSec = static_cast<float>(endQ) / kTimeQPerSecond;
    const DirectX::XMFLOAT3 a = SampleJointTranslation(model, clip, joint, startSec);
    const DirectX::XMFLOAT3 b = SampleJointTranslation(model, clip, joint, endSec);
    const Yaw za = YawFrom(SampleJointYaw(model, clip, joint, startSec, up));
    const Yaw zb = YawFrom(SampleJointYaw(model, clip, joint, endSec, up));
    RootLayerMotion m;
    float d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    Yaw cycles; // 1 周のヨーの wraps 乗
    if (wraps != 0) {
        const float tailSec = static_cast<float>(BlendSpanQ(lengthTicks)) / kTimeQPerSecond;
        const DirectX::XMFLOAT3 head = SampleJointTranslation(model, clip, joint, 0.0f);
        const DirectX::XMFLOAT3 tail = SampleJointTranslation(model, clip, joint, tailSec);
        const Yaw cycleYaw = YawFrom(SampleJointYaw(model, clip, joint, tailSec, up));
        if (cycleYaw.s == 0.0f) {
            // 1 周で向きが変わらない: 周ごとの移動は同じ向き (M89j と同じ式)
            const float k = static_cast<float>(wraps);
            d[0] += k * (tail.x - head.x);
            d[1] += k * (tail.y - head.y);
            d[2] += k * (tail.z - head.z);
        } else {
            // 移動 = Σ_{k=0}^{n-1} z^k·C + z^n·(b − head) − (a − head)。n < 0 の和は −z^n·Σ_{k<|n|} z^k·C
            const float cycle[3] = { tail.x - head.x, tail.y - head.y, tail.z - head.z };
            float sum[3];
            Yaw power;
            RepeatCycle(cycleYaw, up, cycle, static_cast<uint64_t>(wraps > 0 ? wraps : -wraps), sum, power);
            if (wraps < 0) {
                cycles = YawConj(power);
                float r[3];
                RotateByYaw(cycles, up, sum, r);
                for (int k = 0; k < 3; ++k) {
                    sum[k] = -r[k];
                }
            } else {
                cycles = power;
            }
            const float last[3] = { b.x - head.x, b.y - head.y, b.z - head.z };
            float lastRotated[3];
            RotateByYaw(cycles, up, last, lastRotated);
            d[0] = sum[0] + lastRotated[0] - (a.x - head.x);
            d[1] = sum[1] + lastRotated[1] - (a.y - head.y);
            d[2] = sum[2] + lastRotated[2] - (a.z - head.z);
        }
    }
    m.yaw = YawMul(YawMul(cycles, zb), YawConj(za));
    if (followYaw) {
        RotateByYaw(YawConj(za), up, d, m.delta);
    } else {
        m.delta[0] = d[0];
        m.delta[1] = d[1];
        m.delta[2] = d[2];
    }
    return m;
}

// この tick のルートの移動とヨー (主 SkinnedMesh のモデルで測る、ルートの親空間)。ポーズプログラムと同じ層・同じ時計・
// 同じ重みで、層ごとの動きを重みで混ぜる (移動は線形、ヨーは cos >= 0 にそろえた和を正規化 = nlerp)。
// 単一クリップは tick の時計 (折り返しは time の周回数)、ブレンドツリーの子は位相 (折り返しは位相の周回数) から
// 時刻を引く。非ループは端に張り付くので折り返さない。up はルートの親空間の上
void RootMotionDelta(const SkinnedModel& model, int32_t joint, const float (&up)[3], bool followYaw,
                     const SkeletalSource* sources, int32_t count, float (&out)[3], Yaw& outYaw)
{
    constexpr int32_t kQ = SkinnedMeshComponent::kPoseTimeQPerTick;
    out[0] = out[1] = out[2] = 0.0f;
    float yawC = 0.0f;
    float yawS = 0.0f;
    for (int32_t i = 0; i < count; ++i) {
        const SkeletalSource& src = sources[i];
        const int32_t clip = model.FindClipByHash(src.clipHash);
        if (clip < 0 || src.weightQ <= 0) {
            continue;
        }
        const int32_t ticks = SkeletalClipTicks(model.clips[static_cast<size_t>(clip)]);
        if (ticks <= 0) {
            continue;
        }
        const StateClock& clock = src.clock;
        RootLayerMotion m;
        if (src.usesPhase) {
            const int64_t wraps = src.loop ? FloorDiv(int64_t(clock.prevPhase) + clock.phaseStep, int64_t(kPhaseCycle)) : 0;
            m = RootLayerDelta(model, clip, joint, up, followYaw, BlendPhaseToTimeQ(clock.prevPhase, ticks, src.loop),
                               BlendPhaseToTimeQ(clock.phase, ticks, src.loop), wraps, ticks);
        } else {
            // 単一クリップのステートの時計は主 SkinnedMesh の長さで回っている (ステートの長さと同じ)
            const int64_t wraps = src.loop ? FloorDiv(int64_t(clock.prevTime) + clock.timeStep, ticks) : 0;
            m = RootLayerDelta(model, clip, joint, up, followYaw, clock.prevTime * kQ, clock.time * kQ, wraps, ticks);
        }
        const float w = static_cast<float>(src.weightQ) / static_cast<float>(kWeightOne);
        out[0] += w * m.delta[0];
        out[1] += w * m.delta[1];
        out[2] += w * m.delta[2];
        // 同じ回転の (c, s) と (−c, −s) を混ぜると打ち消し合うので、短い弧 (c >= 0) にそろえてから足す
        const float sign = m.yaw.c < 0.0f ? -1.0f : 1.0f;
        yawC += w * sign * m.yaw.c;
        yawS += w * sign * m.yaw.s;
    }
    outYaw = {};
    if (yawS != 0.0f) {
        const float len = std::sqrt(yawC * yawC + yawS * yawS);
        outYaw = { yawC / len, yawS / len };
    }
}

// ルートモーションのヨーでエンティティを回すか (= ポーズからひねりを抜くか)。NavMeshAgent が updateRotation で
// 向きを握っている間は回さない (両方で回すと Nav の旋回と取り合う)。そのときポーズはクリップのひねりを残す
bool RootYawFollows(World& world, EntityID entity, const AnimatorControllerComponent& c)
{
    if (!c.applyRootMotion) {
        return false;
    }
    const NavMeshAgentComponent* agent = world.GetComponent<NavMeshAgentComponent>(entity);
    return agent == nullptr || !agent->updateRotation;
}

// ワールドの Y 軸まわりのヨー yaw で entity の LocalTransform.rotation を回す。親があれば、ワールドの上を
// 親空間へ戻した軸で回す (連鎖の回転の共役)。四元数は回した後に正規化する
void ApplyRootYaw(World& world, EntityID entity, const Yaw& yaw)
{
    LocalTransform* lt = world.GetComponent<LocalTransform>(entity);
    if (lt == nullptr || yaw.s == 0.0f) {
        return;
    }
    float axis[3] = { 0.0f, 1.0f, 0.0f };
    float s = yaw.s;
    if (const EntityID parent = world.GetParent(entity); !parent.IsNull()) {
        WorldUpInEntitySpace(world, parent, axis);
        const ChainRotScale f = ComposeChain(world, parent);
        if (f.s[0] * f.s[1] * f.s[2] < 0.0f) {
            s = -s; // 鏡映した親の下では、同じワールドの回りが親空間では逆回りになる
        }
    }
    // q' = q_yaw · q (親空間で後から回す)
    const float ax = s * axis[0], ay = s * axis[1], az = s * axis[2], aw = yaw.c;
    const float bx = lt->rotation.x, by = lt->rotation.y, bz = lt->rotation.z, bw = lt->rotation.w;
    float q[4] = {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    };
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 0.0f) {
        lt->rotation = { q[0] / len, q[1] / len, q[2] / len, q[3] / len };
    }
}

// ルートモーションの水平の移動 deltaWorld (1 tick ぶん、ワールド) で entity を動かす (applyRootMotion の適用先、
// AnimatorControllerComponent の注記の順)。速度で渡す先は velocity (m/s) を、LocalTransform は移動そのものを書く
void ApplyRootMotion(World& world, EntityID entity, const float (&deltaWorld)[3], const DirectX::XMFLOAT3& velocity)
{
    if (const NavMeshAgentComponent* agent = world.GetComponent<NavMeshAgentComponent>(entity);
        agent != nullptr && agent->updatePosition) {
        return; // Nav が moveInput を書く。両方で動かすと速度が倍になる
    }
    if (RigidbodyComponent* rb = world.GetComponent<RigidbodyComponent>(entity)) {
        if (!rb->isKinematic) {
            // 縦 (重力・ジャンプ) は物理に任せる
            rb->velocity.x = velocity.x;
            rb->velocity.z = velocity.z;
            return;
        }
        // kinematic は物理が積分しない。CharacterController も Rigidbody が居ると無効なので Transform へ
    } else if (CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(entity)) {
        cc->moveInput.x = velocity.x;
        cc->moveInput.z = velocity.z;
        return;
    }
    LocalTransform* lt = world.GetComponent<LocalTransform>(entity);
    if (lt == nullptr) {
        return;
    }
    // 親があればワールドの移動を親空間へ戻す (回転の共役と拡大の割り算。拡大 0 の軸は動かさない)
    float local[3] = { deltaWorld[0], deltaWorld[1], deltaWorld[2] };
    if (const EntityID parent = world.GetParent(entity); !parent.IsNull()) {
        const ChainRotScale f = ComposeChain(world, parent);
        const float conj[4] = { -f.q[0], -f.q[1], -f.q[2], f.q[3] };
        float r[3];
        RotateByQuat(conj, deltaWorld, r);
        for (int k = 0; k < 3; ++k) {
            local[k] = f.s[k] != 0.0f ? r[k] / f.s[k] : 0.0f;
        }
    }
    lt->position.x += local[0];
    lt->position.y += local[1];
    lt->position.z += local[2];
}

} // namespace

// ==== ControllerLibrary ====

uint64_t ControllerLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t ControllerLibrary::Register(const std::wstring& path, ControllerAsset asset)
{
    const uint64_t hash = HashForPath(path);
    asset.hash = hash;
    asset.path = path;
    if (asset.name.empty()) {
        asset.name = NameFromPath(path);
    }
    controllers_[hash] = std::move(asset);
    return hash;
}

uint64_t ControllerLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(path);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (...) {
        MYE_LOG_WARN("[controller] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    ControllerAsset a;
    try {
        if (!FromJson(j, a)) {
            return 0;
        }
    } catch (const json::exception&) {
        return 0;
    }
    a.name = NameFromPath(path);
    // 旧形式の clipPath (文字列参照) は controller ファイルのディレクトリからの相対で解決する。
    // GUID 参照 (M39a) は FromJson が clipHash を直接埋めるのでここは素通り
    const fs::path dir = fs::path(path).parent_path();
    for (ControllerState& st : a.states) {
        if (!st.clipPath.empty()) {
            const std::wstring full = (dir / Utf8ToWide(st.clipPath)).wstring();
            st.clipHash = AnimationLibrary::HashForPath(full);
        }
    }
    return Register(path, std::move(a));
}

bool ControllerLibrary::SaveToFile(uint64_t hash) const
{
    const ControllerAsset* c = Get(hash);
    if (!c) {
        return false;
    }
    return WriteFileReplacing(c->path, ToJson(*c).dump(2));
}

const ControllerAsset* ControllerLibrary::Get(uint64_t hash) const
{
    auto it = controllers_.find(hash);
    return it == controllers_.end() ? nullptr : &it->second;
}

ControllerAsset* ControllerLibrary::GetMutable(uint64_t hash)
{
    auto it = controllers_.find(hash);
    return it == controllers_.end() ? nullptr : &it->second;
}

std::vector<ControllerEntry> ControllerLibrary::Enumerate() const
{
    std::vector<ControllerEntry> out;
    out.reserve(controllers_.size());
    for (const auto& [h, c] : controllers_) {
        out.push_back({ h, c.name });
    }
    return out;
}

json ControllerLibrary::ToJson(const ControllerAsset& c)
{
    json root;
    root["engine"] = "MyEngine";
    root["controller"] = 2; // v2 (M89b): ステートの "skel"。v1 はそのまま読める (無い = 骨を駆動しない)
    root["defaultState"] = c.defaultState;
    json params = json::array();
    for (const ControllerParam& p : c.parameters) {
        params.push_back({ { "name", p.name }, { "type", ParamTypeToStr(p.type) } });
    }
    root["parameters"] = std::move(params);
    json states = json::array();
    for (const ControllerState& s : c.states) {
        // M39a: 解決済みクリップは GUID (数値) で書く — クリップのリネーム/移動に追従する。
        // 未解決 (clipHash==0) は旧 clipPath 文字列を温存 (壊れた参照を消さない)
        json clip;
        if (s.clipHash != 0) {
            clip = s.clipHash;
        } else {
            clip = s.clipPath;
        }
        json st = { { "name", s.name },
                    { "clip", std::move(clip) },
                    { "speed", s.speed },
                    { "loop", s.loop } };
        if (s.blendType == ControllerBlendType::Blend2D) {
            json children = json::array();
            for (const ControllerBlendChild& ch : s.blendChildren) {
                children.push_back({ { "clip", ch.clip }, { "x", ch.posX }, { "y", ch.posY } });
            }
            st["skel"] = { { BlendTypeToStr(s.blendType),
                             { { "paramX", s.blendParam }, { "paramY", s.blendParamY }, { "children", std::move(children) } } } };
        } else if (s.blendType != ControllerBlendType::None) {
            json children = json::array();
            for (const ControllerBlendChild& ch : s.blendChildren) {
                children.push_back({ { "clip", ch.clip }, { "threshold", ch.threshold } });
            }
            st["skel"] = { { BlendTypeToStr(s.blendType),
                             { { "param", s.blendParam }, { "children", std::move(children) } } } };
        } else if (!s.skelClip.empty()) {
            st["skel"] = { { "clip", s.skelClip } };
        }
        states.push_back(std::move(st));
    }
    root["states"] = std::move(states);
    if (!c.clipEvents.empty()) {
        // M89h。クリップ名がキー。同名の 2 つ目以降は使われないので書かない
        json clipEvents = json::object();
        for (const ControllerClipEvents& ce : c.clipEvents) {
            if (clipEvents.contains(ce.clip)) {
                continue;
            }
            json& list = clipEvents[ce.clip];
            list = json::array();
            for (const ControllerClipEvent& ev : ce.events) {
                // 種類ごとに使う欄だけを書く (M89i)
                json e = { { "tick", ev.tick },
                           { "name", ev.name },
                           { "kind", ClipEventKindToStr(ev.kind) },
                           { "minWeight", ev.minWeight } };
                switch (ev.kind) {
                case ClipEventKind::Script:
                    e["value"] = ev.value;
                    e["int"] = ev.intValue;
                    break;
                case ClipEventKind::Sound:
                    e["sound"] = ev.asset;
                    e["volume"] = ev.volume;
                    e["pitch"] = ev.pitch;
                    break;
                case ClipEventKind::Effect:
                    e["prefab"] = ev.asset;
                    break;
                case ClipEventKind::Noise:
                    e["loudness"] = ev.loudness;
                    e["range"] = ev.range;
                    break;
                }
                if (!ev.joint.empty()) {
                    e["joint"] = ev.joint;
                }
                list.push_back(std::move(e));
            }
        }
        root["clipEvents"] = std::move(clipEvents);
    }
    json trans = json::array();
    for (const ControllerTransition& t : c.transitions) {
        json conds = json::array();
        for (const ControllerCondition& cc : t.conditions) {
            // 値は参照先の型で書き分ける (Float は小数、他は整数)。Trigger は値を見ないが、形をそろえて書く
            json value = cc.value;
            if (ControllerParamTypeAt(c, cc.param) == ControllerParamType::Float) {
                value = cc.floatValue;
            }
            conds.push_back({ { "param", cc.param }, { "op", OpToStr(cc.op) }, { "value", std::move(value) } });
        }
        trans.push_back({ { "from", t.from },
                          { "to", t.to },
                          { "duration", t.duration },
                          { "hasExitTime", t.hasExitTime },
                          { "conditions", std::move(conds) } });
    }
    root["transitions"] = std::move(trans);
    return root;
}

bool ControllerLibrary::FromJson(const json& j, ControllerAsset& out)
{
    if (!j.contains("states") || !j["states"].is_array()) {
        return false;
    }
    out.defaultState = j.value("defaultState", 0);
    out.parameters.clear();
    out.states.clear();
    out.transitions.clear();

    if (j.contains("parameters") && j["parameters"].is_array()) {
        for (const json& p : j["parameters"]) {
            ControllerParam cp;
            cp.name = p.value("name", std::string());
            cp.type = StrToParamType(p.value("type", std::string("int"))); // M89c。無い = v1 = int
            out.parameters.push_back(std::move(cp));
        }
        if (out.parameters.size() > static_cast<size_t>(AnimatorControllerComponent::kMaxParams)) {
            // 値の置き場 (params) が無い宣言は読まずに捨てる (後ろを黙って無視すると条件が常に偽になる)
            MYE_LOG_WARN("[controller] %zu parameters declared; only the first %d are kept", out.parameters.size(),
                         AnimatorControllerComponent::kMaxParams);
            out.parameters.resize(static_cast<size_t>(AnimatorControllerComponent::kMaxParams));
        }
    }
    for (const json& s : j["states"]) {
        ControllerState cs;
        cs.name = s.value("name", std::string());
        // M39a: "clip" 両対応読み — 数値なら GUID (= AnimationLibrary のキーそのもの)、
        // 文字列なら従来の相対パス (LoadFromFile が baseDir 相対で clipHash に解決)
        if (s.contains("clip")) {
            const json& clip = s["clip"];
            if (clip.is_number_unsigned() || clip.is_number_integer()) {
                cs.clipHash = clip.get<uint64_t>();
            } else if (clip.is_string()) {
                cs.clipPath = clip.get<std::string>();
            }
        }
        cs.speed = s.value("speed", 1);
        cs.loop = s.value("loop", 1);
        // M89b: 骨クリップは名前で持つ (モデルごとに index が違ってよい)
        if (s.contains("skel") && s["skel"].is_object()) {
            const json& skel = s["skel"];
            if (skel.contains("blend2d") && skel["blend2d"].is_object()) {
                // M89e: 2D ブレンドツリー。blend1d / "clip" より優先する
                const json& blend = skel["blend2d"];
                cs.blendType = ControllerBlendType::Blend2D;
                cs.blendParam = blend.value("paramX", 0);
                cs.blendParamY = blend.value("paramY", 0);
                if (blend.contains("children") && blend["children"].is_array()) {
                    for (const json& ch : blend["children"]) {
                        ControllerBlendChild child;
                        child.clip = ch.value("clip", std::string());
                        child.clipHash = child.clip.empty() ? 0 : HashStr(child.clip);
                        child.posX = ch.value("x", 0.0f);
                        child.posY = ch.value("y", 0.0f);
                        cs.blendChildren.push_back(std::move(child));
                    }
                }
            } else if (skel.contains("blend1d") && skel["blend1d"].is_object()) {
                // M89d: 1D ブレンドツリー。あれば "clip" より優先する
                const json& blend = skel["blend1d"];
                cs.blendType = ControllerBlendType::Blend1D;
                cs.blendParam = blend.value("param", 0);
                if (blend.contains("children") && blend["children"].is_array()) {
                    for (const json& ch : blend["children"]) {
                        ControllerBlendChild child;
                        child.clip = ch.value("clip", std::string());
                        child.clipHash = child.clip.empty() ? 0 : HashStr(child.clip);
                        child.threshold = ch.value("threshold", 0.0f);
                        cs.blendChildren.push_back(std::move(child));
                    }
                }
            } else {
                cs.skelClip = skel.value("clip", std::string());
                cs.skelClipHash = cs.skelClip.empty() ? 0 : HashStr(cs.skelClip);
            }
        }
        out.states.push_back(std::move(cs));
    }
    if (j.contains("transitions") && j["transitions"].is_array()) {
        for (const json& t : j["transitions"]) {
            ControllerTransition ct;
            ct.from = t.value("from", -1);
            ct.to = t.value("to", 0);
            ct.duration = t.value("duration", 8);
            ct.hasExitTime = t.value("hasExitTime", 0);
            if (t.contains("conditions") && t["conditions"].is_array()) {
                for (const json& cj : t["conditions"]) {
                    ControllerCondition cc;
                    cc.param = cj.value("param", 0);
                    cc.op = StrToOp(cj.value("op", std::string("gt")));
                    // 参照先が Float なら小数で読む (parameters は上で読み終えている)
                    if (ControllerParamTypeAt(out, cc.param) == ControllerParamType::Float) {
                        cc.floatValue = cj.value("value", 0.0f);
                    } else {
                        cc.value = cj.value("value", 0);
                    }
                    ct.conditions.push_back(cc);
                }
            }
            out.transitions.push_back(std::move(ct));
        }
    }
    out.clipEvents.clear();
    if (j.contains("clipEvents") && j["clipEvents"].is_object()) {
        // M89h。オブジェクトのキー順 (nlohmann はキーの辞書順) で並ぶ
        for (const auto& [clipName, list] : j["clipEvents"].items()) {
            if (!list.is_array()) {
                continue;
            }
            ControllerClipEvents ce;
            ce.clip = clipName;
            ce.clipHash = HashStr(clipName);
            for (const json& e : list) {
                const std::string kind = e.value("kind", std::string("script"));
                ControllerClipEvent ev;
                if (!ClipEventKindFromStr(kind, ev.kind)) {
                    MYE_LOG_WARN("[controller] clip event kind '%s' is not supported; skipped", kind.c_str());
                    continue;
                }
                ev.name = e.value("name", std::string());
                ev.nameHash = HashStr(ev.name);
                ev.tick = e.value("tick", 0);
                ev.minWeight = e.value("minWeight", 0.0f);
                ev.value = e.value("value", 0.0f);
                ev.intValue = e.value("int", 0);
                ev.joint = e.value("joint", std::string());
                if (ev.kind == ClipEventKind::Sound) {
                    ev.asset = e.value("sound", std::string());
                } else if (ev.kind == ClipEventKind::Effect) {
                    ev.asset = e.value("prefab", std::string());
                }
                ev.assetHash = HashStr(ev.asset);
                ev.volume = e.value("volume", 1.0f);
                ev.pitch = e.value("pitch", 1.0f);
                ev.loudness = e.value("loudness", 1.0f);
                ev.range = e.value("range", 10.0f);
                ce.events.push_back(std::move(ev));
            }
            out.clipEvents.push_back(std::move(ce));
        }
    }
    return true;
}

bool StateDrivesSkeleton(const ControllerState& state)
{
    if (state.blendType != ControllerBlendType::None) {
        return !state.blendChildren.empty();
    }
    return state.skelClipHash != 0;
}

int32_t ComputeBlendWeights(const ControllerState& state, float x, float y, BlendChildWeight (&out)[kMaxBlendLayers])
{
    if (state.blendChildren.empty()) {
        return 0;
    }
    switch (state.blendType) {
    case ControllerBlendType::Blend1D: return ComputeBlend1DWeights(state, x, out);
    case ControllerBlendType::Blend2D: return ComputeBlend2DWeights(state, x, y, out);
    case ControllerBlendType::None: break;
    }
    return 0;
}

int32_t BlendPhaseToTimeQ(uint32_t phase, int32_t lengthTicks, int32_t loop)
{
    if (lengthTicks <= 0) {
        return 0;
    }
    const int64_t span = BlendSpanQ(lengthTicks);
    if (!loop && phase == UINT32_MAX) {
        return static_cast<int32_t>(span);
    }
    return static_cast<int32_t>((static_cast<uint64_t>(phase) * static_cast<uint64_t>(span)) >> 32);
}

int64_t ClipEventPassDistance(int64_t pos, int64_t old, int64_t step, int64_t cycle, int32_t loop, bool entered)
{
    if (cycle <= 0 || pos < 0 || pos > cycle) {
        return -1;
    }
    const int64_t span = step >= 0 ? step : -step;
    if (!loop) {
        // 時計は [0, cycle] に張り付くので、old + step が端を越えることはない
        const int64_t dist = step >= 0 ? pos - old : old - pos;
        if (dist == 0) {
            return entered ? 0 : -1;
        }
        return (dist > 0 && dist <= span) ? dist : -1;
    }
    // 進む向きに測った old から pos までの距離 (0..cycle-1)。cycle ちょうどの pos は 0 に重なる
    const int64_t p = pos % cycle;
    const int64_t o = ((old % cycle) + cycle) % cycle;
    const int64_t dist = (((step >= 0 ? p - o : o - p) % cycle) + cycle) % cycle;
    if (dist == 0) {
        // old ちょうど = 前の tick に通った位置。入った tick と、1 tick で 1 周以上するときだけ含める
        if (entered) {
            return 0;
        }
        return span >= cycle ? cycle : -1;
    }
    return (span >= cycle || dist <= span) ? dist : -1;
}

float ControllerParamAsFloat(const ControllerAsset& controller, int32_t index, const int32_t* params)
{
    if (params == nullptr || index < 0 || index >= AnimatorControllerComponent::kMaxParams) {
        return 0.0f;
    }
    const int32_t raw = params[index];
    switch (ControllerParamTypeAt(controller, index)) {
    case ControllerParamType::Int: return static_cast<float>(raw);
    case ControllerParamType::Float: return std::bit_cast<float>(raw);
    case ControllerParamType::Bool:
    case ControllerParamType::Trigger: return raw != 0 ? 1.0f : 0.0f;
    }
    return 0.0f;
}

int32_t FindControllerState(const ControllerAsset& controller, const std::string& name)
{
    for (size_t i = 0; i < controller.states.size(); ++i) {
        if (controller.states[i].name == name) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t FindControllerParam(const ControllerAsset& controller, uint64_t nameHash)
{
    const size_t n = std::min(controller.parameters.size(), static_cast<size_t>(AnimatorControllerComponent::kMaxParams));
    for (size_t i = 0; i < n; ++i) {
        if (HashStr(controller.parameters[i].name) == nameHash) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

ControllerParamType ControllerParamTypeAt(const ControllerAsset& controller, int32_t index)
{
    if (index < 0 || index >= AnimatorControllerComponent::kMaxParams
        || index >= static_cast<int32_t>(controller.parameters.size())) {
        return ControllerParamType::Int;
    }
    return controller.parameters[static_cast<size_t>(index)].type;
}

bool AnimatorSetParam(World& world, EntityID entity, uint64_t nameHash, ControllerParamType type, int32_t bits,
                      const ControllerLibrary& controllers)
{
    AnimatorControllerComponent* c = world.GetComponent<AnimatorControllerComponent>(entity);
    const ControllerAsset* ctrl = c != nullptr ? controllers.Get(c->controller.value) : nullptr;
    if (ctrl == nullptr) {
        return false;
    }
    const int32_t index = FindControllerParam(*ctrl, nameHash);
    if (index < 0 || ctrl->parameters[static_cast<size_t>(index)].type != type) {
        return false;
    }
    if (type == ControllerParamType::Bool || type == ControllerParamType::Trigger) {
        bits = bits != 0 ? 1 : 0;
    }
    c->params[index] = bits;
    return true;
}

void CollectDrivenSkinnedMeshes(World& world, EntityID controllerEntity, std::vector<EntityID>& out)
{
    out.clear();
    ForEachInSubtree(world, controllerEntity, [&](EntityID e, uint32_t) {
        if (e != controllerEntity && world.GetComponent<AnimatorControllerComponent>(e) != nullptr) {
            return WalkStep::SkipChildren;
        }
        if (world.GetComponent<SkinnedMeshComponent>(e) != nullptr) {
            out.push_back(e);
        }
        return WalkStep::Continue;
    });
}

namespace {

// driven (CollectDrivenSkinnedMeshes の結果) のうち entity index が最小のもの (主 SkinnedMesh)。無ければ kNullEntity。
// 走査順 (前順) ではなく index で選ぶのは、兄弟の並べ替えでステートの長さが変わらないようにするため
EntityID MainSkinnedEntity(const std::vector<EntityID>& driven)
{
    EntityID main = kNullEntity;
    for (EntityID e : driven) {
        if (main == kNullEntity || e.index < main.index) {
            main = e;
        }
    }
    return main;
}

// 主 SkinnedMesh のモデル
const SkinnedModel* MainModelOf(World& world, const std::vector<EntityID>& driven, const SkinnedModelLibrary* models)
{
    if (models == nullptr) {
        return nullptr;
    }
    const EntityID main = MainSkinnedEntity(driven);
    const SkinnedMeshComponent* sm = main != kNullEntity ? world.GetComponent<SkinnedMeshComponent>(main) : nullptr;
    return sm != nullptr ? models->Get(sm->model) : nullptr;
}

// WorldMatrix の平行移動 (WorldMatrix が無ければ原点)
void WorldTranslation(World& world, EntityID entity, float (&out)[3])
{
    const WorldMatrixComponent* wm = world.GetComponent<WorldMatrixComponent>(entity);
    out[0] = wm != nullptr ? wm->value._41 : 0.0f;
    out[1] = wm != nullptr ? wm->value._42 : 0.0f;
    out[2] = wm != nullptr ? wm->value._43 : 0.0f;
}

// fired[first..] (1 つのコントローラが今 tick に発火させた分) の位置を埋める (M89i)。
// joint のあるイベントは主 SkinnedMesh の今のポーズ (今 tick に書いたプログラム) を 1 回だけ組み、
// JointGlobalFromLocals × 主 SkinnedMesh の WorldMatrix (部位ソケットと同じ式、M48a) の平行移動にする。
// joint が空・モデルに無いジョイントは controllerEntity の WorldMatrix の平行移動
void ResolveEventPositions(World& world, EntityID controllerEntity, const std::vector<EntityID>& driven,
                           const SkinnedModelLibrary* models, std::vector<AnimEventFired>& fired, size_t first)
{
    float origin[3];
    WorldTranslation(world, controllerEntity, origin);
    const EntityID mainEntity = MainSkinnedEntity(driven);
    const SkinnedMeshComponent* sm =
        mainEntity != kNullEntity ? world.GetComponent<SkinnedMeshComponent>(mainEntity) : nullptr;
    const SkinnedModel* model = (sm != nullptr && models != nullptr) ? models->Get(sm->model) : nullptr;
    std::vector<DirectX::XMMATRIX> locals; // joint のあるイベントが発火したときだけ組む
    for (size_t i = first; i < fired.size(); ++i) {
        AnimEventFired& f = fired[i];
        std::copy(std::begin(origin), std::end(origin), std::begin(f.pos));
        const int32_t joint = (model != nullptr && !f.def->joint.empty()) ? model->FindJointByName(f.def->joint) : -1;
        if (joint < 0) {
            continue;
        }
        if (locals.empty()) {
            SampleSkinnedLocals(*model, *sm, locals);
        }
        const WorldMatrixComponent* wm = world.GetComponent<WorldMatrixComponent>(mainEntity);
        const DirectX::XMMATRIX entityWorld =
            wm != nullptr ? DirectX::XMLoadFloat4x4(&wm->value) : DirectX::XMMatrixIdentity();
        DirectX::XMFLOAT4X4 g;
        DirectX::XMStoreFloat4x4(&g, DirectX::XMMatrixMultiply(JointGlobalFromLocals(*model, locals, joint), entityWorld));
        f.pos[0] = g._41;
        f.pos[1] = g._42;
        f.pos[2] = g._43;
    }
}

const ControllerClipEvents* FindClipEvents(const ControllerAsset& ctrl, uint64_t clipHash)
{
    for (const ControllerClipEvents& ce : ctrl.clipEvents) {
        if (ce.clipHash == clipHash) {
            return &ce;
        }
    }
    return nullptr;
}

// minWeight (0..1) を Q16 に 1 回だけ切り捨てる。NaN と負は 0 (絞らない)
int32_t MinWeightQ(float minWeight)
{
    if (!(minWeight > 0.0f)) {
        return 0;
    }
    return static_cast<int32_t>(std::min(minWeight, 1.0f) * static_cast<float>(kWeightOne));
}

// イベントの位置 (tick) を長さ lengthTicks のクリップの位相にする (ブレンドツリーの時計と同じ単位)。
// ループは 1 周 = 2^32 (lengthTicks ちょうどは 2^32 = 0 と同じ位置)。非ループの末尾は位相が張り付く UINT32_MAX。
// クリップの外は -1 (発火しない)
int64_t EventPhase(int32_t tick, int32_t lengthTicks, int32_t loop)
{
    if (tick < 0 || tick > lengthTicks) {
        return -1;
    }
    if (!loop && tick == lengthTicks) {
        return int64_t(UINT32_MAX);
    }
    return (int64_t(tick) << 32) / lengthTicks;
}

// 1 層 (長さ lengthTicks のクリップを clock で回した) が通ったイベントを、通った順に fired へ足す
void FireClipEvents(EntityID entity, const ControllerClipEvents& ce, int32_t lengthTicks, bool usesPhase,
                    const StateClock& clock, int32_t loop, bool entered, int32_t weightQ,
                    std::vector<std::pair<int64_t, int32_t>>& passed, std::vector<AnimEventFired>& fired)
{
    if (lengthTicks <= 0 || weightQ <= 0) {
        return;
    }
    passed.clear();
    for (size_t i = 0; i < ce.events.size(); ++i) {
        const ControllerClipEvent& ev = ce.events[i];
        if (weightQ < MinWeightQ(ev.minWeight)) {
            continue;
        }
        int64_t dist = -1;
        if (usesPhase) {
            // 非ループの位相は 0..UINT32_MAX に張り付くので、端を含む [0, UINT32_MAX] を 1 周として見る
            const int64_t cycle = loop ? int64_t(kPhaseCycle) : int64_t(UINT32_MAX);
            dist = ClipEventPassDistance(EventPhase(ev.tick, lengthTicks, loop), clock.prevPhase, clock.phaseStep,
                                         cycle, loop, entered);
        } else {
            dist = ClipEventPassDistance(ev.tick, clock.prevTime, clock.timeStep, lengthTicks, loop, entered);
        }
        if (dist >= 0) {
            passed.push_back({ dist, static_cast<int32_t>(i) });
        }
    }
    std::sort(passed.begin(), passed.end()); // 通った順、同じ位置はイベントの並び順
    for (const auto& [dist, index] : passed) {
        const ControllerClipEvent& ev = ce.events[static_cast<size_t>(index)];
        AnimEventFired f;
        f.entity = entity;
        f.nameHash = ev.nameHash;
        f.value = ev.value;
        f.intValue = ev.intValue;
        f.kind = ev.kind;
        f.def = &ev; // 位置は ResolveEventPositions がプログラムを書いた後に埋める
        fired.push_back(f);
    }
}

// 1 ステートのイベント層。骨クリップ 1 本はその時計で、ブレンドツリーは最大重みの子 (同値なら index の小さい子)
// 1 本だけを位相で判定する。長さは主 SkinnedMesh のモデルで引く
void FireStateEvents(const ControllerAsset& ctrl, const ControllerState& st, const int32_t* params,
                     const StateClock& clock, bool entered, int32_t stateWeightQ, const SkinnedModel& mainModel,
                     EntityID entity, std::vector<std::pair<int64_t, int32_t>>& passed,
                     std::vector<AnimEventFired>& fired)
{
    if (st.blendType == ControllerBlendType::None) {
        const ControllerClipEvents* ce = st.skelClipHash != 0 ? FindClipEvents(ctrl, st.skelClipHash) : nullptr;
        const int32_t clip = ce != nullptr ? mainModel.FindClipByHash(st.skelClipHash) : -1;
        if (clip >= 0) {
            FireClipEvents(entity, *ce, SkeletalClipTicks(mainModel.clips[static_cast<size_t>(clip)]), false, clock,
                           st.loop, entered, stateWeightQ, passed, fired);
        }
        return;
    }
    BlendChildWeight w[kMaxBlendLayers];
    const int32_t n = StateBlendWeights(ctrl, st, params, w);
    if (n == 0) {
        return;
    }
    int32_t top = 0; // w は子の index の昇順なので、最初に見つけた最大が index の小さい子
    for (int32_t k = 1; k < n; ++k) {
        if (w[k].weightQ > w[top].weightQ) {
            top = k;
        }
    }
    const ControllerBlendChild& child = st.blendChildren[static_cast<size_t>(w[top].child)];
    const ControllerClipEvents* ce = FindClipEvents(ctrl, child.clipHash);
    if (ce != nullptr) {
        const int32_t weightQ = static_cast<int32_t>(int64_t(stateWeightQ) * w[top].weightQ / kWeightOne);
        FireClipEvents(entity, *ce, BlendChildTicks(&mainModel, child), true, clock, st.loop, entered, weightQ, passed,
                       fired);
    }
}

} // namespace

const SkinnedModel* MainSkinnedModel(World& world, EntityID controllerEntity, const SkinnedModelLibrary* models)
{
    if (models == nullptr) {
        return nullptr;
    }
    std::vector<EntityID> driven;
    CollectDrivenSkinnedMeshes(world, controllerEntity, driven);
    return MainModelOf(world, driven, models);
}

int32_t ControllerStateLengthTicks(const ControllerAsset& controller, const ControllerState& state,
                                   const int32_t* params, const AnimationLibrary* clips,
                                   const SkinnedModel* mainModel)
{
    if (state.blendType != ControllerBlendType::None) {
        BlendChildWeight w[kMaxBlendLayers];
        const int32_t n = StateBlendWeights(controller, state, params, w);
        const int64_t weightedTicks = BlendWeightedTicks(state, w, n, mainModel);
        if (weightedTicks > 0) {
            return static_cast<int32_t>((weightedTicks + kWeightOne - 1) / kWeightOne);
        }
    } else if (state.skelClipHash != 0 && mainModel != nullptr) {
        const int32_t clip = mainModel->FindClipByHash(state.skelClipHash);
        if (clip >= 0) {
            return SkeletalClipTicks(mainModel->clips[static_cast<size_t>(clip)]);
        }
    }
    const AnimationClipAsset* cl = clips != nullptr ? clips->Get(state.clipHash) : nullptr;
    return cl != nullptr ? cl->lengthTicks : 0;
}

bool AnimatorPlay(World& world, EntityID entity, int32_t stateIndex, int32_t durationTicks, const ControllerLibrary& controllers)
{
    AnimatorControllerComponent* c = world.GetComponent<AnimatorControllerComponent>(entity);
    if (c == nullptr) {
        return false;
    }
    const ControllerAsset* ctrl = controllers.Get(c->controller.value);
    if (ctrl == nullptr || stateIndex < 0 || stateIndex >= static_cast<int32_t>(ctrl->states.size())) {
        return false;
    }
    if (durationTicks > 0) {
        // 既存の遷移 (Update の 1.) と同じ値の立て方。混ぜる元 (currentState / stateTimeTicks) は触らない
        c->transitionTo = stateIndex;
        c->transitionTick = 0;
        c->transitionDuration = durationTicks;
        c->transitionToTime = 0;
        c->transitionToPhase = 0;
        return true;
    }
    c->currentState = stateIndex;
    c->stateTimeTicks = 0;
    c->statePhase = 0;
    c->stateEntered = 1;
    c->transitionTo = -1;
    c->transitionTick = 0;
    c->transitionDuration = 0;
    c->transitionToTime = 0;
    c->transitionToPhase = 0;
    return true;
}

// ==== AnimatorControllerSystem ====

void AnimatorControllerSystem::Update(World& world, const ControllerLibrary& controllers,
                                      const AnimationLibrary& clips, const SkinnedModelLibrary* models)
{
    fired_.clear();
    const ComponentTypeId req[] = { AnimatorControllerComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(AnimatorControllerComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            auto* c = static_cast<AnimatorControllerComponent*>(arch.GetPtr(ci, row));
            const ControllerAsset* ctrl = controllers.Get(c->controller.value);
            if (!ctrl || ctrl->states.empty()) {
                continue;
            }
            // 骨クリップを持たないコントローラは部分木を辿らない (M22 からの経路を 1 つも変えない)
            const bool drivesSkeleton = HasSkeletalStates(*ctrl);
            driven_.clear();
            if (drivesSkeleton) {
                CollectDrivenSkinnedMeshes(world, e, driven_);
            }
            if (!IsEntityActive(world, e)) {
                // 凍らせる: 書いてあるプログラムをそのまま保たせる (時刻も層も進めない)。
                // 描画補間の進みは 0 に落とす — 残すと、止まっている間も最後の tick の動きを毎 tick 繰り返して描く
                for (EntityID m : driven_) {
                    SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(m);
                    if (sm->poseLayerCount > 0) {
                        sm->poseClaim = 1;
                        for (SkinnedMeshComponent::PoseLayer& layer : sm->poseLayers) {
                            layer.prevTimeQ = layer.timeQ;
                            layer.stepQ = 0;
                        }
                    }
                }
                continue;
            }
            const SkinnedModel* mainModel = MainModelOf(world, driven_, models);
            const auto stateLength = [&](int32_t index) {
                return ControllerStateLengthTicks(*ctrl, ctrl->states[static_cast<size_t>(index)], c->params, &clips,
                                                  mainModel);
            };
            const auto phaseDelta = [&](int32_t index) {
                return BlendPhaseDelta(*ctrl, ctrl->states[static_cast<size_t>(index)], c->params, mainModel);
            };
            const auto isBlend = [&](int32_t index) {
                return ctrl->states[static_cast<size_t>(index)].blendType != ControllerBlendType::None;
            };
            const int32_t nStates = static_cast<int32_t>(ctrl->states.size());
            if (c->currentState < 0 || c->currentState >= nStates) {
                c->currentState = 0;
            }

            // 1. 遷移チェック (遷移中でなければ)。最初に条件を満たした遷移を採用
            if (c->transitionTo < 0) {
                for (const ControllerTransition& t : ctrl->transitions) {
                    if (t.to < 0 || t.to >= nStates || t.to == c->currentState) {
                        continue;
                    }
                    if (t.from != -1 && t.from != c->currentState) {
                        continue;
                    }
                    if (t.hasExitTime) {
                        if (isBlend(c->currentState)) {
                            if (!BlendExitReached(c->statePhase, phaseDelta(c->currentState))) {
                                continue;
                            }
                        } else {
                            const int32_t len = stateLength(c->currentState);
                            if (!(len > 0 && c->stateTimeTicks >= len - 1)) {
                                continue;
                            }
                        }
                    }
                    if (!AllConditionsMet(*ctrl, t, c->params)) {
                        continue;
                    }
                    ConsumeTriggers(*ctrl, t, c->params);
                    c->transitionTo = t.to;
                    c->transitionTick = 0;
                    c->transitionDuration = t.duration > 0 ? t.duration : 1;
                    c->transitionToTime = 0;
                    c->transitionToPhase = 0;
                    break;
                }
            }

            // 2. ポーズ適用 (遷移中はブレンド)
            const ControllerState& sa = ctrl->states[c->currentState];
            const AnimationClipAsset* clipA = clips.Get(sa.clipHash);
            if (c->transitionTo >= 0) {
                const ControllerState& sb = ctrl->states[c->transitionTo];
                const AnimationClipAsset* clipB = clips.Get(sb.clipHash);
                const float w =
                    static_cast<float>(c->transitionTick) / static_cast<float>(c->transitionDuration);
                if (clipA && clipB) {
                    ApplyClipPoseBlended(world, e, *clipA, c->stateTimeTicks, *clipB,
                                         c->transitionToTime, w);
                } else if (clipB) {
                    ApplyClipPose(world, e, *clipB, c->transitionToTime);
                } else if (clipA) {
                    ApplyClipPose(world, e, *clipA, c->stateTimeTicks);
                }
            } else if (clipA) {
                ApplyClipPose(world, e, *clipA, c->stateTimeTicks);
            }

            // 3. 時刻を進める (骨クリップを持つステートは主 SkinnedMesh の骨クリップの長さで回す)。
            //    進める前の値と折り返す前の進みを時計に残す (M89f の描画補間。sim はこれを読まない)
            const auto advance = [&](int32_t index, int32_t& time, uint32_t& phase, StateClock& clock) {
                const ControllerState& st = ctrl->states[static_cast<size_t>(index)];
                const int32_t length = stateLength(index);
                clock.prevTime = time;
                clock.prevPhase = phase;
                AdvanceStateTime(time, st.speed, st.loop, length);
                // ループは折り返しても進みは speed のまま。非ループは末尾に張り付いた分を差で取る
                clock.timeStep = (st.loop && length > 0) ? st.speed : time - clock.prevTime;
                if (isBlend(index)) {
                    const int64_t delta = phaseDelta(index);
                    phase = AdvanceBlendPhase(phase, delta, st.loop);
                    clock.phaseStep = st.loop ? delta : int64_t(phase) - clock.prevPhase;
                }
                clock.time = time;
                clock.phase = phase;
            };
            // 入った tick (M89h のイベント): 遷移先は遷移を始めて最初の進み、今のステートは stateEntered
            const bool fromEntered = c->stateEntered != 0;
            const bool toEntered = c->transitionTo >= 0 && c->transitionTick == 0;
            StateClock fromClock;
            StateClock toClock;
            advance(c->currentState, c->stateTimeTicks, c->statePhase, fromClock);
            c->stateEntered = 0;
            if (c->transitionTo >= 0) {
                advance(c->transitionTo, c->transitionToTime, c->transitionToPhase, toClock);
                ++c->transitionTick;
            }

            // 4. アニメイベント (M89h)。遷移を終える tick も、終える前の元と先で判定する (元は重み 0 で発火しない)
            const size_t firstFired = fired_.size();
            if (mainModel != nullptr && !ctrl->clipEvents.empty()) {
                const ControllerState& from = ctrl->states[static_cast<size_t>(c->currentState)];
                int32_t fromWeightQ = static_cast<int32_t>(kWeightOne);
                int32_t toWeightQ = 0;
                if (c->transitionTo >= 0) {
                    // ステートの重みは BuildSkeletalSources と同じ (片側だけが骨を駆動するならそちらが満杯)
                    const bool fromSkel = StateDrivesSkeleton(from);
                    const bool toSkel = StateDrivesSkeleton(ctrl->states[static_cast<size_t>(c->transitionTo)]);
                    if (toSkel && !fromSkel) {
                        fromWeightQ = 0;
                        toWeightQ = static_cast<int32_t>(kWeightOne);
                    } else if (toSkel) {
                        const int32_t duration = c->transitionDuration > 0 ? c->transitionDuration : 1;
                        const int64_t tick = std::clamp<int64_t>(c->transitionTick, 0, duration);
                        toWeightQ = static_cast<int32_t>(tick * kWeightOne / duration);
                        fromWeightQ = static_cast<int32_t>(kWeightOne) - toWeightQ;
                    }
                }
                FireStateEvents(*ctrl, from, c->params, fromClock, fromEntered, fromWeightQ, *mainModel, e, passed_,
                                fired_);
                if (c->transitionTo >= 0) {
                    FireStateEvents(*ctrl, ctrl->states[static_cast<size_t>(c->transitionTo)], c->params, toClock,
                                    toEntered, toWeightQ, *mainModel, e, passed_, fired_);
                }
            }

            // 5. 遷移を終える
            if (c->transitionTo >= 0) {
                if (c->transitionTick >= c->transitionDuration) {
                    fromClock = toClock;
                    c->currentState = c->transitionTo;
                    c->stateTimeTicks = c->transitionToTime;
                    c->statePhase = c->transitionToPhase;
                    c->transitionTo = -1;
                    c->transitionTick = 0;
                    c->transitionDuration = 0;
                    c->transitionToTime = 0;
                    c->transitionToPhase = 0;
                }
            }

            // 6. 骨のポーズプログラム (M89b)。進めた後の時刻で書く = 旧経路 (SkinningSystem が進めてから
            //    描画が読む) と同じ「その tick の終わりの姿勢」になる
            //    applyRootMotion (M89j) ならメッシュごとに自分のモデルのルートジョイントの水平移動を抜かせる。
            //    エンティティがヨーを受け取って回るなら (M89k) ひねりも抜かせる
            const bool yawFollows = RootYawFollows(world, e, *c);
            SkeletalSource sources[kMaxSkeletalSources];
            int32_t sourceCount = 0;
            if (!driven_.empty()) {
                sourceCount = BuildSkeletalSources(*ctrl, *c, fromClock, toClock, sources);
                if (sourceCount > 0) {
                    for (EntityID m : driven_) {
                        SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(m);
                        const SkinnedModel* model = models != nullptr ? models->Get(sm->model) : nullptr;
                        WriteSkeletalProgram(*sm, model, sources, sourceCount);
                        sm->poseRootJoint = (c->applyRootMotion && model != nullptr) ? FindRootJoint(*model) : -1;
                        sm->poseRootYaw = (sm->poseRootJoint >= 0 && yawFollows) ? 1 : 0;
                        if (sm->poseRootJoint >= 0) {
                            WorldUpInEntitySpace(world, m, sm->poseRootUp);
                        } else {
                            sm->poseRootUp[0] = 0.0f;
                            sm->poseRootUp[1] = 1.0f;
                            sm->poseRootUp[2] = 0.0f;
                        }
                    }
                }
            }

            // 7. 発火したイベントの位置 (M89i)。ジョイントの位置は 6. で書いたプログラム (この tick の終わりの姿勢) で引く
            if (fired_.size() > firstFired) {
                ResolveEventPositions(world, e, driven_, models, fired_, firstFired);
            }

            // 8. ルートモーション (M89j)。主 SkinnedMesh のモデルのルートジョイントの、この tick の移動を
            //    主 SkinnedMesh の LocalTransform の連鎖でワールドへ回し、水平分 (y を捨てる) を速度にする。
            //    縦は捨てる (重力・接地は物理と CharacterController の担当)。
            //    ヨー (M89k) はルートの親空間の上まわりのひねりを測り、ワールドの Y 軸まわりとして書く。
            //    移動は回す前の向きでワールドへ出す (この tick の移動は tick の頭の向きで歩いたぶん)
            float deltaWorld[3] = {};
            Yaw yawWorld;
            const int32_t rootJoint = mainModel != nullptr ? FindRootJoint(*mainModel) : -1;
            if (sourceCount > 0 && rootJoint >= 0) {
                const EntityID mainEntity = MainSkinnedEntity(driven_);
                float up[3];
                WorldUpInEntitySpace(world, mainEntity, up);
                float deltaLocal[3];
                Yaw yawLocal;
                RootMotionDelta(*mainModel, rootJoint, up, yawFollows, sources, sourceCount, deltaLocal, yawLocal);
                const ChainRotScale f = ComposeChain(world, mainEntity);
                const float scaled[3] = { deltaLocal[0] * f.s[0], deltaLocal[1] * f.s[1], deltaLocal[2] * f.s[2] };
                RotateByQuat(f.q, scaled, deltaWorld);
                deltaWorld[1] = 0.0f;
                // 連鎖の回転は up をワールドの上へ移すので角度はそのまま。鏡映 (拡大の積が負) なら回りが逆になる
                yawWorld = yawLocal;
                if (f.s[0] * f.s[1] * f.s[2] < 0.0f) {
                    yawWorld.s = -yawWorld.s;
                }
            }
            c->rootMotionVelocity = { deltaWorld[0] * kTicksPerSecond, 0.0f, deltaWorld[2] * kTicksPerSecond };
            c->rootMotionDeltaRotation = { 0.0f, yawWorld.s, 0.0f, yawWorld.c };
            if (c->applyRootMotion) {
                ApplyRootMotion(world, e, deltaWorld, c->rootMotionVelocity);
                if (yawFollows) {
                    ApplyRootYaw(world, e, yawWorld);
                }
            }
        }
    });
}

} // namespace mye
