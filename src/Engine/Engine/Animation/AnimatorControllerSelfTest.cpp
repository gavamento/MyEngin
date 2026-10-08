#include "Engine/Engine/Animation/AnimatorControllerSelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Animation/SkinningSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

namespace mye {

using nlohmann::json;

namespace {

json Key(int t, std::vector<float> v)
{
    json k;
    k["t"] = t;
    k["v"] = v;
    return k;
}

// LocalTransform.position を動かす 1 トラックのクリップを作って登録し、ハッシュを返す
uint64_t MakePosClip(AnimationLibrary& lib, const std::wstring& path, int lengthTicks,
                     const json& keys)
{
    json tPos;
    tPos["target"] = 0;
    tPos["component"] = "LocalTransform";
    tPos["field"] = "position";
    tPos["interp"] = "linear";
    tPos["keys"] = keys;
    json cj;
    cj["name"] = "clip";
    cj["lengthTicks"] = lengthTicks;
    cj["tracks"] = json::array({ tPos });
    AnimationClipAsset clip;
    AnimationLibrary::FromJson(cj, clip);
    return lib.Register(path, clip);
}

// 1 ジョイントのスケルトンに、名前と長さ (秒) だけのクリップを並べる (トラックはバインド)
SkinnedModel MakeNamedClipModel(std::initializer_list<std::pair<const char*, float>> clips)
{
    SkinnedModel model;
    model.joints.resize(1);
    for (const auto& [name, duration] : clips) {
        SkeletalClip clip;
        clip.name = name;
        clip.duration = duration;
        clip.tracks.resize(1);
        model.clips.push_back(std::move(clip));
    }
    return model;
}

} // namespace

bool RunAnimatorControllerSelfTest()
{
    MYE_LOG_INFO("==== Animator Controller self test ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 共有アセット: idle (Y bob) / walk (X sway) クリップ + 2 状態コントローラ ----
    AnimationLibrary animLib;
    const uint64_t idleH = MakePosClip(animLib, L"idle.anim.json", 60,
                                       json::array({ Key(0, { 0, 0, 0 }), Key(30, { 0, 0.3f, 0 }),
                                                     Key(60, { 0, 0, 0 }) }));
    const uint64_t walkH = MakePosClip(animLib, L"walk.anim.json", 60,
                                       json::array({ Key(0, { 0, 0, 0 }), Key(15, { 0.5f, 0, 0 }),
                                                     Key(30, { 0, 0, 0 }), Key(45, { -0.5f, 0, 0 }),
                                                     Key(60, { 0, 0, 0 }) }));

    ControllerLibrary ctrlLib;
    ControllerAsset ca;
    ca.defaultState = 0;
    ca.parameters = { { "speed" } };
    ca.states.push_back({ "Idle", "", idleH, 1, 1 });
    ca.states.push_back({ "Walk", "", walkH, 1, 1 });
    { // Idle → Walk (param0 > 0)
        ControllerTransition t;
        t.from = 0;
        t.to = 1;
        t.duration = 8;
        t.conditions = { { 0, CondOp::Gt, 0 } };
        ca.transitions.push_back(t);
    }
    { // Walk → Idle (param0 <= 0)
        ControllerTransition t;
        t.from = 1;
        t.to = 0;
        t.duration = 8;
        t.conditions = { { 0, CondOp::Le, 0 } };
        ca.transitions.push_back(t);
    }
    const uint64_t ctrlHash = ctrlLib.Register(L"test.controller.json", ca);

    auto buildScene = [&](Scene& s) {
        GameObject go = s.CreateGameObjectTracked("Char");
        auto* acc = go.AddComponent<AnimatorControllerComponent>();
        acc->controller = AssetID{ ctrlHash };
        s.GetWorld().ApplyStructuralChanges();
        return go;
    };

    AnimatorControllerSystem sys;

    // ---- (1) 決定論: 同一シーン2個を同じ param スケジュールで走らせ per-tick ハッシュ一致 ----
    {
        Scene sa, sb;
        GameObject ga = buildScene(sa);
        GameObject gb = buildScene(sb);
        bool det = true;
        uint64_t finalHash = 0;
        for (int i = 0; i < 120 && det; ++i) {
            const int32_t p = (i >= 10 && i < 70) ? 1 : 0; // 10..69 は Walk 要求
            ga.GetComponent<AnimatorControllerComponent>()->params[0] = p;
            gb.GetComponent<AnimatorControllerComponent>()->params[0] = p;
            sys.Update(sa.GetWorld(), ctrlLib, animLib);
            sys.Update(sb.GetWorld(), ctrlLib, animLib);
            const uint64_t ha = HashWorld(sa.GetWorld());
            const uint64_t hb = HashWorld(sb.GetWorld());
            if (ha != hb) {
                det = false;
            }
            finalHash = ha;
        }
        check(det, "determinism: two controllers hash-identical for 120 ticks (param-driven)");
        MYE_LOG_INFO("  [ctrl] Idle<->Walk scene hash @120 = %016llX",
                     static_cast<unsigned long long>(finalHash));
    }

    // ---- (2) 機能: Idle 開始 → param>0 で Walk へ遷移 → param<=0 で Idle へ戻る ----
    {
        Scene s;
        GameObject go = buildScene(s);
        auto* acc = go.GetComponent<AnimatorControllerComponent>();
        acc->params[0] = 0;
        for (int i = 0; i < 5; ++i) {
            sys.Update(s.GetWorld(), ctrlLib, animLib);
        }
        check(acc->currentState == 0, "starts in Idle (state 0)");

        acc->params[0] = 1; // Walk 要求
        for (int i = 0; i < 20; ++i) { // duration 8 → 遷移完了
            sys.Update(s.GetWorld(), ctrlLib, animLib);
        }
        check(acc->currentState == 1, "transitioned to Walk on param>0");
        check(acc->transitionTo == -1, "transition completed (transitionTo cleared)");

        acc->params[0] = 0; // Idle 要求
        for (int i = 0; i < 20; ++i) {
            sys.Update(s.GetWorld(), ctrlLib, animLib);
        }
        check(acc->currentState == 0, "transitioned back to Idle on param<=0");
    }

    // ---- (3) クリップが実際にポーズを書いている (Idle の Y bob) ----
    {
        Scene s;
        GameObject go = buildScene(s);
        go.GetComponent<AnimatorControllerComponent>()->params[0] = 0; // Idle 維持
        for (int i = 0; i < 30; ++i) { // 最後の適用は stateTime=29 → y≈0.29
            sys.Update(s.GetWorld(), ctrlLib, animLib);
        }
        auto* lt = go.GetComponent<LocalTransform>();
        check(lt && std::fabs(lt->position.y) > 0.2f, "idle clip animates LocalTransform.position.y");
    }

    // ---- (M39a) clip サブ参照の GUID round-trip + 旧形式後方互換 ----
    {
        // 解決済み clipHash は数値 (GUID) で保存される
        const json cj = ControllerLibrary::ToJson(ca);
        const json& st0 = cj["states"][0];
        check(st0["clip"].is_number_unsigned(), "ToJson writes resolved clip as guid number");
        check(st0["clip"].get<uint64_t>() == idleH, "ToJson clip guid == clipHash");

        // 数値 clip の読み戻し: clipHash 直接 (パス解決不要)
        ControllerAsset back;
        check(ControllerLibrary::FromJson(cj, back), "FromJson accepts guid clip");
        check(back.states.size() == 2 && back.states[0].clipHash == idleH
                  && back.states[1].clipHash == walkH,
              "FromJson restores clipHash from guid");
        check(back.states[0].clipPath.empty(), "guid clip leaves legacy clipPath empty");

        // 旧形式 (文字列パス) の後方互換読み: clipPath に載り clipHash は未解決のまま
        json legacy = cj;
        legacy["states"][0]["clip"] = "idle.anim.json";
        ControllerAsset old;
        check(ControllerLibrary::FromJson(legacy, old), "FromJson accepts legacy path clip");
        check(old.states[0].clipPath == "idle.anim.json" && old.states[0].clipHash == 0,
              "legacy string clip -> clipPath (resolved later by LoadFromFile)");

        // 未解決 (clipHash==0) の保存は旧 clipPath を温存する (壊れた参照を消さない)
        const json rewritten = ControllerLibrary::ToJson(old);
        check(rewritten["states"][0]["clip"].is_string()
                  && rewritten["states"][0]["clip"].get<std::string>() == "idle.anim.json",
              "unresolved clip keeps legacy path on save");
    }

    // ---- (M85f) AnimatorPlay: 遷移開始 / 即切り替え / 拒否 / 名前引き ----
    {
        check(FindControllerState(ca, "Walk") == 1 && FindControllerState(ca, "Idle") == 0
                  && FindControllerState(ca, "Run") == -1 && FindControllerState(ca, "") == -1,
              "FindControllerState: by name, -1 when missing");

        Scene s;
        GameObject go = buildScene(s);
        World& world = s.GetWorld();
        auto* acc = go.GetComponent<AnimatorControllerComponent>();
        for (int i = 0; i < 5; ++i) {
            sys.Update(world, ctrlLib, animLib);
        }
        const EntityID e = go.Id();

        // 遷移: 立てた値が既存の遷移 (Update の 1.) と同じ形で、duration tick 後に currentState が替わる
        check(AnimatorPlay(world, e, 1, 8, ctrlLib), "AnimatorPlay duration>0 accepted");
        check(acc->transitionTo == 1 && acc->transitionTick == 0 && acc->transitionDuration == 8 && acc->transitionToTime == 0
                  && acc->currentState == 0,
              "AnimatorPlay duration>0 starts a transition without touching currentState");
        for (int i = 0; i < 7; ++i) {
            sys.Update(world, ctrlLib, animLib);
        }
        check(acc->currentState == 0 && acc->transitionTo == 1, "still blending 1 tick before the end");
        sys.Update(world, ctrlLib, animLib);
        check(acc->currentState == 1 && acc->transitionTo == -1 && acc->stateTimeTicks == 8,
              "transition completes after durationTicks (target time = durationTicks)");

        // 遷移中の再要求は遷移先だけ差し替えて混ぜ直す
        acc->params[0] = 1; // Walk の間は Walk → Idle の条件 (param <= 0) を偽に保つ
        check(AnimatorPlay(world, e, 0, 10, ctrlLib), "AnimatorPlay to Idle");
        sys.Update(world, ctrlLib, animLib);
        sys.Update(world, ctrlLib, animLib);
        check(AnimatorPlay(world, e, 1, 4, ctrlLib) && acc->transitionTo == 1 && acc->transitionTick == 0 && acc->transitionDuration == 4
                  && acc->currentState == 1,
              "AnimatorPlay during a transition retargets and restarts the blend");

        // 即切り替え: duration 0 は currentState を替えて再生位置・遷移を捨てる
        check(AnimatorPlay(world, e, 0, 0, ctrlLib), "AnimatorPlay duration 0 accepted");
        check(acc->currentState == 0 && acc->stateTimeTicks == 0 && acc->transitionTo == -1 && acc->transitionTick == 0
                  && acc->transitionDuration == 0 && acc->transitionToTime == 0,
              "AnimatorPlay duration 0 switches immediately and clears the transition");

        // 拒否: 範囲外 / Animator なし / controller 未登録。何も変えない
        const AnimatorControllerComponent before = *acc;
        check(!AnimatorPlay(world, e, 2, 8, ctrlLib) && !AnimatorPlay(world, e, -1, 0, ctrlLib)
                  && std::memcmp(&before, acc, sizeof(before)) == 0,
              "AnimatorPlay rejects an out-of-range state and leaves the component untouched");
        GameObject bare = s.CreateGameObjectTracked("Bare");
        world.ApplyStructuralChanges();
        check(!AnimatorPlay(world, bare.Id(), 0, 0, ctrlLib), "AnimatorPlay rejects an entity without an Animator");
        ControllerLibrary emptyLib;
        check(!AnimatorPlay(world, e, 0, 0, emptyLib), "AnimatorPlay rejects an unregistered controller");
    }

    // ---- (M89b) 骨クリップ: v2 の JSON / 名前で引く / 部分木の駆動 / 長さ / 凍結 / 旧経路へ戻る ----
    {
        // A と B は同じ名前のクリップを違う index・違う長さで持つ。C は Walk を持たない
        RenderResources res; // Init しない = GPU バッファを作らない
        const AssetID modelA =
            res.skinnedModels.Register("selftest_ctrl_a", MakeNamedClipModel({ { "Idle", 1.0f }, { "Walk", 0.5f } }));
        const AssetID modelB =
            res.skinnedModels.Register("selftest_ctrl_b", MakeNamedClipModel({ { "Walk", 0.75f }, { "Idle", 1.0f } }));
        const AssetID modelC = res.skinnedModels.Register("selftest_ctrl_c", MakeNamedClipModel({ { "Idle", 1.0f } }));
        const SkinnedModel* a = res.skinnedModels.Get(modelA);
        const SkinnedModel* b = res.skinnedModels.Get(modelB);
        check(a->FindClipByHash(HashStr("Walk")) == 1 && b->FindClipByHash(HashStr("Walk")) == 0
                  && res.skinnedModels.Get(modelC)->FindClipByHash(HashStr("Walk")) == -1
                  && a->FindClipByHash(HashStr("")) == -1,
              "FindClipByHash: by name per model, -1 when missing");

        ControllerAsset sk;
        sk.states.push_back({ "Idle", "", 0, 1, 1, "Idle", HashStr("Idle") });
        sk.states.push_back({ "Walk", "", 0, 1, 1, "Walk", HashStr("Walk") });
        sk.states.push_back({ "Prop", "", idleH, 1, 1 }); // 骨クリップの無いステート
        { // Idle → Walk (param0 == 1)
            ControllerTransition t;
            t.from = 0;
            t.to = 1;
            t.duration = 4;
            t.conditions = { { 0, CondOp::Eq, 1 } };
            sk.transitions.push_back(t);
        }
        { // Walk → Prop (param0 == 2、末尾で)
            ControllerTransition t;
            t.from = 1;
            t.to = 2;
            t.duration = 2;
            t.hasExitTime = 1;
            t.conditions = { { 0, CondOp::Eq, 2 } };
            sk.transitions.push_back(t);
        }
        const uint64_t skHash = ctrlLib.Register(L"skel.controller.json", sk);

        {
            const json j = ControllerLibrary::ToJson(sk);
            ControllerAsset back;
            check(j["controller"] == 2 && j["states"][1]["skel"]["clip"] == "Walk" && !j["states"][2].contains("skel")
                      && ControllerLibrary::FromJson(j, back) && back.states[1].skelClip == "Walk"
                      && back.states[1].skelClipHash == HashStr("Walk") && back.states[2].skelClip.empty()
                      && back.states[2].skelClipHash == 0,
                  "v2 JSON: the skeletal clip round-trips by name; states without one omit the key");
            json v1 = ControllerLibrary::ToJson(ca);
            v1["controller"] = 1;
            ControllerAsset old;
            check(ControllerLibrary::FromJson(v1, old) && old.states.size() == 2 && old.states[0].skelClipHash == 0
                      && old.states[0].clipHash == idleH,
                  "v1 JSON (no skel) still loads as property-only states");
        }

        Scene s;
        World& world = s.GetWorld();
        GameObject actor = s.CreateGameObjectTracked("Actor");
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ skHash };
        const auto addBody = [&](const char* name, GameObject parent, AssetID model) {
            GameObject go = s.CreateGameObjectTracked(name);
            go.SetParent(parent);
            go.AddComponent<SkinnedMeshComponent>()->model = model;
            return go;
        };
        GameObject bodyA = addBody("BodyA", actor, modelA); // 最初に作る = entity index 最小 = 主 SkinnedMesh
        GameObject bodyB = addBody("BodyB", actor, modelB);
        GameObject bodyC = addBody("BodyC", actor, modelC);
        GameObject rider = s.CreateGameObjectTracked("Rider");
        rider.SetParent(actor);
        rider.AddComponent<AnimatorControllerComponent>(); // controller 未設定 = 何もしないが、外側の駆動はここで止まる
        GameObject riderBody = addBody("RiderBody", rider, modelA);
        world.ApplyStructuralChanges();

        std::vector<EntityID> driven;
        CollectDrivenSkinnedMeshes(world, actor.Id(), driven);
        check(driven.size() == 3 && driven[0] == bodyA.Id() && driven[1] == bodyB.Id() && driven[2] == bodyC.Id(),
              "driven set: the subtree in pre-order, stopping at a nested AnimatorController");
        check(MainSkinnedModel(world, actor.Id(), &res.skinnedModels) == a, "main SkinnedMesh = smallest entity index");
        check(ControllerStateLengthTicks(sk.states[0], &animLib, a) == 60
                  && ControllerStateLengthTicks(sk.states[1], &animLib, a) == 30
                  && ControllerStateLengthTicks(sk.states[1], &animLib, b) == 45
                  && ControllerStateLengthTicks(sk.states[1], &animLib, nullptr) == 0
                  && ControllerStateLengthTicks(sk.states[2], &animLib, a) == 60,
              "ControllerStateLengthTicks: skeletal length from the given model, else the property clip, else 0");

        AnimatorControllerSystem skSys;
        SkinningSystem skinning;
        // 構造変更でアーキタイプが動くので、ポインタは毎回引き直す
        const auto ac = [&] { return actor.GetComponent<AnimatorControllerComponent>(); };
        const auto sm = [&](GameObject go) { return go.GetComponent<SkinnedMeshComponent>(); };
        const auto step = [&] {
            skSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            skinning.Update(world, res);
        };

        skSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
        check(sm(bodyA)->poseLayerCount == 1 && sm(bodyA)->poseClaim == 1 && sm(bodyA)->poseLayers[0].clip == 0
                  && sm(bodyA)->poseLayers[0].timeQ == 1 * SkinnedMeshComponent::kPoseTimeQPerTick
                  && sm(bodyA)->poseLayers[0].weightQ == SkinnedMeshComponent::kPoseWeightOne && ac()->stateTimeTicks == 1,
              "one state = one full-weight layer at the advanced time (timeQ = stateTimeTicks * 256)");
        check(sm(bodyB)->poseClaim == 1 && sm(bodyB)->poseLayers[0].clip == 1 && sm(bodyC)->poseLayers[0].clip == 0,
              "each mesh resolves the clip name against its own model");
        check(sm(riderBody)->poseLayerCount == 0 && sm(riderBody)->poseClaim == 0,
              "a nested AnimatorController's subtree is not driven by the outer one");
        sm(bodyA)->clip = 1; // 駆動中の clip 直書きは無視される
        skinning.Update(world, res);
        check(sm(bodyA)->poseClaim == 0 && sm(bodyA)->poseLayerCount == 1 && sm(bodyA)->timeTicks == 0
                  && sm(bodyA)->observedClip == 1,
              "SkinningSystem consumes the claim without advancing the legacy clock");

        for (int i = 0; i < 59; ++i) {
            step();
        }
        check(ac()->stateTimeTicks == 0 && sm(bodyA)->poseLayers[0].timeQ == 0,
              "the state loops at the main model's skeletal clip length (60 ticks)");

        ac()->params[0] = 1;
        step();
        check(ac()->transitionTo == 1 && ac()->transitionTick == 1 && sm(bodyA)->poseLayerCount == 2
                  && sm(bodyA)->poseLayers[0].clip == 0 && sm(bodyA)->poseLayers[1].clip == 1
                  && sm(bodyA)->poseLayers[0].weightQ == 49152 && sm(bodyA)->poseLayers[1].weightQ == 16384
                  && sm(bodyA)->poseLayers[1].timeQ == 256 && sm(bodyB)->poseLayers[0].clip == 1
                  && sm(bodyB)->poseLayers[1].clip == 0,
              "a transition writes from -> to layers with weights tick/duration in Q16 summing to 65536");
        for (int i = 0; i < 3; ++i) {
            step();
        }
        check(ac()->currentState == 1 && ac()->transitionTo == -1 && sm(bodyA)->poseLayerCount == 1
                  && sm(bodyA)->poseLayers[0].clip == 1 && sm(bodyA)->poseLayers[0].timeQ == 4 * 256
                  && sm(bodyA)->poseLayers[1].weightQ == 0,
              "after the transition: one layer of the target state, the unused layer is cleared");

        // 非アクティブの間は凍る (旧経路の時計も進まない)
        actor.AddComponent<ActiveComponent>()->enabled = false;
        world.ApplyStructuralChanges();
        const int32_t frozenTime = ac()->stateTimeTicks;
        const int32_t frozenQ = sm(bodyA)->poseLayers[0].timeQ;
        for (int i = 0; i < 5; ++i) {
            step();
        }
        check(ac()->stateTimeTicks == frozenTime && sm(bodyA)->poseLayerCount == 1
                  && sm(bodyA)->poseLayers[0].timeQ == frozenQ && sm(bodyA)->timeTicks == 0,
              "an inactive controller freezes its meshes: the claim is kept and neither clock advances");
        actor.GetComponent<ActiveComponent>()->enabled = true;

        // 末尾で抜ける遷移は主 SkinnedMesh の骨クリップの長さ (A の 30、B の 45 ではない) で判定する
        ac()->params[0] = 2;
        int32_t timeAtExit = -1;
        for (int i = 0; i < 40 && ac()->transitionTo != 2; ++i) {
            timeAtExit = ac()->stateTimeTicks;
            step();
        }
        check(ac()->transitionTo == 2 && timeAtExit == 29,
              "hasExitTime uses the main mesh's skeletal length (30 ticks), not another mesh's (45)");
        check(sm(bodyA)->poseLayerCount == 1 && sm(bodyA)->poseLayers[0].clip == 1
                  && sm(bodyA)->poseLayers[0].weightQ == SkinnedMeshComponent::kPoseWeightOne,
              "a transition into a state without a skeletal clip keeps the skeletal side at full weight");
        step();
        step();
        check(ac()->currentState == 2 && sm(bodyA)->poseLayerCount == 0 && sm(bodyA)->poseClaim == 0,
              "a state without a skeletal clip stops writing: the mesh falls back to the legacy path");

        // プロパティだけのコントローラは SkinnedMesh に触れない
        Scene s2;
        GameObject propActor = buildScene(s2);
        GameObject propBody = s2.CreateGameObjectTracked("Body");
        propBody.SetParent(propActor);
        propBody.AddComponent<SkinnedMeshComponent>()->model = modelA;
        s2.GetWorld().ApplyStructuralChanges();
        skSys.Update(s2.GetWorld(), ctrlLib, animLib, &res.skinnedModels);
        check(propBody.GetComponent<SkinnedMeshComponent>()->poseLayerCount == 0
                  && propBody.GetComponent<SkinnedMeshComponent>()->poseClaim == 0,
              "a property-only controller leaves SkinnedMesh on the legacy path");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Animator Controller self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Animator Controller self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
