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
        for (int i = 0; i < 30; ++i) {
            step();
            const int32_t runQ = smA()->poseLayers[0].timeQ;
            const int32_t walkQ = smA()->poseLayers[1].timeQ;
            synced = synced && walkQ - 2 * runQ >= 0 && walkQ - 2 * runQ <= 1;
        }
        check(synced, "M89d: phase sync: Walk (60) is always at twice Run's (30) time");

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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Animator Controller self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Animator Controller self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
