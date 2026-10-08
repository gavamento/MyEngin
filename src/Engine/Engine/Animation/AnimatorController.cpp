#include "Engine/Engine/Animation/AnimatorController.h"

#include <algorithm>
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

bool EvalCond(CondOp op, int32_t a, int32_t b)
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

bool AllConditionsMet(const ControllerTransition& t, const int32_t params[4])
{
    for (const ControllerCondition& c : t.conditions) {
        if (c.param < 0 || c.param >= 4) {
            return false;
        }
        if (!EvalCond(c.op, params[c.param], c.value)) {
            return false;
        }
    }
    return true;
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
        if (st.skelClipHash != 0) {
            return true;
        }
    }
    return false;
}

// ポーズプログラムの元になる 1 層 (ステートと再生位置と重み)
struct SkeletalSource {
    const ControllerState* state = nullptr;
    int32_t timeTicks = 0;
    int32_t weightQ = 0;
};

// 今の tick のプログラムの元を組む (時刻を進めた後の値で)。返り値 = 層数 (0..2)。
// 遷移中は元 → 先の順に並べる (層を先頭から畳むので順序も入力の一部)。重みは
// transitionTick / transitionDuration を Q16 に 1 回だけ切り捨て、残りを元へ回す = 和がちょうど kPoseWeightOne
int32_t BuildSkeletalSources(const ControllerAsset& ctrl, const AnimatorControllerComponent& c,
                             SkeletalSource (&out)[2])
{
    constexpr int64_t kOne = SkinnedMeshComponent::kPoseWeightOne;
    const ControllerState& from = ctrl.states[static_cast<size_t>(c.currentState)];
    const bool fromSkel = from.skelClipHash != 0;
    if (c.transitionTo < 0) {
        if (!fromSkel) {
            return 0;
        }
        out[0] = { &from, c.stateTimeTicks, static_cast<int32_t>(kOne) };
        return 1;
    }
    const ControllerState& to = ctrl.states[static_cast<size_t>(c.transitionTo)];
    const bool toSkel = to.skelClipHash != 0;
    if (!fromSkel && !toSkel) {
        return 0;
    }
    if (!toSkel) {
        out[0] = { &from, c.stateTimeTicks, static_cast<int32_t>(kOne) };
        return 1;
    }
    if (!fromSkel) {
        out[0] = { &to, c.transitionToTime, static_cast<int32_t>(kOne) };
        return 1;
    }
    const int32_t duration = c.transitionDuration > 0 ? c.transitionDuration : 1;
    const int64_t tick = std::clamp<int64_t>(c.transitionTick, 0, duration);
    const int32_t toWeight = static_cast<int32_t>(tick * kOne / duration);
    out[0] = { &from, c.stateTimeTicks, static_cast<int32_t>(kOne) - toWeight };
    out[1] = { &to, c.transitionToTime, toWeight };
    return 2;
}

void WriteSkeletalProgram(SkinnedMeshComponent& sm, const SkinnedModel* model, const SkeletalSource* sources,
                          int32_t count)
{
    sm.poseLayerCount = count;
    for (int32_t i = 0; i < count; ++i) {
        SkinnedMeshComponent::PoseLayer& layer = sm.poseLayers[i];
        // モデルが未登録・名前のクリップが無いモデルは -1 = バインドポーズ (層の数と重みは他のメッシュとそろえる)
        layer.clip = model != nullptr ? model->FindClipByHash(sources[i].state->skelClipHash) : -1;
        layer.timeQ = sources[i].timeTicks * SkinnedMeshComponent::kPoseTimeQPerTick;
        layer.weightQ = sources[i].weightQ;
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
        params.push_back({ { "name", p.name } });
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
        if (!s.skelClip.empty()) {
            st["skel"] = { { "clip", s.skelClip } };
        }
        states.push_back(std::move(st));
    }
    root["states"] = std::move(states);
    json trans = json::array();
    for (const ControllerTransition& t : c.transitions) {
        json conds = json::array();
        for (const ControllerCondition& cc : t.conditions) {
            conds.push_back({ { "param", cc.param }, { "op", OpToStr(cc.op) }, { "value", cc.value } });
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
            out.parameters.push_back(std::move(cp));
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
            cs.skelClip = s["skel"].value("clip", std::string());
            cs.skelClipHash = cs.skelClip.empty() ? 0 : HashStr(cs.skelClip);
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
                    cc.value = cj.value("value", 0);
                    ct.conditions.push_back(cc);
                }
            }
            out.transitions.push_back(std::move(ct));
        }
    }
    return true;
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

int32_t ControllerStateLengthTicks(const ControllerState& state, const AnimationLibrary* clips,
                                   const SkinnedModel* mainModel)
{
    if (state.skelClipHash != 0 && mainModel != nullptr) {
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
        return true;
    }
    c->currentState = stateIndex;
    c->stateTimeTicks = 0;
    c->transitionTo = -1;
    c->transitionTick = 0;
    c->transitionDuration = 0;
    c->transitionToTime = 0;
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
                return ControllerStateLengthTicks(ctrl->states[static_cast<size_t>(index)], &clips, mainModel);
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
                        const int32_t len = stateLength(c->currentState);
                        if (!(len > 0 && c->stateTimeTicks >= len - 1)) {
                            continue;
                        }
                    }
                    if (!AllConditionsMet(t, c->params)) {
                        continue;
                    }
                    c->transitionTo = t.to;
                    c->transitionTick = 0;
                    c->transitionDuration = t.duration > 0 ? t.duration : 1;
                    c->transitionToTime = 0;
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
            if (c->transitionTo >= 0) {
                const ControllerState& sb = ctrl->states[c->transitionTo];
                AdvanceStateTime(c->transitionToTime, sb.speed, sb.loop, stateLength(c->transitionTo));
                ++c->transitionTick;
                if (c->transitionTick >= c->transitionDuration) {
                    c->currentState = c->transitionTo;
                    c->stateTimeTicks = c->transitionToTime;
                    c->transitionTo = -1;
                    c->transitionTick = 0;
                    c->transitionDuration = 0;
                    c->transitionToTime = 0;
                }
            }

            // 4. 骨のポーズプログラム (M89b)。進めた後の時刻で書く = 旧経路 (SkinningSystem が進めてから
            //    描画が読む) と同じ「その tick の終わりの姿勢」になる
            if (!driven_.empty()) {
                SkeletalSource sources[2];
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
