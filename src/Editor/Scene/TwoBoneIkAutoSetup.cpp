//====================================================================================
//                          TwoBoneIkAutoSetup.cpp
//  MyEngin/ 秋田蓮音                                                     11/05/2026
//                                          TwoBoneIK の手足の自動設定の実装
//====================================================================================
#include "Editor/Scene/TwoBoneIkAutoSetup.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/EntityNaming.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr int kLimbCount = static_cast<int>(IkLimb::Count);
static_assert(kLimbCount == kMaxTwoBoneIkChains, "手足 1 本 = 鎖 1 本で対応させている");

// 目標の子の名前 (手の鎖だけが使う)
constexpr const char* kTargetNames[kLimbCount] = { "IK Target Hand L", "IK Target Hand R", nullptr, nullptr };

// 語を含む骨は手首・足首として扱わない。指 / つま先 (手首・足首の子孫で "hand" "foot" を名前に含むことが多い)、
// IK の補助骨 (UE の ik_hand_l などはルート直下に居て深さが浅く、「ルートに最も近い」で選ばれてしまう)、
// 末端の飾り (Hand_L_end) とねじり補助
constexpr std::string_view kExcludedWords[] = {
    "end",   "nub",  "tip",    "ik",    "target", "pole",   "twist", "roll",  "finger", "thumb",
    "index", "middle", "ring", "pinky", "little", "toe",    "toes",  "ball",  "helper", "dummy",
    "socket", "attach", "prop", "weapon",
};

enum class Part { None, Hand, Foot };
enum class Side { None, Left, Right };

Part PartOfWord(std::string_view w)
{
    if (w == "hand" || w == "wrist") {
        return Part::Hand;
    }
    if (w == "foot" || w == "ankle") {
        return Part::Foot;
    }
    return Part::None;
}

Side SideOfWord(std::string_view w)
{
    if (w == "l" || w == "left") {
        return Side::Left;
    }
    if (w == "r" || w == "right") {
        return Side::Right;
    }
    return Side::None;
}

bool IsUpper(char c)
{
    return std::isupper(static_cast<unsigned char>(c)) != 0;
}

bool IsLower(char c)
{
    return std::islower(static_cast<unsigned char>(c)) != 0;
}

bool IsDigit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsAlnum(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// 骨名を小文字の語に分ける。"mixamorig:LeftHand" → left / hand、"Hand_L" → hand / l、"HandL" → hand / l、
// "Bip01 L Foot" → bip / 01 / l / foot。続けて書かれた "lefthand" "handl" も分ける
std::vector<std::string> SplitWords(std::string_view name)
{
    const size_t prefix = name.find_last_of(":|");
    if (prefix != std::string_view::npos) {
        name.remove_prefix(prefix + 1);
    }
    std::vector<std::string> raw;
    std::string cur;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (!IsAlnum(c)) {
            if (!cur.empty()) {
                raw.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (!cur.empty()) {
            const char prev = name[i - 1];
            const bool next = (i + 1 < name.size()) && IsLower(name[i + 1]);
            // 小→大 (handL)、大→大+小 (IKHand の H)、文字↔数字 (Bip01) で切る
            const bool boundary = (IsLower(prev) && IsUpper(c)) || (IsUpper(prev) && IsUpper(c) && next)
                                  || (IsDigit(prev) != IsDigit(c));
            if (boundary) {
                raw.push_back(cur);
                cur.clear();
            }
        }
        cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!cur.empty()) {
        raw.push_back(cur);
    }

    std::vector<std::string> words;
    for (const std::string& w : raw) {
        bool split = false;
        for (std::string_view side : { std::string_view("left"), std::string_view("right") }) {
            if (w.size() > side.size() && w.compare(0, side.size(), side) == 0) {
                words.emplace_back(side);
                words.push_back(w.substr(side.size()));
                split = true;
                break;
            }
        }
        // "handl" "footr" (すべて小文字で左右が後ろに付いた形)
        if (!split && w.size() > 1 && (w.back() == 'l' || w.back() == 'r')
            && PartOfWord(std::string_view(w).substr(0, w.size() - 1)) != Part::None) {
            words.push_back(w.substr(0, w.size() - 1));
            words.push_back(w.substr(w.size() - 1));
            split = true;
        }
        if (!split) {
            words.push_back(w);
        }
    }
    return words;
}

// 骨名が手足のどれに当たるか。当たらなければ -1
int32_t LimbOfName(std::string_view name)
{
    Part part = Part::None;
    Side side = Side::None;
    for (const std::string& w : SplitWords(name)) {
        for (std::string_view ex : kExcludedWords) {
            if (w == ex) {
                return -1;
            }
        }
        if (const Part p = PartOfWord(w); p != Part::None) {
            if (part != Part::None && part != p) {
                return -1; // 手と足の両方を含む名前はどちらとも決めない
            }
            part = p;
        }
        if (const Side s = SideOfWord(w); s != Side::None) {
            if (side != Side::None && side != s) {
                return -1;
            }
            side = s;
        }
    }
    if (part == Part::None || side == Side::None) {
        return -1;
    }
    if (part == Part::Hand) {
        return static_cast<int32_t>(side == Side::Left ? IkLimb::HandL : IkLimb::HandR);
    }
    return static_cast<int32_t>(side == Side::Left ? IkLimb::FootL : IkLimb::FootR);
}

// ルートからの深さ。親の表が壊れて輪になっていても止まるよう、ジョイント数で打ち切る (-1 = 壊れている)
int32_t JointDepth(const SkinnedModel& model, int32_t joint)
{
    const int32_t n = static_cast<int32_t>(model.joints.size());
    int32_t depth = 0;
    for (int32_t cur = model.joints[static_cast<size_t>(joint)].parent; cur >= 0;
         cur = model.joints[static_cast<size_t>(cur)].parent) {
        if (cur >= n || ++depth > n) {
            return -1;
        }
    }
    return depth;
}

void WriteName(char (&dst)[64], const std::string& name)
{
    // 生バイトがワールドハッシュ対象なので末尾までゼロ埋めしてから書く (Part.joint と同じ)
    std::memset(dst, 0, sizeof(dst));
    std::snprintf(dst, sizeof(dst), "%s", name.c_str());
}

bool AnyChainUses(const TwoBoneIKComponent& ik, const std::string& name)
{
    for (const TwoBoneIKComponent::Chain& c : ik.chains) {
        if (std::string_view(c.endJoint, strnlen(c.endJoint, sizeof(c.endJoint))) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

IkLimbJoints FindIkLimbJoints(const SkinnedModel& model)
{
    IkLimbJoints out;
    int32_t bestDepth[kLimbCount] = { 0, 0, 0, 0 };
    for (size_t j = 0; j < model.joints.size(); ++j) {
        const std::string& name = model.joints[j].name;
        // 鎖は名前 (char[64]) でしか骨を指せない。空名と 63 バイト超は指せない
        if (name.empty() || name.size() >= 64) {
            continue;
        }
        const int32_t limb = LimbOfName(name);
        if (limb < 0) {
            continue;
        }
        const int32_t depth = JointDepth(model, static_cast<int32_t>(j));
        if (depth < 2) {
            continue; // 中間 (親) と根 (祖父母) が無い = 2 ボーンの鎖にならない
        }
        if (out.joint[limb] < 0 || depth < bestDepth[limb]) {
            out.joint[limb] = static_cast<int32_t>(j);
            bestDepth[limb] = depth;
        }
    }
    return out;
}

TwoBoneIkAutoSetupResult ApplyTwoBoneIkAutoSetup(Scene& scene, EntityID mesh, const SkinnedModel& model)
{
    TwoBoneIkAutoSetupResult result;
    World& world = scene.GetWorld();
    const IkLimbJoints found = FindIkLimbJoints(model);
    for (int k = 0; k < kLimbCount; ++k) {
        if (found.joint[k] < 0) {
            result.missingMask |= 1u << k;
        }
    }

    // 段 1: 鎖の文字列と mode を埋め、目標が要る手を決める。
    // 子の生成 (段 2) より前に済ませる — 生成はアーキタイプを動かしうるので、ik のポインタはここでしか使わない
    bool needTarget[kLimbCount] = {};
    {
        auto* ik = world.GetComponent<TwoBoneIKComponent>(mesh);
        if (ik == nullptr) {
            return result;
        }
        for (int k = 0; k < kLimbCount; ++k) {
            const int32_t joint = found.joint[k];
            TwoBoneIKComponent::Chain& chain = ik->chains[k];
            if (joint < 0 || chain.endJoint[0] != '\0') {
                continue;
            }
            const std::string& name = model.joints[static_cast<size_t>(joint)].name;
            if (AnyChainUses(*ik, name)) {
                continue; // 別の鎖で手設定済みの骨を二重に解かない
            }
            WriteName(chain.endJoint, name);
            const bool hand = kTargetNames[k] != nullptr;
            chain.mode = hand ? twoboneikmode::kPosition : twoboneikmode::kGround;
            chain.weight = hand ? 0.0f : 1.0f;
            result.filledMask |= 1u << k;
            needTarget[k] = hand && (chain.target.IsNull() || !world.IsAlive(chain.target));
        }
    }

    // 段 2: 手の目標を mesh の直子として手首のバインド姿勢に作る。
    // TwoBoneIkSystem は目標を「目標エンティティのローカルの点 (0, 0, 0)」→ ワールド → メッシュの空間と直すので、
    // 直子の LocalTransform に jointGlobal (メッシュの空間) を入れれば、目標はちょうど今の手首に重なる
    EntityID targets[kLimbCount] = { kNullEntity, kNullEntity, kNullEntity, kNullEntity };
    std::vector<XMMATRIX> locals;
    for (int k = 0; k < kLimbCount; ++k) {
        if (!needTarget[k]) {
            continue;
        }
        if (locals.empty()) {
            // バインドポーズ (clip < 0)。押した時刻のアニメで目標の位置が変わらないようにする
            ComputeJointLocals(model, -1, 0.0f, locals);
        }
        const XMMATRIX g = JointGlobalFromLocals(model, locals, found.joint[k]);
        XMVECTOR s, r, t;
        if (!XMMatrixDecompose(&s, &r, &t, g)) {
            r = XMQuaternionIdentity();
            t = g.r[3];
        }
        const std::string unique = MakeUniqueSiblingName(world, mesh, kTargetNames[k], kNullEntity);
        GameObject obj = scene.CreateGameObjectTracked(unique.c_str());
        world.SetParent(obj.Id(), mesh);
        if (auto* lt = obj.GetComponent<LocalTransform>()) {
            XMStoreFloat3(&lt->position, t);
            XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(r));
            lt->scale = { 1.0f, 1.0f, 1.0f };
        }
        // 次の目標の MakeUniqueSiblingName に今作った兄弟を見せる (SetParent は遅延)
        world.ApplyStructuralChanges();
        targets[k] = obj.Id();
        ++result.createdTargets;
    }

    // 段 3: 目標を鎖に結ぶ。点は目標のローカル原点、回転は目標の回転そのまま (mode 2 に切り替えても手の向きが飛ばない)
    if (result.createdTargets > 0) {
        if (auto* ik = world.GetComponent<TwoBoneIKComponent>(mesh)) {
            for (int k = 0; k < kLimbCount; ++k) {
                if (targets[k].IsNull()) {
                    continue;
                }
                TwoBoneIKComponent::Chain& chain = ik->chains[k];
                chain.target = targets[k];
                chain.targetPosition = { 0.0f, 0.0f, 0.0f };
                chain.targetRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
            }
        }
    }
    world.ApplyStructuralChanges();
    return result;
}

} // namespace mye
