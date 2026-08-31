#include "Engine/Platform/Input.h"

#include <cstring>

#include <Windows.h>
#include <windowsx.h>

#include <Xinput.h>
// XInput9_1_0 は Win7+ で常在 (再頒布 DLL 不要)。Engine.lib 経由で exe のリンクに伝播する
#pragma comment(lib, "Xinput9_1_0.lib")

namespace mye {

void Input::SetKey(uint8_t vk, bool down)
{
    if (down) {
        keys_[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
    } else {
        keys_[vk >> 3] &= static_cast<uint8_t>(~(1u << (vk & 7)));
    }
}

bool Input::HandleMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result)
{
    (void)hwnd;
    (void)result;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        SetKey(static_cast<uint8_t>(wparam & 0xFF), true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        SetKey(static_cast<uint8_t>(wparam & 0xFF), false);
        break;
    case WM_MOUSEMOVE:
        mouseX_ = GET_X_LPARAM(lparam);
        mouseY_ = GET_Y_LPARAM(lparam);
        break;
    case WM_INPUT: {
        // 生マウスデルタ (M64a)。AttachRawInput 済みのときだけ届く。
        // ★ここで mouseX_/mouseY_ は触らない — 絶対座標の正本は今も WM_MOUSEMOVE で、
        //   生デルタは**別レーン**として積む。混ぜるとカーソルロック中に
        //   「見えないカーソルの位置」で UI のヒットテストが動いてしまう
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        const HRAWINPUT h = reinterpret_cast<HRAWINPUT>(static_cast<intptr_t>(lparam));
        if (GetRawInputData(h, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER))
            == static_cast<UINT>(-1)) {
            break;
        }
        if (raw.header.dwType != RIM_TYPEMOUSE) {
            break;
        }
        const RAWMOUSE& m = raw.data.mouse;
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            // リモートデスクトップ / タブレット / 一部の仮想機は正規化**絶対**座標を寄越す。
            // 前回値との差を取る。基準が無い初回は 0 (視点を跳ねさせない)
            if (rawAbsValid_) {
                mouseDeltaX_ += static_cast<int32_t>(m.lLastX) - rawAbsX_;
                mouseDeltaY_ += static_cast<int32_t>(m.lLastY) - rawAbsY_;
            }
            rawAbsX_ = static_cast<int32_t>(m.lLastX);
            rawAbsY_ = static_cast<int32_t>(m.lLastY);
            rawAbsValid_ = true;
        } else {
            mouseDeltaX_ += static_cast<int32_t>(m.lLastX);
            mouseDeltaY_ += static_cast<int32_t>(m.lLastY);
            rawAbsValid_ = false;
        }
        break;
    }
    case WM_LBUTTONDOWN: buttons_ |= 1u << 0; break;
    case WM_LBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 0)); break;
    case WM_RBUTTONDOWN: buttons_ |= 1u << 1; break;
    case WM_RBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 1)); break;
    case WM_MBUTTONDOWN: buttons_ |= 1u << 2; break;
    case WM_MBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 2)); break;
    case WM_XBUTTONDOWN:
        buttons_ |= static_cast<uint8_t>((GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? (1u << 3) : (1u << 4));
        break;
    case WM_XBUTTONUP:
        buttons_ &= static_cast<uint8_t>(~((GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? (1u << 3) : (1u << 4)));
        break;
    case WM_MOUSEWHEEL:
        wheelAccum_ += GET_WHEEL_DELTA_WPARAM(wparam);
        break;
    case WM_KILLFOCUS:
        // フォーカス喪失中の KEYUP は届かないため全解除 (キー押しっぱなし防止)
        memset(keys_, 0, sizeof(keys_));
        buttons_ = 0;
        // M64a: 溜まっていた生デルタも捨てる。裏で動かしたぶんが復帰した瞬間に
        // 1 tick でまとめて入ると視点が飛ぶ。絶対値モードの基準も無効化する
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
        rawAbsValid_ = false;
        break;
    default:
        break;
    }
    return false; // 消費しない (ImGui など他のハンドラにも流す)
}

InputSnapshot Input::CaptureSnapshot(uint32_t lane)
{
    InputSnapshot s = {};
    if (lane == 0) {
        // キーボード/マウスはレーン 0 だけが受け取る (Input.h のレーン規約)。
        // ★wheel の累積リセットもここでしかしない — レーンの数だけ呼ばれるので、
        //   どのレーンでも消費する作りにすると 2 人目以降でホイールが消える
        memcpy(s.keys, keys_, sizeof(s.keys));
        s.mouseX = mouseX_;
        s.mouseY = mouseY_;
        s.mouseDeltaX = mouseDeltaX_;
        s.mouseDeltaY = mouseDeltaY_;
        s.wheelDelta = wheelAccum_;
        s.mouseButtons = buttons_;
        wheelAccum_ = 0;
        mouseDeltaX_ = 0; // M64a: wheel と同じ「1 tick で消費」規約
        mouseDeltaY_ = 0;
    }

    // gamepad (XInput、スロット = レーン番号)。verify 中は記録値が上書きするので透過 (spec 11.3)
    XINPUT_STATE xs = {};
    if (lane < kMaxPlayers && XInputGetState(lane, &xs) == ERROR_SUCCESS) {
        s.padConnected = 1;
        s.padButtons = xs.Gamepad.wButtons;
        s.padLeftTrigger = xs.Gamepad.bLeftTrigger;
        s.padRightTrigger = xs.Gamepad.bRightTrigger;
        s.padLX = xs.Gamepad.sThumbLX;
        s.padLY = xs.Gamepad.sThumbLY;
        s.padRX = xs.Gamepad.sThumbRX;
        s.padRY = xs.Gamepad.sThumbRY;
    }
    return s;
}

void Input::ApplyVibration(float left, float right)
{
    const auto quantize = [](float v) -> uint16_t {
        const float c = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint16_t>(c * 65535.0f + 0.5f);
    };
    const uint16_t l = quantize(left);
    const uint16_t r = quantize(right);
    if (l == lastVibLeft_ && r == lastVibRight_) {
        return; // 値が変わらない限りドライバへ再送しない
    }
    lastVibLeft_ = l;
    lastVibRight_ = r;
    XINPUT_VIBRATION vib = {};
    vib.wLeftMotorSpeed = l;
    vib.wRightMotorSpeed = r;
    XInputSetState(0, &vib);
}

void Input::AttachRawInput(void* hwnd)
{
    if (hwnd == nullptr) {
        return; // ヘッドレス (--selftest 等) はウィンドウを作らない
    }
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01; // Generic Desktop Controls
    rid.usUsage = 0x02;     // Mouse
    // dwFlags = 0: 従来のマウスメッセージを**殺さない**。RIDEV_NOLEGACY を付けると
    // WM_MOUSEMOVE / WM_LBUTTONDOWN が止まり、ImGui もエディタのヒットテストも死ぬ。
    // RIDEV_INPUTSINK も付けない — 非フォアグラウンド時にまで視点を回す必要は無い
    rid.dwFlags = 0;
    rid.hwndTarget = static_cast<HWND>(hwnd);
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        // 失敗しても致命ではない (デルタが常に 0 になるだけ) ので続行する
        OutputDebugStringW(L"[input] RegisterRawInputDevices failed; mouse delta will be 0\n");
    }
}

void Input::ApplyCursorLock(void* hwnd, bool locked)
{
    const HWND h = static_cast<HWND>(hwnd);
    if (locked && h != nullptr) {
        // ★毎フレーム打ち直す。ClipCursor はスクリーン座標の矩形なので、
        //   ウィンドウを動かす / サイズを変えると前の矩形は無関係な場所に残る
        RECT rc = {};
        GetClientRect(h, &rc);
        POINT tl = { rc.left, rc.top };
        POINT br = { rc.right, rc.bottom };
        ClientToScreen(h, &tl);
        ClientToScreen(h, &br);
        const RECT screenRect = { tl.x, tl.y, br.x, br.y };
        ClipCursor(&screenRect);
        if (!cursorLocked_) {
            // 掴んだ瞬間だけ中央へ寄せる。毎フレーム SetCursorPos しないのは、
            // 生デルタがカーソル位置に依存しないので単に無駄だから
            SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
        }
    } else if (cursorLocked_) {
        ClipCursor(nullptr);
    }
    if (locked != cursorLocked_) {
        // ★ShowCursor は**内部カウンタ**。同じ向きに 2 回呼ぶとカーソルが戻らなくなる。
        //   状態が変わったときだけ 1 回呼ぶ (だから現状態を持っている)
        ShowCursor(locked ? FALSE : TRUE);
        cursorLocked_ = locked;
    }
}

namespace {

// SplitMix64 (整数四則とシフトのみ = /fp:precise 以前に浮動小数を触らない)。
// PCG32 (エンジンの sim 乱数) を使わないのは、合成入力が **sim の外**で作られる値で、
// ワールド RNG の列を 1 歩でも進めてはいけないため
uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

InputSnapshot SynthLaneInput(uint64_t tick, uint32_t lane)
{
    InputSnapshot s = {};
    // レーンごとにブロック長を変える = 同じ tick でもレーン間で位相も内容も揃わない。
    // 「全レーンに同じ入力を配ってしまった」実装ミスがここで必ず値の差として出る
    const uint64_t block = 11ull + static_cast<uint64_t>(lane) * 5ull;
    const uint64_t h = SplitMix64((tick / block) * 4ull + lane + 1ull);

    const auto press = [&s](uint8_t vk) { s.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7)); };
    // actions.json の既定マップ (MoveX = A/D、MoveY = W/S、Jump = Space) を叩く。
    // 左右/上下は排他にする — 同時押しは軸が 0 になるだけで動きが死ぬ
    if (h & 1u) {
        press('A');
    } else if (h & 2u) {
        press('D');
    }
    if (h & 4u) {
        press('W');
    } else if (h & 8u) {
        press('S');
    }
    if (((h >> 4) & 7u) == 0u) {
        press(VK_SPACE); // Jump は疎に (pressed/released のエッジを作るのが目的)
    }

    // パッド成分も埋める: アクションマップのデッドゾーン適用とパッドボタンの経路、
    // および「レーン n はパッド n を見る」という規約そのものを被覆に入れる
    s.padConnected = 1;
    s.padLX = static_cast<int16_t>((static_cast<int32_t>((h >> 8) & 0xFFFFu) - 32768) / 2);
    s.padLY = static_cast<int16_t>((static_cast<int32_t>((h >> 24) & 0xFFFFu) - 32768) / 2);
    s.padButtons = static_cast<uint16_t>((h >> 40) & 0x1000u); // A ボタンだけ

    // ★マウスの**位置**は動かさない。動かすとエディタの GameView ヒットテストと UI が
    //   合成入力で誤爆し、「検証フラグを足した途端に UI が勝手に操作される」ことになる。
    // 一方 **生デルタ (M64a) は載せる** — どのヒットテストにも入らない純粋な視点入力で、
    // これを流して初めて新フィールドが .rep と SimSnapshot の往復照合に被覆される。
    //
    // ★**平均 0 になる作り方をすること**。`((h >> n) & 15) - 7` のような非対称な範囲だと
    //   1 カウントぶんの直流バイアスが残り、**デルタを積分する側 (視点角) がクランプに
    //   張り付く** — 実測で 1400 tick 後にピッチが上限 80 度に到達し、合成入力の実行が
    //   「ずっと真下を向いて歩くだけ」になった (診断としても絵としても読めない)。
    //   独立な 3bit を 2 本引いて差を取れば、範囲 [-7,+7] で平均が厳密に 0 になる
    const auto span3 = [h](int shift) { return static_cast<int32_t>((h >> shift) & 7u); };
    s.mouseDeltaX = span3(40) - span3(43);
    s.mouseDeltaY = span3(46) - span3(49);
    return s;
}

} // namespace mye
