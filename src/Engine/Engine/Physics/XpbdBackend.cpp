//====================================================================================
//                          XpbdBackend.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          変形体コンポーネントと粒子池の同期
//====================================================================================
#include "Engine/Engine/Physics/XpbdBackend.h"

#include <algorithm>
#include <utility>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/PhysicsSystem.h" // ComposeEntityWorldPose (M60'c)

namespace mye {
namespace {

// component の値を決定論的に矯正する (Inspector のレンジは UI の飾りでしかなく、
// JSON / スクリプト経由では何でも入りうる。NaN や 0 割をここで堰き止める)
int SanitizedSegments(const RopeComponent& rope)
{
    int segs = rope.segmentCount;
    if (segs < 1) {
        segs = 1;
    } else if (segs > 256) {
        segs = 256;
    }
    return segs;
}

// ロープの池を初期配置で組む。姿勢のローカル -Y へ直線に垂らす (計画 M60'c)。
// ★rest 長・質量・ピンはここで焼き込む = 生成後のコンポーネント編集は効かない
//   (compliance / damping だけはソルバが毎 tick コンポーネントから読む導出値)
XpbdBackend::Pool BuildRopePool(World& world, EntityID e, const RopeComponent& rope)
{
    const int segs = SanitizedSegments(rope);
    const int n = segs + 1;
    const float len = rope.length > 0.001f ? rope.length : 0.001f;
    const float mass = rope.mass > 0.0001f ? rope.mass : 0.0001f;
    const float step = len / static_cast<float>(segs);
    const float invMass = static_cast<float>(n) / mass; // 粒子質量 = mass/n の逆数

    float px, py, pz, qx, qy, qz, qw;
    ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
    // ローカル -Y をワールドへ回す (クォータニオン回転の scalar 展開、PhysicsSystem と同式)
    const float tx = 2.0f * (qy * 0.0f - qz * (-1.0f));
    const float ty = 2.0f * (qz * 0.0f - qx * 0.0f);
    const float tz = 2.0f * (qx * (-1.0f) - qy * 0.0f);
    const float dx = 0.0f + qw * tx + (qy * tz - qz * ty);
    const float dy = -1.0f + qw * ty + (qz * tx - qx * tz);
    const float dz = 0.0f + qw * tz + (qx * ty - qy * tx);

    XpbdBackend::Pool p;
    p.owner = e;
    p.kind = static_cast<uint32_t>(XpbdBackend::PoolKind::Rope);
    p.px.resize(n); p.py.resize(n); p.pz.resize(n);
    p.vx.assign(n, 0.0f); p.vy.assign(n, 0.0f); p.vz.assign(n, 0.0f);
    p.invMass.assign(n, invMass);
    for (int i = 0; i < n; ++i) {
        const float t = step * static_cast<float>(i);
        p.px[static_cast<size_t>(i)] = px + dx * t;
        p.py[static_cast<size_t>(i)] = py + dy * t;
        p.pz[static_cast<size_t>(i)] = pz + dz * t;
    }
    p.prevX = p.px; p.prevY = p.py; p.prevZ = p.pz;
    if (rope.attachStart) {
        p.invMass[0] = 0.0f;
    }
    if (rope.attachEnd) {
        p.invMass[static_cast<size_t>(n - 1)] = 0.0f;
    }
    p.ca.resize(static_cast<size_t>(segs));
    p.cb.resize(static_cast<size_t>(segs));
    p.rest.assign(static_cast<size_t>(segs), step);
    for (int i = 0; i < segs; ++i) {
        p.ca[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
        p.cb[static_cast<size_t>(i)] = static_cast<uint32_t>(i + 1);
    }
    return p;
}

} // namespace

// コンポーネントの有無と池を突き合わせる (毎 tick、PhysicsSystem::Update の先頭)。
// ★組み直しの条件は「粒子数の不一致」**だけ**。値の一致まで見ると snapshot 復元直後の
//   Sync が「復元された池」を壊す (復元 → 再シムのハッシュ照合が必ず割れる)。
// ★owner.index 昇順を常に維持する (ハッシュ/blob の畳み込み順 = 決定論)
void XpbdBackend::Sync(World& world)
{
    // 生きている Rope オーナーを index 昇順で収集 (アーキタイプ走査は大域順序を
    // 保証しないので明示的に整列する — 剛体収集と同じ理由)
    std::vector<std::pair<EntityID, const RopeComponent*>> ropes;
    const ComponentTypeId req[] = { RopeComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ri = arch.FindTypeIndex(RopeComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue; // 無効化中は池ごと無し (再有効化で初期配置から組み直し)
            }
            ropes.emplace_back(e, static_cast<const RopeComponent*>(arch.GetPtr(ri, row)));
        }
    });
    std::sort(ropes.begin(), ropes.end(),
              [](const auto& a, const auto& b) { return a.first.index < b.first.index; });

    // 両側とも昇順なので 2 ポインタで突き合わせる
    std::vector<Pool> next;
    next.reserve(ropes.size());
    size_t pi = 0;
    for (const auto& [e, rope] : ropes) {
        while (pi < pools_.size() && pools_[pi].owner.index < e.index) {
            ++pi; // 池だけにある owner = コンポーネントが消えた → 破棄 (next へ移さない)
        }
        const int wantParticles = SanitizedSegments(*rope) + 1;
        if (pi < pools_.size() && pools_[pi].owner.index == e.index
            && pools_[pi].owner.generation == e.generation
            && pools_[pi].px.size() == static_cast<size_t>(wantParticles)) {
            next.push_back(std::move(pools_[pi]));
            ++pi;
        } else {
            if (pi < pools_.size() && pools_[pi].owner.index == e.index) {
                ++pi; // 同 index の旧池 (世代違い or 粒子数違い) は捨てて組み直す
            }
            next.push_back(BuildRopePool(world, e, *rope));
        }
        // 始端ピンはエンティティへ毎 tick 追従する (invMass==0 なのでソルバは動かさない。
        // ここで書くのが唯一の駆動点)。終端ピンは生成時のワールド位置に留まる
        Pool& p = next.back();
        if (rope->attachStart && !p.invMass.empty() && p.invMass[0] == 0.0f) {
            float px, py, pz, qx, qy, qz, qw;
            ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
            p.px[0] = px;
            p.py[0] = py;
            p.pz[0] = pz;
        }
    }
    pools_ = std::move(next);
}

} // namespace mye
