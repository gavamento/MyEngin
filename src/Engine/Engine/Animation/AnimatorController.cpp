#include "Engine/Engine/Animation/AnimatorController.h"

#include <algorithm>
#include <bit>
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
    const int32_t n = ComputeBlendWeights(st, ControllerParamAsFloat(ctrl, st.blendParam, params), w);
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

// ポーズプログラムの元になる 1 層 (骨クリップと再生位置と重み)
struct SkeletalSource {
    uint64_t clipHash = 0;
    bool usesPhase = false; // true = ブレンドツリーの子 (位相をメッシュごとの長さで時刻にする)
    int32_t timeTicks = 0;  // 単一クリップの再生位置
    uint32_t phase = 0;     // ブレンドツリーの位相
    int32_t loop = 1;
    int32_t weightQ = 0;
};
// 遷移の元と先がどちらもブレンドツリーでも収まる
constexpr int32_t kMaxSkeletalSources = 2 * kMaxBlendLayers;
static_assert(kMaxSkeletalSources <= SkinnedMeshComponent::kMaxPoseLayers,
              "a transition between two blend trees must fit in one pose program");

// 1 ステートぶんの層を out へ足す。stateWeightQ をステート内の子の重みで割り振る (端数は呼び出し側で直す)
int32_t AppendStateSources(const ControllerAsset& ctrl, const ControllerState& st, const int32_t* params,
                           int32_t timeTicks, uint32_t phase, int32_t stateWeightQ, SkeletalSource* out)
{
    if (st.blendType == ControllerBlendType::None) {
        out[0] = { st.skelClipHash, false, timeTicks, 0, st.loop, stateWeightQ };
        return 1;
    }
    BlendChildWeight w[kMaxBlendLayers];
    const int32_t n = ComputeBlendWeights(st, ControllerParamAsFloat(ctrl, st.blendParam, params), w);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t weightQ = static_cast<int32_t>(int64_t(stateWeightQ) * w[i].weightQ / kWeightOne);
        out[i] = { st.blendChildren[static_cast<size_t>(w[i].child)].clipHash, true, 0, phase, st.loop, weightQ };
    }
    return n;
}

// 今の tick のプログラムの元を組む (時刻を進めた後の値で)。返り値 = 層数 (0..kMaxSkeletalSources)。
// 遷移中は元 → 先の順に並べる (層を先頭から畳むので順序も入力の一部)。ステートの重みは
// transitionTick / transitionDuration を Q16 に 1 回だけ切り捨て、残りを元へ回す。ブレンドの子へ割り振った
// 切り捨ての端数は最大重みの層 (同値なら先の層) に足す = 和はちょうど kPoseWeightOne
int32_t BuildSkeletalSources(const ControllerAsset& ctrl, const AnimatorControllerComponent& c,
                             SkeletalSource (&out)[kMaxSkeletalSources])
{
    const ControllerState& from = ctrl.states[static_cast<size_t>(c.currentState)];
    const bool fromSkel = StateDrivesSkeleton(from);
    int32_t count = 0;
    const auto appendFrom = [&](int64_t weightQ) {
        count += AppendStateSources(ctrl, from, c.params, c.stateTimeTicks, c.statePhase,
                                    static_cast<int32_t>(weightQ), out + count);
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
            count += AppendStateSources(ctrl, to, c.params, c.transitionToTime, c.transitionToPhase,
                                        static_cast<int32_t>(weightQ), out + count);
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

void WriteSkeletalProgram(SkinnedMeshComponent& sm, const SkinnedModel* model, const SkeletalSource* sources,
                          int32_t count)
{
    sm.poseLayerCount = count;
    for (int32_t i = 0; i < count; ++i) {
        const SkeletalSource& src = sources[i];
        SkinnedMeshComponent::PoseLayer& layer = sm.poseLayers[i];
        // モデルが未登録・名前のクリップが無いモデルは -1 = バインドポーズ (層の数と重みは他のメッシュとそろえる)
        layer.clip = model != nullptr ? model->FindClipByHash(src.clipHash) : -1;
        if (src.usesPhase) {
            // 位相はメッシュ自身のモデルでの長さで時刻にする (長さの違うモデルでも周がそろう)
            const int32_t ticks =
                layer.clip >= 0 ? SkeletalClipTicks(model->clips[static_cast<size_t>(layer.clip)]) : 0;
            layer.timeQ = BlendPhaseToTimeQ(src.phase, ticks, src.loop);
        } else {
            layer.timeQ = src.timeTicks * SkinnedMeshComponent::kPoseTimeQPerTick;
        }
        layer.weightQ = src.weightQ;
    }
    // 使っていない層は既定値に戻す (snapshot の生バイトを入力だけで決まる形にしておく)
    for (int32_t i = count; i < SkinnedMeshComponent::kMaxPoseLayers; ++i) {
        sm.poseLayers[i] = {};
    }
    sm.poseClaim = 1;
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
        if (s.blendType != ControllerBlendType::None) {
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
            if (skel.contains("blend1d") && skel["blend1d"].is_object()) {
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
    return true;
}

bool StateDrivesSkeleton(const ControllerState& state)
{
    if (state.blendType != ControllerBlendType::None) {
        return !state.blendChildren.empty();
    }
    return state.skelClipHash != 0;
}

int32_t ComputeBlendWeights(const ControllerState& state, float x, BlendChildWeight (&out)[kMaxBlendLayers])
{
    const int32_t n = static_cast<int32_t>(state.blendChildren.size());
    if (state.blendType != ControllerBlendType::Blend1D || n == 0) {
        return 0;
    }
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

int32_t BlendPhaseToTimeQ(uint32_t phase, int32_t lengthTicks, int32_t loop)
{
    if (lengthTicks <= 0) {
        return 0;
    }
    // timeQ (int32) に収まる長さに丸める (約 38 時間。実際のクリップは届かない)
    const int64_t span = static_cast<int64_t>(std::min(lengthTicks, INT32_MAX / SkinnedMeshComponent::kPoseTimeQPerTick))
                         * SkinnedMeshComponent::kPoseTimeQPerTick;
    if (!loop && phase == UINT32_MAX) {
        return static_cast<int32_t>(span);
    }
    return static_cast<int32_t>((static_cast<uint64_t>(phase) * static_cast<uint64_t>(span)) >> 32);
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

// driven (CollectDrivenSkinnedMeshes の結果) のうち entity index が最小の SkinnedMesh のモデル。
// 走査順 (前順) ではなく index で選ぶのは、兄弟の並べ替えでステートの長さが変わらないようにするため
const SkinnedModel* MainModelOf(World& world, const std::vector<EntityID>& driven, const SkinnedModelLibrary* models)
{
    if (models == nullptr) {
        return nullptr;
    }
    const SkinnedMeshComponent* main = nullptr;
    uint32_t mainIndex = 0;
    for (EntityID e : driven) {
        if (main == nullptr || e.index < mainIndex) {
            main = world.GetComponent<SkinnedMeshComponent>(e);
            mainIndex = e.index;
        }
    }
    return main != nullptr ? models->Get(main->model) : nullptr;
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
        const int32_t n = ComputeBlendWeights(state, ControllerParamAsFloat(controller, state.blendParam, params), w);
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
                // 凍らせる: 書いてあるプログラムをそのまま保たせる (時刻も層も進めない)
                for (EntityID m : driven_) {
                    SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(m);
                    if (sm->poseLayerCount > 0) {
                        sm->poseClaim = 1;
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

            // 3. 時刻を進める (骨クリップを持つステートは主 SkinnedMesh の骨クリップの長さで回す)
            AdvanceStateTime(c->stateTimeTicks, sa.speed, sa.loop, stateLength(c->currentState));
            if (isBlend(c->currentState)) {
                c->statePhase = AdvanceBlendPhase(c->statePhase, phaseDelta(c->currentState), sa.loop);
            }
            if (c->transitionTo >= 0) {
                const ControllerState& sb = ctrl->states[c->transitionTo];
                AdvanceStateTime(c->transitionToTime, sb.speed, sb.loop, stateLength(c->transitionTo));
                if (isBlend(c->transitionTo)) {
                    c->transitionToPhase =
                        AdvanceBlendPhase(c->transitionToPhase, phaseDelta(c->transitionTo), sb.loop);
                }
                ++c->transitionTick;
                if (c->transitionTick >= c->transitionDuration) {
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

            // 4. 骨のポーズプログラム (M89b)。進めた後の時刻で書く = 旧経路 (SkinningSystem が進めてから
            //    描画が読む) と同じ「その tick の終わりの姿勢」になる
            if (!driven_.empty()) {
                SkeletalSource sources[kMaxSkeletalSources];
                const int32_t count = BuildSkeletalSources(*ctrl, *c, sources);
                if (count > 0) {
                    for (EntityID m : driven_) {
                        SkinnedMeshComponent* sm = world.GetComponent<SkinnedMeshComponent>(m);
                        const SkinnedModel* model = models != nullptr ? models->Get(sm->model) : nullptr;
                        WriteSkeletalProgram(*sm, model, sources, count);
                    }
                }
            }
        }
    });
}

} // namespace mye
