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

    // ---- マルチ入力レーン (M52g) ----
    // ここで固定したいのは 3 つ: レーンが独立していること / 未接続レーンがゼロであること /
    // レーン 1 以降の pressed・released が**そのレーンの前 tick**で決まること
    {
        InputSnapshot cur[kMaxPlayers] = {};
        InputSnapshot prev[kMaxPlayers] = {};
        Down(cur[0], 0x20);   // レーン 0: Space (Jump 押下)
        Down(cur[1], 0x44);   // レーン 1: D (MoveX +1)
        Down(prev[1], 0x44);  // レーン 1 は前 tick も押している = held のみ
        Down(cur[2], 0x20);   // レーン 2 にも入れておく (playerCount=2 で無視されること)
        ia.Evaluate(cur, prev, 2);

        check(ia.ActionState(hJump, 0) == (kActionHeld | kActionPressed),
              "lane 0 evaluates its own snapshot");
        check(ia.ActionState(hJump, 1) == 0, "lane 1 does not see lane 0's keys");
        check(Near(ia.AxisValue(hMoveX, 1), 1.0f), "lane 1 evaluates its own axis");
        check(Near(ia.AxisValue(hMoveX, 0), 0.0f), "lane 0 does not see lane 1's keys");
        check(ia.ActionState(hJump, 2) == 0 && Near(ia.AxisValue(hMoveX, 2), 0.0f),
              "lanes beyond playerCount are forced to zero (not left stale)");
        check(ia.ActionState(hJump, kMaxPlayers) == 0
                  && Near(ia.AxisValue(hMoveX, kMaxPlayers), 0.0f),
              "out-of-range lane index reads as zero");

        // 未接続レーンは「評価をスキップ」ではなく「毎 tick ゼロで潰す」— スキップだと
        // 切断した瞬間の値が残り、押しっぱなしのまま止まる
        InputSnapshot cur2[kMaxPlayers] = {};
        Down(cur2[1], 0x44);
        ia.Evaluate(cur2, cur, 2); // レーン 0 は離した / レーン 1 は継続
        check(ia.ActionState(hJump, 0) == kActionReleased,
              "lane 0 released is decided by lane 0's previous snapshot");
        check(ia.ActionState(hJump, 1) == 0 && Near(ia.AxisValue(hMoveX, 1), 1.0f),
              "lane 1 is unaffected by what lane 0 did");
        ia.Evaluate(cur2, cur2, 1); // playerCount を 1 へ落とす
        check(Near(ia.AxisValue(hMoveX, 1), 0.0f),
              "shrinking playerCount zeroes the lanes that dropped out");
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

    // ---- 生マウスデルタ (M64a) ----
    // SynthLaneInput は (tick, lane) の純関数という契約なので、新フィールドも
    // その契約に従っていること + 恒常ゼロでない (= .rep の被覆として意味がある) ことを見る
    {
        const InputSnapshot a = SynthLaneInput(37, 1);
        const InputSnapshot b = SynthLaneInput(37, 1);
        check(a.mouseDeltaX == b.mouseDeltaX && a.mouseDeltaY == b.mouseDeltaY,
              "synth: mouse delta is a pure function of (tick, lane)");

        bool nonZero = false;
        bool laneDiffers = false;
        for (uint64_t t = 0; t < 256 && !(nonZero && laneDiffers); ++t) {
            const InputSnapshot s0 = SynthLaneInput(t, 0);
            const InputSnapshot s1 = SynthLaneInput(t, 1);
            nonZero = nonZero || s0.mouseDeltaX != 0 || s0.mouseDeltaY != 0;
            laneDiffers = laneDiffers
                || s0.mouseDeltaX != s1.mouseDeltaX || s0.mouseDeltaY != s1.mouseDeltaY;
        }
        check(nonZero, "synth: mouse delta is actually driven (not always 0)");
        check(laneDiffers, "synth: lanes get different mouse deltas");

        // ★直流バイアスが無いこと。バイアスが残ると、デルタを積分する側 (一人称の視点角)
        //   が必ずクランプへ張り付き、合成入力の実行が「ずっと真下を向いて歩く」になる
        {
            // 判定は平均 |0.15| カウント未満。厳密な 0 は要求しない — 合成入力は
            // **ブロック単位で量子化**されている (レーン 0 なら 11 tick に 1 個の
            // 独立サンプル) ので、有限区間の平均には必ず端数が残る。
            // ★`(h & 15) - 7` のような非対称な範囲だと平均が**ちょうど +0.5** になる。
            //   この閾値との差は一桁あるので、この幅で十分に検出できる
            const int64_t n = 65536;
            for (uint32_t lane = 0; lane < 2; ++lane) {
                int64_t sumX = 0;
                int64_t sumY = 0;
                for (int64_t t = 0; t < n; ++t) {
                    const InputSnapshot s = SynthLaneInput(static_cast<uint64_t>(t), lane);
                    sumX += s.mouseDeltaX;
                    sumY += s.mouseDeltaY;
                }
                const auto small = [n](int64_t sum) { return sum * 20 < n * 3 && -sum * 20 < n * 3; };
                check(small(sumX) && small(sumY),
                      "synth: mouse delta has no DC bias (integrating it must not pin to a clamp)");
            }
        }
        // ★位置は動かさない。合成入力で UI ヒットテストが誤爆しないことの回帰
        check(SynthLaneInput(37, 0).mouseX == 0 && SynthLaneInput(37, 0).mouseY == 0,
              "synth: mouse position stays untouched (UI hit-test must not fire)");
    }

    // ---- ゲーム面と文字キュー (M75b) ----
    // 取り込み (WM_CHAR / WM_MOUSEMOVE) → CaptureSnapshot → ConsumeChars の往復。
    // ウィンドウは作らない — HandleMessage は hwnd を読まないので nullptr で叩ける
    {
        const uint32_t kWmChar = 0x0102;      // WM_CHAR (Windows.h をこのテストへ持ち込まない)
        const uint32_t kWmMouseMove = 0x0200; // WM_MOUSEMOVE
        Input dev;
        int64_t res = 0;
        const auto type = [&dev, &res, kWmChar](uint64_t cu) {
            dev.HandleMessage(nullptr, kWmChar, cu, 0, res);
        };
        const auto tailZero = [](const InputSnapshot& s) {
            bool ok = true;
            for (int i = s.charCount; i < 8; ++i) {
                ok = ok && s.chars[i] == 0;
            }
            for (int i = 0; i < 7; ++i) {
                ok = ok && s.pad3[i] == 0;
            }
            return ok;
        };
        type('h');
        type(0x08); // Backspace (制御文字)
        type('i');
        type(0x0D); // Enter
        type(0x7F); // Ctrl+Backspace
        type(0xD83D); // サロゲートの片割れ
        type(0x3042); // あ (BMP の可視文字)
        dev.HandleMessage(nullptr, kWmMouseMove, 0, (static_cast<int64_t>(50) << 16) | 100, res);
        InputSurface surf;
        surf.w = 960;
        surf.h = 540;
        InputSnapshot s = dev.CaptureSnapshot(0, surf);
        check(s.charCount == 3 && s.chars[0] == 'h' && s.chars[1] == 'i' && s.chars[2] == 0x3042,
              "chars: visible BMP characters are queued, control chars and surrogates are dropped");
        check(tailZero(s), "chars: the tail past charCount and pad3 are zero (no garbage in .rep)");
        check(s.mouseSurfX == 100.0f && s.mouseSurfY == 50.0f && s.surfW == 960 && s.surfH == 540,
              "surface: the mouse is recorded in surface px together with the surface size");
        check(dev.CaptureSnapshot(0, surf).charCount == 3,
              "chars: capturing again (a frame without a tick) does not consume the queue");
        const InputSnapshot lane1 = dev.CaptureSnapshot(1, surf);
        check(lane1.charCount == 0 && lane1.surfW == 0 && lane1.mouseSurfX == 0.0f,
              "chars/surface: lanes other than 0 carry neither");

        // 写した後に 1 文字届いた → 写した 3 文字だけ捨て、遅れて来た文字は残す
        type('!');
        dev.ConsumeChars(3);
        s = dev.CaptureSnapshot(0, surf);
        check(s.charCount == 1 && s.chars[0] == '!' && tailZero(s),
              "chars: ConsumeChars drops only the captured prefix and keeps late arrivals");
        dev.ConsumeChars(s.charCount);
        for (int i = 0; i < 12; ++i) {
            type(static_cast<uint64_t>('a' + i));
        }
        s = dev.CaptureSnapshot(0, surf);
        check(s.charCount == 8 && s.chars[7] == 'h', "chars: the queue saturates at 8 (overflow is dropped)");

        const InputSurface degenerate;
        const InputSnapshot d = dev.CaptureSnapshot(0, degenerate);
        check(d.surfW == 0 && d.surfH == 0 && d.mouseSurfX == 100.0f,
              "surface: a degenerate surface stays 0 (readers fall back to the reference canvas)");
    }

    // 合成入力の面と文字 (M75b)。(tick, lane) の純関数のまま、レーン 0 にだけ可視 ASCII が周期的に載る
    {
        const auto sameBytes = [](const InputSnapshot& a, const InputSnapshot& b) {
            const auto* pa = reinterpret_cast<const uint8_t*>(&a);
            const auto* pb = reinterpret_cast<const uint8_t*>(&b);
            for (size_t i = 0; i < sizeof(InputSnapshot); ++i) {
                if (pa[i] != pb[i]) {
                    return false;
                }
            }
            return true;
        };
        bool pure = true;
        bool sawChars = false;
        bool visible = true;
        bool surfaceFixed = true;
        bool lane1Clean = true;
        for (uint64_t t = 0; t < 400; ++t) {
            const InputSnapshot s0 = SynthLaneInput(t, 0);
            pure = pure && sameBytes(s0, SynthLaneInput(t, 0));
            sawChars = sawChars || s0.charCount > 0;
            surfaceFixed = surfaceFixed && s0.surfW == 1920 && s0.surfH == 1080;
            visible = visible && s0.charCount <= 8;
            for (int i = 0; i < 8; ++i) {
                const bool used = i < s0.charCount;
                visible = visible
                    && (used ? (s0.chars[i] >= 0x20 && s0.chars[i] < 0x7F) : (s0.chars[i] == 0));
            }
            const InputSnapshot s1 = SynthLaneInput(t, 1);
            lane1Clean = lane1Clean && s1.charCount == 0 && s1.surfW == 0 && s1.surfH == 0;
        }
        check(pure, "synth: the M75b fields stay a pure function of (tick, lane)");
        check(sawChars, "synth: lane 0 actually carries characters (coverage for .rep / snapshot)");
        check(visible, "synth: only visible ASCII is synthesized and the tail stays zero");
        check(surfaceFixed, "synth: lane 0 surface is pinned to the reference 1920x1080");
        check(lane1Clean, "synth: lanes other than 0 carry no characters and no surface");
    }

    // ---- ゲームの画面の外のクリックを捨てる (2026-09-14、エディタの Game ビュー) ----
    // 停止ボタンのクリックがゲームに届いてカーソルを掴み直していた不具合の固定。
    // 捨てるのはボタンとホイールだけ (キー・位置・生デルタは残す)
    {
        InputRect area;
        area.x = 100;
        area.y = 50;
        area.w = 640;
        area.h = 360;
        InputSnapshot base = Snap();
        base.mouseX = 420;
        base.mouseY = 200;
        base.mouseButtons = 0x03;
        base.wheelDelta = 120;
        base.mouseDeltaX = 5;
        Down(base, kVkW);

        InputSnapshot inside = base;
        Input::MaskMouseOutside(inside, area);
        check(std::memcmp(&inside, &base, sizeof(base)) == 0, "game area: a click inside passes untouched");

        InputSnapshot outside = base;
        outside.mouseX = 99; // 左端の 1 px 外
        Input::MaskMouseOutside(outside, area);
        check(outside.mouseButtons == 0 && outside.wheelDelta == 0,
              "game area: outside drops mouse buttons and wheel");
        check(outside.KeyDown(kVkW) && outside.mouseX == 99 && outside.mouseDeltaX == 5,
              "game area: outside keeps keys, position and raw delta");

        InputSnapshot rightEdge = base;
        rightEdge.mouseX = area.x + area.w; // 半開区間 = 右端は外
        Input::MaskMouseOutside(rightEdge, area);
        InputSnapshot leftEdge = base;
        leftEdge.mouseX = area.x; // 左端は中
        Input::MaskMouseOutside(leftEdge, area);
        check(rightEdge.mouseButtons == 0 && leftEdge.mouseButtons == 0x03,
              "game area: the left edge is inside, the right edge is outside");

        InputSnapshot hidden = base;
        Input::MaskMouseOutside(hidden, InputRect{});
        check(hidden.mouseButtons == 0, "game area: an empty area (view not visible) receives no clicks");
    }

    // ---- マウス量の持ち越し (2026-09-15、PointerDeltaCarry) ----
    // EngineLoop と同じ順 (写す → AddTo → tick ごとに読んで ClearAfterTick → EndFrame) で回し、
    // フレームと tick の本数が食い違っても「動かした総量 = tick が読んだ総量」になることを見る。
    // 本数の列は 180Hz の 0,0,1 に、fps が落ちた 2 本 / 3 本のフレームを混ぜたもの
    {
        PointerDeltaCarry carry;
        const int ticksPerFrame[] = { 0, 0, 1, 0, 0, 1, 2, 0, 3, 1 };
        int32_t movedX = 0, movedWheel = 0, readX = 0, readY = 0, readWheel = 0;
        int repeated = 0; // 同じフレームの 2 本目以降が非 0 を読んだ回数
        for (int ticks : ticksPerFrame) {
            InputSnapshot s = Snap(); // このフレームで写した量
            s.mouseDeltaX = 5;
            s.mouseDeltaY = -2;
            s.wheelDelta = 120;
            movedX += 5;
            movedWheel += 120;
            carry.AddTo(s);
            for (int t = 0; t < ticks; ++t) {
                if (t > 0 && (s.mouseDeltaX != 0 || s.wheelDelta != 0)) {
                    ++repeated;
                }
                readX += s.mouseDeltaX;
                readY += s.mouseDeltaY;
                readWheel += s.wheelDelta;
                PointerDeltaCarry::ClearAfterTick(s);
            }
            carry.EndFrame(s, /*drop*/ false);
        }
        check(readX == movedX && readY == -movedX * 2 / 5 && readWheel == movedWheel,
              "pointer carry: movement in frames that ran no tick reaches the next tick");
        check(repeated == 0, "pointer carry: a frame that runs several ticks hands the movement to the first one only");

        PointerDeltaCarry dropped;
        InputSnapshot scrub = Snap();
        scrub.mouseDeltaX = 7;
        scrub.wheelDelta = 120;
        dropped.AddTo(scrub);
        dropped.EndFrame(scrub, /*drop*/ true);
        InputSnapshot resumed = Snap();
        dropped.AddTo(resumed);
        check(resumed.mouseDeltaX == 0 && resumed.wheelDelta == 0,
              "pointer carry: drop (scrub / focus loss) carries nothing into the resumed tick");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== InputActions self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== InputActions self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
