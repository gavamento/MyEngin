//====================================================================================
//                          DisturbanceStep.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の 1 tick の step と読み取り
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceState.h"
#include "Engine/Engine/Disturbance/DisturbanceParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // ResolvePhysicsEnvironment (風)
#include "Engine/Engine/Scene/Scene.h"

// ★決定論 (check_rules 規則 18): このファイルの計算は + - * / sqrt floor ceil min max だけ。
//   exp / pow / log / sin / cos 等の CRT の超越関数は Debug / Release / WARP でビットが揃う保証が無いので使わない

namespace mye {

using namespace disturbance; // DisturbanceParams.h (場の設定の切り方と ECS から集める行。描画側と共有)

namespace {

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
    s.originCell[0] = OriginCellOf(p.pos[0], p.cellSize, p.cellsX);
    // Volume は y も中心に置く (GroundLayer の y は使わない)
    s.originCell[1] = p.shape == kDisturbanceVolume ? OriginCellOf(p.pos[1], p.cellSize, p.cellsY) : 0;
    s.originCell[2] = OriginCellOf(p.pos[2], p.cellSize, p.cellsZ);
    s.baseY = p.pos[1];
    const size_t n = s.CellCount();
    s.flowX.assign(n, 0.0f);
    s.flowZ.assign(n, 0.0f);
    if (p.shape == kDisturbanceVolume) {
        s.flowY.assign(n, 0.0f);
    }
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
    float flowY = 0.0f;       // Volume だけ
    float radialSpeed = 0.0f; // Radial: 外向きの速さ
    float coneAxisX = 0.0f;   // Cone: 軸 (2D は xz、Volume は xyz。正規化済み。長さ 0 なら何も起きない)
    float coneAxisZ = 0.0f;
    float coneAxisY = 0.0f;   // Volume だけ
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

// radialDx / radialDz が非 null なら、流れに足した差分をそこへ加算する (Radial の発散源。投影が引く)
void Inject(DisturbanceSlot& s, float thickness, const Injection& in, std::vector<float>* radialDx = nullptr,
            std::vector<float>* radialDz = nullptr)
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
            const float oldX = s.flowX[i];
            const float oldZ = s.flowZ[i];
            s.flowX[i] += a * (ux - oldX);
            s.flowZ[i] += a * (uz - oldZ);
            if (radialDx != nullptr) {
                (*radialDx)[i] += s.flowX[i] - oldX;
                (*radialDz)[i] += s.flowZ[i] - oldZ;
            }
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

// Inject の 3D 版 (Volume)。距離は 3D のユークリッド距離で、濃さだけを持つ (跡・波紋は無い)。
// radialDx / Dy / Dz が非 null なら流れに足した差分をそこへ加算する (Radial の発散源)
void Inject3D(DisturbanceSlot& s, const Injection& in, std::vector<float>* radialDx, std::vector<float>* radialDy,
              std::vector<float>* radialDz)
{
    if (!(in.radius > 0.0f) || !std::isfinite(in.radius) || !std::isfinite(in.pos[0]) || !std::isfinite(in.pos[1])
        || !std::isfinite(in.pos[2])) {
        return;
    }
    int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
    if (!CellRange(in.pos[0], in.radius, s.cellSize, s.originCell[0], s.cellsX, x0, x1)
        || !CellRange(in.pos[1], in.radius, s.cellSize, s.originCell[1], s.cellsY, y0, y1)
        || !CellRange(in.pos[2], in.radius, s.cellSize, s.originCell[2], s.cellsZ, z0, z1)) {
        return;
    }
    const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
    for (int32_t y = y0; y <= y1; ++y) {
        const float ey = (static_cast<float>(s.originCell[1] + y) + 0.5f) * s.cellSize - in.pos[1];
        for (int32_t z = z0; z <= z1; ++z) {
            const float ez = (static_cast<float>(s.originCell[2] + z) + 0.5f) * s.cellSize - in.pos[2];
            for (int32_t x = x0; x <= x1; ++x) {
                const float ex = (static_cast<float>(s.originCell[0] + x) + 0.5f) * s.cellSize - in.pos[0];
                const float d = std::sqrt(ex * ex + ey * ey + ez * ez);
                if (!(d < in.radius)) {
                    continue;
                }
                const float t = 1.0f - d / in.radius;
                float w = t * t;
                float ux = in.flowX;
                float uy = in.flowY;
                float uz = in.flowZ;
                if (in.flow == FlowKind::Radial) {
                    // 中心 (d < 1e-6) は向きが定まらないので 0
                    ux = d < 1.0e-6f ? 0.0f : ex / d * in.radialSpeed;
                    uy = d < 1.0e-6f ? 0.0f : ey / d * in.radialSpeed;
                    uz = d < 1.0e-6f ? 0.0f : ez / d * in.radialSpeed;
                } else if (in.flow == FlowKind::Cone) {
                    const float axisLen = (in.coneAxisX * in.coneAxisX + in.coneAxisY * in.coneAxisY) + in.coneAxisZ * in.coneAxisZ;
                    float factor = 0.0f;
                    if (axisLen > 0.0f && in.coneCos < 1.0f) {
                        const float cosTheta =
                            d < 1.0e-6f ? 1.0f : ((in.coneAxisX * ex + in.coneAxisY * ey) + in.coneAxisZ * ez) / d;
                        factor = Saturate((cosTheta - in.coneCos) / (1.0f - in.coneCos));
                    }
                    w *= factor;
                }
                const size_t i = static_cast<size_t>(x)
                                 + static_cast<size_t>(s.cellsX) * (static_cast<size_t>(z) + static_cast<size_t>(s.cellsZ) * static_cast<size_t>(y));
                const float a = Saturate(w * in.mix);
                const float oldX = s.flowX[i];
                const float oldY = s.flowY[i];
                const float oldZ = s.flowZ[i];
                s.flowX[i] += a * (ux - oldX);
                s.flowY[i] += a * (uy - oldY);
                s.flowZ[i] += a * (uz - oldZ);
                if (radialDx != nullptr) {
                    (*radialDx)[i] += s.flowX[i] - oldX;
                    (*radialDy)[i] += s.flowY[i] - oldY;
                    (*radialDz)[i] += s.flowZ[i] - oldZ;
                }
                if (hasDensity) {
                    const float delta = w * in.speedFactor * in.densityDelta;
                    const float rho = s.density[i];
                    s.density[i] = delta < 0.0f ? rho + delta * rho : rho + delta * (1.0f - rho);
                }
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
    if (s.shape == kDisturbanceVolume) {
        // Volume は 3D の速さ・向きで注入する (2D の経路は上のまま)
        in.flowY = c.mode == 1 ? n.constant[1] : n.vel[1];
        if (c.mode != 1) {
            const float speed = std::sqrt((n.vel[0] * n.vel[0] + n.vel[1] * n.vel[1]) + n.vel[2] * n.vel[2]);
            in.speedFactor = c.fullSpeed > 0.0f ? std::min(speed / c.fullSpeed, 1.0f) : 1.0f;
        }
        Inject3D(s, in, nullptr, nullptr, nullptr);
        return;
    }
    Inject(s, thickness, in);
}

void InjectImpulse(DisturbanceSlot& s, float thickness, const FieldImpulse& imp, std::vector<float>* radialDx,
                   std::vector<float>* radialDz, std::vector<float>* radialDy)
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
    if (s.shape == kDisturbanceVolume) {
        in.flowY = imp.dir[1] * imp.speed;
        if (imp.shape == kFieldImpulseCone) {
            // 3D の軸 (長さ 0 の向きは何も起きない)
            const float len = std::sqrt((imp.dir[0] * imp.dir[0] + imp.dir[1] * imp.dir[1]) + imp.dir[2] * imp.dir[2]);
            if (len > 1.0e-6f) {
                in.coneAxisX = imp.dir[0] / len;
                in.coneAxisY = imp.dir[1] / len;
                in.coneAxisZ = imp.dir[2] / len;
            }
        }
        // 発散源に積むのは Radial だけ (2D と同じ)
        const bool radial = imp.shape == kFieldImpulseRadial;
        Inject3D(s, in, radial ? radialDx : nullptr, radial ? radialDy : nullptr, radial ? radialDz : nullptr);
        return;
    }
    if (imp.shape == kFieldImpulseRadial) {
        Inject(s, thickness, in, radialDx, radialDz);
    } else {
        Inject(s, thickness, in);
    }
}

// ---- 補間 ----

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// floor の整数版 (CRT の floor を呼ばない)。有限で |f| < 2^31 のとき floor(f) と同じ値
int32_t FloorToInt(float f)
{
    const int32_t i = static_cast<int32_t>(f);
    return i - (f < static_cast<float>(i) ? 1 : 0);
}

// 双線形 (マス座標 fx, fz。マス中心が整数)。格子の外のマスは outside を返す
float Bilinear(const std::vector<float>& a, int32_t nx, int32_t nz, float fx, float fz, float outside)
{
    const int32_t x0 = FloorToInt(fx);
    const int32_t z0 = FloorToInt(fz);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    if (x0 >= 0 && x0 + 1 < nx && z0 >= 0 && z0 + 1 < nz) {
        const float* r0 = a.data() + static_cast<size_t>(x0) + static_cast<size_t>(nx) * static_cast<size_t>(z0);
        const float* r1 = r0 + nx;
        return Lerp(Lerp(r0[0], r0[1], tx), Lerp(r1[0], r1[1], tx), tz);
    }
    const auto at = [&](int32_t x, int32_t z) {
        if (x < 0 || x >= nx || z < 0 || z >= nz) {
            return outside;
        }
        return a[static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(z)];
    };
    return Lerp(Lerp(at(x0, z0), at(x0 + 1, z0), tx), Lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

// 三線形の参照位置 (マス座標 fx, fy, fz。マス中心が整数)。同じ位置で複数の配列を引くとき (流れの 3 成分) に
// floor と重みを 1 回で済ませるため、位置の計算と配列の参照を分けてある
struct TriCoord {
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t z0 = 0;
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    bool interior = false; // 8 隅が全て格子の内側
};

TriCoord MakeTriCoord(int32_t nx, int32_t ny, int32_t nz, float fx, float fy, float fz)
{
    TriCoord c;
    c.x0 = FloorToInt(fx);
    c.y0 = FloorToInt(fy);
    c.z0 = FloorToInt(fz);
    c.tx = fx - static_cast<float>(c.x0);
    c.ty = fy - static_cast<float>(c.y0);
    c.tz = fz - static_cast<float>(c.z0);
    c.interior = c.x0 >= 0 && c.x0 + 1 < nx && c.y0 >= 0 && c.y0 + 1 < ny && c.z0 >= 0 && c.z0 + 1 < nz;
    return c;
}

// a は index = x + nx * (z + nz * y)。格子の外のマスは outside
float TriSample(const float* a, int32_t nx, int32_t ny, int32_t nz, const TriCoord& c, float outside)
{
    const size_t rowStride = static_cast<size_t>(nx);
    const size_t sliceStride = rowStride * static_cast<size_t>(nz);
    if (c.interior) {
        const float* p0 = a + static_cast<size_t>(c.x0) + rowStride * static_cast<size_t>(c.z0)
                          + sliceStride * static_cast<size_t>(c.y0);
        const float* p1 = p0 + sliceStride;
        const float lower = Lerp(Lerp(p0[0], p0[1], c.tx), Lerp(p0[rowStride], p0[rowStride + 1], c.tx), c.tz);
        const float upper = Lerp(Lerp(p1[0], p1[1], c.tx), Lerp(p1[rowStride], p1[rowStride + 1], c.tx), c.tz);
        return Lerp(lower, upper, c.ty);
    }
    const auto at = [&](int32_t x, int32_t y, int32_t z) {
        if (x < 0 || x >= nx || y < 0 || y >= ny || z < 0 || z >= nz) {
            return outside;
        }
        return a[static_cast<size_t>(x) + rowStride * static_cast<size_t>(z) + sliceStride * static_cast<size_t>(y)];
    };
    const float lower = Lerp(Lerp(at(c.x0, c.y0, c.z0), at(c.x0 + 1, c.y0, c.z0), c.tx),
                             Lerp(at(c.x0, c.y0, c.z0 + 1), at(c.x0 + 1, c.y0, c.z0 + 1), c.tx), c.tz);
    const float upper = Lerp(Lerp(at(c.x0, c.y0 + 1, c.z0), at(c.x0 + 1, c.y0 + 1, c.z0), c.tx),
                             Lerp(at(c.x0, c.y0 + 1, c.z0 + 1), at(c.x0 + 1, c.y0 + 1, c.z0 + 1), c.tx), c.tz);
    return Lerp(lower, upper, c.ty);
}

float Trilinear(const std::vector<float>& a, int32_t nx, int32_t ny, int32_t nz, float fx, float fy, float fz,
                float outside)
{
    return TriSample(a.data(), nx, ny, nz, MakeTriCoord(nx, ny, nz, fx, fy, fz), outside);
}

// ---- 流れのソルバ (spec §4.1 B の 5〜7、§4.1 J) ----
// 移流・投影・復元・波紋は「前の配列を読み、自分のマスだけを書く」ダブルバッファにして z 行で割る (ADR-028)。
// マスの結果は分割の仕方に依らず、縮約も無いので jobs あり / なしでビット一致する。注入は直列のまま。
// 3D (sub-07) では y 方向を足す: 隣の参照に ±cellsX*cellsZ を加え、行の単位を y スライスにする

// 1 回の ParallelRanges が受け持つ行数の下限 (これ未満の場は直列と同じ経路になる)
constexpr size_t kRowGrain = 8;

// fn(zBegin, zEnd) を z 行で割って回す。fn は自分の行のマスだけを書くこと
template <typename Fn>
void ForRows(int32_t rows, Fn&& fn)
{
    jobs::System().ParallelRanges(static_cast<size_t>(rows), kRowGrain, [&](size_t begin, size_t end) {
        fn(static_cast<int32_t>(begin), static_cast<int32_t>(end));
    });
}

// 風の目標速度 (xz)。PhysicsEnvironment が無ければ 0。流れは速さの上限を超えられないので、目標も上限で切る
void WindTarget(World& world, const FieldParams& p, float& outX, float& outZ)
{
    outX = 0.0f;
    outZ = 0.0f;
    const PhysicsEnvironmentComponent* env = ResolvePhysicsEnvironment(world);
    if (env == nullptr) {
        return;
    }
    float tx = FiniteOr(env->windVelocity.x, 0.0f) * p.windScale;
    float tz = FiniteOr(env->windVelocity.z, 0.0f) * p.windScale;
    const float maxSpeed = p.cellSize * kDisturbanceTickRate;
    const float len = std::sqrt(tx * tx + tz * tz);
    if (len > maxSpeed) {
        const float k = maxSpeed / len;
        tx *= k;
        tz *= k;
    }
    outX = tx;
    outZ = tz;
}

// 速度の半ラグランジュ移流 → 目標速度への減衰 → 速さの上限 (B の 5)。
// セル中心から -v / 60 戻った点を双線形で引く。格子外は静止状態 (流れは目標速度) = 開いた境界。
// 非有限の値が出たら 0 に戻して true を返す
bool AdvectVelocity(DisturbanceSlot& s, const FieldParams& p, float targetX, float targetZ,
                    DisturbanceState::SolverScratch& sc)
{
    const int32_t nx = s.cellsX;
    const int32_t nz = s.cellsZ;
    const float maxSpeed = s.cellSize * kDisturbanceTickRate;
    // 速度 [m/s] -> 1 tick に戻るマス数
    const float toCells = 1.0f / maxSpeed;
    sc.flowX.resize(s.CellCount());
    sc.flowZ.resize(s.CellCount());
    std::atomic<bool> repaired{ false };
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        bool bad = false;
        for (int32_t z = z0; z < z1; ++z) {
            for (int32_t x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(z);
                const float fx = static_cast<float>(x) - s.flowX[i] * toCells;
                const float fz = static_cast<float>(z) - s.flowZ[i] * toCells;
                float vx = Bilinear(s.flowX, nx, nz, fx, fz, targetX);
                float vz = Bilinear(s.flowZ, nx, nz, fx, fz, targetZ);
                vx = targetX + (vx - targetX) * p.velocityKeep;
                vz = targetZ + (vz - targetZ) * p.velocityKeep;
                if (!std::isfinite(vx) || !std::isfinite(vz)) {
                    vx = 0.0f;
                    vz = 0.0f;
                    bad = true;
                }
                const float len = std::sqrt(vx * vx + vz * vz);
                if (len > maxSpeed) {
                    const float k = maxSpeed / len;
                    vx *= k;
                    vz *= k;
                }
                sc.flowX[i] = vx;
                sc.flowZ[i] = vz;
            }
        }
        if (bad) {
            repaired.store(true, std::memory_order_relaxed);
        }
    });
    s.flowX.swap(sc.flowX);
    s.flowZ.swap(sc.flowZ);
    return repaired.load(std::memory_order_relaxed);
}

// 圧力投影 (B の 6): 発散 → Jacobi → 勾配を引く。発散の格子外は目標速度 (開いた境界)、圧力の格子外は 0 (ディリクレ)。
// 圧力は q = p / cellSize で持つ (単位は速度。マスの大きさが式から消える)。毎 tick 0 から始める作業領域で、状態ではない。
// 行ごとに隣の行のポインタを取り、格子外の行は 0 / 目標速度の行を渡す。x の内側は分岐の無いループにしてベクトル化させる
// (演算の順序は式のとおりなのでビット結果は変わらない)。
// Radial のインパルスが注入した差分 (sc.radialX / Z) は発散源: その発散を右辺から引く (爆発の流れを投影で消さない)。
// 前進 / 後退差分で隣接 5 点に揃える案も試したが、中心差分で測った発散が 20 反復で投影前の 22% 残ったので採らない
void ProjectVelocity(DisturbanceSlot& s, const FieldParams& p, float targetX, float targetZ,
                     DisturbanceState::SolverScratch& sc, size_t slotIndex)
{
    if (p.pressureIterations <= 0) {
        return;
    }
    const int32_t nx = s.cellsX;
    const int32_t nz = s.cellsZ;
    const size_t stride = static_cast<size_t>(nx);
    const size_t n = s.CellCount();
    sc.divergence.resize(n);
    sc.pressureA.assign(n, 0.0f);
    sc.pressureB.resize(n);
    sc.zeroRow.assign(stride, 0.0f);
    sc.targetRowZ.assign(stride, targetZ);
    const bool radial = sc.radialActive[slotIndex];
    const float* radialX = radial ? sc.radialX[slotIndex].data() : nullptr;
    const float* radialZ = radial ? sc.radialZ[slotIndex].data() : nullptr;

    // 発散 (中心差分。単位は速度)。格子外の流れは目標速度
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        for (int32_t z = z0; z < z1; ++z) {
            const size_t row = stride * static_cast<size_t>(z);
            const float* __restrict ux = s.flowX.data() + row;
            const float* __restrict wN = z + 1 < nz ? s.flowZ.data() + row + stride : sc.targetRowZ.data();
            const float* __restrict wS = z > 0 ? s.flowZ.data() + row - stride : sc.targetRowZ.data();
            float* __restrict dv = sc.divergence.data() + row;
            dv[0] = ((ux[1] - targetX) + (wN[0] - wS[0])) * 0.5f;
            for (int32_t x = 1; x < nx - 1; ++x) {
                dv[x] = ((ux[x + 1] - ux[x - 1]) + (wN[x] - wS[x])) * 0.5f;
            }
            dv[nx - 1] = ((targetX - ux[nx - 2]) + (wN[nx - 1] - wS[nx - 1])) * 0.5f;
            if (radial) {
                // 発散源の差分の発散 (格子外の差分は 0)
                const float* rx = radialX + row;
                const float* rzN = z + 1 < nz ? radialZ + row + stride : sc.zeroRow.data();
                const float* rzS = z > 0 ? radialZ + row - stride : sc.zeroRow.data();
                for (int32_t x = 0; x < nx; ++x) {
                    const float rE = x + 1 < nx ? rx[x + 1] : 0.0f;
                    const float rW = x > 0 ? rx[x - 1] : 0.0f;
                    dv[x] -= ((rE - rW) + (rzN[x] - rzS[x])) * 0.5f;
                }
            }
        }
    });

    // Jacobi。発散も勾配も中心差分なので、合成した作用素は 2 マス離れた隣を使う (幅 2 の 5 点)。
    // 隣り合うマスで解く 5 点の式だと、離散化の食い違いで投影後の発散が 2 割ほど残る
    std::vector<float>* src = &sc.pressureA;
    std::vector<float>* dst = &sc.pressureB;
    for (int32_t iter = 0; iter < p.pressureIterations; ++iter) {
        const std::vector<float>& q = *src;
        std::vector<float>& out = *dst;
        ForRows(nz, [&](int32_t z0, int32_t z1) {
            for (int32_t z = z0; z < z1; ++z) {
                const size_t row = stride * static_cast<size_t>(z);
                const float* __restrict mid = q.data() + row;
                const float* __restrict north = z + 2 < nz ? q.data() + row + 2 * stride : sc.zeroRow.data();
                const float* __restrict south = z > 1 ? q.data() + row - 2 * stride : sc.zeroRow.data();
                const float* __restrict dv = sc.divergence.data() + row;
                float* __restrict dstRow = out.data() + row;
                const auto edge = [&](int32_t x) {
                    const float qE = x + 2 < nx ? mid[x + 2] : 0.0f;
                    const float qW = x > 1 ? mid[x - 2] : 0.0f;
                    // 加算は (E+W)+(N+S): x と z を入れ替えても結果がビット一致する順序
                    dstRow[x] = ((qE + qW) + (north[x] + south[x])) * 0.25f - dv[x];
                };
                for (int32_t x = 0; x < std::min(nx, 2); ++x) {
                    edge(x);
                }
                for (int32_t x = std::max(nx - 2, 2); x < nx; ++x) {
                    edge(x);
                }
                for (int32_t x = 2; x < nx - 2; ++x) {
                    dstRow[x] = ((mid[x + 2] + mid[x - 2]) + (north[x] + south[x])) * 0.25f - dv[x];
                }
            }
        });
        std::swap(src, dst);
    }

    // 勾配を引く (中心差分。格子外の圧力は 0)
    const std::vector<float>& q = *src;
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        for (int32_t z = z0; z < z1; ++z) {
            const size_t row = stride * static_cast<size_t>(z);
            const float* __restrict mid = q.data() + row;
            const float* __restrict north = z + 1 < nz ? q.data() + row + stride : sc.zeroRow.data();
            const float* __restrict south = z > 0 ? q.data() + row - stride : sc.zeroRow.data();
            float* __restrict fx = s.flowX.data() + row;
            float* __restrict fz = s.flowZ.data() + row;
            fx[0] -= (mid[1] - 0.0f) * 0.5f;
            fz[0] -= (north[0] - south[0]) * 0.5f;
            for (int32_t x = 1; x < nx - 1; ++x) {
                fx[x] -= (mid[x + 1] - mid[x - 1]) * 0.5f;
                fz[x] -= (north[x] - south[x]) * 0.5f;
            }
            fx[nx - 1] -= (0.0f - mid[nx - 2]) * 0.5f;
            fz[nx - 1] -= (north[nx - 1] - south[nx - 1]) * 0.5f;
        }
    });
}

// 濃さの移流 (B の 7 の前半)。投影後の流れで引く。格子外は restDensity (風上の縁から静止濃度が入る)。
// 跡は移流しない (spec §2 #17)
void AdvectDensity(DisturbanceSlot& s, const FieldParams& p, DisturbanceState::SolverScratch& sc)
{
    const int32_t nx = s.cellsX;
    const int32_t nz = s.cellsZ;
    const float toCells = 1.0f / (s.cellSize * kDisturbanceTickRate);
    sc.density.resize(s.CellCount());
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        for (int32_t z = z0; z < z1; ++z) {
            for (int32_t x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(z);
                const float fx = static_cast<float>(x) - s.flowX[i] * toCells;
                const float fz = static_cast<float>(z) - s.flowZ[i] * toCells;
                sc.density[i] = Bilinear(s.density, nx, nz, fx, fz, p.restDensity);
            }
        }
    });
    s.density.swap(sc.density);
}

// 濃さと跡の復元・値の範囲の強制 (B の 7 の後半と 9)。各マスが自分のマスだけを書く。
// 非有限の値が出たら静止状態へ戻して true を返す
bool RelaxScalars(DisturbanceSlot& s, const FieldParams& p)
{
    const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
    const bool hasTrail = (s.channels & kDisturbanceChannelTrail) != 0;
    if (!hasDensity && !hasTrail) {
        return false;
    }
    const size_t rowCells = static_cast<size_t>(s.cellsX);
    std::atomic<bool> repaired{ false };
    // Volume は (y, z) の行 = cellsZ * cellsY 本 (GroundLayer は cellsY = 1 なので従来と同じ分割)
    ForRows(s.cellsZ * s.cellsY, [&](int32_t z0, int32_t z1) {
        bool bad = false;
        for (size_t i = static_cast<size_t>(z0) * rowCells; i < static_cast<size_t>(z1) * rowCells; ++i) {
            if (hasDensity) {
                float rho = p.restDensity + (s.density[i] - p.restDensity) * p.densityKeep;
                if (!std::isfinite(rho)) {
                    rho = p.restDensity;
                    bad = true;
                }
                s.density[i] = Saturate(rho);
            }
            if (hasTrail) {
                float tau = s.trail[i] * p.trailKeep;
                if (!std::isfinite(tau)) {
                    tau = 0.0f;
                    bad = true;
                }
                s.trail[i] = Saturate(tau);
            }
        }
        if (bad) {
            repaired.store(true, std::memory_order_relaxed);
        }
    });
    return repaired.load(std::memory_order_relaxed);
}

// 波紋の 1 段 (spec §4.1 H)。h と hPrev を読み、新しい h を scratch へ書いてから回す
bool StepRipple(DisturbanceSlot& s, const FieldParams& p, std::vector<float>& scratch)
{
    const int32_t nx = s.cellsX;
    const int32_t nz = s.cellsZ;
    const float courant = std::min(p.rippleSpeed / (s.cellSize * kDisturbanceTickRate), kMaxRippleCourant);
    const float c2 = courant * courant;
    scratch.resize(s.CellCount());
    std::atomic<bool> repaired{ false };
    // 注入で上限を超えた h を先に切る (非有限は 0)。隣を読む本体の前に全マス終える
    const size_t rowCells = static_cast<size_t>(nx);
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        bool bad = false;
        for (size_t i = static_cast<size_t>(z0) * rowCells; i < static_cast<size_t>(z1) * rowCells; ++i) {
            float v = s.ripple[i];
            if (!std::isfinite(v)) {
                v = 0.0f;
                bad = true;
            }
            s.ripple[i] = std::min(std::max(v, -kMaxRippleHeight), kMaxRippleHeight);
        }
        if (bad) {
            repaired.store(true, std::memory_order_relaxed);
        }
    });
    const std::vector<float>& h = s.ripple;
    const std::vector<float>& hp = s.ripplePrev;
    ForRows(nz, [&](int32_t z0, int32_t z1) {
        bool bad = false;
        for (int32_t z = z0; z < z1; ++z) {
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
                    bad = true;
                }
                scratch[i] = std::min(std::max(next, -kMaxRippleHeight), kMaxRippleHeight);
            }
        }
        if (bad) {
            repaired.store(true, std::memory_order_relaxed);
        }
    });
    // hPrev <- h、h <- 新しい h、scratch <- 古い hPrev (コピー無しで回す)
    s.ripplePrev.swap(s.ripple);
    s.ripple.swap(scratch);
    return repaired.load(std::memory_order_relaxed);
}

// ---- Volume (3D) のソルバ ----
// 2D の関数と同じ式を y 方向込みに広げたもの。2D の関数には手を入れない (2D のハッシュ列を 1 ビットも変えないため)。
// 並列の単位は (y, z) の行 r = y * cellsZ + z で、マスの添字は x + cellsX * r。行ごとに自分のマスだけを書く

// 風の目標速度 (xyz)。長さの上限は 2D と同じ cellSize * 60
void WindTarget3D(World& world, const FieldParams& p, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    const PhysicsEnvironmentComponent* env = ResolvePhysicsEnvironment(world);
    if (env == nullptr) {
        return;
    }
    float t[3] = { FiniteOr(env->windVelocity.x, 0.0f) * p.windScale, FiniteOr(env->windVelocity.y, 0.0f) * p.windScale,
                   FiniteOr(env->windVelocity.z, 0.0f) * p.windScale };
    const float maxSpeed = p.cellSize * kDisturbanceTickRate;
    const float len = std::sqrt((t[0] * t[0] + t[1] * t[1]) + t[2] * t[2]);
    if (len > maxSpeed) {
        const float k = maxSpeed / len;
        t[0] *= k;
        t[1] *= k;
        t[2] *= k;
    }
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
}

// 速度の半ラグランジュ移流 → 目標速度への減衰 → 浮力 → 速さの上限。非有限の値が出たら 0 に戻して true を返す。
// 浮力: vy += buoyancy * (ρ - restDensity) / 60 (濃い煙が昇る。濃さチャネルが無い場では 0)
bool AdvectVelocity3D(DisturbanceSlot& s, const FieldParams& p, const float target[3], DisturbanceState::SolverScratch& sc)
{
    const int32_t nx = s.cellsX;
    const int32_t ny = s.cellsY;
    const int32_t nz = s.cellsZ;
    const float maxSpeed = s.cellSize * kDisturbanceTickRate;
    const float toCells = 1.0f / maxSpeed;
    const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
    const float lift = hasDensity ? p.buoyancy / kDisturbanceTickRate : 0.0f;
    sc.flowX.resize(s.CellCount());
    sc.flowY.resize(s.CellCount());
    sc.flowZ.resize(s.CellCount());
    std::atomic<bool> repaired{ false };
    ForRows(ny * nz, [&](int32_t r0, int32_t r1) {
        bool bad = false;
        for (int32_t r = r0; r < r1; ++r) {
            const int32_t y = r / nz;
            const int32_t z = r - y * nz;
            for (int32_t x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(r);
                const float fx = static_cast<float>(x) - s.flowX[i] * toCells;
                const float fy = static_cast<float>(y) - s.flowY[i] * toCells;
                const float fz = static_cast<float>(z) - s.flowZ[i] * toCells;
                const TriCoord coord = MakeTriCoord(nx, ny, nz, fx, fy, fz);
                float vx = TriSample(s.flowX.data(), nx, ny, nz, coord, target[0]);
                float vy = TriSample(s.flowY.data(), nx, ny, nz, coord, target[1]);
                float vz = TriSample(s.flowZ.data(), nx, ny, nz, coord, target[2]);
                vx = target[0] + (vx - target[0]) * p.velocityKeep;
                vy = target[1] + (vy - target[1]) * p.velocityKeep;
                vz = target[2] + (vz - target[2]) * p.velocityKeep;
                if (hasDensity) {
                    vy += lift * (s.density[i] - p.restDensity);
                }
                if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
                    vx = 0.0f;
                    vy = 0.0f;
                    vz = 0.0f;
                    bad = true;
                }
                const float len = std::sqrt((vx * vx + vy * vy) + vz * vz);
                if (len > maxSpeed) {
                    const float k = maxSpeed / len;
                    vx *= k;
                    vy *= k;
                    vz *= k;
                }
                sc.flowX[i] = vx;
                sc.flowY[i] = vy;
                sc.flowZ[i] = vz;
            }
        }
        if (bad) {
            repaired.store(true, std::memory_order_relaxed);
        }
    });
    s.flowX.swap(sc.flowX);
    s.flowY.swap(sc.flowY);
    s.flowZ.swap(sc.flowZ);
    return repaired.load(std::memory_order_relaxed);
}

// 圧力投影の 3D 版。発散・勾配は中心差分、Jacobi は幅 2 の 7 点 (q = (Σ6 近傍 - 4 * 発散) / 6)。
// 圧力は q = p / cellSize で持つ作業領域 (状態ではない)。格子外は発散が目標速度、圧力が 0。Radial の差分は発散源として右辺から引く
void ProjectVelocity3D(DisturbanceSlot& s, const FieldParams& p, const float target[3],
                       DisturbanceState::SolverScratch& sc, size_t slotIndex)
{
    if (p.pressureIterations <= 0) {
        return;
    }
    const int32_t nx = s.cellsX;
    const int32_t ny = s.cellsY;
    const int32_t nz = s.cellsZ;
    const size_t stride = static_cast<size_t>(nx);
    const size_t slice = stride * static_cast<size_t>(nz);
    const size_t n = s.CellCount();
    sc.divergence.resize(n);
    sc.pressureA.assign(n, 0.0f);
    sc.pressureB.resize(n);
    sc.zeroRow.assign(stride, 0.0f);
    sc.targetRowZ.assign(stride, target[2]);
    sc.targetRowY.assign(stride, target[1]);
    const bool radial = sc.radialActive[slotIndex];
    const float* radialX = radial ? sc.radialX[slotIndex].data() : nullptr;
    const float* radialY = radial ? sc.radialY[slotIndex].data() : nullptr;
    const float* radialZ = radial ? sc.radialZ[slotIndex].data() : nullptr;

    ForRows(ny * nz, [&](int32_t r0, int32_t r1) {
        for (int32_t r = r0; r < r1; ++r) {
            const int32_t y = r / nz;
            const int32_t z = r - y * nz;
            const size_t row = stride * static_cast<size_t>(r);
            const float* __restrict ux = s.flowX.data() + row;
            const float* __restrict wN = z + 1 < nz ? s.flowZ.data() + row + stride : sc.targetRowZ.data();
            const float* __restrict wS = z > 0 ? s.flowZ.data() + row - stride : sc.targetRowZ.data();
            const float* __restrict vU = y + 1 < ny ? s.flowY.data() + row + slice : sc.targetRowY.data();
            const float* __restrict vD = y > 0 ? s.flowY.data() + row - slice : sc.targetRowY.data();
            float* __restrict dv = sc.divergence.data() + row;
            dv[0] = (((ux[1] - target[0]) + (wN[0] - wS[0])) + (vU[0] - vD[0])) * 0.5f;
            for (int32_t x = 1; x < nx - 1; ++x) {
                dv[x] = (((ux[x + 1] - ux[x - 1]) + (wN[x] - wS[x])) + (vU[x] - vD[x])) * 0.5f;
            }
            dv[nx - 1] = (((target[0] - ux[nx - 2]) + (wN[nx - 1] - wS[nx - 1])) + (vU[nx - 1] - vD[nx - 1])) * 0.5f;
            if (radial) {
                // 発散源の差分の発散 (格子外の差分は 0)
                const float* rx = radialX + row;
                const float* rzN = z + 1 < nz ? radialZ + row + stride : sc.zeroRow.data();
                const float* rzS = z > 0 ? radialZ + row - stride : sc.zeroRow.data();
                const float* ryU = y + 1 < ny ? radialY + row + slice : sc.zeroRow.data();
                const float* ryD = y > 0 ? radialY + row - slice : sc.zeroRow.data();
                for (int32_t x = 0; x < nx; ++x) {
                    const float rE = x + 1 < nx ? rx[x + 1] : 0.0f;
                    const float rW = x > 0 ? rx[x - 1] : 0.0f;
                    dv[x] -= (((rE - rW) + (rzN[x] - rzS[x])) + (ryU[x] - ryD[x])) * 0.5f;
                }
            }
        }
    });

    std::vector<float>* src = &sc.pressureA;
    std::vector<float>* dst = &sc.pressureB;
    for (int32_t iter = 0; iter < p.pressureIterations; ++iter) {
        const std::vector<float>& q = *src;
        std::vector<float>& out = *dst;
        ForRows(ny * nz, [&](int32_t r0, int32_t r1) {
            for (int32_t r = r0; r < r1; ++r) {
                const int32_t y = r / nz;
                const int32_t z = r - y * nz;
                const size_t row = stride * static_cast<size_t>(r);
                const float* __restrict mid = q.data() + row;
                const float* __restrict north = z + 2 < nz ? q.data() + row + 2 * stride : sc.zeroRow.data();
                const float* __restrict south = z > 1 ? q.data() + row - 2 * stride : sc.zeroRow.data();
                const float* __restrict up = y + 2 < ny ? q.data() + row + 2 * slice : sc.zeroRow.data();
                const float* __restrict down = y > 1 ? q.data() + row - 2 * slice : sc.zeroRow.data();
                const float* __restrict dv = sc.divergence.data() + row;
                float* __restrict dstRow = out.data() + row;
                const auto edge = [&](int32_t x) {
                    const float qE = x + 2 < nx ? mid[x + 2] : 0.0f;
                    const float qW = x > 1 ? mid[x - 2] : 0.0f;
                    dstRow[x] = ((((qE + qW) + (north[x] + south[x])) + (up[x] + down[x])) - 4.0f * dv[x]) / 6.0f;
                };
                for (int32_t x = 0; x < std::min(nx, 2); ++x) {
                    edge(x);
                }
                for (int32_t x = std::max(nx - 2, 2); x < nx; ++x) {
                    edge(x);
                }
                for (int32_t x = 2; x < nx - 2; ++x) {
                    dstRow[x] = ((((mid[x + 2] + mid[x - 2]) + (north[x] + south[x])) + (up[x] + down[x])) - 4.0f * dv[x]) / 6.0f;
                }
            }
        });
        std::swap(src, dst);
    }

    // 勾配を引く (中心差分。格子外の圧力は 0)
    const std::vector<float>& q = *src;
    ForRows(ny * nz, [&](int32_t r0, int32_t r1) {
        for (int32_t r = r0; r < r1; ++r) {
            const int32_t y = r / nz;
            const int32_t z = r - y * nz;
            const size_t row = stride * static_cast<size_t>(r);
            const float* __restrict mid = q.data() + row;
            const float* __restrict north = z + 1 < nz ? q.data() + row + stride : sc.zeroRow.data();
            const float* __restrict south = z > 0 ? q.data() + row - stride : sc.zeroRow.data();
            const float* __restrict up = y + 1 < ny ? q.data() + row + slice : sc.zeroRow.data();
            const float* __restrict down = y > 0 ? q.data() + row - slice : sc.zeroRow.data();
            float* __restrict fx = s.flowX.data() + row;
            float* __restrict fy = s.flowY.data() + row;
            float* __restrict fz = s.flowZ.data() + row;
            fx[0] -= (mid[1] - 0.0f) * 0.5f;
            fy[0] -= (up[0] - down[0]) * 0.5f;
            fz[0] -= (north[0] - south[0]) * 0.5f;
            for (int32_t x = 1; x < nx - 1; ++x) {
                fx[x] -= (mid[x + 1] - mid[x - 1]) * 0.5f;
                fy[x] -= (up[x] - down[x]) * 0.5f;
                fz[x] -= (north[x] - south[x]) * 0.5f;
            }
            fx[nx - 1] -= (0.0f - mid[nx - 2]) * 0.5f;
            fy[nx - 1] -= (up[nx - 1] - down[nx - 1]) * 0.5f;
            fz[nx - 1] -= (north[nx - 1] - south[nx - 1]) * 0.5f;
        }
    });
}

// 濃さの移流の 3D 版。格子外は restDensity (開いた境界)
void AdvectDensity3D(DisturbanceSlot& s, const FieldParams& p, DisturbanceState::SolverScratch& sc)
{
    const int32_t nx = s.cellsX;
    const int32_t ny = s.cellsY;
    const int32_t nz = s.cellsZ;
    const float toCells = 1.0f / (s.cellSize * kDisturbanceTickRate);
    sc.density.resize(s.CellCount());
    ForRows(ny * nz, [&](int32_t r0, int32_t r1) {
        for (int32_t r = r0; r < r1; ++r) {
            const int32_t y = r / nz;
            const int32_t z = r - y * nz;
            for (int32_t x = 0; x < nx; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(r);
                const float fx = static_cast<float>(x) - s.flowX[i] * toCells;
                const float fy = static_cast<float>(y) - s.flowY[i] * toCells;
                const float fz = static_cast<float>(z) - s.flowZ[i] * toCells;
                sc.density[i] = Trilinear(s.density, nx, ny, nz, fx, fy, fz, p.restDensity);
            }
        }
    });
    s.density.swap(sc.density);
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

    // 1. 掃除と初期化。先頭 4 個だけが有効
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
        active.push_back(SanitizeField(r.entity, r.comp, r.pos));
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
        // Radial があれば差分の置き場を 0 で作る (無い場は触らない)
        DisturbanceState::SolverScratch& sc = state.solver_;
        sc.radialActive[f] = false;
        for (const FieldImpulse& imp : state.queue_) {
            if (imp.shape == kFieldImpulseRadial && !sc.radialActive[f]) {
                sc.radialActive[f] = true;
                sc.radialX[f].assign(s.CellCount(), 0.0f);
                sc.radialZ[f].assign(s.CellCount(), 0.0f);
                if (s.shape == kDisturbanceVolume) {
                    sc.radialY[f].assign(s.CellCount(), 0.0f);
                }
            }
            InjectImpulse(s, thickness, imp, sc.radialActive[f] ? &sc.radialX[f] : nullptr,
                          sc.radialActive[f] ? &sc.radialZ[f] : nullptr,
                          sc.radialActive[f] ? &sc.radialY[f] : nullptr);
        }
    }
    // 場が無いシーンで積まれたものもここで捨てる
    state.queue_.clear();
    state.queueFullWarned_ = false;

    // 5〜9. 流れ (移流 → 減衰 → 上限) → 投影 → 濃さの移流 → 復元 → 波紋 → 範囲の強制
    for (size_t f = 0; f < state.slots_.size(); ++f) {
        DisturbanceSlot& s = state.slots_[f];
        const FieldParams& p = active[f];
        if (s.shape == kDisturbanceVolume) {
            // 3D: 風 (xyz) → 移流・減衰・浮力 → 投影 → 濃さの移流 → 復元。跡・波紋は持たない
            float target[3];
            WindTarget3D(world, p, target);
            bool repaired3 = AdvectVelocity3D(s, p, target, state.solver_);
            ProjectVelocity3D(s, p, target, state.solver_, f);
            if ((s.channels & kDisturbanceChannelDensity) != 0) {
                AdvectDensity3D(s, p, state.solver_);
            }
            repaired3 = RelaxScalars(s, p) || repaired3;
            if (repaired3) {
                MYE_LOG_WARN("[disturbance] field entity %u: a non-finite value was reset to the rest state", s.entity.index);
            }
            continue;
        }
        float targetX = 0.0f;
        float targetZ = 0.0f;
        WindTarget(world, p, targetX, targetZ);
        bool repaired = AdvectVelocity(s, p, targetX, targetZ, state.solver_);
        ProjectVelocity(s, p, targetX, targetZ, state.solver_, f);
        if ((s.channels & kDisturbanceChannelDensity) != 0) {
            AdvectDensity(s, p, state.solver_);
        }
        repaired = RelaxScalars(s, p) || repaired;
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
        if (s.shape == kDisturbanceVolume) {
            // 3D: 格子の中 (3 軸とも [0, cells)) の三線形。高さの形 p(y) は無く、濃さはそのまま
            const float gx = pos.x / s.cellSize - static_cast<float>(s.originCell[0]);
            const float gy = pos.y / s.cellSize - static_cast<float>(s.originCell[1]);
            const float gz = pos.z / s.cellSize - static_cast<float>(s.originCell[2]);
            if (!(gx >= 0.0f && gx < static_cast<float>(s.cellsX) && gy >= 0.0f && gy < static_cast<float>(s.cellsY)
                  && gz >= 0.0f && gz < static_cast<float>(s.cellsZ))) {
                continue;
            }
            const float fx = gx - 0.5f;
            const float fy = gy - 0.5f;
            const float fz = gz - 0.5f;
            out.field = s.entity;
            out.flow[0] = Trilinear(s.flowX, s.cellsX, s.cellsY, s.cellsZ, fx, fy, fz, 0.0f);
            out.flow[1] = Trilinear(s.flowY, s.cellsX, s.cellsY, s.cellsZ, fx, fy, fz, 0.0f);
            out.flow[2] = Trilinear(s.flowZ, s.cellsX, s.cellsY, s.cellsZ, fx, fy, fz, 0.0f);
            if ((s.channels & kDisturbanceChannelDensity) != 0) {
                out.density = Trilinear(s.density, s.cellsX, s.cellsY, s.cellsZ, fx, fy, fz, rest);
            }
            return true;
        }
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
