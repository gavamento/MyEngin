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
        s.wheelDelta = wheelAccum_;
        s.mouseButtons = buttons_;
        wheelAccum_ = 0;
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

    // ★マウスは動かさない。動かすとエディタの GameView ヒットテストと UI が
    //   合成入力で誤爆し、「検証フラグを足した途端に UI が勝手に操作される」ことになる
    return s;
}

} // namespace mye
