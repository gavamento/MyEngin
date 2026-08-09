#include "Engine/Platform/InputActionsSelfTest.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/InputActions.h"

namespace mye {

namespace {

InputSnapshot Snap()
{
    InputSnapshot s = {};
    return s;
}

void Down(InputSnapshot& s, uint8_t vk)
{
    s.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
}

bool Near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVkW = 0x57;
constexpr uint8_t kVkS = 0x53;
constexpr uint16_t kPadA = 0x1000;

const char* kSampleJson = R"({
  "actions": [
    { "name": "Jump", "keys": ["Space"], "pad": ["A"] },
    { "name": "Fire", "keys": ["J"], "pad": ["RB"], "mouse": ["Left"] }
  ],
  "axes": [
    { "name": "MoveX", "posKey": "D", "negKey": "A", "padAxis": "LX", "deadzone": 0.5 },
    { "name": "MoveY", "posKey": "W", "negKey": "S", "padAxis": "LY", "deadzone": 0.5 },
    { "name": "Throttle", "padAxis": "RT", "deadzone": 0.0 }
  ]
})";

} // namespace

bool RunInputActionsSelfTest()
{
    MYE_LOG_INFO("==== InputActions self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    const uint64_t hJump = HashStr("Jump");
    const uint64_t hFire = HashStr("Fire");
    const uint64_t hMoveX = HashStr("MoveX");
    const uint64_t hThrottle = HashStr("Throttle");

    // ---- ロード: 件数・ハッシュ・束縛の取り込み ----
    InputActions ia;
    {
        const bool ok = ia.LoadFromJsonText(kSampleJson);
        check(ok && ia.Actions().size() == 2 && ia.Axes().size() == 3,
              "sample json loads (2 actions, 3 axes)");
        check(ia.Actions()[0].nameHash == hJump && ia.Actions()[0].keys.size() == 1
                  && ia.Actions()[0].keys[0] == kVkSpace && ia.Actions()[0].padMask == kPadA,
              "action def carries name hash + VK + pad mask");
        check(ia.Actions()[1].mouseMask == 0x01, "mouse binding parsed (Left = bit0)");
        check(ia.Axes()[0].posKey == 0x44 && ia.Axes()[0].negKey == 0x41
                  && ia.Axes()[0].padAxis == PadAxis::LX && Near(ia.Axes()[0].deadzone, 0.5f),
              "axis def carries posKey/negKey/padAxis/deadzone");
    }

    // ---- held / pressed / released の全遷移 (tick 0 の prev = ゼロ値) ----
    {
        InputSnapshot none = Snap();
        InputSnapshot space = Snap();
        Down(space, kVkSpace);

        ia.Evaluate(space, none); // 押した瞬間
        check(ia.ActionState(hJump) == (kActionHeld | kActionPressed),
              "transition none->down = held|pressed");
        ia.Evaluate(space, space); // 押しっぱなし
        check(ia.ActionState(hJump) == kActionHeld, "transition down->down = held only");
        ia.Evaluate(none, space); // 離した瞬間
        check(ia.ActionState(hJump) == kActionReleased, "transition down->none = released");
        ia.Evaluate(none, none); // 無入力
        check(ia.ActionState(hJump) == 0, "transition none->none = 0");
        check(ia.ActionState(HashStr("NoSuchAction")) == 0, "unknown action hash = 0");
    }

    // ---- OR 合成: キー→パッドへ持ち替えても press/release イベントは出ない ----
    {
        InputSnapshot key = Snap();
        Down(key, kVkSpace);
        InputSnapshot pad = Snap();
        pad.padButtons = kPadA;
        ia.Evaluate(pad, key);
        check(ia.ActionState(hJump) == kActionHeld,
              "key->pad crossover stays held (no pressed/released)");
        InputSnapshot mouse = Snap();
        mouse.mouseButtons = 0x01;
        ia.Evaluate(mouse, Snap());
        check(ia.ActionState(hFire) == (kActionHeld | kActionPressed), "mouse binding fires action");
    }

    // ---- 軸: キー成分 ----
    {
        InputSnapshot pos = Snap();
        Down(pos, kVkW);
        ia.Evaluate(pos, Snap());
        check(Near(ia.AxisValue(HashStr("MoveY")), 1.0f), "axis posKey alone = +1");
        InputSnapshot neg = Snap();
        Down(neg, kVkS);
        ia.Evaluate(neg, Snap());
        check(Near(ia.AxisValue(HashStr("MoveY")), -1.0f), "axis negKey alone = -1");
        InputSnapshot both = Snap();
        Down(both, kVkW);
        Down(both, kVkS);
        ia.Evaluate(both, Snap());
        check(Near(ia.AxisValue(HashStr("MoveY")), 0.0f), "axis pos+neg cancel to 0");
    }

    // ---- 軸: パッド成分 (deadzone 0.5 の再スケール / 飽和 / トリガー) ----
    {
        InputSnapshot s = Snap();
        s.padLX = 32767; // フルデフレクション
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hMoveX), 1.0f), "stick full deflection = +1");

        s = Snap();
        s.padLX = static_cast<int16_t>(32767 * 0.3f); // deadzone 0.5 未満
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hMoveX), 0.0f), "stick inside deadzone = 0");

        s = Snap();
        s.padLX = static_cast<int16_t>(std::round(32767 * 0.75f)); // (0.75-0.5)/(1-0.5) = 0.5
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hMoveX), 0.5f, 1e-3f), "deadzone rescales remaining range");

        s = Snap();
        s.padLX = -32768; // 負側の飽和が -1 で対称になる
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hMoveX), -1.0f), "stick -32768 saturates to -1");

        s = Snap();
        s.padRightTrigger = 255;
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hThrottle), 1.0f), "trigger axis 255 = +1 (deadzone 0)");

        // 合成クランプ: キー +1 とスティック +1 を足しても +1 に収まる
        s = Snap();
        Down(s, 0x44); // D
        s.padLX = 32767;
        ia.Evaluate(s, Snap());
        check(Near(ia.AxisValue(hMoveX), 1.0f), "key + pad sum clamps to +1");
    }

    // ---- ApplyDeadzone の端 ----
    {
        check(Near(InputActions::ApplyDeadzone(2.0f, 0.0f), 1.0f)
                  && Near(InputActions::ApplyDeadzone(-2.0f, 0.0f), -1.0f),
              "deadzone 0 clamps to [-1, 1]");
        check(Near(InputActions::ApplyDeadzone(1.0f, 1.0f), 0.0f), "deadzone >= 1 kills axis");
    }

    // ---- 不正 JSON 耐性: 失敗しても空マップで継続 (エンジンは落とさない) ----
    {
        InputActions bad;
        check(!bad.LoadFromJsonText("{ oops") && bad.Actions().empty() && bad.Axes().empty(),
              "malformed json -> false + empty map");
        bad.Evaluate(Snap(), Snap());
        check(bad.ActionState(hJump) == 0 && Near(bad.AxisValue(hMoveX), 0.0f),
              "empty map evaluates to all-zero (no-op)");
        InputActions arr;
        check(!arr.LoadFromJsonText("[1,2,3]"), "non-object root -> false");
        InputActions empty;
        check(empty.LoadFromJsonText("{}") && empty.Actions().empty(),
              "object without actions/axes -> valid empty map");
    }

    // ---- 不明な名前は WARN + スキップし、エントリ自体は生かす ----
    {
        InputActions ia2;
        const bool ok = ia2.LoadFromJsonText(R"({
            "actions": [
              { "name": "Jump", "keys": ["Space", "NoSuchKey"], "pad": ["NoSuchPad"] },
              { "name": "Jump", "keys": ["Z"] },
              { "name": "", "keys": ["X"] }
            ],
            "axes": [ { "name": "Bad", "posKey": "Nope", "padAxis": "QQ" } ]
        })");
        check(ok && ia2.Actions().size() == 1 && ia2.Actions()[0].keys.size() == 1
                  && ia2.Actions()[0].keys[0] == kVkSpace && ia2.Actions()[0].padMask == 0,
              "unknown key/pad names skipped, duplicate + nameless dropped");
        check(ia2.Axes().size() == 1 && ia2.Axes()[0].posKey == 0
                  && ia2.Axes()[0].padAxis == PadAxis::None,
              "unknown posKey/padAxis fall back to unassigned");
    }

    // ---- VK 名テーブルの往復 (収載名 + 16 進名フォールバック) ----
    {
        bool all = true;
        for (int vk = 0; vk < 256; ++vk) {
            const uint8_t v = static_cast<uint8_t>(vk);
            if (const char* n = InputActions::VkNameInTable(v)) {
                if (InputActions::VkFromName(n) != v) {
                    MYE_LOG_ERROR("    round trip failed for '%s'", n);
                    all = false;
                }
            }
        }
        check(all, "all table entries round trip name -> vk");
        check(InputActions::VkNameStr(0xE8) == "0xE8"
                  && InputActions::VkFromName("0xE8") == 0xE8,
              "unlisted vk round trips via 0xNN hex name");
        check(InputActions::VkFromName("NoSuchKey") == 0 && InputActions::VkFromName("") == 0
                  && InputActions::VkFromName("0xZZ") == 0
                  && InputActions::VkFromName("0x123") == 0,
              "unknown / malformed names resolve to 0");
        check(InputActions::PadMaskFromName("A") == kPadA
                  && std::strcmp(InputActions::PadButtonName(kPadA), "A") == 0
                  && InputActions::PadAxisFromName("RT") == PadAxis::RT
                  && std::strcmp(InputActions::PadAxisName(PadAxis::RT), "RT") == 0,
              "pad button / axis name tables round trip");
    }

    // ---- Save -> Load のファイル往復 (定義の構造一致) ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_inputactions_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        const std::wstring rootW = root.wstring();

        InputActions saved;
        saved.LoadFromJsonText(kSampleJson);
        const bool wrote = saved.Save(rootW);
        InputActions loaded;
        loaded.Load(rootW);
        bool same = wrote && loaded.Actions().size() == saved.Actions().size()
            && loaded.Axes().size() == saved.Axes().size();
        if (same) {
            for (size_t i = 0; i < saved.Actions().size(); ++i) {
                const InputActionDef& a = saved.Actions()[i];
                const InputActionDef& b = loaded.Actions()[i];
                same = same && a.name == b.name && a.nameHash == b.nameHash && a.keys == b.keys
                    && a.padMask == b.padMask && a.mouseMask == b.mouseMask;
            }
            for (size_t i = 0; i < saved.Axes().size(); ++i) {
                const InputAxisDef& a = saved.Axes()[i];
                const InputAxisDef& b = loaded.Axes()[i];
                same = same && a.name == b.name && a.nameHash == b.nameHash
                    && a.posKey == b.posKey && a.negKey == b.negKey && a.padAxis == b.padAxis
                    && Near(a.deadzone, b.deadzone);
            }
        }
        check(same, "Save -> Load round trips all definitions");

        // 不在ファイルは空マップ (Load は force で読み直す)
        fs::remove(root / L"input" / L"actions.json", ec);
        loaded.Load(rootW, true);
        check(loaded.Actions().empty() && loaded.Axes().empty(),
              "missing actions.json -> empty map");
        fs::remove_all(root, ec);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== InputActions self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== InputActions self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
