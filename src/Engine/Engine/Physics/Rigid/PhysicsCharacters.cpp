// キャラクターコントローラ (M29b) の解決。剛体ソルバ (PhysicsSystem::Update) が剛体を解き終えた最終 pose を
// 障害物として CC のカプセルを押し出す (段差登り M82d、2D モード M94i、CC 同士の押し出し M95 を含む)。
// CharBody と SolveCharacters の契約は PhysicsSystemInternal.h。決定論契約は PhysicsSystem.cpp と同じ (scalar float のみ、固定パス数、entity.index 昇順)
#include "Engine/Engine/Physics/Rigid/PhysicsSystemInternal.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // SolidContact
#include "Engine/Engine/Physics/Rigid/Shapes.h"

using namespace DirectX;

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

// CC 同士の押し出しの掃引 (M95)
constexpr float kCharSweepTol = 1e-4f;   // 動かす前の深さにこれを足した値までは「障害物に新たに食い込んでいない」とみなす
constexpr int kCharSweepScanSteps = 8;   // 走査の最小区間数
constexpr int kCharSweepMaxSteps = 64;   // 走査の区間数の上限

// 2D モード (M94i) の定数
constexpr float kCharWallNormalX = 0.9f;    // 押し出し法線の |nx| がこれを超えたら壁の接触
constexpr float kCharPushNormalX = 0.5f;    // 剛体を押す接触とみなす |nx| の下限 (床に立っているだけでは押さない)
constexpr int kCharMaxPushTargets = 8;      // 1 tick に押せる剛体の数 (固定長 = 確保なし)

// CC の一方通行判定 (M94i)。plane2D の CC だけが on にする
struct CharOneWayContext {
    bool on = false;
    float startFeetY = 0.0f; // tick 開始時のカプセル最下点
    bool passAll = false;    // ignoreOneWay、または上昇中 (velocity.y > 0) = 全部すり抜ける
};

// obs が CC から見て「今 tick はすり抜ける」一方通行の床か。床になれるのは静的 / kinematic の oneWay 形状だけ
// (動的な oneWay 剛体は通常の接触。RejectOneWayContact と同じ規約)
bool CharPassesThroughOneWay(const CharOneWayContext& ow, const Body& obs)
{
    if (!ow.on || !obs.oneWay || (obs.rb != nullptr && !obs.kinematic)) {
        return false;
    }
    if (ow.passAll) {
        return true;
    }
    float minX, minY, minZ, maxX, maxY, maxZ;
    shapes::ComputeAabb(obs.pose, minX, minY, minZ, maxX, maxY, maxZ);
    return ow.startFeetY < maxY - OneWayFloorShift(obs) - kOneWaySlop;
}

// 通り抜けない一方通行の床でも、上向きでない法線 (横や下からの接触) は捨てる
bool CharRejectsOneWayNormal(const CharOneWayContext& ow, const Body& obs, float ny)
{
    return ow.on && obs.oneWay && (obs.rb == nullptr || obs.kinematic) && ny <= 0.0f;
}

// 他 CC の tick 頭の姿勢から作る一方通行の文脈。plane2D の CC だけが on にする
CharOneWayContext MakeCharOneWayContext(const CharBody& c)
{
    CharOneWayContext ow;
    if (c.cc->plane2D) {
        ow.on = true;
        ow.startFeetY = c.py - c.halfSeg - c.radius;
        ow.passAll = c.cc->ignoreOneWay || c.startVelY > 0.0f;
    }
    return ow;
}

// from を (dx,dy,dz) 方向へ t (0..L) 動かしても、solid の障害物への深さが「動かす前の深さ + kCharSweepTol」を
// 超えない最大の t を返す。CC 同士の押し出しが壁へ押し込むのを防ぐ。障害物の除外は押し出しループと同じ。
// 固定回数 (区間走査 + 二分探索 kStepBisectIters) で早期 break なし = 決定論。どこも破れなければ L をそのまま返す
float CharPushAllowance(const std::vector<Body>& bodies, const ShapePose& from, uint32_t selfIndex, int32_t layer,
                        uint32_t mask, const CharOneWayContext& ow, float dx, float dy, float dz, float L)
{
    auto violates = [&](float t) {
        ShapePose p = from;
        p.px += dx * t;
        p.py += dy * t;
        p.pz += dz * t;
        bool broken = false;
        for (const Body& obs : bodies) {
            if (!obs.solid || obs.entity.index == selfIndex || !shapes::CanCollide(layer, mask, obs.layer, obs.mask)
                || CharPassesThroughOneWay(ow, obs)) {
                continue;
            }
            float nx, ny, nz, depth;
            if (!shapes::Collide(p, obs.pose, nx, ny, nz, depth) || CharRejectsOneWayNormal(ow, obs, ny)) {
                continue;
            }
            float depth0 = 0.0f;
            float nx0, ny0, nz0;
            if (!shapes::Collide(from, obs.pose, nx0, ny0, nz0, depth0)) {
                depth0 = 0.0f;
            }
            broken = broken || depth > depth0 + kCharSweepTol;
        }
        return broken;
    };
    // 半径 0 (scale 0) や極小で L / radius が int に収まらなくても未定義動作にならないよう、float のまま上限をかける
    const float wantSteps = from.radius > 0.0f ? std::ceil(L / from.radius) : static_cast<float>(kCharSweepMaxSteps);
    const int steps = std::max(kCharSweepScanSteps,
                               static_cast<int>((std::min)(wantSteps, static_cast<float>(kCharSweepMaxSteps))));
    int firstBroken = 0; // 0 = 破れなし
    for (int i = 1; i <= steps; ++i) {
        const bool broken = violates(L * static_cast<float>(i) / static_cast<float>(steps));
        if (broken && firstBroken == 0) {
            firstBroken = i;
        }
    }
    if (firstBroken == 0) {
        return L;
    }
    float lo = L * static_cast<float>(firstBroken - 1) / static_cast<float>(steps);
    float hi = L * static_cast<float>(firstBroken) / static_cast<float>(steps);
    for (int i = 0; i < kStepBisectIters; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (violates(mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return lo;
}

// 段差登り。接地中に水平移動 (dirX,dirZ は単位ベクトル、len は今 tick の歩幅) が遮られたときだけ呼ぶ。
// 「stepHeight だけ持ち上げる → 前へ出す → 真下へ着地点を探す」を、前へ出す量を歩幅から半径まで
// 増やしながら試す。足を載せるのに必要なぶんだけ前へ出る = 低速でも 1 tick で登れる。
// 着地面の法線が登れる傾斜 (cosSlope 以上) で、接触点の高さが足元から stepHeight 以内のときだけ成功。
// 戻り値 true なら (outX,outY,outZ) が登った後のカプセル中心
bool TryCharStepUp(const std::vector<Body>& bodies, const CharBody& c, float cosSlope, float dirX,
                   float dirZ, float len, const CharOneWayContext& ow, float& outX, float& outY, float& outZ)
{
    auto overlapsAny = [&](const ShapePose& p) {
        for (const Body& obs : bodies) {
            if (!obs.solid || obs.entity.index == c.entity.index) {
                continue;
            }
            if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                continue;
            }
            if (CharPassesThroughOneWay(ow, obs)) {
                continue; // M94i
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
            if (CharPassesThroughOneWay(ow, obs)) {
                continue; // M94i
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

// CC の接触記録 (M95、ADR-034)。self が相手 other (法線 n は other → self) に触れたことを 1 対 1 件で積む。
// key と法線の向きは SolidContact の規約 (小 index が上位 32 bit、法線は大 index → 小 index)。impulse は 0
void RecordCharContact(std::vector<SolidContact>& out, uint32_t selfIndex, const ShapePose& selfPose,
                       uint32_t otherIndex, const ShapePose& otherPose, float nx, float ny, float nz)
{
    SolidContact sc;
    const bool selfIsSmall = selfIndex < otherIndex;
    sc.key = selfIsSmall ? ((static_cast<uint64_t>(selfIndex) << 32) | otherIndex)
                         : ((static_cast<uint64_t>(otherIndex) << 32) | selfIndex);
    const float sign = selfIsSmall ? 1.0f : -1.0f;
    sc.nx = sign * nx;
    sc.ny = sign * ny;
    sc.nz = sign * nz;
    shapes::ClosestPointOnShape(otherPose, selfPose.px, selfPose.py, selfPose.pz, sc.px, sc.py, sc.pz);
    sc.impulse = 0.0f;
    out.push_back(sc);
}

// 記録した CC の接触を outContacts (key 昇順) へ合流させる。同じ key は最初の記録だけ残し、
// outContacts に既にある key (剛体ソルバ側の接触) は剛体側を優先する。結果は key 昇順のまま
void MergeCharContacts(std::vector<SolidContact>& recorded, std::vector<SolidContact>& outContacts)
{
    std::stable_sort(recorded.begin(), recorded.end(),
                     [](const SolidContact& a, const SolidContact& b) { return a.key < b.key; });
    std::vector<SolidContact> merged;
    merged.reserve(outContacts.size() + recorded.size());
    size_t i = 0, j = 0;
    while (i < outContacts.size() || j < recorded.size()) {
        if (j < recorded.size() && j > 0 && recorded[j].key == recorded[j - 1].key) {
            ++j; // 記録内の重複
        } else if (j >= recorded.size() || (i < outContacts.size() && outContacts[i].key <= recorded[j].key)) {
            if (j < recorded.size() && outContacts[i].key == recorded[j].key) {
                ++j; // 剛体側を残す
            }
            merged.push_back(outContacts[i++]);
        } else {
            merged.push_back(recorded[j++]);
        }
    }
    outContacts.swap(merged);
}

} // namespace

// move-then-depenetrate: 変位を一括適用してから固定パス × 障害物 index 昇順で押し出す。
// スイープしないため 1 tick で薄い壁を大きく越える速度 (v·dt > 壁厚) では貫通し得るが、
// 60Hz × 常識的な移動速度では起きない (v=30m/s でも 0.5m/tick)。剛体ソルバの後に走るので
// 障害物 (静的 + 解決後の動的剛体) の pose は当該 tick の最終位置。CC が剛体を押す応答は
// 無し (必要ならソリッド capsule Collider を併用し OnCollision + AddForce で組む)。
// M95: collideCharacters の CC は他の CC の tick 頭のカプセルからも押し出す。reportContacts の CC が触れた相手は
// outContacts (null なら記録しない) へ impulse 0 の SolidContact として合流させる
void SolveCharacters(std::vector<Body>& bodies, std::vector<CharBody>& chars, float dt,
                     std::vector<SolidContact>* outContacts)
{
    if (chars.empty()) {
        return;
    }
    std::sort(chars.begin(), chars.end(),
              [](const CharBody& a, const CharBody& b) { return a.entity.index < b.entity.index; });
    // reportContacts の CC が 1 体も無ければ記録の経路には入らない (確保なし)
    bool anyReport = false;
    for (const CharBody& c : chars) {
        anyReport = anyReport || c.cc->reportContacts;
    }
    const bool record = anyReport && outContacts != nullptr;
    std::vector<SolidContact> recorded;
    for (CharBody& c : chars) {
        constexpr float kDeg2Rad = 3.14159265f / 180.0f;
        const float cosSlope = std::cos(c.cc->slopeLimitDeg * kDeg2Rad);
        // M95: ルートモーションの縦 (useVerticalInput) の tick は、重力積分の代わりに verticalInput を縦速度にする
        float vy = c.cc->useVerticalInput ? c.cc->verticalInput : c.cc->velocity.y + kGravity * c.cc->gravityScale * dt;
        // 変位一括適用 (水平は moveInput 直接駆動、垂直は重力積分)
        ShapePose pose;
        pose.shape = collidershape::kCapsule; // 常にワールド Y 軸 = 単位基底
        pose.identityRot = 1;
        pose.radius = c.radius;
        pose.halfSeg = c.halfSeg;
        pose.px = c.px + c.cc->moveInput.x * dt;
        pose.py = c.py + vy * dt;
        pose.pz = c.pz + c.cc->moveInput.z * dt;
        // 2D モード (M94i)。plane2D が false なら以下の変数は一度も読まれない (既存 CC の演算は増えない)
        const bool p2 = c.cc->plane2D;
        CharOneWayContext oneWay;
        float groundDx = 0.0f; // 前 tick の床が動かす今 tick の変位 (移動床)
        bool wallLeft = false;
        bool wallRight = false;
        const Body* pushed[kCharMaxPushTargets] = {};
        int pushedCount = 0;
        if (p2) {
            // moveInput.z は無視し z は tick 開始時の値に固定する (押し出しの z 成分も下で捨てる)
            pose.pz = c.pz;
            oneWay.on = true;
            oneWay.startFeetY = c.py - c.halfSeg - c.radius;
            oneWay.passAll = c.cc->ignoreOneWay || c.cc->velocity.y > 0.0f;
            if (c.cc->isGrounded) {
                groundDx = c.cc->groundVelocity.x * dt;
                pose.px += groundDx;
                pose.py += c.cc->groundVelocity.y * dt;
            }
        }
        bool grounded = false;
        // 押し出し 1 回ぶんの適用 (n は相手 → 自分)。障害物も CC も同じ接地・天井の分岐を通る
        auto applyPush = [&](float nx, float ny, float nz, float depth) {
            pose.px += nx * depth;
            pose.py += ny * depth;
            if (!p2) {
                pose.pz += nz * depth; // 2D は面外へ押し出さない
            }
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
        };
        // 押し出し: 固定パス × 障害物 index 昇順 (bodies はソート済)。自分の collider は skip
        for (int pass = 0; pass < kCharPushPasses; ++pass) {
            for (const Body& obs : bodies) {
                if (!obs.solid || obs.entity.index == c.entity.index) {
                    continue;
                }
                if (!shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)) {
                    continue; // M36a
                }
                if (p2 && CharPassesThroughOneWay(oneWay, obs)) {
                    continue; // M94i
                }
                float nx, ny, nz, depth;
                if (!shapes::Collide(pose, obs.pose, nx, ny, nz, depth)) {
                    continue;
                }
                if (p2) {
                    if (CharRejectsOneWayNormal(oneWay, obs, ny)) {
                        continue;
                    }
                    if (nx > kCharWallNormalX) {
                        wallLeft = true; // 法線は障害物 → CC。+x を向く = 壁は左にある
                    } else if (nx < -kCharWallNormalX) {
                        wallRight = true;
                    }
                    if (c.cc->pushPower > 0.0f && obs.rb != nullptr && !obs.kinematic && obs.plane2D
                        && std::fabs(nx) > kCharPushNormalX && nx * c.cc->moveInput.x < 0.0f
                        && pushedCount < kCharMaxPushTargets) {
                        bool seen = false;
                        for (int k = 0; k < pushedCount; ++k) {
                            seen = seen || (pushed[k] == &obs);
                        }
                        if (!seen) {
                            pushed[pushedCount++] = &obs;
                        }
                    }
                }
                if (record && c.cc->reportContacts) {
                    RecordCharContact(recorded, c.entity.index, pose, obs.entity.index, obs.pose, nx, ny, nz);
                }
                applyPush(nx, ny, nz, depth);
            }
            // CC 同士 (M95)。相手の tick 頭のカプセルだけを見る = 全 CC が同じ姿勢を見るので走査順に依らない。
            // 押し出し量は障害物への掃引で切る: 受ける側は自分が、押す側は相手が壁へ入らない範囲までしか動かさず、
            // 相手が動けない分は押す側が引き受けて止まる (moveInput は変えない)
            if (c.cc->collideCharacters) {
                for (const CharBody& other : chars) {
                    if (other.entity.index == c.entity.index || !other.cc->collideCharacters) {
                        continue;
                    }
                    if (!shapes::CanCollide(c.layer, c.mask, other.layer, other.mask)) {
                        continue;
                    }
                    ShapePose otherPose;
                    otherPose.shape = collidershape::kCapsule;
                    otherPose.identityRot = 1;
                    otherPose.radius = other.radius;
                    otherPose.halfSeg = other.halfSeg;
                    otherPose.px = other.px;
                    otherPose.py = other.py;
                    otherPose.pz = other.pz;
                    float nx, ny, nz, depth;
                    if (!shapes::Collide(pose, otherPose, nx, ny, nz, depth)) {
                        continue;
                    }
                    if (other.cc->pushPriority < c.cc->pushPriority) {
                        // 押す側。相手が -n へ動ける量 room を引いた残りだけ自分が戻る
                        const float oz = other.cc->plane2D ? 0.0f : -nz;
                        const float room = CharPushAllowance(bodies, otherPose, other.entity.index, other.layer, other.mask,
                                                             MakeCharOneWayContext(other), -nx, -ny, oz, depth);
                        if (depth - room > 0.0f) {
                            applyPush(nx, ny, nz, depth - room);
                        }
                        continue;
                    }
                    if (record && (c.cc->reportContacts || other.cc->reportContacts)) {
                        RecordCharContact(recorded, c.entity.index, pose, other.entity.index, otherPose, nx, ny, nz);
                    }
                    const float sz = p2 ? 0.0f : nz;
                    applyPush(nx, ny, nz, CharPushAllowance(bodies, pose, c.entity.index, c.layer, c.mask, oneWay, nx, ny, sz, depth));
                }
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
                if (p2 && CharPassesThroughOneWay(oneWay, obs)) {
                    continue; // M94i
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
            const float mz = p2 ? 0.0f : c.cc->moveInput.z;
            const float speed = std::sqrt(mx * mx + mz * mz);
            if (speed > 0.0f) {
                const float dirX = mx / speed;
                const float dirZ = mz / speed;
                const float len = speed * dt;
                // 移動床が運んだ分は歩いた量に数えない (groundDx は 2D 以外では使わない)
                const float movedX = p2 ? (pose.px - c.px - groundDx) : (pose.px - c.px);
                const float progress = movedX * dirX + (pose.pz - c.pz) * dirZ;
                float sx, sy, sz;
                if (progress < 0.5f * len
                    && TryCharStepUp(bodies, c, cosSlope, dirX, dirZ, len, oneWay, sx, sy, sz)) {
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
        // 2D は移動床が運んだ分を引いた「床に対する速度」を書く (歩行アニメ / 足音が床の速度で歩き出さない)
        c.cc->velocity.x = p2 ? (pose.px - c.px - groundDx) * invDt : (pose.px - c.px) * invDt;
        c.cc->velocity.y = vy;
        c.cc->useVerticalInput = false; // 1 tick で消費 (次の tick は書き直されなければ重力に戻る)
        c.cc->velocity.z = p2 ? 0.0f : (pose.pz - c.pz) * invDt;
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
        if (p2) {
            // 報告 (M94i)。接地した床 = 接地プローブと同じ姿勢で最も上向きの面を持つ形状 (同値は bodies の先頭)
            EntityID groundEntity = {};
            XMFLOAT3 groundVelocity = { 0.0f, 0.0f, 0.0f };
            if (grounded) {
                ShapePose probe = pose;
                probe.py -= (c.cc->skinWidth + 0.01f);
                const Body* ground = nullptr;
                float groundNy = -2.0f;
                for (const Body& obs : bodies) {
                    if (!obs.solid || obs.entity.index == c.entity.index
                        || !shapes::CanCollide(c.layer, c.mask, obs.layer, obs.mask)
                        || CharPassesThroughOneWay(oneWay, obs)) {
                        continue;
                    }
                    float nx, ny, nz, depth;
                    if (shapes::Collide(probe, obs.pose, nx, ny, nz, depth) && ny >= cosSlope
                        && ny > groundNy) {
                        groundNy = ny;
                        ground = &obs;
                    }
                }
                if (ground != nullptr) {
                    groundEntity = ground->entity;
                    if (ground->rb != nullptr) {
                        groundVelocity = ground->rb->velocity;
                    }
                }
            }
            c.cc->groundEntity = groundEntity;
            c.cc->groundVelocity = groundVelocity;
            c.cc->ticksSinceGrounded = grounded
                ? 0
                : std::min(c.cc->ticksSinceGrounded + 1, CharacterControllerComponent::kMaxTicksSinceGrounded);
            c.cc->wallContact = (wallLeft && wallRight) ? 2 : (wallLeft ? -1 : (wallRight ? 1 : 0));
            // 剛体を押す: 次 tick の物理で動く。眠っていれば起こす
            for (int k = 0; k < pushedCount; ++k) {
                RigidbodyComponent* target = pushed[k]->rb;
                target->velocity.x += c.cc->pushPower * c.cc->moveInput.x;
                target->isSleeping = false;
                target->sleepTicks = 0;
            }
        }
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
    if (!recorded.empty()) {
        MergeCharContacts(recorded, *outContacts);
    }
}

} // namespace mye
