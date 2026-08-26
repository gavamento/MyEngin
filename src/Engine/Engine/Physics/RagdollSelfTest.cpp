#include "Engine/Engine/Physics/RagdollSelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/PartFollowSystem.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Ragdoll.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
const char* const kBoneNames[3] = { "root", "mid", "tip" };

// 3 骨の直鎖スケルトンを手で組む: root(0) → mid(1) → tip(2)、各段 -1m (下に垂れる腕)。
// clip 0 は mid を Z 軸まわりに 1 秒で 90 度回す = 「アニメが骨を動かしている」状態を作る
SkinnedModel MakeChainSkeleton()
{
    SkinnedModel m;
    const int parents[3] = { -1, 0, 1 };
    const XMFLOAT3 offs[3] = { { 0, 0, 0 }, { 0, -1, 0 }, { 0, -1, 0 } };
    for (int i = 0; i < 3; ++i) {
        SkeletonJoint j;
        j.parent = parents[i];
        j.name = kBoneNames[i];
        j.bindT = offs[i];
        m.joints.push_back(j);
    }
    // inverseBind = バインドグローバルの逆。**試験のセットアップなので XMMatrixInverse で
    // よい** (sim ではない)。これが恒等でないおかげでパレット比較が「IB が素通しだから
    // たまたま合った」で通らなくなる
    {
        std::vector<XMMATRIX> locals;
        ComputeJointLocals(m, -1, 0.0f, locals);
        for (size_t i = 0; i < m.joints.size(); ++i) {
            const XMMATRIX g = JointGlobalFromLocals(m, locals, static_cast<int32_t>(i));
            XMStoreFloat4x4(&m.joints[i].inverseBind, XMMatrixInverse(nullptr, g));
        }
    }
    SkeletalClip c;
    c.name = "swing";
    c.duration = 1.0f;
    c.tracks.resize(3);
    JointTrack& tr = c.tracks[1]; // mid だけ回す
    tr.rTimes = { 0.0f, 1.0f };
    XMFLOAT4 q0, q1;
    XMStoreFloat4(&q0, XMQuaternionIdentity());
    XMStoreFloat4(&q1, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), 1.2f));
    tr.rVals = { q0, q1 };
    m.clips.push_back(std::move(c));
    return m;
}

struct Rig {
    GameObject skin;
    GameObject bone[3];
};

// ラグドールの形をしたシーンを組む。
//   boneMask … ビット i が立っている骨だけ部位エンティティを作る (0b111 = 全部)
//   withBodies … 部位に Collider + Rigidbody を載せるか
Rig BuildRig(Scene& s, AssetID model, bool withRagdoll, bool active, bool withBodies,
             int boneMask = 0b111, int clip = 0, int timeTicks = 20)
{
    Rig r;
    r.skin = s.CreateGameObjectTracked("Skin");
    auto* sm = r.skin.AddComponent<SkinnedMeshComponent>();
    sm->model = model;
    sm->clip = clip;
    sm->timeTicks = timeTicks; // SkinningSystem は回さない (試験が時刻を明示的に置く)
    sm->playing = 0;
    if (withRagdoll) {
        r.skin.AddComponent<RagdollComponent>()->active = active;
    }
    for (int i = 0; i < 3; ++i) {
        if ((boneMask & (1 << i)) == 0) {
            continue;
        }
        GameObject b = s.CreateGameObjectTracked(kBoneNames[i]);
        b.SetParent(r.skin);
        auto* p = b.AddComponent<PartComponent>();
        std::snprintf(p->joint, sizeof(p->joint), "%s", kBoneNames[i]);
        if (withBodies) {
            auto* col = b.AddComponent<ColliderComponent>();
            col->shape = 2; // カプセル
            col->radius = 0.15f;
            col->height = 0.8f;
            auto* rb = b.AddComponent<RigidbodyComponent>();
            rb->mass = 1.0f;
        }
        r.bone[i] = b;
    }
    s.GetWorld().ApplyStructuralChanges();
    return r;
}

// 部位の LocalTransform を丸ごと比較 (ビット厳密)
bool SameLocal(const LocalTransform& a, const LocalTransform& b)
{
    return std::memcmp(&a, &b, sizeof(LocalTransform)) == 0;
}

bool SamePalette(const std::vector<XMFLOAT4X4>& a, const std::vector<XMFLOAT4X4>& b)
{
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(XMFLOAT4X4)) == 0);
}

} // namespace

bool RunRagdollSelfTest()
{
    MYE_LOG_INFO("==== Ragdoll self test ====");
    RegisterBuiltinComponents(); // sTypeId 解決 (冪等)
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RenderResources res; // Init しない = GPU バッファを作らない (PartSelfTest と同じ手)
    const AssetID model = res.skinnedModels.Register("ragdoll_chain", MakeChainSkeleton());
    const SkinnedModel* skel = res.skinnedModels.Get(model);
    check(skel != nullptr && skel->joints.size() == 3,
          "setup: the hand-built 3-bone chain registers headlessly");
    if (!skel) {
        MYE_LOG_ERROR("==== Ragdoll self test: FAIL ====");
        return false;
    }

    // アニメが置く骨のグローバル行列 (期待値の基準)
    std::vector<XMMATRIX> animLocals;
    ComputeJointLocals(*skel, 0, 20.0f / 60.0f, animLocals);

    // ---- (g1-1) active=false: アニメが骨を駆動し、物理は部位を 1 ミリも動かさない ----
    // ★これが「休止中のラグドールは kinematic 扱い」の直接の証明。ゲートが無ければ
    //   部位は Rigidbody を持っているので重力で落ちていく
    {
        Scene s;
        Rig r = BuildRig(s, model, /*withRagdoll*/ true, /*active*/ false, /*withBodies*/ true);
        PartFollowSystem pf;
        PhysicsSystem phys;
        for (int i = 0; i < 120; ++i) {
            pf.Update(s.GetWorld(), res);
            phys.Update(s.GetWorld(), kDt);
        }
        bool allMatch = true;
        for (int i = 0; i < 3; ++i) {
            const XMMATRIX g = JointGlobalFromLocals(*skel, animLocals, i);
            XMFLOAT4X4 f;
            XMStoreFloat4x4(&f, g);
            const auto* lt = r.bone[i].GetComponent<LocalTransform>();
            // PartFollowSystem は行ベクトル規約の第 4 行をそのまま position へ書く
            allMatch = allMatch && lt && lt->position.x == f._41 && lt->position.y == f._42
                       && lt->position.z == f._43;
        }
        check(allMatch, "held: an inactive ragdoll leaves the parts exactly where the animation "
                        "put them (120 ticks of gravity change nothing)");
        const auto* rb = r.bone[2].GetComponent<RigidbodyComponent>();
        check(rb && rb->velocity.x == 0.0f && rb->velocity.y == 0.0f && rb->velocity.z == 0.0f,
              "held: a held bone accumulates no velocity (it cannot shove what it touches, and "
              "cannot explode when the ragdoll is switched back on)");
        check(rb && !rb->isSleeping && rb->sleepTicks == 0,
              "held: a held bone never counts down to sleep (it is kinematic, not resting)");
    }

    // ---- (g1-2) 存在ゲート: 剛体ごと消したアニメ専用シーンとビット一致 ----
    // 「休止中のラグドールに載せた剛体は物理的に居ないのと同じ」ことの機械的証明
    {
        Scene animOnly;
        Rig ra = BuildRig(animOnly, model, /*withRagdoll*/ false, false, /*withBodies*/ false);
        Scene withRb;
        Rig rb2 = BuildRig(withRb, model, /*withRagdoll*/ true, /*active*/ false,
                           /*withBodies*/ true);
        PartFollowSystem pfa, pfb;
        PhysicsSystem pa, pb;
        for (int i = 0; i < 60; ++i) {
            pfa.Update(animOnly.GetWorld(), res);
            pa.Update(animOnly.GetWorld(), kDt);
            pfb.Update(withRb.GetWorld(), res);
            pb.Update(withRb.GetWorld(), kDt);
        }
        bool same = true;
        for (int i = 0; i < 3; ++i) {
            const auto* la = ra.bone[i].GetComponent<LocalTransform>();
            const auto* lb = rb2.bone[i].GetComponent<LocalTransform>();
            same = same && la && lb && SameLocal(*la, *lb);
        }
        check(same, "held: the bone poses are bit-identical to a scene with no rigid bodies at "
                    "all (the physics gate is a true no-op)");
    }

    // ---- (g1-3) active=true: 物理が骨を駆動し、PartFollowSystem は手を引く ----
    {
        Scene s;
        Rig r = BuildRig(s, model, /*withRagdoll*/ true, /*active*/ true, /*withBodies*/ true);
        PartFollowSystem pf;
        PhysicsSystem phys;
        for (int i = 0; i < 60; ++i) {
            pf.Update(s.GetWorld(), res);
            phys.Update(s.GetWorld(), kDt);
        }
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, JointGlobalFromLocals(*skel, animLocals, 0));
        const auto* lt = r.bone[0].GetComponent<LocalTransform>();
        // 1 秒の自由落下 ≈ -4.9m。アニメが書いていたらここは f._42 のまま
        check(lt && lt->position.y < f._42 - 3.0f,
              "driven: an active ragdoll falls under gravity (PartFollowSystem stopped writing)");
    }

    // ---- (g1-4) 関節で繋いだラグドールが崩れて静止し、**全部位が同じ tick に揃って眠る** ----
    // ★M60a で関節ペアを島と起床へ配線した効果が Cone でも効いていることの確認。
    //   配線が抜けていると「腕だけ眠って胴が起きている」= 関節が破綻する
    {
        Scene s;
        // 床
        {
            GameObject g = s.CreateGameObjectTracked("Floor");
            g.SetLocalPosition(0.0f, -0.5f, 0.0f);
            auto* col = g.AddComponent<ColliderComponent>();
            col->shape = 1;
            col->halfExtents = { 10.0f, 0.5f, 10.0f };
        }
        // 物理環境 (**存在ゲートの内側** — スリープの閾値は env の中にしかない)
        {
            GameObject e = s.CreateGameObjectTracked("Env");
            auto* env = e.AddComponent<PhysicsEnvironmentComponent>();
            env->substeps = 4;
            env->sleepDelayTicks = 30;
        }
        Rig r = BuildRig(s, model, true, /*active*/ false, /*withBodies*/ true);
        r.skin.SetLocalPosition(0.0f, 3.0f, 0.0f);
        // 骨と骨を Cone で繋ぐ (ラグドールの本体)。可動域はヒトの関節を模した程度
        for (int i = 1; i < 3; ++i) {
            auto* j = r.bone[i].AddComponent<JointComponent>();
            j->connectedEntity = r.bone[i - 1].Id();
            j->type = 4; // Cone
            j->anchor = { 0.0f, 0.5f, 0.0f };
            j->connectedAnchor = { 0.0f, -0.5f, 0.0f };
            j->axis = { 0.0f, 1.0f, 0.0f };
            j->useLimit = true;
            j->swingLimitDeg = 45.0f;
            j->limitMin = -30.0f;
            j->limitMax = 30.0f;
        }
        s.GetWorld().ApplyStructuralChanges();

        PartFollowSystem pf;
        PhysicsSystem phys;
        pf.Update(s.GetWorld(), res); // アニメのポーズへ置いてから作動させる (実運用の手順)
        r.skin.GetComponent<RagdollComponent>()->active = true;

        int sleptAt[3] = { -1, -1, -1 };
        for (int tick = 0; tick < 1200; ++tick) {
            pf.Update(s.GetWorld(), res);
            phys.Update(s.GetWorld(), kDt);
            for (int i = 0; i < 3; ++i) {
                const auto* rb = r.bone[i].GetComponent<RigidbodyComponent>();
                if (sleptAt[i] < 0 && rb && rb->isSleeping) {
                    sleptAt[i] = tick;
                }
            }
            if (sleptAt[0] >= 0 && sleptAt[1] >= 0 && sleptAt[2] >= 0) {
                break;
            }
        }
        MYE_LOG_INFO("  [ragdoll] slept at ticks %d / %d / %d", sleptAt[0], sleptAt[1], sleptAt[2]);
        check(sleptAt[0] >= 0 && sleptAt[1] >= 0 && sleptAt[2] >= 0,
              "island: a jointed ragdoll settles on the floor and falls asleep");
        check(sleptAt[0] == sleptAt[1] && sleptAt[1] == sleptAt[2],
              "island: every bone of the ragdoll sleeps on the same tick (the joint pairs are "
              "wired into the islands, so no limb dozes off alone)");
    }

    // ---- (g1-5/g1-6) パレット: 純関数であること + 骨の位置にある部位で ComputeBonePalette と一致 ----
    // ★RenderSystem::Render() は**ビュー毎に呼ばれる** (SceneView / GameView /
    //   AssetPreview / ProbeBaker …)。パレットが ECS 状態の純関数でないと、
    //   同じ tick なのにビューごとに絵が割れる
    {
        Scene s;
        Rig r = BuildRig(s, model, /*withRagdoll*/ true, /*active*/ false, /*withBodies*/ false);
        PartFollowSystem pf;
        pf.Update(s.GetWorld(), res); // 部位をアニメの骨姿勢へ置く
        // ここで作動させる = 「部位が骨とちょうど一致している」状態のラグドール
        r.skin.GetComponent<RagdollComponent>()->active = true;

        std::vector<XMFLOAT4X4> p1, p2, anim;
        ragdoll::BuildBonePalette(s.GetWorld(), r.skin.Id(), *skel, 0, 20.0f / 60.0f, p1);
        ragdoll::BuildBonePalette(s.GetWorld(), r.skin.Id(), *skel, 0, 20.0f / 60.0f, p2);
        ComputeBonePalette(*skel, 0, 20.0f / 60.0f, anim);
        check(SamePalette(p1, p2),
              "palette: building twice from the same world gives byte-identical results (pure "
              "function of ECS state, so every view agrees)");
        check(SamePalette(p1, anim),
              "palette: parts sitting exactly on the bones reproduce ComputeBonePalette bit for "
              "bit (partLocal == jointGlobal read backwards is exact)");
    }

    // ---- (g1-6) 部位を持たない骨は階層合成で埋まり、親の部位に追従する ----
    {
        Scene s;
        // mid (bit 1) だけに部位を作る。root と tip は骨だけ = override なし
        Rig r = BuildRig(s, model, true, /*active*/ false, /*withBodies*/ false, /*boneMask*/ 0b010);
        PartFollowSystem pf;
        pf.Update(s.GetWorld(), res);
        r.skin.GetComponent<RagdollComponent>()->active = true;

        std::vector<XMFLOAT4X4> before, after, anim;
        ragdoll::BuildBonePalette(s.GetWorld(), r.skin.Id(), *skel, 0, 20.0f / 60.0f, before);
        ComputeBonePalette(*skel, 0, 20.0f / 60.0f, anim);
        check(SamePalette(before, anim),
              "palette: bones without a part fall back to the animated hierarchy (a partial "
              "ragdoll still poses the whole skeleton)");

        // mid の部位を横へずらす → tip (子・部位なし) は付いてくるが root (親) は動かない
        r.bone[1].GetComponent<LocalTransform>()->position.x += 2.0f;
        ragdoll::BuildBonePalette(s.GetWorld(), r.skin.Id(), *skel, 0, 20.0f / 60.0f, after);
        const bool rootStill = std::memcmp(&before[0], &after[0], sizeof(XMFLOAT4X4)) == 0;
        const bool tipMoved = std::memcmp(&before[2], &after[2], sizeof(XMFLOAT4X4)) != 0;
        check(rootStill && tipMoved,
              "palette: moving a bodied bone carries its part-less children and leaves its "
              "parent alone");
    }

    // ---- (g1-7) 切替の連続性: false → true の瞬間に骨が飛ばない ----
    // ★これが「Part を再利用する」設計の配当。作動させた瞬間の剛体の初期姿勢は
    //   アニメが置いた骨姿勢そのものなので、初期化のコードが 1 行も要らない
    {
        Scene s;
        Rig r = BuildRig(s, model, true, /*active*/ false, /*withBodies*/ true);
        PartFollowSystem pf;
        PhysicsSystem phys;
        for (int i = 0; i < 30; ++i) {
            pf.Update(s.GetWorld(), res);
            phys.Update(s.GetWorld(), kDt);
        }
        XMFLOAT3 held[3];
        for (int i = 0; i < 3; ++i) {
            held[i] = r.bone[i].GetComponent<LocalTransform>()->position;
        }
        r.skin.GetComponent<RagdollComponent>()->active = true;
        pf.Update(s.GetWorld(), res);
        phys.Update(s.GetWorld(), kDt);
        float worst = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const XMFLOAT3 p = r.bone[i].GetComponent<LocalTransform>()->position;
            const float d = std::sqrt((p.x - held[i].x) * (p.x - held[i].x)
                                      + (p.y - held[i].y) * (p.y - held[i].y)
                                      + (p.z - held[i].z) * (p.z - held[i].z));
            worst = (d > worst) ? d : worst;
        }
        // 1 tick の自由落下 = g*dt^2 ≈ 2.7mm。それを超えて動いたら「飛んだ」
        MYE_LOG_INFO("  [ragdoll] switch-on jump: %.6f m (1 tick of free fall = %.6f m)", worst,
                     9.81f * kDt * kDt);
        check(worst < 0.005f,
              "switch: turning the ragdoll on does not teleport the bones (the bodies start from "
              "the animated pose)");
    }

    // ---- (g1-8) active は sim 状態 = ワールドハッシュに載る ----
    // ★載っていないと snapshot 往復とタイムトラベルで駆動方向が食い違う。
    //   フィールドがハッシュ対象なら SimSnapshot も ReplayFile も自動で従うので、
    //   専用の保存経路は 1 行も要らない (M60d-8 と同じ論法)
    {
        Scene s;
        Rig r = BuildRig(s, model, true, /*active*/ false, /*withBodies*/ true);
        const uint64_t h0 = HashWorld(s.GetWorld(), nullptr);
        r.skin.GetComponent<RagdollComponent>()->active = true;
        const uint64_t h1 = HashWorld(s.GetWorld(), nullptr);
        r.skin.GetComponent<RagdollComponent>()->active = false;
        const uint64_t h2 = HashWorld(s.GetWorld(), nullptr);
        check(h0 != h1 && h0 == h2, "hash: Ragdoll.active is sim state (snapshots and replays "
                                    "carry it without a dedicated path)");
    }

    // ---- (g1-9) 並走ハッシュ: 同一シーン 2 個の per-tick ハッシュが一致 ----
    {
        Scene a, b;
        BuildRig(a, model, true, /*active*/ true, true);
        BuildRig(b, model, true, /*active*/ true, true);
        PartFollowSystem pfa, pfb;
        PhysicsSystem pa, pb;
        bool same = true;
        for (int i = 0; i < 120 && same; ++i) {
            pfa.Update(a.GetWorld(), res);
            pa.Update(a.GetWorld(), kDt);
            pfb.Update(b.GetWorld(), res);
            pb.Update(b.GetWorld(), kDt);
            same = HashWorld(a.GetWorld(), nullptr) == HashWorld(b.GetWorld(), nullptr);
        }
        check(same, "determinism: two identical driven ragdolls agree on the world hash every "
                    "tick for 120 ticks");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Ragdoll self test: PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Ragdoll self test: FAIL (%d) ====", failCount);
    return false;
}

} // namespace mye
