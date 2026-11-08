//====================================================================================
//                          DisturbanceStep.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の 1 tick の step と読み取り
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceState.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/Scene.h"

// ★決定論 (check_rules 規則 18): このファイルの計算は + - * / sqrt floor ceil min max だけ。
//   exp / pow / log / sin / cos 等の CRT の超越関数は Debug / Release / WARP でビットが揃う保証が無いので使わない

namespace mye {

namespace {

// ---- コンポーネントの値を sim が使う範囲へ切る ----

float FiniteOr(float v, float fallback)
{
    return std::isfinite(v) ? v : fallback;
}

float ClampOr(float v, float lo, float hi, float fallback)
{
    return std::min(std::max(FiniteOr(v, fallback), lo), hi);
}

// 切った後の場の設定 (sim はこの値だけで動く)
struct FieldParams {
    EntityID entity = kNullEntity;
    int32_t shape = kDisturbanceGroundLayer;
    int32_t cellsX = 0;
    int32_t cellsY = 1;
    int32_t cellsZ = 0;
    float cellSize = 0.25f;
    float thickness = 1.0f;
    uint32_t channels = 0;
    float restDensity = 1.0f;
    float densityKeep = 1.0f;
    float velocityKeep = 1.0f;
    float trailKeep = 1.0f;
    float rippleSpeed = 0.0f;
    float rippleKeep = 1.0f;
    float pos[3] = { 0.0f, 0.0f, 0.0f }; // 場のエンティティのワールド位置
};

FieldParams SanitizeField(EntityID e, const DisturbanceFieldComponent& c, const float pos[3])
{
    FieldParams p;
    p.entity = e;
    p.shape = c.shape == kDisturbanceVolume ? kDisturbanceVolume : kDisturbanceGroundLayer;
    p.cellsX = std::clamp(c.cellsX, kMinDisturbanceSide, kMaxDisturbanceSide);
    p.cellsZ = std::clamp(c.cellsZ, kMinDisturbanceSide, kMaxDisturbanceSide);
    p.cellsY = p.shape == kDisturbanceGroundLayer ? 1 : std::clamp(c.cellsY, kMinDisturbanceSide, kMaxDisturbanceSide);
    // 総マス数の上限 (2D は 512 x 512 = 262144 で上限ちょうど。Y を持つ形に備えて Z を詰める)
    while (static_cast<int64_t>(p.cellsX) * p.cellsY * p.cellsZ > kMaxDisturbanceCells && p.cellsZ > kMinDisturbanceSide) {
        --p.cellsZ;
    }
    p.cellSize = ClampOr(c.cellSize, 0.05f, 8.0f, 0.25f);
    p.thickness = ClampOr(c.thickness, 0.05f, 20.0f, 1.0f);
    p.channels = c.channels & kDisturbanceChannelMask;
    p.restDensity = ClampOr(c.restDensity, 0.0f, 1.0f, 1.0f);
    p.densityKeep = ClampOr(c.densityKeep, 0.0f, 1.0f, 1.0f);
    p.velocityKeep = ClampOr(c.velocityKeep, 0.0f, 1.0f, 1.0f);
    p.trailKeep = ClampOr(c.trailKeep, 0.0f, 1.0f, 1.0f);
    p.rippleSpeed = ClampOr(c.rippleSpeed, 0.0f, 1.0e6f, 0.0f);
    p.rippleKeep = ClampOr(c.rippleKeep, 0.0f, 1.0f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        p.pos[i] = FiniteOr(pos[i], 0.0f);
    }
    return p;
}

// int32 へ落とす前に範囲を切る (巨大・非有限の座標で未定義動作を踏まない)
int32_t ClampToInt(float v)
{
    constexpr float kLimit = 1.0e8f;
    return static_cast<int32_t>(std::min(std::max(FiniteOr(v, 0.0f), -kLimit), kLimit));
}

// 静止状態の新しいスロット (全マス: 流れ 0、濃さ restDensity、跡 0、波紋 0)
DisturbanceSlot MakeRestSlot(const FieldParams& p)
{
    DisturbanceSlot s;
    s.entity = p.entity;
    s.shape = p.shape;
    s.cellsX = p.cellsX;
    s.cellsY = p.cellsY;
    s.cellsZ = p.cellsZ;
    s.channels = p.channels;
    s.cellSize = p.cellSize;
    s.originCell[0] = ClampToInt(std::floor(p.pos[0] / p.cellSize + 0.5f)) - p.cellsX / 2;
    s.originCell[1] = 0;
    s.originCell[2] = ClampToInt(std::floor(p.pos[2] / p.cellSize + 0.5f)) - p.cellsZ / 2;
    s.baseY = p.pos[1];
    const size_t n = s.CellCount();
    s.flowX.assign(n, 0.0f);
    s.flowZ.assign(n, 0.0f);
    if ((p.channels & kDisturbanceChannelDensity) != 0) {
        s.density.assign(n, p.restDensity);
    }
    if ((p.channels & kDisturbanceChannelTrail) != 0) {
        s.trail.assign(n, 0.0f);
    }
    if ((p.channels & kDisturbanceChannelRipple) != 0) {
        s.ripple.assign(n, 0.0f);
        s.ripplePrev.assign(n, 0.0f);
    }
    return s;
}

// 既存のスロットを作り直さずに使えるか (形・寸法・チャネル・マスの大きさが同じ)
bool SlotMatches(const DisturbanceSlot& s, const FieldParams& p)
{
    return s.shape == p.shape && s.cellsX == p.cellsX && s.cellsY == p.cellsY && s.cellsZ == p.cellsZ
           && s.channels == p.channels && s.cellSize == p.cellSize;
}

// ---- ECS から集める行 ----

struct FieldRow {
    EntityID entity;
    DisturbanceFieldComponent comp;
    float pos[3];
};

struct SourceRow {
    EntityID entity;
    DisturbanceSourceComponent comp;
    float matrix[16]; // WorldMatrix (行ベクトル規約: 平行移動は 12..14)
    bool hasMatrix = false;
    bool hasCc = false;
    bool hasBody = false;
    float ccVelocity[3] = { 0.0f, 0.0f, 0.0f };
    float bodyVelocity[3] = { 0.0f, 0.0f, 0.0f };
};

void CollectFields(World& world, std::vector<FieldRow>& out)
{
    const ComponentTypeId req[] = { DisturbanceFieldComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(DisturbanceFieldComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            FieldRow r{};
            r.entity = arch.EntityAt(row);
            r.comp = *static_cast<const DisturbanceFieldComponent*>(arch.GetPtr(ti, row));
            if (wi >= 0) {
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                r.pos[0] = wm->value._41;
                r.pos[1] = wm->value._42;
                r.pos[2] = wm->value._43;
            }
            out.push_back(r);
        }
    });
    // アーキタイプの並びに依らない明示キー (規則 7)
    std::sort(out.begin(), out.end(), [](const FieldRow& a, const FieldRow& b) { return a.entity.index < b.entity.index; });
}

void CollectSources(World& world, std::vector<SourceRow>& out)
{
    const ComponentTypeId req[] = { DisturbanceSourceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(DisturbanceSourceComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
        const int bi = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            SourceRow r{};
            r.entity = arch.EntityAt(row);
            r.comp = *static_cast<const DisturbanceSourceComponent*>(arch.GetPtr(ti, row));
            if (wi >= 0) {
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                r.hasMatrix = true;
                for (int row4 = 0; row4 < 4; ++row4) {
                    for (int col4 = 0; col4 < 4; ++col4) {
                        r.matrix[row4 * 4 + col4] = wm->value.m[row4][col4];
                    }
                }
            }
            if (ci >= 0) {
                const auto* cc = static_cast<const CharacterControllerComponent*>(arch.GetPtr(ci, row));
                r.hasCc = true;
                r.ccVelocity[0] = cc->velocity.x;
                r.ccVelocity[1] = cc->velocity.y;
                r.ccVelocity[2] = cc->velocity.z;
            }
            if (bi >= 0) {
                const auto* rb = static_cast<const RigidbodyComponent*>(arch.GetPtr(bi, row));
                r.hasBody = !rb->isKinematic;
                r.bodyVelocity[0] = rb->velocity.x;
                r.bodyVelocity[1] = rb->velocity.y;
                r.bodyVelocity[2] = rb->velocity.z;
            }
            out.push_back(r);
        }
    });
    std::sort(out.begin(), out.end(), [](const SourceRow& a, const SourceRow& b) { return a.entity.index < b.entity.index; });
}

// ---- 注入 ----

enum class FlowKind { Uniform, Radial, Cone };

// 源・インパルス共通の注入 1 件 (spec §4.1 C)
struct Injection {
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float radius = 0.0f;
    FlowKind flow = FlowKind::Uniform;
    float flowX = 0.0f;       // Uniform / Cone: 目標速度 (xz)
    float flowZ = 0.0f;
    float radialSpeed = 0.0f; // Radial: 外向きの速さ
    float coneAxisX = 0.0f;   // Cone: 軸 (xz、正規化済み。長さ 0 なら何も起きない)
    float coneAxisZ = 0.0f;
    float coneCos = 0.0f;
    float mix = 1.0f;         // 流れの混ぜ率の係数 (源 = strength、インパルス = 1)
    float speedFactor = 1.0f; // 速さ係数 s
    float densityDelta = 0.0f;
    float trailDepth = 0.0f;
    float ripple = 0.0f;
};

float Saturate(float v)
{
    return std::min(std::max(v, 0.0f), 1.0f);
}

// 範囲 [lo, hi) のセル番号 (中心が (pos - r, pos + r) に入りうる範囲) を格子の内側へ切って返す。空なら false
bool CellRange(float pos, float radius, float cellSize, int32_t origin, int32_t count, int32_t& lo, int32_t& hi)
{
    const float fLo = std::floor((pos - radius) / cellSize - static_cast<float>(origin) - 0.5f);
    const float fHi = std::ceil((pos + radius) / cellSize - static_cast<float>(origin) - 0.5f);
    const float cLo = std::max(fLo, 0.0f);
    const float cHi = std::min(fHi, static_cast<float>(count - 1));
    if (!(cLo <= cHi)) {
        return false;
    }
    lo = static_cast<int32_t>(cLo);
    hi = static_cast<int32_t>(cHi);
    return true;
}

void Inject(DisturbanceSlot& s, float thickness, const Injection& in)
{
    // 非有限の入力は何も書かない (半径 0 以下も同じ)
    if (!(in.radius > 0.0f) || !std::isfinite(in.radius) || !std::isfinite(in.pos[0]) || !std::isfinite(in.pos[1])
        || !std::isfinite(in.pos[2])) {
        return;
    }
    int32_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if (!CellRange(in.pos[0], in.radius, s.cellSize, s.originCell[0], s.cellsX, x0, x1)
        || !CellRange(in.pos[2], in.radius, s.cellSize, s.originCell[2], s.cellsZ, z0, z1)) {
        return;
    }
    // 板からの縦のはみ出し (板の中なら 0)
    const float dy = std::max(std::max(0.0f, s.baseY - in.pos[1]), in.pos[1] - (s.baseY + thickness));
    const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
    const bool hasTrail = (s.channels & kDisturbanceChannelTrail) != 0;
    const bool hasRipple = (s.channels & kDisturbanceChannelRipple) != 0;
    for (int32_t z = z0; z <= z1; ++z) {
        const float cz = (static_cast<float>(s.originCell[2] + z) + 0.5f) * s.cellSize;
        const float ez = cz - in.pos[2];
        for (int32_t x = x0; x <= x1; ++x) {
            const float cx = (static_cast<float>(s.originCell[0] + x) + 0.5f) * s.cellSize;
            const float ex = cx - in.pos[0];
            const float d = std::sqrt(ex * ex + ez * ez + dy * dy);
            if (!(d < in.radius)) {
                continue;
            }
            const float t = 1.0f - d / in.radius;
            float w = t * t;
            float ux = in.flowX;
            float uz = in.flowZ;
            if (in.flow != FlowKind::Uniform) {
                const float dh = std::sqrt(ex * ex + ez * ez);
                if (in.flow == FlowKind::Radial) {
                    // 中心 (dh < 1e-6) は向きが定まらないので 0
                    ux = dh < 1.0e-6f ? 0.0f : ex / dh * in.radialSpeed;
                    uz = dh < 1.0e-6f ? 0.0f : ez / dh * in.radialSpeed;
                } else {
                    const float axisLen = in.coneAxisX * in.coneAxisX + in.coneAxisZ * in.coneAxisZ;
                    float factor = 0.0f;
                    if (axisLen > 0.0f && in.coneCos < 1.0f) {
                        const float cosTheta = dh < 1.0e-6f ? 1.0f : (in.coneAxisX * ex + in.coneAxisZ * ez) / dh;
                        factor = Saturate((cosTheta - in.coneCos) / (1.0f - in.coneCos));
                    }
                    w *= factor;
                }
            }
            const size_t i = static_cast<size_t>(x) + static_cast<size_t>(s.cellsX) * static_cast<size_t>(z);
            const float a = Saturate(w * in.mix);
            s.flowX[i] += a * (ux - s.flowX[i]);
            s.flowZ[i] += a * (uz - s.flowZ[i]);
            if (hasDensity) {
                const float delta = w * in.speedFactor * in.densityDelta;
                const float rho = s.density[i];
                s.density[i] = delta < 0.0f ? rho + delta * rho : rho + delta * (1.0f - rho);
            }
            if (hasTrail) {
                s.trail[i] += w * in.trailDepth * (1.0f - s.trail[i]);
            }
            if (hasRipple) {
                s.ripple[i] += w * in.speedFactor * in.ripple;
            }
        }
    }
}

// 源 1 件の位置と速度 (全ての場に共通)
struct SourceNow {
    bool valid = false;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float vel[3] = { 0.0f, 0.0f, 0.0f };      // Moving の速度
    float constant[3] = { 0.0f, 0.0f, 0.0f }; // Constant の流れ (ワールド)
};

SourceNow EvaluateSource(const SourceRow& r)
{
    SourceNow n;
    const DisturbanceSourceComponent& c = r.comp;
    if (r.hasMatrix) {
        const float* m = r.matrix;
        const float ox = c.offset.x, oy = c.offset.y, oz = c.offset.z;
        n.pos[0] = ox * m[0] + oy * m[4] + oz * m[8] + m[12];
        n.pos[1] = ox * m[1] + oy * m[5] + oz * m[9] + m[13];
        n.pos[2] = ox * m[2] + oy * m[6] + oz * m[10] + m[14];
        // 定常の流れはエンティティの向きで回す (スケールがあればその分伸びる)
        const float cx = c.constantVelocity.x, cy = c.constantVelocity.y, cz = c.constantVelocity.z;
        n.constant[0] = cx * m[0] + cy * m[4] + cz * m[8];
        n.constant[1] = cx * m[1] + cy * m[5] + cz * m[9];
        n.constant[2] = cx * m[2] + cy * m[6] + cz * m[10];
    } else {
        n.pos[0] = c.offset.x;
        n.pos[1] = c.offset.y;
        n.pos[2] = c.offset.z;
        n.constant[0] = c.constantVelocity.x;
        n.constant[1] = c.constantVelocity.y;
        n.constant[2] = c.constantVelocity.z;
    }
    n.valid = std::isfinite(n.pos[0]) && std::isfinite(n.pos[1]) && std::isfinite(n.pos[2]);
    if (!n.valid) {
        return n;
    }
    if (r.hasCc) {
        std::copy(r.ccVelocity, r.ccVelocity + 3, n.vel);
    } else if (r.hasBody) {
        std::copy(r.bodyVelocity, r.bodyVelocity + 3, n.vel);
    } else if (c.prevValid != 0) {
        n.vel[0] = (n.pos[0] - c.prevPos.x) * kDisturbanceTickRate;
        n.vel[1] = (n.pos[1] - c.prevPos.y) * kDisturbanceTickRate;
        n.vel[2] = (n.pos[2] - c.prevPos.z) * kDisturbanceTickRate;
    }
    for (float& v : n.vel) {
        v = FiniteOr(v, 0.0f);
    }
    for (float& v : n.constant) {
        v = FiniteOr(v, 0.0f);
    }
    return n;
}

void InjectSource(DisturbanceSlot& s, float thickness, const SourceRow& r, const SourceNow& n)
{
    const DisturbanceSourceComponent& c = r.comp;
    Injection in;
    std::copy(n.pos, n.pos + 3, in.pos);
    in.radius = c.radius;
    in.mix = ClampOr(c.strength, 0.0f, 1.0f, 0.0f);
    in.densityDelta = ClampOr(c.densityDelta, -1.0f, 1.0f, 0.0f);
    in.trailDepth = ClampOr(c.trailDepth, 0.0f, 1.0f, 0.0f);
    in.ripple = FiniteOr(c.ripple, 0.0f);
    if (c.mode == 1) {
        in.flowX = n.constant[0];
        in.flowZ = n.constant[2];
        in.speedFactor = 1.0f;
    } else {
        in.flowX = n.vel[0];
        in.flowZ = n.vel[2];
        // 2D の速さは水平成分だけ
        const float speed = std::sqrt(n.vel[0] * n.vel[0] + n.vel[2] * n.vel[2]);
        in.speedFactor = c.fullSpeed > 0.0f ? std::min(speed / c.fullSpeed, 1.0f) : 1.0f;
    }
    Inject(s, thickness, in);
}

void InjectImpulse(DisturbanceSlot& s, float thickness, const FieldImpulse& imp)
{
    Injection in;
    std::copy(imp.pos, imp.pos + 3, in.pos);
    in.radius = imp.radius;
    in.mix = 1.0f;
    in.speedFactor = 1.0f;
    in.densityDelta = ClampOr(imp.density, -1.0f, 1.0f, 0.0f);
    in.trailDepth = ClampOr(imp.trail, 0.0f, 1.0f, 0.0f);
    in.ripple = imp.ripple;
    if (imp.shape == kFieldImpulseRadial) {
        in.flow = FlowKind::Radial;
        in.radialSpeed = imp.speed;
    } else {
        in.flowX = imp.dir[0] * imp.speed;
        in.flowZ = imp.dir[2] * imp.speed;
        if (imp.shape == kFieldImpulseCone) {
            in.flow = FlowKind::Cone;
            const float len = std::sqrt(imp.dir[0] * imp.dir[0] + imp.dir[2] * imp.dir[2]);
            if (len > 1.0e-6f) {
                in.coneAxisX = imp.dir[0] / len;
                in.coneAxisZ = imp.dir[2] / len;
            }
            in.coneCos = imp.coneCos;
        }
    }
    Inject(s, thickness, in);
}

// ---- 減衰と復元 ----

// 流れの減衰・濃さと跡の復元・値の範囲の強制 (spec §4.1 B の 5, 7, 9)。各マスが自分のマスだけを書く。
// 非有限の値が出たら静止状態へ戻して true を返す
bool Relax(DisturbanceSlot& s, const FieldParams& p)
{
    const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
    const bool hasTrail = (s.channels & kDisturbanceChannelTrail) != 0;
    const float maxSpeed = s.cellSize * kDisturbanceTickRate;
    const float targetX = 0.0f; // 風の目標速度は sub-04 から
    const float targetZ = 0.0f;
    bool repaired = false;
    const size_t n = s.CellCount();
    for (size_t i = 0; i < n; ++i) {
        float vx = targetX + (s.flowX[i] - targetX) * p.velocityKeep;
        float vz = targetZ + (s.flowZ[i] - targetZ) * p.velocityKeep;
        if (!std::isfinite(vx) || !std::isfinite(vz)) {
            vx = 0.0f;
            vz = 0.0f;
            repaired = true;
        }
        const float len = std::sqrt(vx * vx + vz * vz);
        if (len > maxSpeed) {
            const float k = maxSpeed / len;
            vx *= k;
            vz *= k;
        }
        s.flowX[i] = vx;
        s.flowZ[i] = vz;
        if (hasDensity) {
            float rho = p.restDensity + (s.density[i] - p.restDensity) * p.densityKeep;
            if (!std::isfinite(rho)) {
                rho = p.restDensity;
                repaired = true;
            }
            s.density[i] = Saturate(rho);
        }
        if (hasTrail) {
            float tau = s.trail[i] * p.trailKeep;
            if (!std::isfinite(tau)) {
                tau = 0.0f;
                repaired = true;
            }
            s.trail[i] = Saturate(tau);
        }
    }
    return repaired;
}

// 波紋の 1 段 (spec §4.1 H)。h と hPrev を読み、新しい h を scratch へ書いてから回す
bool StepRipple(DisturbanceSlot& s, const FieldParams& p, std::vector<float>& scratch)
{
    const int32_t nx = s.cellsX;
    const int32_t nz = s.cellsZ;
    const float courant = std::min(p.rippleSpeed / (s.cellSize * kDisturbanceTickRate), kMaxRippleCourant);
    const float c2 = courant * courant;
    scratch.resize(s.CellCount());
    bool repaired = false;
    // 注入で上限を超えた h を先に切る (非有限は 0)
    for (float& v : s.ripple) {
        if (!std::isfinite(v)) {
            v = 0.0f;
            repaired = true;
        }
        v = std::min(std::max(v, -kMaxRippleHeight), kMaxRippleHeight);
    }
    const std::vector<float>& h = s.ripple;
    const std::vector<float>& hp = s.ripplePrev;
    for (int32_t z = 0; z < nz; ++z) {
        for (int32_t x = 0; x < nx; ++x) {
            const size_t i = static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(z);
            const float hc = h[i];
            const float hE = x + 1 < nx ? h[i + 1] : 0.0f;
            const float hW = x > 0 ? h[i - 1] : 0.0f;
            const float hN = z + 1 < nz ? h[i + static_cast<size_t>(nx)] : 0.0f;
            const float hS = z > 0 ? h[i - static_cast<size_t>(nx)] : 0.0f;
            // 加算は (E+W)+(N+S): x と z を入れ替えても結果がビット一致する順序
            const float lap = (hE + hW) + (hN + hS) - 4.0f * hc;
            float next = (2.0f * hc - hp[i] + c2 * lap) * p.rippleKeep;
            if (!std::isfinite(next)) {
                next = 0.0f;
                repaired = true;
            }
            scratch[i] = std::min(std::max(next, -kMaxRippleHeight), kMaxRippleHeight);
        }
    }
    // hPrev <- h、h <- 新しい h、scratch <- 古い hPrev (コピー無しで回す)
    s.ripplePrev.swap(s.ripple);
    s.ripple.swap(scratch);
    return repaired;
}

void WarnOnce(std::vector<EntityID>& warned, uint32_t& counter, EntityID e, const char* why)
{
    if (std::find(warned.begin(), warned.end(), e) != warned.end()) {
        return;
    }
    warned.push_back(e);
    ++counter;
    MYE_LOG_WARN("[disturbance] field entity %u: %s", e.index, why);
}

} // namespace

void DisturbanceStep(World& world, DisturbanceState& state)
{
    std::vector<FieldRow> fieldRows;
    CollectFields(world, fieldRows);
    std::vector<SourceRow> sourceRows;
    CollectSources(world, sourceRows);

    // 1. 掃除と初期化。先頭 4 個だけが有効 (Volume は状態を作らない)
    std::vector<FieldParams> active;
    std::vector<EntityID> present;
    for (size_t i = 0; i < fieldRows.size(); ++i) {
        const FieldRow& r = fieldRows[i];
        present.push_back(r.entity);
        if (i >= kMaxDisturbanceFields) {
            WarnOnce(state.warnedFields_, state.overflowWarnCount_, r.entity,
                     "more than 4 fields exist; this one is not simulated");
            continue;
        }
        const FieldParams p = SanitizeField(r.entity, r.comp, r.pos);
        if (p.shape == kDisturbanceVolume) {
            WarnOnce(state.warnedFields_, state.overflowWarnCount_, r.entity,
                     "volume fields are not simulated yet");
            continue;
        }
        active.push_back(p);
    }
    // 警告済みの記録から、もう無い場を外す (同じ番号の再利用で警告が出なくなるのを防ぐ。世代つきで比べる)
    state.warnedFields_.erase(std::remove_if(state.warnedFields_.begin(), state.warnedFields_.end(),
                                             [&](const EntityID& e) {
                                                 return std::find(present.begin(), present.end(), e) == present.end();
                                             }),
                              state.warnedFields_.end());

    std::vector<DisturbanceSlot> slots;
    slots.reserve(active.size());
    for (const FieldParams& p : active) {
        auto it = std::find_if(state.slots_.begin(), state.slots_.end(),
                               [&](const DisturbanceSlot& s) { return s.entity == p.entity; });
        if (it != state.slots_.end() && SlotMatches(*it, p)) {
            slots.push_back(std::move(*it));
        } else {
            slots.push_back(MakeRestSlot(p));
        }
    }
    state.slots_ = std::move(slots);

    // 3. 源の注入 / 4. 配送待ちの注入 (全て直列。順序が結果に効く)
    std::vector<SourceNow> nows;
    nows.reserve(sourceRows.size());
    for (const SourceRow& r : sourceRows) {
        nows.push_back(EvaluateSource(r));
    }
    for (size_t f = 0; f < state.slots_.size(); ++f) {
        DisturbanceSlot& s = state.slots_[f];
        const float thickness = active[f].thickness;
        for (size_t i = 0; i < sourceRows.size(); ++i) {
            if (nows[i].valid) {
                InjectSource(s, thickness, sourceRows[i], nows[i]);
            }
        }
        for (const FieldImpulse& imp : state.queue_) {
            InjectImpulse(s, thickness, imp);
        }
    }
    // 場が無いシーンで積まれたものもここで捨てる
    state.queue_.clear();
    state.queueFullWarned_ = false;

    // 5〜9. 減衰・復元・波紋・範囲の強制
    for (size_t f = 0; f < state.slots_.size(); ++f) {
        DisturbanceSlot& s = state.slots_[f];
        const FieldParams& p = active[f];
        bool repaired = Relax(s, p);
        if ((s.channels & kDisturbanceChannelRipple) != 0) {
            repaired = StepRipple(s, p, state.rippleScratch_) || repaired;
        }
        if (repaired) {
            MYE_LOG_WARN("[disturbance] field entity %u: a non-finite value was reset to the rest state", s.entity.index);
        }
    }

    // 源の「前の位置」を書く (場が無くても進める: 場が後から現れたときに古い位置との差で速度が跳ねない)
    for (size_t i = 0; i < sourceRows.size(); ++i) {
        auto* c = world.GetComponent<DisturbanceSourceComponent>(sourceRows[i].entity);
        if (c == nullptr) {
            continue;
        }
        if (nows[i].valid) {
            c->prevPos = { nows[i].pos[0], nows[i].pos[1], nows[i].pos[2] };
            c->prevValid = 1;
        } else {
            c->prevValid = 0;
        }
    }

    // 10. 描画用の通番
    ++state.renderSerial_;
}

bool DisturbanceQueueImpulse(Scene& scene, const FieldImpulse& impulse)
{
    return scene.Disturbance().QueueImpulse(impulse);
}

// ---- 読み取り ----

namespace {

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// 双線形。格子の外のマスは outside を返す
float Bilinear(const std::vector<float>& a, int32_t nx, int32_t nz, float fx, float fz, float outside)
{
    const float x0f = std::floor(fx);
    const float z0f = std::floor(fz);
    const float tx = fx - x0f;
    const float tz = fz - z0f;
    const int32_t x0 = static_cast<int32_t>(x0f);
    const int32_t z0 = static_cast<int32_t>(z0f);
    const auto at = [&](int32_t x, int32_t z) {
        if (x < 0 || x >= nx || z < 0 || z >= nz) {
            return outside;
        }
        return a[static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(z)];
    };
    return Lerp(Lerp(at(x0, z0), at(x0 + 1, z0), tx), Lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

} // namespace

bool DisturbanceSampleAt(const DisturbanceState& state, World& world, EntityID field, const DirectX::XMFLOAT3& pos,
                         FieldSampleOut& out)
{
    out = FieldSampleOut{};
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        return false;
    }
    for (const DisturbanceSlot& s : state.Slots()) {
        if (!field.IsNull() && !(s.entity == field)) {
            continue;
        }
        const auto* comp = world.GetComponent<DisturbanceFieldComponent>(s.entity);
        if (comp == nullptr) {
            continue;
        }
        const float thickness = ClampOr(comp->thickness, 0.05f, 20.0f, 1.0f);
        const float rest = ClampOr(comp->restDensity, 0.0f, 1.0f, 1.0f);
        // 格子の中 (マス座標が [0, cells)) かつ層の帯の中
        const float gx = pos.x / s.cellSize - static_cast<float>(s.originCell[0]);
        const float gz = pos.z / s.cellSize - static_cast<float>(s.originCell[2]);
        if (!(gx >= 0.0f && gx < static_cast<float>(s.cellsX) && gz >= 0.0f && gz < static_cast<float>(s.cellsZ))) {
            continue;
        }
        if (!(pos.y >= s.baseY - thickness && pos.y <= s.baseY + 2.0f * thickness)) {
            continue;
        }
        const float fx = gx - 0.5f;
        const float fz = gz - 0.5f;
        out.field = s.entity;
        out.flow[0] = Bilinear(s.flowX, s.cellsX, s.cellsZ, fx, fz, 0.0f);
        out.flow[2] = Bilinear(s.flowZ, s.cellsX, s.cellsZ, fx, fz, 0.0f);
        if ((s.channels & kDisturbanceChannelDensity) != 0) {
            // 高さの形 p(y): 板の下端より下は 1、上へ thickness かけて 0 (描画と同じ式)
            const float shape = pos.y <= s.baseY ? 1.0f : Saturate(1.0f - (pos.y - s.baseY) / thickness);
            out.density = Bilinear(s.density, s.cellsX, s.cellsZ, fx, fz, rest) * shape;
        }
        if ((s.channels & kDisturbanceChannelTrail) != 0) {
            out.trail = Bilinear(s.trail, s.cellsX, s.cellsZ, fx, fz, 0.0f);
        }
        if ((s.channels & kDisturbanceChannelRipple) != 0) {
            out.ripple = Bilinear(s.ripple, s.cellsX, s.cellsZ, fx, fz, 0.0f);
        }
        return true;
    }
    return false;
}

} // namespace mye
