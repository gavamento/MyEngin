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
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"

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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Animator Controller self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Animator Controller self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
