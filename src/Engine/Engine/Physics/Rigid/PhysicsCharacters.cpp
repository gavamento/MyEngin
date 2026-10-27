// キャラクターコントローラ (M29b) の解決。剛体ソルバ (PhysicsSystem::Update) が剛体を解き終えた最終 pose を
// 障害物として CC のカプセルを押し出す。CharBody と SolveCharacters の契約は PhysicsSystemInternal.h。
// 決定論契約は PhysicsSystem.cpp と同じ (scalar float のみ、固定パス数、entity.index 昇順)
#include "Engine/Engine/Physics/Rigid/PhysicsSystemInternal.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {

namespace {

constexpr int kCharPushPasses = 4; // 固定パス数 (収束早期終了はしない = 決定論)

// 段差登り (M82d) の定数
constexpr float kStepSkin = 0.002f;     // 探索用カプセルの半径の縮み。接地直後の 0 深度接触を重なりと誤判定しない
constexpr float kStepLandProbe = 0.005f; // 着地面の法線を測るために探索姿勢から押し込む量
constexpr float kStepHeightTol = 0.005f; // 段差の上面が stepOffset ちょうどのとき浮動小数点誤差で落とさない余裕
constexpr float kStepMinRise = 0.02f;    // これ以下の凹凸は通常の押し出しに任せる
constexpr int kStepAdvanceTries = 5;     // 遮られた歩幅を超えて前へ出す試行回数 (半径の 1/4 刻み)
constexpr int kStepBisectIters = 12;     // 着地高さの二分探索の回数 (固定 = 決定論)

// 段差登り。接地中に水平移動 (dirX,dirZ は単位ベクトル、len は今 tick の歩幅) が遮られたときだけ呼ぶ。
// 「stepHeight だけ持ち上げる → 前へ出す → 真下へ着地点を探す」を、前へ出す量を歩幅から半径まで
// 増やしながら試す。足を載せるのに必要なぶんだけ前へ出る = 低速でも 1 tick で登れる。
// 着地面の法線が登れる傾斜 (cosSlope 以上) で、接触点の高さが足元から stepHeight 以内のときだけ成功。
// 戻り値 true なら (outX,outY,outZ) が登った後のカプセル中心
bool TryCharStepUp(const std::vector<Body>& bodies, const CharBody& c, float cosSlope, float dirX,
                   float dirZ, float len, float& outX, float& outY, float& outZ)
{
    auto overlapsAny = [&](const ShapePose& p) {
        for (const Body& obs : bodies) {
            if (!obs.solid || obs.entity.index == c.entity.index) {
                continue;
            }
            if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                continue;
            }
            if (shapes::Overlap(p, obs.pose)) {
                return true;
            }
        }
        return false;
    };
    ShapePose p;
    p.shape = collidershape::kCapsule;
    p.identityRot = 1;
    p.radius = c.radius - kStepSkin;
    p.halfSeg = c.halfSeg;
    const float feetY = c.py - c.halfSeg - c.radius;
    const float raisedY = c.py + c.stepHeight;
    // 持ち上げた位置が天井に塞がれていれば登れない
    p.px = c.px;
    p.py = raisedY;
    p.pz = c.pz;
    if (overlapsAny(p)) {
        return false;
    }
    for (int k = 0; k < kStepAdvanceTries; ++k) {
        const float adv = len + static_cast<float>(k) * (c.radius * 0.25f);
        p.px = c.px + dirX * adv;
        p.pz = c.pz + dirZ * adv;
        p.py = raisedY;
        if (overlapsAny(p)) {
            break; // 持ち上げた高さでも壁がある。これ以上前へ出すと薄い壁を抜けるので打ち切る
        }
        // 真下へ着地点を探す。下端は足元より少し下 (床があれば必ず重なる) に置く
        float lo = c.py - 0.01f;
        float hi = raisedY;
        p.py = lo;
        if (!overlapsAny(p)) {
            continue; // 足場なし
        }
        for (int i = 0; i < kStepBisectIters; ++i) {
            const float mid = 0.5f * (lo + hi);
            p.py = mid;
            if (overlapsAny(p)) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        // 着地面: 少し押し込んだ姿勢 (本来の半径) で重なる障害物のうち最も上向きの法線を採る
        ShapePose q = p;
        q.radius = c.radius;
        q.py = hi - kStepLandProbe;
        const Body* support = nullptr;
        float supportNy = -2.0f;
        for (const Body& obs : bodies) {
            if (!obs.solid || obs.entity.index == c.entity.index) {
                continue;
            }
            if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                continue;
            }
            float nx, ny, nz, depth;
            if (shapes::Collide(q, obs.pose, nx, ny, nz, depth) && ny > supportNy) {
                supportNy = ny;
                support = &obs;
            }
        }
        if (support == nullptr || supportNy < cosSlope) {
            continue;
        }
        float qx, qy, qz;
        shapes::ClosestPointOnShape(support->pose, p.px, hi - c.halfSeg, p.pz, qx, qy, qz);
        const float riseOfSurface = qy - feetY;
        if (riseOfSurface <= kStepMinRise || riseOfSurface > c.stepHeight + kStepHeightTol) {
            continue;
        }
        outX = p.px;
        outY = hi;
        outZ = p.pz;
        return true;
    }
    return false;
}

} // namespace

// move-then-depenetrate: 変位を一括適用してから固定パス × 障害物 index 昇順で押し出す。
// スイープしないため 1 tick で薄い壁を大きく越える速度 (v·dt > 壁厚) では貫通し得るが、
// 60Hz × 常識的な移動速度では起きない (v=30m/s でも 0.5m/tick)。剛体ソルバの後に走るので
// 障害物 (静的 + 解決後の動的剛体) の pose は当該 tick の最終位置。CC が剛体を押す応答は
// 無し (必要ならソリッド capsule Collider を併用し OnCollision + AddForce で組む)。
// outContacts (M93e、dogfooding #17): 非 null なら、押し出しで触れた剛体 (obs.rb != nullptr) との接触を積む。
// 静的コライダー (床・壁) との接触は積まない — 接地中は毎 tick 全キャラ分の Stay が出て、OnCollision* を
// 持つスクリプトの負荷になるだけで、使い道 (轢く・押される) は剛体側にしか無い
void SolveCharacters(std::vector<Body>& bodies, std::vector<CharBody>& chars, float dt,
                     std::vector<SolidContact>* outContacts)
{
    if (chars.empty()) {
        return;
    }
    std::sort(chars.begin(), chars.end(),
              [](const CharBody& a, const CharBody& b) { return a.entity.index < b.entity.index; });
    for (CharBody& c : chars) {
        constexpr float kDeg2Rad = 3.14159265f / 180.0f;
        const float cosSlope = std::cos(c.cc->slopeLimitDeg * kDeg2Rad);
        float vy = c.cc->velocity.y + kGravity * c.cc->gravityScale * dt;
        // 変位一括適用 (水平は moveInput 直接駆動、垂直は重力積分)
        ShapePose pose;
        pose.shape = collidershape::kCapsule; // 常にワールド Y 軸 = 単位基底
        pose.identityRot = 1;
        pose.radius = c.radius;
        pose.halfSeg = c.halfSeg;
        pose.px = c.px + c.cc->moveInput.x * dt;
        pose.py = c.py + vy * dt;
        pose.pz = c.pz + c.cc->moveInput.z * dt;
        bool grounded = false;
        // 押し出し: 固定パス × 障害物 index 昇順 (bodies はソート済)。自分の collider は skip
        for (int pass = 0; pass < kCharPushPasses; ++pass) {
            for (const Body& obs : bodies) {
                if (!obs.solid || obs.entity.index == c.entity.index) {
                    continue;
                }
                if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                    continue; // M36a
                }
                float nx, ny, nz, depth;
                if (!shapes::Collide(pose, obs.pose, nx, ny, nz, depth)) {
                    continue;
                }
                if (outContacts != nullptr && obs.rb != nullptr) {
                    // M93e (dogfooding #17): 剛体に触れたことを接触列へ (運動量の交換はしない)。
                    // key と法線は SolidContact の規約 (小 index << 32 | 大 index、法線は大 → 小) に揃える。
                    // Collide の法線は障害物 → キャラ (キャラを押し出す向き)
                    const bool charIsSmall = c.entity.index < obs.entity.index;
                    SolidContact sc;
                    sc.key = charIsSmall
                        ? ((static_cast<uint64_t>(c.entity.index) << 32) | obs.entity.index)
                        : ((static_cast<uint64_t>(obs.entity.index) << 32) | c.entity.index);
                    const float sign = charIsSmall ? 1.0f : -1.0f;
                    sc.nx = nx * sign;
                    sc.ny = ny * sign;
                    sc.nz = nz * sign;
                    // 代表点はカプセル表面で最も深く食い込んだ点の近似 (中心から法線の逆へ半径ぶん)
                    sc.px = pose.px - nx * c.radius;
                    sc.py = pose.py - ny * c.radius;
                    sc.pz = pose.pz - nz * c.radius;
                    sc.impulse = 0.0f; // キャラは運動量を持たない (通知だけ)
                    outContacts->push_back(sc);
                }
                pose.px += nx * depth;
                pose.py += ny * depth;
                pose.pz += nz * depth;
                if (ny >= cosSlope) {
                    grounded = true; // 登れる斜面 = 接地
                    if (vy < 0.0f) {
                        vy = 0.0f;
                    }
                } else if (ny <= -cosSlope) {
                    if (vy > 0.0f) {
                        vy = 0.0f; // 天井
                    }
                } // 壁/急斜面は押し出しのみ (滑り落ちは重力が担う)
            }
        }
        // 接地プローブ (状態は変更しない): skinWidth+0.01 下げて接地面を探る
        if (!grounded) {
            ShapePose probe = pose;
            probe.py -= (c.cc->skinWidth + 0.01f);
            for (const Body& obs : bodies) {
                if (!obs.solid || obs.entity.index == c.entity.index) {
                    continue;
                }
                if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                    continue; // M36a
                }
                float nx, ny, nz, depth;
                if (shapes::Collide(probe, obs.pose, nx, ny, nz, depth) && ny >= cosSlope) {
                    grounded = true;
                    break; // 発見のみが目的 (状態を変えないので早期終了しても決定論)
                }
            }
        }
        // 段差登り (M82d): 前 tick に接地していて、水平移動が半分以上遮られたときだけ試す。
        // stepOffset 0 のキャラ・ジャンプ tick・遮られていない tick は上の経路の結果をそのまま使う
        float stepSpeedCap = -1.0f; // 段差を登った tick の水平 velocity の上限 (m/s)。負 = 登っていない
        if (c.stepHeight > 0.0f && c.cc->isGrounded && c.cc->jumpSpeed <= 0.0f) {
            const float mx = c.cc->moveInput.x;
            const float mz = c.cc->moveInput.z;
            const float speed = std::sqrt(mx * mx + mz * mz);
            if (speed > 0.0f) {
                const float dirX = mx / speed;
                const float dirZ = mz / speed;
                const float len = speed * dt;
                const float progress = (pose.px - c.px) * dirX + (pose.pz - c.pz) * dirZ;
                float sx, sy, sz;
                if (progress < 0.5f * len
                    && TryCharStepUp(bodies, c, cosSlope, dirX, dirZ, len, sx, sy, sz)) {
                    pose.px = sx;
                    pose.py = sy;
                    pose.pz = sz;
                    grounded = true;
                    vy = 0.0f;
                    stepSpeedCap = speed;
                }
            }
        }
        // ジャンプ: 接地時のみ発火。接地可否に関わらず消費 (バッファリング無し = 予測可能)
        if (c.cc->jumpSpeed > 0.0f) {
            if (grounded) {
                vy = c.cc->jumpSpeed;
            }
            c.cc->jumpSpeed = 0.0f;
        }
        // 書き戻し (剛体と同じ親フレーム逆変換。回転は触らない)
        const float invDt = 1.0f / dt;
        c.cc->velocity.x = (pose.px - c.px) * invDt;
        c.cc->velocity.y = vy;
        c.cc->velocity.z = (pose.pz - c.pz) * invDt;
        if (stepSpeedCap >= 0.0f) {
            // 登りのために前へ出した分は移動速度に数えない (足音の歩幅やアニメーションが velocity を読む)
            const float horizontal = std::sqrt(c.cc->velocity.x * c.cc->velocity.x + c.cc->velocity.z * c.cc->velocity.z);
            if (horizontal > stepSpeedCap && horizontal > 0.0f) {
                const float k = stepSpeedCap / horizontal;
                c.cc->velocity.x *= k;
                c.cc->velocity.z *= k;
            }
        }
        c.cc->isGrounded = grounded;
        if (c.frame.identity) {
            c.lt->position = { pose.px, pose.py, pose.pz };
        } else {
            const float cqx = -c.frame.qx, cqy = -c.frame.qy, cqz = -c.frame.qz,
                        cqw = c.frame.qw;
            float lx, ly, lz;
            QuatRotate(cqx, cqy, cqz, cqw, pose.px - c.frame.px, pose.py - c.frame.py,
                       pose.pz - c.frame.pz, lx, ly, lz);
            const float isx = (std::fabs(c.frame.sx) > 1e-8f) ? 1.0f / c.frame.sx : 1.0f;
            const float isy = (std::fabs(c.frame.sy) > 1e-8f) ? 1.0f / c.frame.sy : 1.0f;
            const float isz = (std::fabs(c.frame.sz) > 1e-8f) ? 1.0f / c.frame.sz : 1.0f;
            c.lt->position = { lx * isx, ly * isy, lz * isz };
        }
    }
    if (outContacts != nullptr && !outContacts->empty()) {
        // 同じペアが複数パスで重複する。key 昇順にして先頭だけ残す (CollisionSystem の二分探索の前提)。
        // 入力の順 (キャラ index 昇順 → パス → 障害物 index 昇順) は決定的なので、安定ソートで
        // 「どの重複が残るか」まで決定的にする
        std::stable_sort(outContacts->begin(), outContacts->end(),
                         [](const SolidContact& a, const SolidContact& b) { return a.key < b.key; });
        outContacts->erase(std::unique(outContacts->begin(), outContacts->end(),
                                       [](const SolidContact& a, const SolidContact& b) { return a.key == b.key; }),
                           outContacts->end());
    }
}

} // namespace mye
