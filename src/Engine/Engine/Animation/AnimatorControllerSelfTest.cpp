#include "Engine/Engine/Animation/AnimatorControllerSelfTest.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
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
#include "Engine/Engine/Script/EngineApiTable.h" // M89c: ABI v28 の Animator* スロット
#include "Engine/Core/Util/Hash.h"
#include "Shared/ScriptAPI.h" // MyeNameHash
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
        check(ControllerStateLengthTicks(sk, sk.states[0], nullptr, &animLib, a) == 60
                  && ControllerStateLengthTicks(sk, sk.states[1], nullptr, &animLib, a) == 30
                  && ControllerStateLengthTicks(sk, sk.states[1], nullptr, &animLib, b) == 45
                  && ControllerStateLengthTicks(sk, sk.states[1], nullptr, &animLib, nullptr) == 0
                  && ControllerStateLengthTicks(sk, sk.states[2], nullptr, &animLib, a) == 60,
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
        check(sm(bodyA)->poseLayers[0].prevTimeQ == 0 && sm(bodyA)->poseLayers[0].stepQ == 256,
              "M89f: a layer records the previous tick's time and this tick's step for render interpolation");
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
        check(sm(bodyA)->poseLayers[0].prevTimeQ == 59 * 256 && sm(bodyA)->poseLayers[0].stepQ == 256,
              "M89f: across the loop wrap the step stays +1 tick (prev + step = the clip end, not a jump back)");

        ac()->params[0] = 1;
        step();
        check(ac()->transitionTo == 1 && ac()->transitionTick == 1 && sm(bodyA)->poseLayerCount == 2
                  && sm(bodyA)->poseLayers[0].clip == 0 && sm(bodyA)->poseLayers[1].clip == 1
                  && sm(bodyA)->poseLayers[0].weightQ == 49152 && sm(bodyA)->poseLayers[1].weightQ == 16384
                  && sm(bodyA)->poseLayers[1].timeQ == 256 && sm(bodyB)->poseLayers[0].clip == 1
                  && sm(bodyB)->poseLayers[1].clip == 0,
              "a transition writes from -> to layers with weights tick/duration in Q16 summing to 65536");
        check(sm(bodyA)->poseLayers[1].prevTimeQ == 0 && sm(bodyA)->poseLayers[1].stepQ == 256
                  && sm(bodyA)->poseLayers[0].prevTimeQ == 0 && sm(bodyA)->poseLayers[0].stepQ == 256,
              "M89f: the target layer of a new transition interpolates from its start (0)");
        for (int i = 0; i < 3; ++i) {
            step();
        }
        check(ac()->currentState == 1 && ac()->transitionTo == -1 && sm(bodyA)->poseLayerCount == 1
                  && sm(bodyA)->poseLayers[0].clip == 1 && sm(bodyA)->poseLayers[0].timeQ == 4 * 256
                  && sm(bodyA)->poseLayers[1].weightQ == 0,
              "after the transition: one layer of the target state, the unused layer is cleared");
        check(sm(bodyA)->poseLayers[0].prevTimeQ == 3 * 256 && sm(bodyA)->poseLayers[0].stepQ == 256
                  && sm(bodyA)->poseLayers[1].stepQ == 0,
              "M89f: on the completing tick the surviving layer keeps the target's clock (no jump)");

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
        check(sm(bodyA)->poseLayers[0].stepQ == 0 && sm(bodyA)->poseLayers[0].prevTimeQ == frozenQ,
              "M89f: a frozen program is not interpolated (it would replay its last tick's motion every tick)");
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

    // ---- (M89c) 型付きパラメータ: JSON / 型ごとの比較 / Trigger の消費 / ABI v28 ----
    {
        using PT = ControllerParamType;
        const auto bitsOf = [](float f) { return std::bit_cast<int32_t>(f); };

        // JSON: v1 (型なし) は Int。型と Float の比較値が往復する。17 個目以降の宣言は捨てる
        {
            ControllerAsset v1;
            const json legacy = { { "states", json::array({ { { "name", "A" } } }) },
                                  { "parameters", json::array({ { { "name", "speed" } } }) },
                                  { "transitions", json::array({ { { "from", -1 }, { "to", 0 },
                                                                  { "conditions", json::array({ { { "param", 0 }, { "op", "gt" }, { "value", 2 } } }) } } }) } };
            check(ControllerLibrary::FromJson(legacy, v1) && v1.parameters.size() == 1 && v1.parameters[0].type == PT::Int
                      && v1.transitions[0].conditions[0].value == 2,
                  "M89c: a v1 parameter without a type reads as int (condition value stays an integer)");

            ControllerAsset typed;
            typed.states.push_back({ "A", "", 0, 1, 1 });
            typed.parameters = { { "speed", PT::Float }, { "grounded", PT::Bool }, { "attack", PT::Trigger }, { "mode", PT::Int } };
            ControllerTransition t;
            t.conditions = { { 0, CondOp::Ge, 0, 0.75f }, { 1, CondOp::Eq, 1 }, { 2, CondOp::Gt, 0 }, { 3, CondOp::Ne, -4 } };
            typed.transitions.push_back(t);
            const json j = ControllerLibrary::ToJson(typed);
            ControllerAsset back;
            const bool parsed = ControllerLibrary::FromJson(j, back);
            check(parsed && j["parameters"][0]["type"] == "float" && j["parameters"][2]["type"] == "trigger"
                      && j["transitions"][0]["conditions"][0]["value"].is_number_float()
                      && j["transitions"][0]["conditions"][3]["value"].is_number_integer(),
                  "M89c: ToJson writes the type and a float value only for float parameters");
            check(parsed && back.parameters.size() == 4 && back.parameters[0].type == PT::Float && back.parameters[1].type == PT::Bool
                      && back.parameters[2].type == PT::Trigger && back.parameters[3].type == PT::Int
                      && back.transitions[0].conditions[0].floatValue == 0.75f && back.transitions[0].conditions[3].value == -4,
                  "M89c: typed parameters and condition values round-trip");

            json many = j;
            many["parameters"] = json::array();
            for (int i = 0; i < AnimatorControllerComponent::kMaxParams + 1; ++i) {
                many["parameters"].push_back({ { "name", "p" + std::to_string(i) }, { "type", "int" } });
            }
            ControllerAsset capped;
            check(ControllerLibrary::FromJson(many, capped)
                      && capped.parameters.size() == static_cast<size_t>(AnimatorControllerComponent::kMaxParams),
                  "M89c: declarations beyond 16 are dropped (no value slot)");

            ControllerAsset dup;
            dup.parameters = { { "x", PT::Int }, { "x", PT::Float } };
            check(FindControllerParam(typed, HashStr("attack")) == 2 && FindControllerParam(typed, HashStr("none")) == -1
                      && FindControllerParam(dup, HashStr("x")) == 0 && ControllerParamTypeAt(typed, 9) == PT::Int
                      && ControllerParamTypeAt(typed, -1) == PT::Int && ControllerParamTypeAt(typed, 99) == PT::Int,
                  "M89c: FindControllerParam by name hash (first of duplicates), undeclared index is int");
        }

        // 遷移の評価: Float / Bool / Int (index 12) / Trigger。Trigger は採用した遷移だけが消費する
        ControllerAsset tc;
        tc.states = { { "Idle", "", 0, 1, 1 }, { "Walk", "", 0, 1, 1 }, { "Attack", "", 0, 1, 1 }, { "Jump", "", 0, 1, 1 },
                      { "Combo", "", 0, 1, 1 } };
        tc.parameters.resize(13);
        for (size_t i = 0; i < tc.parameters.size(); ++i) {
            tc.parameters[i] = { "unused" + std::to_string(i), PT::Int };
        }
        tc.parameters[0] = { "speed", PT::Float };
        tc.parameters[1] = { "grounded", PT::Bool };
        tc.parameters[2] = { "attack", PT::Trigger };
        tc.parameters[3] = { "jump", PT::Trigger };
        tc.parameters[12] = { "combo", PT::Int };
        const auto addTransition = [&](int32_t from, int32_t to, std::vector<ControllerCondition> conds) {
            ControllerTransition t;
            t.from = from;
            t.to = to;
            t.duration = 2;
            t.conditions = std::move(conds);
            tc.transitions.push_back(std::move(t));
        };
        addTransition(0, 1, { { 0, CondOp::Gt, 0, 0.5f } });                 // Idle → Walk: speed > 0.5
        addTransition(1, 2, { { 2, CondOp::Gt, 0 } });                       // Walk → Attack: attack
        addTransition(2, 3, { { 3, CondOp::Gt, 0 }, { 1, CondOp::Eq, 1 } }); // Attack → Jump: jump かつ grounded
        addTransition(3, 4, { { 12, CondOp::Ge, 3 } });                      // Jump → Combo: combo >= 3
        const uint64_t tcHash = ctrlLib.Register(L"typed.controller.json", tc);

        Scene s;
        GameObject go = s.CreateGameObjectTracked("Typed");
        go.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ tcHash };
        s.GetWorld().ApplyStructuralChanges();
        World& world = s.GetWorld();
        auto* ac = go.GetComponent<AnimatorControllerComponent>();
        const auto step = [&] { sys.Update(world, ctrlLib, animLib); };
        const auto settle = [&] {
            for (int i = 0; i < 4; ++i) {
                step();
            }
        };

        ac->params[0] = bitsOf(0.5f);
        step();
        check(ac->transitionTo == -1, "M89c: float condition speed > 0.5 is false at exactly 0.5");
        ac->params[0] = bitsOf(0.625f);
        step();
        check(ac->transitionTo == 1, "M89c: float condition compares the float value, not the raw bits");
        // 遷移中に立てた Trigger は、遷移の判定が走らないので残る
        ac->params[2] = 1;
        step();
        check(ac->params[2] == 1, "M89c: a trigger set while no transition is evaluated stays set");
        settle();
        check(ac->currentState == 2 && ac->params[2] == 0, "M89c: the adopted transition consumes its trigger");

        ac->params[3] = 1; // jump は立てたが grounded が偽 = 採用されない
        settle();
        check(ac->currentState == 2 && ac->params[3] == 1, "M89c: a trigger is not consumed when its transition is not adopted (bool false)");
        ac->params[1] = 1;
        settle();
        check(ac->currentState == 3 && ac->params[3] == 0 && ac->params[1] == 1,
              "M89c: bool condition true adopts the transition; only the trigger is consumed");
        ac->params[12] = 2;
        settle();
        check(ac->currentState == 3, "M89c: int condition at index 12 (beyond the old 4) is evaluated (2 < 3)");
        ac->params[12] = 3;
        settle();
        check(ac->currentState == 4, "M89c: int condition at index 12 adopts at combo >= 3");

        // params[4..15] もハッシュに入る (シーン保存・replay の対象)
        const uint64_t before = HashWorld(world);
        ac->params[15] = 7;
        check(HashWorld(world) != before, "M89c: params[15] is part of the world hash");
        ac->params[15] = 0;

        // ABI v28
        ScriptApiContext apiCtx;
        apiCtx.scene = &s;
        apiCtx.controllers = &ctrlLib;
        MyeEngineApi api = {};
        BuildEngineApi(api, &apiCtx);
        const MyeEntityId self = { go.Id().index, go.Id().generation };
        const MyeEntityId bare = [&] {
            const EntityID e = s.CreateGameObjectTracked("Bare").Id();
            return MyeEntityId{ e.index, e.generation };
        }();
        world.ApplyStructuralChanges();
        ac = go.GetComponent<AnimatorControllerComponent>();
        check(MyeNameHash("speed") == HashStr("speed"), "M89c ABI: (premise) MyeNameHash == HashStr");
        check(api.AnimatorSetFloat(api.engine, self, MyeNameHash("speed"), 1.25f) == 1 && ac->params[0] == bitsOf(1.25f),
              "M89c ABI: AnimatorSetFloat writes the float bits");
        const int32_t speedBits = ac->params[0];
        check(api.AnimatorSetFloat(api.engine, self, MyeNameHash("speed"), std::nanf("")) == 0
                  && api.AnimatorSetFloat(api.engine, self, MyeNameHash("speed"), INFINITY) == 0 && ac->params[0] == speedBits,
              "M89c ABI: AnimatorSetFloat rejects non-finite values");
        check(api.AnimatorSetInt(api.engine, self, MyeNameHash("speed"), 3) == 0 && api.AnimatorSetFloat(api.engine, self, MyeNameHash("combo"), 1.0f) == 0
                  && api.AnimatorSetBool(api.engine, self, MyeNameHash("attack"), 1) == 0 && api.AnimatorSetTrigger(api.engine, self, MyeNameHash("grounded")) == 0
                  && ac->params[0] == speedBits && ac->params[1] == 1 && ac->params[2] == 0 && ac->params[12] == 3,
              "M89c ABI: a setter of the wrong type returns 0 and writes nothing");
        check(api.AnimatorSetInt(api.engine, self, MyeNameHash("combo"), -9) == 1 && ac->params[12] == -9
                  && api.AnimatorSetBool(api.engine, self, MyeNameHash("grounded"), 0) == 1 && ac->params[1] == 0
                  && api.AnimatorSetBool(api.engine, self, MyeNameHash("grounded"), 5) == 1 && ac->params[1] == 1
                  && api.AnimatorSetTrigger(api.engine, self, MyeNameHash("jump")) == 1 && ac->params[3] == 1,
              "M89c ABI: SetInt / SetBool (normalized to 0/1) / SetTrigger write by name");
        check(api.AnimatorSetInt(api.engine, self, MyeNameHash("nope"), 1) == 0 && api.AnimatorSetInt(api.engine, bare, MyeNameHash("combo"), 1) == 0,
              "M89c ABI: unknown name / entity without an Animator returns 0");

        MyeAnimatorParam p = {};
        const bool gotFloat = api.AnimatorGetParam(api.engine, self, MyeNameHash("speed"), &p) == 1 && p.type == MYE_ANIM_PARAM_FLOAT && p.f == 1.25f && p.i == 0;
        const bool gotInt = api.AnimatorGetParam(api.engine, self, MyeNameHash("combo"), &p) == 1 && p.type == MYE_ANIM_PARAM_INT && p.i == -9;
        const bool gotTrigger = api.AnimatorGetParam(api.engine, self, MyeNameHash("jump"), &p) == 1 && p.type == MYE_ANIM_PARAM_TRIGGER && p.i == 1;
        p = MyeAnimatorParam{ 77, 77, 77.0f };
        const bool missUntouched = api.AnimatorGetParam(api.engine, self, MyeNameHash("nope"), &p) == 0 && p.type == 77;
        check(gotFloat && gotInt && gotTrigger && missUntouched, "M89c ABI: AnimatorGetParam returns type and value; a miss leaves out untouched");

        MyeAnimatorState st = {};
        const bool gotState = api.AnimatorGetState(api.engine, self, &st) == 1;
        check(gotState && st.stateIndex == 4 && st.stateNameHash == MyeNameHash("Combo") && st.transitionTo == -1
                  && st.transitionToNameHash == 0 && st.stateTimeTicks == ac->stateTimeTicks,
              "M89c ABI: AnimatorGetState reports the current state by index and name hash");
        check(AnimatorPlay(world, go.Id(), 0, 5, ctrlLib) && api.AnimatorGetState(api.engine, self, &st) == 1 && st.transitionTo == 0
                  && st.transitionToNameHash == MyeNameHash("Idle") && st.transitionDuration == 5,
              "M89c ABI: AnimatorGetState reports an ongoing transition");
        check(api.AnimatorGetState(api.engine, bare, &st) == 0, "M89c ABI: AnimatorGetState on an entity without an Animator returns 0");

        check(api.SetAnimatorParam(api.engine, self, 15, 42) == 1 && ac->params[15] == 42 && api.SetAnimatorParam(api.engine, self, 16, 1) == 0,
              "M89c ABI: the legacy SetAnimatorParam reaches index 15 and rejects 16");

        ScriptApiContext noLibCtx;
        noLibCtx.scene = &s;
        MyeEngineApi noLib = {};
        BuildEngineApi(noLib, &noLibCtx);
        check(noLib.AnimatorSetInt(noLib.engine, self, MyeNameHash("combo"), 1) == 0 && noLib.AnimatorGetParam(noLib.engine, self, MyeNameHash("combo"), &p) == 0
                  && noLib.AnimatorGetState(noLib.engine, self, &st) == 0,
              "M89c ABI: without a controller library every animator slot returns 0");
    }

    // ---- (M89d) 1D ブレンドツリー: 重み / JSON / 位相同期 / 遷移 / hasExitTime / 非ループ / 再開 ----
    {
        using PT = ControllerParamType;
        const auto bitsOf = [](float f) { return std::bit_cast<int32_t>(f); };
        constexpr int32_t kOne = SkinnedMeshComponent::kPoseWeightOne;

        // 子は閾値の昇順に並べない (index 0 = Run@2, 1 = Idle@0, 2 = Walk@1)
        ControllerState move;
        move.name = "Move";
        move.blendType = ControllerBlendType::Blend1D;
        move.blendParam = 0;
        move.blendChildren = { { "Run", HashStr("Run"), 2.0f }, { "Idle", HashStr("Idle"), 0.0f },
                               { "Walk", HashStr("Walk"), 1.0f } };

        // 重み (純関数)
        {
            BlendChildWeight w[kMaxBlendLayers];
            const auto is1 = [&](float x, int32_t child) {
                return ComputeBlendWeights(move, x, 0.0f, w) == 1 && w[0].child == child && w[0].weightQ == kOne;
            };
            const auto is2 = [&](float x, int32_t c0, int32_t q0, int32_t c1, int32_t q1) {
                return ComputeBlendWeights(move, x, 0.0f, w) == 2 && w[0].child == c0 && w[0].weightQ == q0 && w[1].child == c1
                       && w[1].weightQ == q1;
            };
            check(is1(-1.0f, 1) && is1(0.0f, 1) && is1(1.0f, 2) && is1(2.0f, 0) && is1(9.0f, 0),
                  "M89d: at a threshold or outside the ends, one child at full weight");
            check(is2(0.5f, 1, 32768, 2, 32768) && is2(1.5f, 0, 32768, 2, 32768) && is2(0.25f, 1, 49152, 2, 16384),
                  "M89d: between thresholds, the adjacent pair is mixed linearly (children in index order)");
            check(is2(1.0f / 3.0f, 1, 43691, 2, 21845),
                  "M89d: the truncation remainder goes to the heaviest child (43690 + 1)");
            check(is1(std::nanf(""), 1), "M89d: NaN selects the lowest threshold");
            bool sweepOk = true;
            for (int i = -100; i <= 500 && sweepOk; ++i) {
                const int32_t n = ComputeBlendWeights(move, static_cast<float>(i) * 0.01f, 0.0f, w);
                int32_t sum = 0;
                for (int32_t k = 0; k < n; ++k) {
                    sweepOk = sweepOk && w[k].weightQ > 0 && (k == 0 || w[k].child > w[k - 1].child);
                    sum += w[k].weightQ;
                }
                sweepOk = sweepOk && n >= 1 && n <= 2 && sum == kOne;
            }
            check(sweepOk, "M89d: sweep -1..5: 1 or 2 children, positive weights summing to exactly 65536");
            ControllerState dup = move;
            dup.blendChildren = { { "A", HashStr("A"), 1.0f }, { "B", HashStr("B"), 1.0f } };
            ControllerState empty = move;
            empty.blendChildren.clear();
            check(ComputeBlendWeights(dup, 1.0f, 0.0f, w) == 1 && w[0].child == 0 && ComputeBlendWeights(empty, 1.0f, 0.0f, w) == 0
                      && !StateDrivesSkeleton(empty) && StateDrivesSkeleton(move),
                  "M89d: equal thresholds pick the first child; a tree without children drives nothing");
            check(BlendPhaseToTimeQ(0, 60, 1) == 0 && BlendPhaseToTimeQ(0x80000000u, 60, 1) == 30 * 256
                      && BlendPhaseToTimeQ(UINT32_MAX, 60, 1) == 60 * 256 - 1 && BlendPhaseToTimeQ(UINT32_MAX, 60, 0) == 60 * 256
                      && BlendPhaseToTimeQ(0x80000000u, 0, 1) == 0,
                  "M89d: phase -> timeQ (a stuck non-loop phase is exactly the end)");
        }

        // JSON
        {
            ControllerAsset ja;
            ja.states.push_back(move);
            const json j = ControllerLibrary::ToJson(ja);
            ControllerAsset back;
            const bool parsed = ControllerLibrary::FromJson(j, back);
            check(parsed && j["states"][0]["skel"]["blend1d"]["param"] == 0
                      && j["states"][0]["skel"]["blend1d"]["children"][0]["clip"] == "Run" && !j["states"][0]["skel"].contains("clip")
                      && back.states[0].blendType == ControllerBlendType::Blend1D && back.states[0].blendChildren.size() == 3
                      && back.states[0].blendChildren[1].clip == "Idle" && back.states[0].blendChildren[1].clipHash == HashStr("Idle")
                      && back.states[0].blendChildren[0].threshold == 2.0f,
                  "M89d: blend1d round-trips in the original child order");
            json both = j;
            both["states"][0]["skel"]["clip"] = "Walk";
            ControllerAsset bothBack;
            check(ControllerLibrary::FromJson(both, bothBack) && bothBack.states[0].blendType == ControllerBlendType::Blend1D
                      && bothBack.states[0].skelClipHash == 0,
                  "M89d: blend1d wins over a single clip in the same skel object");
        }

        // システム: Idle (骨 1 本) → Move (Walk/Run) → Idle、非ループの Once
        RenderResources res;
        const AssetID modelA = res.skinnedModels.Register(
            "selftest_blend_a", MakeNamedClipModel({ { "Idle", 1.0f }, { "Walk", 1.0f }, { "Run", 0.5f } }));
        const AssetID modelB =
            res.skinnedModels.Register("selftest_blend_b", MakeNamedClipModel({ { "Run", 1.0f }, { "Walk", 2.0f } }));
        const SkinnedModel* a = res.skinnedModels.Get(modelA);

        ControllerAsset bc;
        bc.parameters = { { "speed", PT::Float }, { "stop", PT::Bool } };
        bc.states.push_back({ "Idle", "", 0, 1, 1, "Idle", HashStr("Idle") });
        bc.states.push_back(move);
        ControllerState once = move;
        once.name = "Once";
        once.loop = 0;
        bc.states.push_back(once);
        { // Idle → Move (speed > 0.5)
            ControllerTransition t;
            t.from = 0;
            t.to = 1;
            t.duration = 3;
            t.conditions = { { 0, CondOp::Gt, 0, 0.5f } };
            bc.transitions.push_back(t);
        }
        { // Move → Idle (stop、周の終わりで)
            ControllerTransition t;
            t.from = 1;
            t.to = 0;
            t.duration = 1;
            t.hasExitTime = 1;
            t.conditions = { { 1, CondOp::Eq, 1 } };
            bc.transitions.push_back(t);
        }
        const uint64_t bcHash = ctrlLib.Register(L"blend.controller.json", bc);

        struct Rig {
            GameObject actor;
            GameObject bodyA;
            GameObject bodyB;
        };
        const auto makeRig = [&](Scene& s) {
            Rig r;
            r.actor = s.CreateGameObjectTracked("Actor");
            r.actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ bcHash };
            r.bodyA = s.CreateGameObjectTracked("BodyA"); // 主 SkinnedMesh
            r.bodyA.SetParent(r.actor);
            r.bodyA.AddComponent<SkinnedMeshComponent>()->model = modelA;
            r.bodyB = s.CreateGameObjectTracked("BodyB");
            r.bodyB.SetParent(r.actor);
            r.bodyB.AddComponent<SkinnedMeshComponent>()->model = modelB;
            s.GetWorld().ApplyStructuralChanges();
            return r;
        };
        Scene s;
        World& world = s.GetWorld();
        Rig rig = makeRig(s);
        AnimatorControllerSystem bsys;
        SkinningSystem skinning;
        const auto ac = [&] { return rig.actor.GetComponent<AnimatorControllerComponent>(); };
        const auto smA = [&] { return rig.bodyA.GetComponent<SkinnedMeshComponent>(); };
        const auto smB = [&] { return rig.bodyB.GetComponent<SkinnedMeshComponent>(); };
        const auto step = [&] {
            bsys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            skinning.Update(world, res);
        };
        // Walk (60) と Run (30) を半々 = Σ(wQ·L) = 32768·90。Δ = 2^48 / Σ
        constexpr uint32_t kDelta = 95443717u;
        const auto timeQOf = [](uint32_t phase, int64_t ticks) {
            return static_cast<int32_t>((static_cast<uint64_t>(phase) * static_cast<uint64_t>(ticks * 256)) >> 32);
        };

        step();
        ac()->params[0] = bitsOf(1.5f);
        check(ControllerStateLengthTicks(bc, bc.states[1], ac()->params, &animLib, a) == 45
                  && ControllerStateLengthTicks(bc, bc.states[1], nullptr, &animLib, a) == 60
                  && ControllerStateLengthTicks(bc, bc.states[1], ac()->params, &animLib, nullptr) == 0,
              "M89d: blend length = weighted mean of the children at the current parameter (45 = (60+30)/2)");
        step();
        {
            const SkinnedMeshComponent* m = smA();
            const int32_t sum = m->poseLayers[0].weightQ + m->poseLayers[1].weightQ + m->poseLayers[2].weightQ;
            check(ac()->transitionTo == 1 && ac()->transitionToPhase == kDelta && m->poseLayerCount == 3
                      && m->poseLayers[0].clip == 0 && m->poseLayers[0].weightQ == 43692 && m->poseLayers[1].clip == 2
                      && m->poseLayers[1].weightQ == 10922 && m->poseLayers[2].clip == 1 && m->poseLayers[2].weightQ == 10922
                      && sum == kOne,
                  "M89d: single -> blend transition: from layer + the target's children, remainder to the heaviest (sum 65536)");
            check(m->poseLayers[1].timeQ == timeQOf(kDelta, 30) && m->poseLayers[2].timeQ == timeQOf(kDelta, 60)
                      && smB()->poseLayers[0].clip == -1 && smB()->poseLayers[1].clip == 0
                      && smB()->poseLayers[1].timeQ == timeQOf(kDelta, 60) && smB()->poseLayers[2].clip == 1
                      && smB()->poseLayers[2].timeQ == timeQOf(kDelta, 120),
                  "M89d: each mesh turns the shared phase into time with its own clip lengths");
        }
        step();
        step();
        check(ac()->currentState == 1 && ac()->transitionTo == -1 && ac()->statePhase == 3 * kDelta && ac()->transitionToPhase == 0
                  && smA()->poseLayerCount == 2 && smA()->poseLayers[0].weightQ == 32768 && smA()->poseLayers[1].weightQ == 32768
                  && smA()->poseLayers[2].weightQ == 0,
              "M89d: after the transition the phase carries over; two children at half weight");
        bool synced = true;
        // M89f: 各層の prevTimeQ は前 tick の timeQ、prevTimeQ + stepQ は今の timeQ (折り返した tick は 1 周先)
        bool continuous = true;
        bool wrapped = false;
        for (int i = 0; i < 45; ++i) { // 1 周 = 45 tick なので、途中で位相の折り返しを 1 回踏む
            const int32_t lastQ[2] = { smA()->poseLayers[0].timeQ, smA()->poseLayers[1].timeQ };
            step();
            const int32_t runQ = smA()->poseLayers[0].timeQ;
            const int32_t walkQ = smA()->poseLayers[1].timeQ;
            synced = synced && walkQ - 2 * runQ >= 0 && walkQ - 2 * runQ <= 1;
            const int32_t spanQ[2] = { 30 * 256, 60 * 256 };
            for (int k = 0; k < 2; ++k) {
                const SkinnedMeshComponent::PoseLayer& la = smA()->poseLayers[k];
                const int32_t end = la.prevTimeQ + la.stepQ;
                wrapped = wrapped || end == la.timeQ + spanQ[k];
                continuous = continuous && la.prevTimeQ == lastQ[k] && la.stepQ > 0
                             && (end == la.timeQ || end == la.timeQ + spanQ[k]);
            }
        }
        check(synced, "M89d: phase sync: Walk (60) is always at twice Run's (30) time");
        check(continuous && wrapped,
              "M89f: blend layers interpolate from the previous tick's time, unwrapping the phase wrap");

        // Move → Idle は位相が 1 周に達する tick にだけ抜ける
        ac()->params[1] = 1;
        bool earlyOk = true;
        bool exitOk = false;
        for (int i = 0; i < 60; ++i) {
            const uint64_t before = ac()->statePhase;
            step();
            if (ac()->currentState == 0) {
                exitOk = before + kDelta >= (uint64_t(1) << 32);
                break;
            }
            earlyOk = earlyOk && before + kDelta < (uint64_t(1) << 32);
        }
        check(earlyOk && exitOk && ac()->statePhase == 0 && smA()->poseLayerCount == 1 && smA()->poseLayers[0].clip == 0,
              "M89d: hasExitTime leaves a blend state on the tick its phase completes a cycle");

        // 非ループは末尾に張り付き、各子のクリップの末尾ちょうどを指す
        check(AnimatorPlay(world, rig.actor.Id(), 2, 0, ctrlLib) && ac()->statePhase == 0, "M89d: AnimatorPlay resets the phase");
        for (int i = 0; i < 50; ++i) {
            step();
        }
        check(ac()->statePhase == UINT32_MAX && smA()->poseLayers[0].timeQ == 30 * 256 && smA()->poseLayers[1].timeQ == 60 * 256,
              "M89d: a non-loop blend state stops at the end of every child");
        check(smA()->poseLayers[0].stepQ == 0 && smA()->poseLayers[1].stepQ == 0,
              "M89f: a non-loop blend state stuck at its end has no step (no interpolation past the end)");

        // 遷移の途中でコンポーネントを写した別シーンが、同じ続きを辿る (snapshot はこの生バイトを運ぶ)
        {
            Scene s2;
            Rig rig2 = makeRig(s2);
            AnimatorControllerSystem sys2;
            SkinningSystem skinning2;
            AnimatorPlay(world, rig.actor.Id(), 0, 0, ctrlLib);
            ac()->params[1] = 0;
            ac()->params[0] = bitsOf(0.75f);
            step();
            step();
            *rig2.actor.GetComponent<AnimatorControllerComponent>() = *ac();
            bool same = true;
            for (int i = 0; i < 20; ++i) {
                ac()->params[0] = bitsOf(0.75f + 0.05f * static_cast<float>(i));
                rig2.actor.GetComponent<AnimatorControllerComponent>()->params[0] = ac()->params[0];
                step();
                sys2.Update(s2.GetWorld(), ctrlLib, animLib, &res.skinnedModels);
                skinning2.Update(s2.GetWorld(), res);
                const SkinnedMeshComponent* m2 = rig2.bodyA.GetComponent<SkinnedMeshComponent>();
                same = same && std::memcmp(ac(), rig2.actor.GetComponent<AnimatorControllerComponent>(), sizeof(AnimatorControllerComponent)) == 0
                       && std::memcmp(smA()->poseLayers, m2->poseLayers, sizeof(m2->poseLayers)) == 0;
            }
            check(same, "M89d: resuming from a component copied mid-transition gives identical phases and programs");
        }

        const uint64_t before = HashWorld(world);
        ac()->statePhase ^= 1u;
        check(HashWorld(world) != before, "M89d: statePhase is part of the world hash");
    }

    // ---- (M89e) 2D ブレンドツリー: 重み / 上位 4 本 / 異常値 / JSON / paramY の配線 ----
    {
        using PT = ControllerParamType;
        const auto bitsOf = [](float f) { return std::bit_cast<int32_t>(f); };
        constexpr int32_t kOne = SkinnedMeshComponent::kPoseWeightOne;
        const auto child2 = [](const char* clip, float x, float y) {
            ControllerBlendChild c;
            c.clip = clip;
            c.clipHash = HashStr(clip);
            c.posX = x;
            c.posY = y;
            return c;
        };

        // 十字 + 中央。index 5 は index 1 と同じ位置 (使われないこと)
        ControllerState cross;
        cross.name = "Locomotion";
        cross.blendType = ControllerBlendType::Blend2D;
        cross.blendParam = 0;
        cross.blendParamY = 1;
        cross.blendChildren = { child2("Idle", 0.0f, 0.0f),  child2("Fwd", 0.0f, 1.0f),   child2("Back", 0.0f, -1.0f),
                                child2("Left", -1.0f, 0.0f), child2("Right", 1.0f, 0.0f), child2("FwdDup", 0.0f, 1.0f) };
        {
            BlendChildWeight w[kMaxBlendLayers];
            const auto is1 = [&](float x, float y, int32_t child) {
                return ComputeBlendWeights(cross, x, y, w) == 1 && w[0].child == child && w[0].weightQ == kOne;
            };
            check(is1(0.0f, 0.0f, 0) && is1(0.0f, 1.0f, 1) && is1(0.0f, -1.0f, 2) && is1(-1.0f, 0.0f, 3) && is1(1.0f, 0.0f, 4),
                  "M89e: at a child's position that child is at full weight (the duplicate never wins)");
            check(ComputeBlendWeights(cross, 0.5f, 0.0f, w) == 2 && w[0].child == 0 && w[0].weightQ == 32768 && w[1].child == 4
                      && w[1].weightQ == 32768,
                  "M89e: halfway between Idle and Right mixes exactly those two (h = 0.5 / 0.5)");
            check(ComputeBlendWeights(cross, 0.0f, 0.5f, w) == 2 && w[0].child == 0 && w[1].child == 1 && w[1].weightQ == 32768,
                  "M89e: y moves toward Fwd (the duplicate at the same position stays out)");
            check(is1(std::nanf(""), 0.0f, 0) && is1(0.0f, std::numeric_limits<float>::infinity(), 0),
                  "M89e: a non-finite x or y gives child 0 at full weight");
            bool sweepOk = true;
            for (int iy = -20; iy <= 20 && sweepOk; ++iy) {
                for (int ix = -20; ix <= 20 && sweepOk; ++ix) {
                    const int32_t n = ComputeBlendWeights(cross, static_cast<float>(ix) * 0.1f, static_cast<float>(iy) * 0.1f, w);
                    int32_t sum = 0;
                    for (int32_t k = 0; k < n; ++k) {
                        sweepOk = sweepOk && w[k].weightQ > 0 && w[k].child != 5 && (k == 0 || w[k].child > w[k - 1].child);
                        sum += w[k].weightQ;
                    }
                    sweepOk = sweepOk && n >= 1 && n <= kMaxBlendLayers && sum == kOne;
                }
            }
            check(sweepOk, "M89e: sweep -2..2 x -2..2: 1..4 children in index order, positive weights summing to exactly 65536");

            // 六角形の中心では 6 本とも h ≈ 0.5 → 上位 4 本に絞る
            ControllerState hex = cross;
            hex.blendChildren = { child2("A", 1.0f, 0.0f),     child2("B", 0.5f, 0.875f),   child2("C", -0.5f, 0.875f),
                                  child2("D", -1.0f, 0.0f),    child2("E", -0.5f, -0.875f), child2("F", 0.5f, -0.875f) };
            int32_t hexSum = 0;
            const int32_t hexN = ComputeBlendWeights(hex, 0.0f, 0.0f, w);
            for (int32_t k = 0; k < hexN; ++k) {
                hexSum += w[k].weightQ;
            }
            check(hexN == kMaxBlendLayers && hexSum == kOne, "M89e: more than 4 influential children are cut to the top 4");

            ControllerState one = cross;
            one.blendChildren = { child2("Solo", 3.0f, -2.0f) };
            check(ComputeBlendWeights(one, -7.0f, 9.0f, w) == 1 && w[0].child == 0 && w[0].weightQ == kOne,
                  "M89e: a single child is always at full weight");
        }

        // JSON
        {
            ControllerAsset ja;
            ja.states.push_back(cross);
            const json j = ControllerLibrary::ToJson(ja);
            ControllerAsset back;
            const bool parsed = ControllerLibrary::FromJson(j, back);
            check(parsed && j["states"][0]["skel"]["blend2d"]["paramX"] == 0 && j["states"][0]["skel"]["blend2d"]["paramY"] == 1
                      && j["states"][0]["skel"]["blend2d"]["children"][3]["x"] == -1.0f
                      && back.states[0].blendType == ControllerBlendType::Blend2D && back.states[0].blendParamY == 1
                      && back.states[0].blendChildren.size() == 6 && back.states[0].blendChildren[2].posY == -1.0f
                      && back.states[0].blendChildren[4].clipHash == HashStr("Right"),
                  "M89e: blend2d round-trips (paramX / paramY / child positions)");
            json both = j;
            both["states"][0]["skel"]["blend1d"] = { { "param", 2 }, { "children", json::array() } };
            ControllerAsset bothBack;
            check(ControllerLibrary::FromJson(both, bothBack) && bothBack.states[0].blendType == ControllerBlendType::Blend2D
                      && bothBack.states[0].blendParam == 0,
                  "M89e: blend2d wins over blend1d in the same skel object");
        }

        // システム: x と y の両方のパラメータが層と長さに届く
        RenderResources res;
        const AssetID model = res.skinnedModels.Register(
            "selftest_blend2d", MakeNamedClipModel({ { "Idle", 1.0f }, { "Fwd", 1.0f }, { "Right", 0.5f } }));
        const SkinnedModel* m = res.skinnedModels.Get(model);
        ControllerAsset bc;
        bc.parameters = { { "x", PT::Float }, { "y", PT::Float } };
        bc.states.push_back(cross);
        const uint64_t bcHash = ctrlLib.Register(L"blend2d.controller.json", bc);

        Scene s;
        World& world = s.GetWorld();
        GameObject actor = s.CreateGameObjectTracked("Actor");
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ bcHash };
        actor.AddComponent<SkinnedMeshComponent>()->model = model;
        world.ApplyStructuralChanges();
        AnimatorControllerSystem bsys;
        SkinningSystem skinning;
        auto* ac = actor.GetComponent<AnimatorControllerComponent>();
        ac->params[0] = bitsOf(0.5f);
        ac->params[1] = bitsOf(0.0f);
        check(ControllerStateLengthTicks(bc, bc.states[0], ac->params, &animLib, m) == 45,
              "M89e: blend2d length = weighted mean (Idle 60 / Right 30 at x = 0.5)");
        bsys.Update(world, ctrlLib, animLib, &res.skinnedModels);
        skinning.Update(world, res);
        const SkinnedMeshComponent* sm = actor.GetComponent<SkinnedMeshComponent>();
        check(sm->poseLayerCount == 2 && sm->poseLayers[0].clip == 0 && sm->poseLayers[0].weightQ == 32768
                  && sm->poseLayers[1].clip == 2 && sm->poseLayers[1].weightQ == 32768,
              "M89e: x = 0.5 drives Idle + Right layers");
        ac->params[0] = bitsOf(0.0f);
        ac->params[1] = bitsOf(0.5f);
        check(ControllerStateLengthTicks(bc, bc.states[0], ac->params, &animLib, m) == 60,
              "M89e: the y parameter reaches the length (Idle 60 / Fwd 60)");
        bsys.Update(world, ctrlLib, animLib, &res.skinnedModels);
        skinning.Update(world, res);
        check(sm->poseLayerCount == 2 && sm->poseLayers[0].clip == 0 && sm->poseLayers[1].clip == 1
                  && sm->poseLayers[1].weightQ == 32768,
              "M89e: y = 0.5 drives Idle + Fwd layers");
    }

    // ---- (M89h) アニメイベント: 発火規則の表 / JSON / システム (入った tick・折り返し・遷移・minWeight・ブレンド) ----
    {
        // 発火規則の表 (計画の「イベントの発火規則」の全行)。返り値は old からの距離、-1 = 発火しない
        struct PassRow {
            int64_t pos, old, step, cycle;
            int32_t loop;
            bool entered;
            int64_t expect;
            const char* what;
        };
        const PassRow rows[] = {
            { 5, 4, 1, 10, 1, false, 1, "forward: (old, new] includes new" },
            { 4, 4, 1, 10, 1, false, -1, "forward: (old, new] excludes old" },
            { 6, 4, 1, 10, 1, false, -1, "forward: beyond new" },
            { 0, 0, 1, 10, 1, true, 0, "entered tick: [0, new] includes 0" },
            { 1, 0, 1, 10, 1, true, 1, "entered tick: [0, new] includes new" },
            { 0, 0, 1, 10, 1, false, -1, "not entered: 0 was passed by the previous tick" },
            { 0, 0, 0, 10, 1, true, 0, "entered with speed 0: position 0 still fires once" },
            { 3, 3, 0, 10, 1, false, -1, "speed 0: nothing" },
            { 0, 9, 2, 10, 1, false, 1, "loop wrap: [0, new] side" },
            { 1, 9, 2, 10, 1, false, 2, "loop wrap: new itself" },
            { 2, 9, 2, 10, 1, false, -1, "loop wrap: beyond new" },
            { 9, 9, 2, 10, 1, false, -1, "loop wrap: old excluded" },
            { 10, 9, 2, 10, 1, false, 1, "loop wrap: L is the same position as 0 (once)" },
            { 0, 9, 1, 10, 1, false, 1, "loop wrap landing exactly on 0 fires 0 on that tick" },
            { 3, 7, 25, 10, 1, false, 6, "several cycles in one tick: every position once" },
            { 7, 7, 25, 10, 1, false, 10, "several cycles in one tick: old itself once (one cycle away)" },
            { 2, 3, -1, 10, 1, false, 1, "reverse: [new, old) includes new" },
            { 3, 3, -1, 10, 1, false, -1, "reverse: [new, old) excludes old" },
            { 9, 0, -2, 10, 1, false, 1, "reverse loop wrap: below 0 comes back from L" },
            { 8, 0, -2, 10, 1, false, 2, "reverse loop wrap: new itself" },
            { 10, 9, 1, 10, 0, false, 1, "non-loop end: the end tick once" },
            { 10, 10, 0, 10, 0, false, -1, "non-loop stuck at the end: not again" },
            { 0, 1, -1, 10, 0, false, 1, "non-loop reverse reaches 0 once" },
            { 0, 0, 0, 10, 0, false, -1, "non-loop stuck at 0: not again" },
            { 11, 9, 1, 10, 0, false, -1, "a position past the clip never fires" },
            { 3, 2, 1, 0, 1, false, -1, "zero length never fires" },
        };
        bool tableOk = true;
        for (const PassRow& r : rows) {
            const int64_t got = ClipEventPassDistance(r.pos, r.old, r.step, r.cycle, r.loop, r.entered);
            if (got != r.expect) {
                MYE_LOG_ERROR("    event rule '%s': got %lld, expected %lld", r.what, static_cast<long long>(got),
                              static_cast<long long>(r.expect));
                tableOk = false;
            }
        }
        check(tableOk, "M89h: ClipEventPassDistance matches every row of the firing-rule table");

        // JSON: クリップ名がキー。未対応の kind は読まない
        {
            const json j = json::parse(R"({"states":[{"name":"Idle","skel":{"clip":"Idle"}}],
                "clipEvents":{"Idle":[{"tick":12,"name":"FootL","minWeight":0.5,"value":2.5,"int":7},
                                      {"tick":3,"name":"Boom","kind":"bogus"}]}})");
            ControllerAsset a;
            const bool ok = ControllerLibrary::FromJson(j, a);
            check(ok && a.clipEvents.size() == 1 && a.clipEvents[0].clip == "Idle"
                      && a.clipEvents[0].clipHash == HashStr("Idle") && a.clipEvents[0].events.size() == 1
                      && a.clipEvents[0].events[0].tick == 12 && a.clipEvents[0].events[0].nameHash == HashStr("FootL")
                      && a.clipEvents[0].events[0].minWeight == 0.5f && a.clipEvents[0].events[0].value == 2.5f
                      && a.clipEvents[0].events[0].intValue == 7,
                  "M89h: clipEvents load by clip name (unsupported kinds are skipped)");
            ControllerAsset back;
            check(ok && ControllerLibrary::FromJson(ControllerLibrary::ToJson(a), back) && back.clipEvents.size() == 1
                      && back.clipEvents[0].events.size() == 1 && back.clipEvents[0].events[0].name == "FootL"
                      && back.clipEvents[0].events[0].intValue == 7,
                  "M89h: clipEvents round-trip");
            check(!ControllerLibrary::ToJson(ControllerAsset{}).contains("clipEvents"),
                  "M89h: a controller without events omits the key");
        }

        RenderResources res;
        const AssetID model =
            res.skinnedModels.Register("selftest_events", MakeNamedClipModel({ { "Idle", 1.0f }, { "Walk", 0.5f } }));
        ControllerAsset ev;
        ev.states.push_back({ "Idle", "", 0, 1, 1, "Idle", HashStr("Idle") });
        ev.states.push_back({ "Walk", "", 0, 1, 1, "Walk", HashStr("Walk") });
        { // Idle → Walk (param0 == 1、4 tick で混ぜる)
            ControllerTransition t;
            t.from = 0;
            t.to = 1;
            t.duration = 4;
            t.conditions = { { 0, CondOp::Eq, 1 } };
            ev.transitions.push_back(t);
        }
        const auto event = [](const char* name, int32_t tick, float minWeight) {
            ControllerClipEvent e;
            e.name = name;
            e.nameHash = HashStr(name);
            e.tick = tick;
            e.minWeight = minWeight;
            return e;
        };
        ev.clipEvents.push_back({ "Idle", HashStr("Idle"), { event("Mid", 30, 0.0f), event("Start", 0, 0.0f) } });
        ev.clipEvents.push_back(
            { "Walk", HashStr("Walk"), { event("Early", 2, 0.75f), event("Late", 3, 0.75f), event("Step", 10, 0.0f) } });
        const uint64_t evHash = ctrlLib.Register(L"events.controller.json", ev);

        Scene s;
        World& world = s.GetWorld();
        GameObject actor = s.CreateGameObjectTracked("Actor");
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ evHash };
        actor.AddComponent<SkinnedMeshComponent>()->model = model;
        world.ApplyStructuralChanges();
        AnimatorControllerSystem esys;
        const auto ac = [&] { return actor.GetComponent<AnimatorControllerComponent>(); };
        const auto names = [&] {
            std::vector<uint64_t> out;
            for (const AnimEventFired& f : esys.FiredEvents()) {
                out.push_back(f.nameHash);
            }
            return out;
        };
        const auto count = [&](int updates, const char* name) {
            int n = 0;
            for (int i = 0; i < updates; ++i) {
                esys.Update(world, ctrlLib, animLib, &res.skinnedModels);
                for (uint64_t h : names()) {
                    n += h == HashStr(name) ? 1 : 0;
                }
            }
            return n;
        };

        esys.Update(world, ctrlLib, animLib, &res.skinnedModels);
        check(names() == std::vector<uint64_t>{ HashStr("Start") } && esys.FiredEvents()[0].entity == actor.Id()
                  && ac()->stateEntered == 0,
              "M89h: the first tick enters the default state and fires its position-0 event");
        // 2..60 tick 目: 30 で Mid、60 で 59 → 0 に折り返して Start
        check(count(28, "Mid") == 0 && count(1, "Mid") == 1 && ac()->stateTimeTicks == 30,
              "M89h: an event fires on the tick whose new time reaches it");
        check(count(30, "Start") == 1 && ac()->stateTimeTicks == 0, "M89h: the loop wrap onto 0 fires Start once");
        check(count(1, "Start") == 0, "M89h: the tick after a wrap onto 0 does not fire Start again");

        // 遷移: 先の Walk は入った tick から判定し、重みは transitionTick / 4。minWeight 0.75 は 3 tick 目から
        ac()->params[0] = 1;
        esys.Update(world, ctrlLib, animLib, &res.skinnedModels); // Walk 1 (重み 0.25)
        const std::vector<uint64_t> w1 = names();
        esys.Update(world, ctrlLib, animLib, &res.skinnedModels); // Walk 2 (重み 0.5)
        const std::vector<uint64_t> w2 = names();
        esys.Update(world, ctrlLib, animLib, &res.skinnedModels); // Walk 3 (重み 0.75)
        const std::vector<uint64_t> w3 = names();
        check(w1.empty() && w2.empty() && w3 == std::vector<uint64_t>{ HashStr("Late") },
              "M89h: during a transition minWeight gates the target's events by the transition weight");
        check(count(7, "Step") == 1 && ac()->currentState == 1 && ac()->stateTimeTicks == 10,
              "M89h: after the transition the target keeps its clock and fires at full weight");

        // AnimatorPlay の即切り替えは入った tick になる (位置 0 のイベントがもう一度鳴る)
        AnimatorPlay(world, actor.Id(), 0, 0, ctrlLib);
        check(ac()->stateEntered == 1 && count(1, "Start") == 1, "M89h: an instant AnimatorPlay re-enters the state");

        // 遷移の元と先が同じ tick に両方発火し、順は元 → 先 (元 Idle の 30 と、先 Walk の 1。どちらも minWeight 0)
        {
            ControllerAsset both = ev;
            both.clipEvents[1].events = { event("To", 1, 0.0f) };
            both.clipEvents[0].events = { event("From", 30, 0.0f) };
            const uint64_t bothHash = ctrlLib.Register(L"events_both.controller.json", both);
            ac()->controller = AssetID{ bothHash };
            AnimatorPlay(world, actor.Id(), 0, 0, ctrlLib);
            ac()->params[0] = 0;
            for (int i = 0; i < 29; ++i) {
                esys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            }
            ac()->params[0] = 1; // 次の tick に遷移を始める = Idle は 29 → 30、Walk は 0 → 1
            esys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            check(names() == std::vector<uint64_t>{ HashStr("From"), HashStr("To") },
                  "M89h: in a transition both states fire, from before to");
            ac()->controller = AssetID{ evHash };
        }

        // ブレンドツリー: 最大重みの子だけが位相で発火する
        {
            ControllerAsset bt;
            bt.parameters = { { "x", ControllerParamType::Float } };
            ControllerState blend;
            blend.name = "Move";
            blend.blendType = ControllerBlendType::Blend1D;
            blend.blendParam = 0;
            blend.blendChildren.push_back({ "Idle", HashStr("Idle"), 0.0f });
            blend.blendChildren.push_back({ "Walk", HashStr("Walk"), 1.0f });
            bt.states.push_back(blend);
            bt.clipEvents.push_back({ "Idle", HashStr("Idle"), { event("IdleHalf", 30, 0.0f) } });
            bt.clipEvents.push_back({ "Walk", HashStr("Walk"), { event("WalkHalf", 15, 0.0f) } });
            const uint64_t btHash = ctrlLib.Register(L"events_blend.controller.json", bt);
            ac()->controller = AssetID{ btHash };
            AnimatorPlay(world, actor.Id(), 0, 0, ctrlLib);
            ac()->params[0] = std::bit_cast<int32_t>(0.25f); // Idle 0.75 / Walk 0.25、1 周 = 52.5 tick
            int idleHalf = 0;
            int walkHalf = 0;
            for (int i = 0; i < 60; ++i) {
                esys.Update(world, ctrlLib, animLib, &res.skinnedModels);
                for (uint64_t h : names()) {
                    idleHalf += h == HashStr("IdleHalf") ? 1 : 0;
                    walkHalf += h == HashStr("WalkHalf") ? 1 : 0;
                }
            }
            check(idleHalf == 1 && walkHalf == 0,
                  "M89h: a blend tree fires only its heaviest child's events, judged by phase (once per cycle)");
        }

        // モデルが無ければ長さが引けないので発火しない
        AnimatorPlay(world, actor.Id(), 0, 0, ctrlLib);
        esys.Update(world, ctrlLib, animLib, nullptr);
        check(esys.FiredEvents().empty(), "M89h: no skinned model library = no events");
    }

    // ---- (M89i) エンジンが直接処理するイベント: 種類別の欄の JSON / 発火位置 (ジョイント × WorldMatrix) ----
    {
        {
            const json j = json::parse(R"({"states":[{"name":"Idle","skel":{"clip":"Idle"}}],
                "clipEvents":{"Idle":[{"tick":1,"name":"Step","kind":"sound","sound":"footstep","volume":0.5,"pitch":1.25,"joint":"Foot"},
                                      {"tick":2,"kind":"effect","prefab":"fx/dust"},
                                      {"tick":3,"kind":"noise","loudness":2.0,"range":15.0,"joint":"Foot"},
                                      {"tick":4,"name":"Hit"}]}})");
            ControllerAsset a;
            const bool ok = ControllerLibrary::FromJson(j, a) && a.clipEvents.size() == 1
                            && a.clipEvents[0].events.size() == 4;
            const auto matches = [](const ControllerAsset& c) {
                const std::vector<ControllerClipEvent>& e = c.clipEvents[0].events;
                return e[0].kind == ClipEventKind::Sound && e[0].asset == "footstep"
                       && e[0].assetHash == HashStr("footstep") && e[0].volume == 0.5f && e[0].pitch == 1.25f
                       && e[0].joint == "Foot" && e[1].kind == ClipEventKind::Effect && e[1].asset == "fx/dust"
                       && e[1].joint.empty() && e[2].kind == ClipEventKind::Noise && e[2].loudness == 2.0f
                       && e[2].range == 15.0f && e[3].kind == ClipEventKind::Script;
            };
            check(ok && matches(a), "M89i: sound / effect / noise events load their own fields (kind defaults to script)");
            ControllerAsset back;
            check(ok && ControllerLibrary::FromJson(ControllerLibrary::ToJson(a), back) && back.clipEvents.size() == 1
                      && back.clipEvents[0].events.size() == 4 && matches(back),
                  "M89i: kinds and their fields round-trip");
        }

        // ルート (原点) と子の Foot (ルートから +0.5 X) の 2 関節。トラックが空なのでバインドポーズ
        SkinnedModel twoJoints = MakeNamedClipModel({ { "Idle", 1.0f } });
        twoJoints.joints.resize(2);
        twoJoints.joints[0].name = "Root";
        twoJoints.joints[1].name = "Foot";
        twoJoints.joints[1].parent = 0;
        twoJoints.joints[1].bindT = { 0.5f, 0.0f, 0.0f };
        twoJoints.clips[0].tracks.resize(2);
        RenderResources res;
        const AssetID model = res.skinnedModels.Register("selftest_event_pos", std::move(twoJoints));
        ControllerAsset ctrl;
        ctrl.states.push_back({ "Idle", "", 0, 1, 1, "Idle", HashStr("Idle") });
        const auto event = [](ClipEventKind kind, const char* joint, const char* asset) {
            ControllerClipEvent e;
            e.kind = kind;
            e.tick = 0;
            e.joint = joint;
            e.asset = asset;
            e.assetHash = HashStr(asset);
            return e;
        };
        ctrl.clipEvents.push_back({ "Idle", HashStr("Idle"),
                                    { event(ClipEventKind::Sound, "Foot", "footstep"),
                                      event(ClipEventKind::Noise, "", ""),
                                      event(ClipEventKind::Effect, "NoSuchJoint", "fx/dust") } });
        const uint64_t posCtrlHash = ctrlLib.Register(L"events_pos.controller.json", ctrl);

        Scene s;
        World& world = s.GetWorld();
        GameObject actor = s.CreateGameObjectTracked("Actor");
        actor.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ posCtrlHash };
        actor.AddComponent<SkinnedMeshComponent>()->model = model;
        if (actor.GetComponent<WorldMatrixComponent>() == nullptr) {
            actor.AddComponent<WorldMatrixComponent>();
        }
        world.ApplyStructuralChanges();
        // TransformSystem を回さずに WorldMatrix を直接置く (= 前 tick の確定値)。平行移動 (10, 2, -3)
        WorldMatrixComponent* wm = actor.GetComponent<WorldMatrixComponent>();
        wm->value._41 = 10.0f;
        wm->value._42 = 2.0f;
        wm->value._43 = -3.0f;
        AnimatorControllerSystem esys;
        esys.Update(world, ctrlLib, animLib, &res.skinnedModels); // 入った tick = 位置 0 が 3 件とも発火
        const std::vector<AnimEventFired>& f = esys.FiredEvents();
        const auto at = [](const AnimEventFired& e, float x, float y, float z) {
            return std::fabs(e.pos[0] - x) < 1e-5f && std::fabs(e.pos[1] - y) < 1e-5f && std::fabs(e.pos[2] - z) < 1e-5f;
        };
        check(f.size() == 3 && f[0].kind == ClipEventKind::Sound && f[1].kind == ClipEventKind::Noise
                  && f[2].kind == ClipEventKind::Effect && f[0].def != nullptr && f[0].def->assetHash == HashStr("footstep"),
              "M89i: fired events carry their kind and definition, in declaration order at the same position");
        check(f.size() == 3 && at(f[0], 10.5f, 2.0f, -3.0f),
              "M89i: an event with a joint fires at jointGlobal x the skinned entity's WorldMatrix");
        check(f.size() == 3 && at(f[1], 10.0f, 2.0f, -3.0f) && at(f[2], 10.0f, 2.0f, -3.0f),
              "M89i: no joint / an unknown joint fires at the controller entity's position");
    }

    // ---- (M89j) ルートモーション: 600 tick の移動量 / 折り返し / ポーズからの除去 / Z-up / 適用先 ----
    {
        using namespace DirectX;
        // ルート 1 関節。Walk (60 tick) で 1 周にモデル空間の axis 方向へ 1.2 m 進み、upAxis 方向は 0.1 で一定
        const auto makeWalker = [](XMFLOAT3 step, XMFLOAT3 lift) {
            SkinnedModel m = MakeNamedClipModel({ { "Walk", 1.0f } });
            m.joints[0].name = "Root";
            JointTrack& tr = m.clips[0].tracks[0];
            tr.tTimes = { 0.0f, 1.0f };
            tr.tVals = { lift, { lift.x + step.x, lift.y + step.y, lift.z + step.z } };
            return m;
        };
        RenderResources res;
        const AssetID yUp = res.skinnedModels.Register("selftest_rm_y", makeWalker({ 0.0f, 0.0f, -1.2f }, { 0.0f, 0.1f, 0.0f }));
        // Z-up: 前進がモデル空間の +Y に出て、上は +Z (メッシュのエンティティを X 軸 -90 度回して立てる)
        const AssetID zUp = res.skinnedModels.Register("selftest_rm_z", makeWalker({ 0.0f, 1.2f, 0.0f }, { 0.0f, 0.0f, 0.1f }));
        ControllerAsset ctrl;
        ctrl.states.push_back({ "Walk", "", 0, 1, 1, "Walk", HashStr("Walk") });
        const uint64_t rmHash = ctrlLib.Register(L"root_motion.controller.json", ctrl);

        struct Walker {
            GameObject actor;
            GameObject body;
        };
        const auto makeWalkerRig = [&](Scene& s, AssetID model, bool zUpBody, bool apply) {
            Walker w;
            w.actor = s.CreateGameObjectTracked("Actor");
            AnimatorControllerComponent* ac = w.actor.AddComponent<AnimatorControllerComponent>();
            ac->controller = AssetID{ rmHash };
            ac->applyRootMotion = apply;
            w.body = s.CreateGameObjectTracked("Body");
            w.body.SetParent(w.actor);
            w.body.AddComponent<SkinnedMeshComponent>()->model = model;
            s.GetWorld().ApplyStructuralChanges();
            if (zUpBody) {
                const float h = std::sqrt(0.5f);
                w.body.GetComponent<LocalTransform>()->rotation = { -h, 0.0f, 0.0f, h };
            }
            return w;
        };
        // tol = 0 は「ちょうど一致」(動かないこと・書かないことの確認に使う) なので <= で比べる
        const auto near3 = [](const XMFLOAT3& a, float x, float y, float z, float tol) {
            return std::fabs(a.x - x) <= tol && std::fabs(a.y - y) <= tol && std::fabs(a.z - z) <= tol;
        };

        for (const bool zUpBody : { false, true }) {
            Scene s;
            World& world = s.GetWorld();
            Walker w = makeWalkerRig(s, zUpBody ? zUp : yUp, zUpBody, true);
            AnimatorControllerSystem rmSys;
            SkinningSystem skinning;
            float sumZ = 0.0f;
            bool velocityOk = true;
            // 615 tick = 10 周と 15 tick (ポーズの除去は時刻 0 以外で見る)
            for (int t = 0; t < 615; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
                skinning.Update(world, res);
                const XMFLOAT3 v = w.actor.GetComponent<AnimatorControllerComponent>()->rootMotionVelocity;
                velocityOk = velocityOk && near3(v, 0.0f, 0.0f, -1.2f, 1e-3f);
                sumZ += v.z / 60.0f;
            }
            const char* tag = zUpBody ? " (Z-up body)" : "";
            check(velocityOk, (std::string("M89j: rootMotionVelocity is a steady 1.2 m/s forward (-Z), including the "
                                           "loop's wrap ticks") + tag).c_str());
            const XMFLOAT3 p = w.actor.GetComponent<LocalTransform>()->position;
            check(near3(p, 0.0f, 0.0f, -12.3f, 1e-2f) && std::fabs(p.z - sumZ) < 1e-3f,
                  (std::string("M89j: 615 ticks move the Transform 10.25 cycles x 1.2 m = the sum of the per-tick deltas") + tag)
                      .c_str());
            const SkinnedMeshComponent* sm = w.body.GetComponent<SkinnedMeshComponent>();
            std::vector<XMMATRIX> locals;
            SampleSkinnedLocals(*res.skinnedModels.Get(sm->model), *sm, locals);
            XMFLOAT4X4 root;
            XMStoreFloat4x4(&root, locals[0]);
            // 時刻 0 のルートの位置 (lift) に戻り、上向きの成分 (0.1) は残る
            const bool stripped = zUpBody ? near3({ root._41, root._42, root._43 }, 0.0f, 0.0f, 0.1f, 1e-5f)
                                          : near3({ root._41, root._42, root._43 }, 0.0f, 0.1f, 0.0f, 1e-5f);
            check(sm->poseRootJoint == 0 && stripped,
                  (std::string("M89j: the pose keeps the root at the clip start horizontally and keeps the vertical part") + tag)
                      .c_str());
        }

        // applyRootMotion = false: 速度は出すが動かさず、ポーズからも抜かない
        {
            Scene s;
            World& world = s.GetWorld();
            Walker w = makeWalkerRig(s, yUp, false, false);
            AnimatorControllerSystem rmSys;
            for (int t = 0; t < 30; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            }
            const SkinnedMeshComponent* sm = w.body.GetComponent<SkinnedMeshComponent>();
            check(near3(w.actor.GetComponent<AnimatorControllerComponent>()->rootMotionVelocity, 0.0f, 0.0f, -1.2f, 1e-3f)
                      && near3(w.actor.GetComponent<LocalTransform>()->position, 0.0f, 0.0f, 0.0f, 0.0f)
                      && sm->poseRootJoint == -1,
                  "M89j: without applyRootMotion the velocity is reported but nothing moves and the pose keeps the motion");
        }

        // 適用先: Rigidbody (縦は残す) / CharacterController / NavMeshAgent が位置を握っている間は何もしない
        {
            Scene s;
            World& world = s.GetWorld();
            Walker rbRig = makeWalkerRig(s, yUp, false, true);
            Walker ccRig = makeWalkerRig(s, yUp, false, true);
            Walker navRig = makeWalkerRig(s, yUp, false, true);
            Walker kinRig = makeWalkerRig(s, yUp, false, true);
            rbRig.actor.AddComponent<RigidbodyComponent>()->velocity = { 5.0f, -2.0f, 5.0f };
            ccRig.actor.AddComponent<CharacterControllerComponent>();
            navRig.actor.AddComponent<CharacterControllerComponent>()->moveInput = { 3.0f, 0.0f, 3.0f };
            navRig.actor.AddComponent<NavMeshAgentComponent>()->updatePosition = true;
            kinRig.actor.AddComponent<RigidbodyComponent>()->isKinematic = true;
            kinRig.actor.AddComponent<CharacterControllerComponent>();
            world.ApplyStructuralChanges();
            AnimatorControllerSystem rmSys;
            rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            check(near3(rbRig.actor.GetComponent<RigidbodyComponent>()->velocity, 0.0f, -2.0f, -1.2f, 1e-3f)
                      && near3(rbRig.actor.GetComponent<LocalTransform>()->position, 0.0f, 0.0f, 0.0f, 0.0f),
                  "M89j: a dynamic Rigidbody gets the horizontal velocity and keeps its vertical velocity");
            check(near3(ccRig.actor.GetComponent<CharacterControllerComponent>()->moveInput, 0.0f, 0.0f, -1.2f, 1e-3f),
                  "M89j: a CharacterController gets the velocity as moveInput");
            check(near3(navRig.actor.GetComponent<CharacterControllerComponent>()->moveInput, 3.0f, 0.0f, 3.0f, 0.0f)
                      && near3(navRig.actor.GetComponent<LocalTransform>()->position, 0.0f, 0.0f, 0.0f, 0.0f),
                  "M89j: a NavMeshAgent with updatePosition keeps control (root motion writes nothing)");
            check(std::fabs(kinRig.actor.GetComponent<LocalTransform>()->position.z + 0.02f) < 1e-4f
                      && near3(kinRig.actor.GetComponent<CharacterControllerComponent>()->moveInput, 0.0f, 0.0f, 0.0f, 0.0f),
                  "M89j: a kinematic Rigidbody (which also disables the CharacterController) moves by the Transform");
        }

        // 親の回転・拡大の下でも、ワールドで正しい向き・長さだけ動く (親空間へ戻して足す)
        {
            Scene s;
            World& world = s.GetWorld();
            GameObject parent = s.CreateGameObjectTracked("Parent");
            Walker w = makeWalkerRig(s, yUp, false, true);
            w.actor.SetParent(parent);
            world.ApplyStructuralChanges();
            const float h = std::sqrt(0.5f);
            LocalTransform* plt = parent.GetComponent<LocalTransform>();
            plt->rotation = { 0.0f, h, 0.0f, h }; // Y 軸 90 度
            plt->scale = { 2.0f, 2.0f, 2.0f };
            AnimatorControllerSystem rmSys;
            rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            // 体はワールドで 2 倍・Y 90 度回っている: モデルの -Z 0.02 m → ワールドでは 0.04 m、向きは -X
            const XMFLOAT3 v = w.actor.GetComponent<AnimatorControllerComponent>()->rootMotionVelocity;
            const XMFLOAT3 p = w.actor.GetComponent<LocalTransform>()->position;
            check(near3(v, -2.4f, 0.0f, 0.0f, 1e-3f) && near3(p, 0.0f, 0.0f, -0.02f, 1e-5f),
                  "M89j: under a rotated / scaled parent the velocity is in world space and the Transform moves in parent space");
        }

        // ---- (M89k) ルートモーションのヨー ----
        // 回らないクリップではヨーは恒等ちょうどで、エンティティの回転のビットも変えない
        {
            Scene s;
            World& world = s.GetWorld();
            Walker w = makeWalkerRig(s, yUp, false, true);
            AnimatorControllerSystem rmSys;
            bool identity = true;
            for (int t = 0; t < 61; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
                const XMFLOAT4 d = w.actor.GetComponent<AnimatorControllerComponent>()->rootMotionDeltaRotation;
                identity = identity && d.x == 0.0f && d.y == 0.0f && d.z == 0.0f && d.w == 1.0f;
            }
            const XMFLOAT4 r = w.actor.GetComponent<LocalTransform>()->rotation;
            check(identity && r.x == 0.0f && r.y == 0.0f && r.z == 0.0f && r.w == 1.0f,
                  "M89k: a clip without root rotation reports exactly no yaw and leaves the rotation bits alone");
        }

        // Turn: 1 周 (60 tick) でルートが上まわりに +90 度回り、モデル空間の前へ 1.2 m 進む (Y-up / Z-up)
        const auto quatAbout = [](XMFLOAT3 axis, float deg) {
            const float half = deg * 0.5f * 3.14159265f / 180.0f;
            return XMFLOAT4{ axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half) };
        };
        const auto makeTurner = [&](XMFLOAT3 upAxis, XMFLOAT3 step) {
            SkinnedModel m = makeWalker(step, { 0.0f, 0.0f, 0.0f });
            JointTrack& tr = m.clips[0].tracks[0];
            tr.rTimes = { 0.0f, 0.5f, 1.0f };
            tr.rVals = { quatAbout(upAxis, 0.0f), quatAbout(upAxis, 45.0f), quatAbout(upAxis, 90.0f) };
            return m;
        };
        const AssetID turnY = res.skinnedModels.Register("selftest_rm_turn_y", makeTurner({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.2f }));
        const AssetID turnZ = res.skinnedModels.Register("selftest_rm_turn_z", makeTurner({ 0.0f, 0.0f, 1.0f }, { 0.0f, 1.2f, 0.0f }));
        const auto near4 = [](const XMFLOAT4& a, const XMFLOAT4& b, float tol) {
            // q と -q は同じ回転なので、b の符号を a にそろえてから成分ごとに比べる
            const float sign = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0.0f ? -1.0f : 1.0f;
            return std::fabs(a.x - sign * b.x) < tol && std::fabs(a.y - sign * b.y) < tol
                   && std::fabs(a.z - sign * b.z) < tol && std::fabs(a.w - sign * b.w) < tol;
        };
        for (const bool zUpBody : { false, true }) {
            Scene s;
            World& world = s.GetWorld();
            Walker w = makeWalkerRig(s, zUpBody ? turnZ : turnY, zUpBody, true);
            AnimatorControllerSystem rmSys;
            SkinningSystem skinning;
            const char* tag = zUpBody ? " (Z-up body)" : "";
            bool stepOk = true;
            XMFLOAT3 halfCycle = {};
            for (int t = 1; t <= 135; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
                skinning.Update(world, res);
                const XMFLOAT4 d = w.actor.GetComponent<AnimatorControllerComponent>()->rootMotionDeltaRotation;
                stepOk = stepOk && near4(d, quatAbout({ 0.0f, 1.0f, 0.0f }, 1.5f), 1e-6f) && d.y > 0.0f;
                if (t == 30) {
                    halfCycle = w.actor.GetComponent<LocalTransform>()->position;
                }
                if (t == 60) {
                    check(near4(w.actor.GetComponent<LocalTransform>()->rotation, quatAbout({ 0.0f, 1.0f, 0.0f }, 90.0f), 1e-5f),
                          (std::string("M89k: one cycle turns the entity +90 degrees about world Y") + tag).c_str());
                }
                if (t == 120) {
                    // 2 周目はエンティティが 90 度回った向き (クリップの -Z = ワールドの -X) へ進む
                    check(near3(w.actor.GetComponent<LocalTransform>()->position, -1.2f, 0.0f, -1.2f, 1e-3f),
                          (std::string("M89k: after a wrap the next cycle walks in the turned direction") + tag).c_str());
                }
            }
            check(stepOk, (std::string("M89k: rootMotionDeltaRotation is a steady +1.5 degrees per tick about world Y, "
                                       "including the wrap ticks") + tag).c_str());
            // 1 周目の途中は、回りながらでもワールドでの位置がクリップのルートの通り道 (前へ 0.6 m) に一致する
            check(near3(halfCycle, 0.0f, 0.0f, -0.6f, 1e-4f),
                  (std::string("M89k: within a cycle the entity follows the clip's root path (no double rotation)") + tag).c_str());
            // 135 tick = 2 周と 15 tick: ポーズのルートはひねりが抜けて恒等の回転、水平は先頭に戻る
            const SkinnedMeshComponent* sm = w.body.GetComponent<SkinnedMeshComponent>();
            std::vector<XMMATRIX> locals;
            SampleSkinnedLocals(*res.skinnedModels.Get(sm->model), *sm, locals);
            XMFLOAT4X4 root;
            XMStoreFloat4x4(&root, locals[0]);
            const bool unrotated = std::fabs(root._11 - 1.0f) < 1e-5f && std::fabs(root._22 - 1.0f) < 1e-5f
                                   && std::fabs(root._33 - 1.0f) < 1e-5f;
            check(sm->poseRootYaw == 1 && unrotated && near3({ root._41, root._42, root._43 }, 0.0f, 0.0f, 0.0f, 1e-5f),
                  (std::string("M89k: the pose has the root's yaw removed and stays at the clip start") + tag).c_str());
        }

        // NavMeshAgent が updateRotation で向きを握っている間は回さず、ポーズのひねりも残す (ヨーの値は出す)。
        // updateRotation を切れば updatePosition 中でもヨーだけ受け取る
        {
            Scene s;
            World& world = s.GetWorld();
            Walker held = makeWalkerRig(s, turnY, false, true);
            Walker free = makeWalkerRig(s, turnY, false, true);
            held.actor.AddComponent<CharacterControllerComponent>();
            held.actor.AddComponent<NavMeshAgentComponent>()->updatePosition = false;
            free.actor.AddComponent<CharacterControllerComponent>();
            free.actor.AddComponent<NavMeshAgentComponent>()->updateRotation = false;
            world.ApplyStructuralChanges();
            AnimatorControllerSystem rmSys;
            for (int t = 0; t < 30; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            }
            const XMFLOAT4 r = held.actor.GetComponent<LocalTransform>()->rotation;
            check(r.x == 0.0f && r.y == 0.0f && r.z == 0.0f && r.w == 1.0f
                      && held.body.GetComponent<SkinnedMeshComponent>()->poseRootYaw == 0
                      && near4(held.actor.GetComponent<AnimatorControllerComponent>()->rootMotionDeltaRotation,
                               quatAbout({ 0.0f, 1.0f, 0.0f }, 1.5f), 1e-6f),
                  "M89k: a NavMeshAgent with updateRotation keeps the facing; the pose keeps the turn and the yaw is still reported");
            check(near4(free.actor.GetComponent<LocalTransform>()->rotation, quatAbout({ 0.0f, 1.0f, 0.0f }, 45.0f), 1e-5f)
                      && free.body.GetComponent<SkinnedMeshComponent>()->poseRootYaw == 1,
                  "M89k: with updateRotation off the entity turns by the yaw even while Nav holds the position");
        }

        // 親が傾いていても、ワールドの Y 軸まわりに回る (ワールドの上を親空間へ戻した軸で回す)
        {
            Scene s;
            World& world = s.GetWorld();
            GameObject parent = s.CreateGameObjectTracked("Parent");
            // 体は X 軸 -90 度、親は X 軸 +90 度: 体のモデル空間の上 (Y) はワールドの上のまま、
            // コントローラのエンティティの空間ではワールドの上が別の軸になる
            Walker w = makeWalkerRig(s, turnY, true, true);
            w.actor.SetParent(parent);
            world.ApplyStructuralChanges();
            const float h = std::sqrt(0.5f);
            parent.GetComponent<LocalTransform>()->rotation = { h, 0.0f, 0.0f, h };
            AnimatorControllerSystem rmSys;
            for (int t = 0; t < 60; ++t) {
                rmSys.Update(world, ctrlLib, animLib, &res.skinnedModels);
            }
            // ワールドの回転 = 親 · 自分。ワールドの Y 90 度 · 親 になっているはず
            const XMVECTOR parentQ = XMLoadFloat4(&parent.GetComponent<LocalTransform>()->rotation);
            const XMVECTOR selfQ = XMLoadFloat4(&w.actor.GetComponent<LocalTransform>()->rotation);
            XMFLOAT4 worldQ;
            XMStoreFloat4(&worldQ, XMQuaternionMultiply(selfQ, parentQ));
            const XMFLOAT4 yaw = quatAbout({ 0.0f, 1.0f, 0.0f }, 90.0f);
            XMFLOAT4 expected;
            XMStoreFloat4(&expected, XMQuaternionMultiply(parentQ, XMLoadFloat4(&yaw)));
            check(near4(worldQ, expected, 1e-5f),
                  "M89k: under a rotated parent the entity turns about world Y (the axis is taken back into parent space)");
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Animator Controller self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Animator Controller self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
