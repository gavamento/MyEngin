#include "Engine/Engine/AnimatorControllerSelfTest.h"

#include <cmath>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

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
            const uint64_t ha = HashWorld(sa.GetWorld(), nullptr);
            const uint64_t hb = HashWorld(sb.GetWorld(), nullptr);
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

    if (failCount == 0) {
        MYE_LOG_INFO("==== Animator Controller self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Animator Controller self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
