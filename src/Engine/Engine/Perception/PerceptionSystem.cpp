//====================================================================================
//                          PerceptionSystem.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          AI の知覚 (視覚・聴覚・ダメージ・接触・予測)
//====================================================================================
#include "Engine/Engine/Perception/PerceptionSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {
namespace {

// 1 体が 1 tick に撃つ視線のレイの上限。距離と角度で絞った候補を近い順 (同距離はエンティティキー順) に撃ち、
// 溢れた相手はその tick は見えない扱い。★時間ではなく件数で絞る = 機種によって結果が変わらない
constexpr int kMaxLosRaysPerPerceiver = 16;
// レイが相手の手前でこれ未満の距離に当たったものは遮蔽と見なさない (相手の表面ちょうどの数値誤差)
constexpr float kLosEpsilon = 0.01f;
// 接触の感覚: CharacterController どうしの水平距離が半径の和 + これ以下で、高さが重なれば触れている
constexpr float kTouchMargin = 0.05f;
// 速度の推定に使う 2 回の目撃の最大間隔 (tick)。これより空いたら速度は 0 (古い位置から速度を作らない)
constexpr uint64_t kMaxVelocityGapTicks = 10;

// 感覚の優先度 (同じ tick に複数の感覚で知覚したとき、どの位置を lastSensedPos にするか)。視覚が最も正確
int SensePriority(uint32_t sense)
{
    switch (sense) {
    case perceptionsense::kSight: return 4;
    case perceptionsense::kTouch: return 3;
    case perceptionsense::kDamage: return 2;
    default: return 1;
    }
}

bool KeyLess(EntityID a, EntityID b)
{
    if (a.index != b.index) {
        return a.index < b.index;
    }
    return a.generation < b.generation;
}

// ancestor が e 自身か e の祖先か
bool IsAncestorOrSelf(World& world, EntityID ancestor, EntityID e)
{
    EntityID cur = e;
    while (!cur.IsNull()) {
        if (cur == ancestor) {
            return true;
        }
        const auto* h = world.GetComponent<HierarchyComponent>(cur);
        cur = (h != nullptr) ? h->parent : kNullEntity;
    }
    return false;
}

// e から親をたどって最初に見つかる AIStimulusSource のエンティティ (無ければ kNullEntity)
EntityID FindStimulusOwner(World& world, EntityID e)
{
    EntityID cur = e;
    while (!cur.IsNull()) {
        if (world.GetComponent<AIStimulusSourceComponent>(cur) != nullptr) {
            return cur;
        }
        const auto* h = world.GetComponent<HierarchyComponent>(cur);
        cur = (h != nullptr) ? h->parent : kNullEntity;
    }
    return kNullEntity;
}

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 Position(const DirectX::XMFLOAT4X4& wm)
{
    return { wm._41, wm._42, wm._43 };
}

float Length(float x, float y, float z)
{
    return std::sqrt(x * x + y * y + z * z);
}

// 見る側 1 体
struct Perceiver {
    EntityID entity;
    AIPerceptionComponent* perception = nullptr;
    const AcousticListenerComponent* ear = nullptr; // hearingMode = Acoustic のときの耳
    const CharacterControllerComponent* cc = nullptr;
    Vec3 pos;      // エンティティの位置 (足元)
    Vec3 eye;      // 目
    Vec3 forward;  // ワールド +Z (正規化。長さ 0 なら視野角の判定は常に失敗)
    float ccScaleY = 1.0f;
    float ccScaleXZ = 1.0f;
};

// 知覚される側 1 体
struct Stimulus {
    EntityID entity;
    const AIStimulusSourceComponent* source = nullptr;
    const CharacterControllerComponent* cc = nullptr;
    Vec3 pos;
    Vec3 point; // 見られる点
    float ccScaleY = 1.0f;
    float ccScaleXZ = 1.0f;
};

// 視線を遮りうるコライダー 1 個 (トリガーは遮らない)
struct LosCollider {
    EntityID entity;
    ShapePose pose;
    int32_t layer = 0;
};

// この tick に起きた知覚 1 件
struct SenseEvent {
    EntityID target;
    uint32_t sense = 0;
    Vec3 pos;
    float strength = 0.0f;
};

float ScaleY(const DirectX::XMFLOAT4X4& wm)
{
    return Length(wm._21, wm._22, wm._23);
}

float ScaleXZ(const DirectX::XMFLOAT4X4& wm)
{
    return std::max(Length(wm._11, wm._12, wm._13), Length(wm._31, wm._32, wm._33));
}

// CC のカプセルの [下端, 上端] (ワールド y) と半径。CC の中心はエンティティ位置 (PhysicsSystem と同じ)
void CapsuleSpan(const CharacterControllerComponent& cc, float scaleY, float scaleXZ, const Vec3& pos, float& lo,
                 float& hi, float& radius)
{
    const float h = cc.height * scaleY;
    lo = pos.y - h * 0.5f;
    hi = pos.y + h * 0.5f;
    radius = cc.radius * scaleXZ;
}

Perceiver MakePerceiver(EntityID e, AIPerceptionComponent* perception, const DirectX::XMFLOAT4X4& wm)
{
    Perceiver p;
    p.entity = e;
    p.perception = perception;
    p.pos = Position(wm);
    p.eye = { p.pos.x, p.pos.y + perception->eyeHeight, p.pos.z };
    const float fl = Length(wm._31, wm._32, wm._33);
    if (fl > 1e-8f) {
        p.forward = { wm._31 / fl, wm._32 / fl, wm._33 / fl };
    }
    return p;
}

Stimulus MakeStimulus(EntityID e, const AIStimulusSourceComponent* source, const DirectX::XMFLOAT4X4& wm)
{
    Stimulus s;
    s.entity = e;
    s.source = source;
    s.pos = Position(wm);
    s.point = { s.pos.x, s.pos.y + source->targetHeight, s.pos.z };
    return s;
}

// 相手 S が見る側 P の知覚対象か (陣営と自分自身の除外)
bool Detects(World& world, const Perceiver& p, const Stimulus& s)
{
    if (s.entity == p.entity || IsAncestorOrSelf(world, p.entity, s.entity)) {
        return false; // 自分自身と、自分の子に付いた刺激源は知覚しない
    }
    return PerceptionDetects(*p.perception, PerceptionAttitudeOf(*p.perception, s.source->faction));
}

// P の目から S の見られる点への線が遮られていないか。P 自身・P の祖先・P の子孫と、S の子孫 (S 自身を含む) の
// コライダーは遮蔽にしない (自分の体と相手の体)
bool LineOfSight(World& world, const std::vector<LosCollider>& colliders, const Perceiver& p, const Stimulus& s,
                 float dist, uint32_t mask, int& rays)
{
    if (dist <= kLosEpsilon) {
        return true;
    }
    ++rays;
    const float dx = (s.point.x - p.eye.x) / dist;
    const float dy = (s.point.y - p.eye.y) / dist;
    const float dz = (s.point.z - p.eye.z) / dist;
    const float limit = dist - kLosEpsilon;
    for (const LosCollider& c : colliders) {
        if (!shapes::LayerHit(mask, c.layer)) {
            continue;
        }
        float t = 0.0f, nx = 0.0f, ny = 0.0f, nz = 0.0f;
        if (!shapes::Raycast(c.pose, p.eye.x, p.eye.y, p.eye.z, dx, dy, dz, limit, t, nx, ny, nz) || t >= limit) {
            continue;
        }
        // 当たった物が自分か相手の体なら遮蔽ではない (階層の判定は当たったときだけ = レイ 1 本あたりの費用を抑える)
        if (IsAncestorOrSelf(world, p.entity, c.entity) || IsAncestorOrSelf(world, c.entity, p.entity)
            || IsAncestorOrSelf(world, s.entity, c.entity)) {
            continue;
        }
        return false;
    }
    return true;
}

// 視線を遮りうるコライダーを集める (トリガーと無効なエンティティを除く。エンティティキー順)
void CollectLosColliders(World& world, std::vector<LosCollider>& out)
{
    const ComponentTypeId req[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            if (col->isTrigger || !IsEntityActive(world, e)) {
                continue;
            }
            const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            out.push_back({ e, shapes::MakePoseFromMatrix(*col, wm), col->layer });
        }
    });
    // 遮られたかどうかだけを見るので順序は結果に効かないが、走査の手順を固定しておく
    std::sort(out.begin(), out.end(), [](const LosCollider& a, const LosCollider& b) { return KeyLess(a.entity, b.entity); });
}

// 前の tick に target を視覚で見ていたか (見失う距離の判定)
bool WasSeenLastTick(const AIPerceptionComponent& perc, EntityID target, uint64_t tick)
{
    const int count = std::clamp(perc.perceivedCount, 0, kMaxPercepts);
    for (int i = 0; i < count; ++i) {
        const AIPercept& q = perc.percepts[i];
        if (q.target == target) {
            return (q.lastSenses & perceptionsense::kSight) != 0 && q.lastSensedTick + 1 == tick;
        }
    }
    return false;
}

// 視覚の判定のうち距離と角度 (視線は見ない)。dist に目から見られる点までの距離を返す。
// 0 = 範囲外 / 1 = 範囲内 (視線の確認が要る) / 2 = 必ず気付く距離の内側 (視線を見ない)
int SightGeometry(const AIPerceptionComponent& perc, const Perceiver& p, const Stimulus& s, bool wasSeen, float& dist)
{
    const float sightR = std::max(perc.sightRadius, 0.0f);
    const float radius = wasSeen ? std::max(perc.loseSightRadius, sightR) : sightR;
    const float vx = s.point.x - p.eye.x, vy = s.point.y - p.eye.y, vz = s.point.z - p.eye.z;
    dist = Length(vx, vy, vz);
    if (dist > radius) {
        return 0;
    }
    if (dist <= perc.autoSuccessRange) {
        return 2;
    }
    if (perc.fovDeg < 360.0f) {
        const float half = std::clamp(perc.fovDeg, 0.0f, 360.0f) * 0.5f * 0.017453292519943295f;
        const float dot = (dist > 0.0f) ? (vx * p.forward.x + vy * p.forward.y + vz * p.forward.z) / dist : 1.0f;
        if (dot < std::cos(half)) {
            return 0;
        }
    }
    return 1;
}

// 視覚の強さ (1 - 距離 / 半径)
float SightStrength(const AIPerceptionComponent& perc, bool wasSeen, float dist)
{
    const float sightR = std::max(perc.sightRadius, 0.0f);
    const float radius = wasSeen ? std::max(perc.loseSightRadius, sightR) : sightR;
    return (radius > 0.0f) ? std::max(1.0f - dist / radius, 0.0f) : 1.0f;
}

// 知覚 1 件を percepts へ反映する。視覚の event は同じ相手の他の感覚より先に来ること (速度の推定が
// tick 頭の lastSensedPos / lastSenses を読むため。Update は視覚 → 聴覚 → ダメージ → 接触の順に積む)
void Merge(AIPerceptionComponent& perc, int& count, const SenseEvent& ev, uint64_t tick, float dt,
           std::vector<int>& bestPriority)
{
    int slot = -1;
    for (int i = 0; i < count; ++i) {
        if (perc.percepts[i].target == ev.target) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (count < kMaxPercepts) {
            slot = count++;
        } else {
            // 満杯: この tick に知覚していない中で最も古い (同じなら キーの大きい) 相手を忘れる。全員を知覚中なら捨てる
            for (int i = 0; i < count; ++i) {
                const AIPercept& c = perc.percepts[i];
                if (c.currentSenses != 0) {
                    continue;
                }
                if (slot < 0) {
                    slot = i;
                    continue;
                }
                const AIPercept& b = perc.percepts[slot];
                if (c.lastSensedTick < b.lastSensedTick
                    || (c.lastSensedTick == b.lastSensedTick && KeyLess(b.target, c.target))) {
                    slot = i;
                }
            }
            if (slot < 0) {
                return;
            }
        }
        perc.percepts[slot] = AIPercept{};
        perc.percepts[slot].target = ev.target;
        bestPriority[slot] = 0;
    }
    AIPercept& p = perc.percepts[slot];
    const bool firstThisTick = (p.currentSenses == 0);
    if (ev.sense == perceptionsense::kSight) {
        // 速度: 少し前にも視覚で見ていれば、その位置との差から求める。初めて見た・久しぶりなら 0
        const uint64_t gap = (p.lastSensedTick < tick) ? (tick - p.lastSensedTick) : 0;
        if ((p.lastSenses & perceptionsense::kSight) != 0 && gap > 0 && gap <= kMaxVelocityGapTicks && dt > 0.0f) {
            const float inv = 1.0f / (static_cast<float>(gap) * dt);
            p.velocity = { (ev.pos.x - p.lastSensedPos.x) * inv, (ev.pos.y - p.lastSensedPos.y) * inv,
                           (ev.pos.z - p.lastSensedPos.z) * inv };
        } else {
            p.velocity = { 0.0f, 0.0f, 0.0f };
        }
    }
    if (firstThisTick) {
        p.lastSenses = 0;
        p.strength = 0.0f;
        bestPriority[slot] = 0;
    }
    p.currentSenses |= ev.sense;
    p.lastSenses |= ev.sense;
    p.lastSensedTick = tick;
    p.strength = std::max(p.strength, ev.strength);
    const int pri = SensePriority(ev.sense);
    if (pri > bestPriority[slot]) {
        bestPriority[slot] = pri;
        p.lastSensedPos = { ev.pos.x, ev.pos.y, ev.pos.z };
    }
}

} // namespace

PerceptionAttitude PerceptionAttitudeOf(const AIPerceptionComponent& perception, int32_t targetFaction)
{
    if (targetFaction == perception.faction) {
        return PerceptionAttitude::Friendly;
    }
    if (targetFaction < 0 || targetFaction >= kMaxFactions) {
        return PerceptionAttitude::Neutral;
    }
    return ((perception.hostileMask >> static_cast<uint32_t>(targetFaction)) & 1u) != 0u ? PerceptionAttitude::Hostile
                                                                                         : PerceptionAttitude::Neutral;
}

bool PerceptionDetects(const AIPerceptionComponent& perception, PerceptionAttitude attitude)
{
    switch (attitude) {
    case PerceptionAttitude::Hostile: return perception.detectEnemies;
    case PerceptionAttitude::Friendly: return perception.detectFriendlies;
    default: return perception.detectNeutrals;
    }
}

int PerceptionReportNoise(World& world, const float pos[3], float loudness, float range, EntityID instigator)
{
    if (pos == nullptr || !(loudness > 0.0f) || !(range > 0.0f) || !std::isfinite(pos[0]) || !std::isfinite(pos[1])
        || !std::isfinite(pos[2])) {
        return 0;
    }
    // 鳴らした者の陣営 (AIStimulusSource があれば)。無ければ「所属の無い音」
    const EntityID owner = instigator.IsNull() ? kNullEntity : FindStimulusOwner(world, instigator);
    const AIStimulusSourceComponent* src =
        owner.IsNull() ? nullptr : world.GetComponent<AIStimulusSourceComponent>(owner);
    if (src != nullptr && !src->hearingEnabled) {
        return 0;
    }
    const EntityID target = owner.IsNull() ? instigator : owner;

    int heard = 0;
    const ComponentTypeId req[] = { AIPerceptionComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int pi = arch.FindTypeIndex(AIPerceptionComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            auto* perc = static_cast<AIPerceptionComponent*>(arch.GetPtr(pi, row));
            if (!perc->hearingEnabled || perc->hearingMode != hearingmode::kDistance || !IsEntityActive(world, e)) {
                continue;
            }
            if (!target.IsNull() && (target == e || IsAncestorOrSelf(world, e, target))) {
                continue; // 自分の音は聞かない
            }
            if (src != nullptr) {
                if (!PerceptionDetects(*perc, PerceptionAttitudeOf(*perc, src->faction))) {
                    continue;
                }
            } else if (!perc->hearUnaffiliated) {
                continue;
            }
            const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            const float ex = wm._41, ey = wm._42 + perc->eyeHeight, ez = wm._43;
            const float d = Length(pos[0] - ex, pos[1] - ey, pos[2] - ez);
            if (d > range || d > perc->hearingRange) {
                continue;
            }
            const float strength = loudness * (1.0f - d / range);
            if (strength < perc->hearingThreshold || !(strength > 0.0f)) {
                continue;
            }
            ++heard;
            // 強い方が勝つ。同じ強さなら先に届いた方 (= 厳密に大きいときだけ上書き)
            if (strength > perc->pendingNoiseStrength) {
                perc->pendingNoiseStrength = strength;
                perc->pendingNoisePos = { pos[0], pos[1], pos[2] };
                perc->pendingNoiseSource = target;
            }
        }
    });
    return heard;
}

bool PerceptionReportDamage(World& world, EntityID victim, EntityID instigator, float amount, const float hitPos[3])
{
    auto* perc = world.GetComponent<AIPerceptionComponent>(victim);
    if (perc == nullptr || !perc->damageEnabled || !(amount > 0.0f) || !std::isfinite(amount)) {
        return false;
    }
    // 位置は攻撃者の位置 (分かれば)、無ければ当たった点
    DirectX::XMFLOAT3 at = { 0.0f, 0.0f, 0.0f };
    bool hasPos = false;
    if (!instigator.IsNull()) {
        if (const auto* wm = world.GetComponent<WorldMatrixComponent>(instigator)) {
            at = { wm->value._41, wm->value._42, wm->value._43 };
            hasPos = true;
        }
    }
    if (!hasPos && hitPos != nullptr && std::isfinite(hitPos[0]) && std::isfinite(hitPos[1])
        && std::isfinite(hitPos[2])) {
        at = { hitPos[0], hitPos[1], hitPos[2] };
        hasPos = true;
    }
    if (!hasPos) {
        if (const auto* wm = world.GetComponent<WorldMatrixComponent>(victim)) {
            at = { wm->value._41, wm->value._42, wm->value._43 };
        }
    }
    if (amount > perc->pendingDamageAmount) {
        const EntityID owner = instigator.IsNull() ? kNullEntity : FindStimulusOwner(world, instigator);
        perc->pendingDamageAmount = amount;
        perc->pendingDamagePos = at;
        perc->pendingDamageSource = owner.IsNull() ? instigator : owner;
    }
    return true;
}

bool PerceptionCanSee(World& world, EntityID observer, EntityID target, uint64_t tick)
{
    auto* perc = world.GetComponent<AIPerceptionComponent>(observer);
    const auto* source = world.GetComponent<AIStimulusSourceComponent>(target);
    const auto* owm = world.GetComponent<WorldMatrixComponent>(observer);
    const auto* twm = world.GetComponent<WorldMatrixComponent>(target);
    if (perc == nullptr || source == nullptr || owm == nullptr || twm == nullptr || !perc->sightEnabled
        || !source->sightEnabled || !IsEntityActive(world, observer) || !IsEntityActive(world, target)) {
        return false;
    }
    const Perceiver p = MakePerceiver(observer, perc, owm->value);
    const Stimulus s = MakeStimulus(target, source, twm->value);
    if (!Detects(world, p, s)) {
        return false;
    }
    const bool wasSeen = WasSeenLastTick(*perc, target, tick);
    float d = 0.0f;
    const int geometry = SightGeometry(*perc, p, s, wasSeen, d);
    if (geometry != 1) {
        return geometry == 2;
    }
    std::vector<LosCollider> colliders;
    CollectLosColliders(world, colliders);
    int rays = 0;
    return LineOfSight(world, colliders, p, s, d, perc->losLayerMask, rays);
}

void PerceptionSystem::Update(World& world, uint64_t tick, float dt, const std::vector<SolidContact>& contacts)
{
    stats_ = PerceptionStats{};

    // ---- 存在ゲート: AIPerception が無ければ何もしない (RNG もハッシュも触らない) ----
    std::vector<Perceiver> perceivers;
    {
        const ComponentTypeId req[] = { AIPerceptionComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int pi = arch.FindTypeIndex(AIPerceptionComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            const int li = arch.FindTypeIndex(AcousticListenerComponent::sTypeId);
            const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                Perceiver p = MakePerceiver(e, static_cast<AIPerceptionComponent*>(arch.GetPtr(pi, row)), wm);
                if (li >= 0) {
                    p.ear = static_cast<const AcousticListenerComponent*>(arch.GetPtr(li, row));
                }
                if (ci >= 0) {
                    p.cc = static_cast<const CharacterControllerComponent*>(arch.GetPtr(ci, row));
                    p.ccScaleY = ScaleY(wm);
                    p.ccScaleXZ = ScaleXZ(wm);
                }
                perceivers.push_back(p);
            }
        });
    }
    if (perceivers.empty()) {
        return;
    }
    MYE_PROFILE_SCOPE("perception");
    const auto t0 = std::chrono::steady_clock::now();
    std::sort(perceivers.begin(), perceivers.end(),
              [](const Perceiver& a, const Perceiver& b) { return KeyLess(a.entity, b.entity); });
    stats_.perceivers = static_cast<int>(perceivers.size());

    // ---- 知覚される側 (エンティティキー順) ----
    std::vector<Stimulus> stimuli;
    {
        const ComponentTypeId req[] = { AIStimulusSourceComponent::sTypeId, WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int si = arch.FindTypeIndex(AIStimulusSourceComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                Stimulus s = MakeStimulus(e, static_cast<const AIStimulusSourceComponent*>(arch.GetPtr(si, row)), wm);
                if (ci >= 0) {
                    s.cc = static_cast<const CharacterControllerComponent*>(arch.GetPtr(ci, row));
                    s.ccScaleY = ScaleY(wm);
                    s.ccScaleXZ = ScaleXZ(wm);
                }
                stimuli.push_back(s);
            }
        });
        std::sort(stimuli.begin(), stimuli.end(),
                  [](const Stimulus& a, const Stimulus& b) { return KeyLess(a.entity, b.entity); });
    }

    // ---- 視線を遮るコライダー (視覚の候補が出たときに 1 回だけ集める) ----
    std::vector<LosCollider> colliders;
    bool collidersReady = false;
    auto ensureColliders = [&]() {
        if (collidersReady) {
            return;
        }
        collidersReady = true;
        CollectLosColliders(world, colliders);
        stats_.losColliders = static_cast<int>(colliders.size());
    };

    // ---- 接触: 前の tick のソリッド接触 (キー = entity.index の対) を entity へ引く表 ----
    std::vector<std::pair<EntityID, EntityID>> touchPairs; // (触れた側の刺激源の持ち主, 触れられたコライダー)
    if (!contacts.empty()) {
        std::vector<std::pair<uint32_t, EntityID>> byIndex;
        const ComponentTypeId creq[] = { ColliderComponent::sTypeId };
        world.ForEachArchetype(creq, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                byIndex.emplace_back(e.index, e);
            }
        });
        std::sort(byIndex.begin(), byIndex.end(),
                  [](const std::pair<uint32_t, EntityID>& a, const std::pair<uint32_t, EntityID>& b) {
                      return a.first < b.first;
                  });
        const auto lookup = [&byIndex](uint32_t index) {
            const auto it = std::lower_bound(
                byIndex.begin(), byIndex.end(), index,
                [](const std::pair<uint32_t, EntityID>& p, uint32_t v) { return p.first < v; });
            return (it != byIndex.end() && it->first == index) ? it->second : kNullEntity;
        };
        for (const SolidContact& c : contacts) { // key 昇順 (PhysicsSystem の出力規約)
            const EntityID ea = lookup(static_cast<uint32_t>(c.key >> 32));
            const EntityID eb = lookup(static_cast<uint32_t>(c.key & 0xFFFFFFFFu));
            if (ea.IsNull() || eb.IsNull()) {
                continue; // 1 tick 古い接触なので、消えたエンティティが混じりうる
            }
            touchPairs.emplace_back(ea, eb);
            touchPairs.emplace_back(eb, ea);
        }
    }

    std::vector<SenseEvent> events;
    std::vector<std::pair<float, int>> sightCandidates; // (距離, stimuli の添字)
    std::vector<int> bestPriority(kMaxPercepts, 0);
    for (Perceiver& p : perceivers) {
        AIPerceptionComponent& perc = *p.perception;
        int count = std::clamp(perc.perceivedCount, 0, kMaxPercepts);
        for (int i = 0; i < count; ++i) {
            perc.percepts[i].currentSenses = 0;
        }
        events.clear();

        // ---- 視覚 ----
        if (perc.sightEnabled && !stimuli.empty()) {
            sightCandidates.clear();
            for (int si = 0; si < static_cast<int>(stimuli.size()); ++si) {
                const Stimulus& s = stimuli[si];
                if (!s.source->sightEnabled || !Detects(world, p, s)) {
                    continue;
                }
                // 前の tick に見えていた相手は loseSightRadius まで追い続ける (UE の LoseSightRadius)
                const bool wasSeen = WasSeenLastTick(perc, s.entity, tick);
                float d = 0.0f;
                const int geometry = SightGeometry(perc, p, s, wasSeen, d);
                if (geometry == 2) {
                    events.push_back({ s.entity, perceptionsense::kSight, s.point, SightStrength(perc, wasSeen, d) });
                } else if (geometry == 1) {
                    sightCandidates.emplace_back(d, si);
                }
            }
            // 近い順 (同距離は stimuli の並び = エンティティキー順) に上限まで視線を確かめる
            std::sort(sightCandidates.begin(), sightCandidates.end());
            const int rays = std::min(static_cast<int>(sightCandidates.size()), kMaxLosRaysPerPerceiver);
            if (rays > 0) {
                ensureColliders();
            }
            for (int k = 0; k < rays; ++k) {
                const float d = sightCandidates[k].first;
                const Stimulus& s = stimuli[sightCandidates[k].second];
                if (LineOfSight(world, colliders, p, s, d, perc.losLayerMask, stats_.losRays)) {
                    events.push_back({ s.entity, perceptionsense::kSight, s.point,
                                       SightStrength(perc, WasSeenLastTick(perc, s.entity, tick), d) });
                }
            }
        }

        // ---- 聴覚 ----
        if (perc.hearingEnabled) {
            if (perc.hearingMode == hearingmode::kAcoustic) {
                // 同じ tick に音響 (フェーズ 3.4) が耳へ配った音。★lastHeardTick == 0 は「まだ聞いていない」
                if (p.ear != nullptr && p.ear->lastHeardTick != 0 && p.ear->lastHeardTick == tick
                    && p.ear->lastLoudness >= perc.hearingThreshold) {
                    const DirectX::XMFLOAT3& hp = p.ear->lastHeardPos;
                    const float d = Length(hp.x - p.eye.x, hp.y - p.eye.y, hp.z - p.eye.z);
                    EntityID src = p.ear->lastSourceEntity;
                    if (!src.IsNull() && !world.IsAlive(src)) {
                        src = kNullEntity;
                    }
                    const EntityID owner = src.IsNull() ? kNullEntity : FindStimulusOwner(world, src);
                    const AIStimulusSourceComponent* ss =
                        owner.IsNull() ? nullptr : world.GetComponent<AIStimulusSourceComponent>(owner);
                    bool accept = d <= perc.hearingRange;
                    if (accept && !src.IsNull() && IsAncestorOrSelf(world, p.entity, src)) {
                        accept = false; // 自分の音
                    }
                    if (accept) {
                        accept = (ss != nullptr)
                            ? ss->hearingEnabled && PerceptionDetects(perc, PerceptionAttitudeOf(perc, ss->faction))
                            : perc.hearUnaffiliated;
                    }
                    if (accept) {
                        events.push_back({ owner.IsNull() ? src : owner, perceptionsense::kHearing,
                                           { hp.x, hp.y, hp.z }, p.ear->lastLoudness });
                    }
                }
            } else if (perc.pendingNoiseStrength > 0.0f) {
                EntityID src = perc.pendingNoiseSource;
                if (!src.IsNull() && !world.IsAlive(src)) {
                    src = kNullEntity;
                }
                const DirectX::XMFLOAT3& np = perc.pendingNoisePos;
                events.push_back({ src, perceptionsense::kHearing, { np.x, np.y, np.z }, perc.pendingNoiseStrength });
            }
        }
        perc.pendingNoiseStrength = 0.0f;
        perc.pendingNoiseSource = kNullEntity;
        perc.pendingNoisePos = { 0.0f, 0.0f, 0.0f };

        // ---- ダメージ ----
        if (perc.damageEnabled && perc.pendingDamageAmount > 0.0f) {
            EntityID src = perc.pendingDamageSource;
            if (!src.IsNull() && !world.IsAlive(src)) {
                src = kNullEntity;
            }
            const DirectX::XMFLOAT3& dp = perc.pendingDamagePos;
            events.push_back({ src, perceptionsense::kDamage, { dp.x, dp.y, dp.z }, perc.pendingDamageAmount });
        }
        perc.pendingDamageAmount = 0.0f;
        perc.pendingDamageSource = kNullEntity;
        perc.pendingDamagePos = { 0.0f, 0.0f, 0.0f };

        // ---- 接触 ----
        if (perc.touchEnabled && !stimuli.empty()) {
            for (const Stimulus& s : stimuli) {
                if (!s.source->touchEnabled || !Detects(world, p, s)) {
                    continue;
                }
                bool touching = false;
                // (a) CharacterController どうしのカプセルが触れている (CC は物理の接触ペアに出ない)
                if (p.cc != nullptr && s.cc != nullptr) {
                    float plo = 0.0f, phi = 0.0f, pr = 0.0f, slo = 0.0f, shi = 0.0f, sr = 0.0f;
                    CapsuleSpan(*p.cc, p.ccScaleY, p.ccScaleXZ, p.pos, plo, phi, pr);
                    CapsuleSpan(*s.cc, s.ccScaleY, s.ccScaleXZ, s.pos, slo, shi, sr);
                    const float dx = s.pos.x - p.pos.x, dz = s.pos.z - p.pos.z;
                    const float reach = pr + sr + kTouchMargin;
                    touching = (dx * dx + dz * dz <= reach * reach) && plo <= shi && slo <= phi;
                }
                // (b) 前の tick の物理のソリッド接触 (自分の体と相手の体のコライダーどうし)
                for (size_t k = 0; !touching && k < touchPairs.size(); ++k) {
                    const EntityID mine = touchPairs[k].second;
                    const EntityID other = touchPairs[k].first;
                    touching = IsAncestorOrSelf(world, p.entity, mine) && IsAncestorOrSelf(world, s.entity, other);
                }
                if (touching) {
                    events.push_back({ s.entity, perceptionsense::kTouch, s.pos, 1.0f });
                }
            }
        }

        // ---- 反映 ----
        std::fill(bestPriority.begin(), bestPriority.end(), 0);
        for (const SenseEvent& ev : events) {
            Merge(perc, count, ev, tick, dt, bestPriority);
        }

        // ---- 忘却と予測 ----
        int seen = 0;
        int write = 0;
        for (int i = 0; i < count; ++i) {
            AIPercept q = perc.percepts[i];
            const uint64_t age = (tick >= q.lastSensedTick) ? (tick - q.lastSensedTick) : 0;
            const bool targetGone = !q.target.IsNull() && !world.IsAlive(q.target);
            if (q.currentSenses == 0
                && (targetGone || age > static_cast<uint64_t>(std::max(perc.forgetTicks, 0)))) {
                continue; // 忘れる (消えた相手は即座に)
            }
            if ((q.currentSenses & perceptionsense::kSight) != 0) {
                ++seen;
                q.predictedPos = q.lastSensedPos;
            } else {
                const uint64_t ahead = std::min<uint64_t>(age, static_cast<uint64_t>(std::max(perc.predictionTicks, 0)));
                const float t = static_cast<float>(ahead) * dt;
                q.predictedPos = { q.lastSensedPos.x + q.velocity.x * t, q.lastSensedPos.y + q.velocity.y * t,
                                   q.lastSensedPos.z + q.velocity.z * t };
            }
            perc.percepts[write++] = q;
        }
        for (int i = write; i < kMaxPercepts; ++i) {
            perc.percepts[i] = AIPercept{};
        }
        // 並びはキー順 (名乗らない音 = kNullEntity が先頭)。スロットの埋まり方に結果を依存させない
        std::sort(perc.percepts, perc.percepts + write, [](const AIPercept& a, const AIPercept& b) {
            const bool an = a.target.IsNull(), bn = b.target.IsNull();
            if (an != bn) {
                return an;
            }
            return KeyLess(a.target, b.target);
        });
        perc.perceivedCount = write;
        perc.seenCount = seen;
    }

    stats_.updateUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

void PerceptionSystem::AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const
{
    constexpr uint32_t kSeenColor = 0x40FF60FFu;      // 見えている相手への線 (緑)
    constexpr uint32_t kLastPosColor = 0xFFD040FFu;   // 最後に知覚した位置 (黄)
    constexpr uint32_t kPredictColor = 0xFF8030FFu;   // 予測位置 (橙)
    constexpr float kMark = 0.2f;
    auto cross = [&out](const DirectX::XMFLOAT3& c, uint32_t rgba) {
        out.push_back({ c.x - kMark, c.y, c.z, c.x + kMark, c.y, c.z, rgba });
        out.push_back({ c.x, c.y - kMark, c.z, c.x, c.y + kMark, c.z, rgba });
        out.push_back({ c.x, c.y, c.z - kMark, c.x, c.y, c.z + kMark, rgba });
    };
    const ComponentTypeId req[] = { AIPerceptionComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int pi = arch.FindTypeIndex(AIPerceptionComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* perc = static_cast<const AIPerceptionComponent*>(arch.GetPtr(pi, row));
            if (!perc->drawDebug) {
                continue;
            }
            const auto& wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            const float ex = wm._41, ey = wm._42 + perc->eyeHeight, ez = wm._43;
            const int count = std::clamp(perc->perceivedCount, 0, kMaxPercepts);
            for (int i = 0; i < count; ++i) {
                const AIPercept& q = perc->percepts[i];
                if ((q.currentSenses & perceptionsense::kSight) != 0) {
                    out.push_back({ ex, ey, ez, q.lastSensedPos.x, q.lastSensedPos.y, q.lastSensedPos.z, kSeenColor });
                    continue;
                }
                cross(q.lastSensedPos, kLastPosColor);
                const float dx = q.predictedPos.x - q.lastSensedPos.x;
                const float dy = q.predictedPos.y - q.lastSensedPos.y;
                const float dz = q.predictedPos.z - q.lastSensedPos.z;
                if (dx * dx + dy * dy + dz * dz > 1e-6f) {
                    out.push_back({ q.lastSensedPos.x, q.lastSensedPos.y, q.lastSensedPos.z, q.predictedPos.x,
                                    q.predictedPos.y, q.predictedPos.z, kPredictColor });
                    cross(q.predictedPos, kPredictColor);
                }
            }
        }
    });
}

} // namespace mye
