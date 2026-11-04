//====================================================================================
//                          FeelLibrarySelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りのプリセット資産と FeelTrigger (M94d) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Feel/FeelLibrarySelfTest.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Feel/FeelLibrary.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Feel/FeelTriggerSystem.h"
#include "Engine/Engine/Loop/GameFlow.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // SolidContact
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"

namespace mye {

bool RunFeelLibrarySelfTest()
{
    MYE_LOG_INFO("==== FeelLibrary self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- JSON の往復 ----
    {
        MyeFeelDesc d;
        d.hitStopTicks = 4;
        d.slowPercent = 35;
        d.slowHoldTicks = 9;
        d.slowRecoverTicks = 15;
        d.slowRecoverEase = MYE_EASE_INOUT_SINE;
        d.shakeTicks = 20;
        d.shakePosAmp = 0.05f;
        d.shakeRotAmpDeg = 1.5f;
        d.shakeFrequencyHz = 24.0f;
        d.shakeEnvelope = MyeMakeCurve(MYE_EASE_OUT_BACK, 1.0f, 0.0f);
        d.shakeEnvelope.param = 2.5f;
        d.kickTicks = 10;
        d.kickOffset = { 0.0f, 0.01f, -0.2f };
        d.kickPitchDeg = 3.0f;
        d.kickFovDeg = -4.0f;
        d.kickCurve.mode = MYE_CURVE_CUSTOM;
        d.kickCurve.keyCount = 3;
        d.kickCurve.keys[0] = { 0.0f, 1.0f, 0.0f, -2.0f };
        d.kickCurve.keys[1] = { 0.4f, 0.3f, -1.0f, -1.0f };
        d.kickCurve.keys[2] = { 1.0f, 0.0f, 0.0f, 0.0f };
        d.rumbleTicks = 8;
        d.rumbleLow = 0.7f;
        d.rumbleHigh = 0.25f;
        d.flashTicks = 6;
        d.flashExposure = 0.5f;
        d.flashColor = { 1.0f, 0.9f, 0.8f, 0.3f };
        SanitizeFeelDesc(d);
        MyeFeelDesc back;
        const bool ok = FeelLibrary::FromJson(FeelLibrary::ToJson(d), back);
        check(ok && std::memcmp(&d, &back, sizeof(MyeFeelDesc)) == 0, "ToJson -> FromJson round-trips bit-exactly");

        MyeFeelDesc partial;
        check(FeelLibrary::FromJson(nlohmann::json{ { "feel", 1 }, { "hitStop", { { "ticks", 2 } } } }, partial)
                  && partial.hitStopTicks == 2 && partial.shakeTicks == 0 && partial.slowPercent == 100,
              "missing sections keep their defaults");
        MyeFeelDesc notFeel;
        check(!FeelLibrary::FromJson(nlohmann::json{ { "physmat", 1 } }, notFeel), "a JSON without the feel key is rejected");
        MyeFeelDesc bad;
        check(FeelLibrary::FromJson(nlohmann::json{ { "feel", 1 },
                                                    { "slow", { { "percent", 400 }, { "recoverEase", "Wobble" } } },
                                                    { "shake", { { "ticks", -5 } } } },
                                    bad)
                  && bad.slowPercent == 100 && bad.slowRecoverEase == MYE_EASE_LINEAR && bad.shakeTicks == 0,
              "out-of-range values are sanitized and unknown eases fall back to Linear");
    }

    // ---- 登録と名前での検索 ----
    {
        FeelLibrary lib;
        MyeFeelDesc a;
        a.hitStopTicks = 1;
        MyeFeelDesc b;
        b.hitStopTicks = 2;
        const uint64_t hb = lib.Register(L"C:\\proj\\assets\\z\\Hit.feel.json", b);
        const uint64_t ha = lib.Register(L"C:\\proj\\assets\\a\\Hit.feel.json", a);
        check(ha != hb && lib.Contains(ha) && lib.Get(hb)->desc.hitStopTicks == 2, "presets are keyed by their asset id");
        const FeelPreset* p = lib.FindByName(HashStr("Hit"));
        check(p != nullptr && p->desc.hitStopTicks == 1 && p->name == "Hit",
              "a duplicated name resolves to the smallest path (independent of map order)");
        check(lib.FindByName(HashStr("Missing")) == nullptr, "an unknown name resolves to nothing");
        check(FeelLibrary::NameFromPath(L"x\\HeavyHit.feel.json") == "HeavyHit", "the name drops .feel.json");
    }

    // ---- FeelTrigger ----
    {
        RegisterBuiltinComponents();
        FeelLibrary lib;
        MyeFeelDesc d;
        d.hitStopTicks = 3;
        d.shakeTicks = 10;
        d.shakePosAmp = 0.1f;
        const uint64_t presetId = lib.Register(L"C:\\proj\\assets\\Bump.feel.json", d);
        FeelLibrary* prev = feellib::Library();
        feellib::Install(&lib);

        Scene scene;
        GameObject box = scene.CreateGameObjectTracked("Box");
        auto* trig = box.AddComponent<FeelTriggerComponent>();
        trig->feel = AssetID{ presetId };
        trig->minImpulse = 2.0f;
        trig->fullImpulse = 8.0f;
        trig->maxIntensity = 1.5f;
        trig->cooldownTicks = 3;
        GameObject floor = scene.CreateGameObjectTracked("Floor");
        scene.GetWorld().ApplyStructuralChanges();
        const uint32_t bi = box.Id().index;
        const uint32_t fi = floor.Id().index;
        SolidContact c;
        c.key = (static_cast<uint64_t>(fi < bi ? fi : bi) << 32) | (fi < bi ? bi : fi);
        std::vector<SolidContact> contacts;
        TimeControl tc;

        c.impulse = 1.0f;
        contacts = { c };
        check(UpdateFeelTriggers(scene.GetWorld(), contacts, scene.Feel(), tc) == 0, "an impulse below minImpulse plays nothing");
        c.impulse = 4.0f;
        contacts = { c };
        check(UpdateFeelTriggers(scene.GetWorld(), contacts, scene.Feel(), tc) == 1 && scene.Feel().ActiveCount() == 1
                  && std::fabs(scene.Feel().Slot(0).intensity - 0.5f) < 1e-6f && tc.freezeTicks == 3,
              "a hard contact plays the preset with intensity impulse / fullImpulse");
        check(UpdateFeelTriggers(scene.GetWorld(), contacts, scene.Feel(), tc) == 0, "the cooldown blocks the next tick");
        UpdateFeelTriggers(scene.GetWorld(), {}, scene.Feel(), tc);
        c.impulse = 100.0f;
        contacts = { c };
        check(UpdateFeelTriggers(scene.GetWorld(), contacts, scene.Feel(), tc) == 1
                  && std::fabs(scene.Feel().Slot(1).intensity - 1.5f) < 1e-6f,
              "after the cooldown it plays again, clamped to maxIntensity");
        trig = scene.GetWorld().GetComponent<FeelTriggerComponent>(box.Id());
        trig->feel = {};
        UpdateFeelTriggers(scene.GetWorld(), {}, scene.Feel(), tc);
        UpdateFeelTriggers(scene.GetWorld(), {}, scene.Feel(), tc);
        UpdateFeelTriggers(scene.GetWorld(), {}, scene.Feel(), tc);
        check(UpdateFeelTriggers(scene.GetWorld(), contacts, scene.Feel(), tc) == 0, "an unassigned preset plays nothing");
        feellib::Install(prev);
    }

    return failCount == 0;
}

} // namespace mye
