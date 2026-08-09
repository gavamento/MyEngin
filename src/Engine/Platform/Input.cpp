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

InputSnapshot Input::CaptureSnapshot()
{
    InputSnapshot s = {};
    memcpy(s.keys, keys_, sizeof(s.keys));
    s.mouseX = mouseX_;
    s.mouseY = mouseY_;
    s.wheelDelta = wheelAccum_;
    s.mouseButtons = buttons_;
    wheelAccum_ = 0;

    // gamepad (XInput、パッド 0 のみ)。verify 中は記録値が上書きするので透過 (spec 11.3)
    XINPUT_STATE xs = {};
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
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

} // namespace mye
