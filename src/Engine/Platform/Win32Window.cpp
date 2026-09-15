#include "Engine/Platform/Win32Window.h"

#include "Engine/Core/Check.h"
#include "Engine/Core/Log.h"

#include <Windows.h>

namespace mye {
namespace {

constexpr const wchar_t* kClassName = L"MyEngineWindowClass";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Win32Window* window = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (window) {
        return static_cast<LRESULT>(window->HandleMsg(hwnd, msg, wparam, lparam));
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

bool Win32Window::Create(const WindowDesc& desc)
{
    // Per-Monitor V2 DPI 対応 (マニフェスト不要の実行時指定)。ウィンドウ作成前に 1 回だけ
    static bool dpiSet = false;
    if (!dpiSet) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        dpiSet = true;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    static ATOM classAtom = RegisterClassExW(&wc);
    MYE_CHECKF(classAtom != 0, "RegisterClassExW failed (%lu)", GetLastError());

    // クライアント領域が desc.width/height になるよう外形を補正
    RECT rect = { 0, 0, desc.width, desc.height };
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kClassName, desc.title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, this);
    if (!hwnd) {
        MYE_LOG_ERROR("CreateWindowExW failed (%lu)", GetLastError());
        return false;
    }

    hwnd_ = hwnd;
    width_ = desc.width;
    height_ = desc.height;
    // 見せる前に当てる = 起動直後にウィンドウの姿を一瞬出してから全面へ化けない
    SetMode(desc.mode);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MYE_LOG_INFO("Window created: %dx%d", width_, height_);
    return true;
}

void Win32Window::Destroy()
{
    if (hwnd_) {
        DestroyWindow(static_cast<HWND>(hwnd_));
        hwnd_ = nullptr;
    }
}

float Win32Window::DpiScale() const
{
    if (!hwnd_) {
        return 1.0f;
    }
    // PerMonitorV2 なので生成時のモニタの実 DPI が返る (96 = 100%)。
    // モニタ間移動 (WM_DPICHANGED) への追従は未対応 — 起動時の値で固定
    return static_cast<float>(GetDpiForWindow(static_cast<HWND>(hwnd_))) / 96.0f;
}

void Win32Window::AddMsgHandler(MsgHandler handler)
{
    handlers_.push_back(std::move(handler));
}

bool Win32Window::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

bool Win32Window::ConsumeResize()
{
    const bool r = resized_;
    resized_ = false;
    return r;
}

void Win32Window::SetMode(WindowMode mode)
{
    const HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr || mode == mode_) {
        return;
    }
    // WS_VISIBLE は引き継ぐ (Create から見せる前に呼ばれたときは隠れたまま切り替える)
    const LONG_PTR visible = GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE;
    if (mode == WindowMode::Borderless) {
        // 戻り先はワークスペース座標の通常位置 + 最大化。GetWindowRect だと最大化中の大きさを覚えてしまう
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hwnd, &wp)) {
            windowedRect_[0] = wp.rcNormalPosition.left;
            windowedRect_[1] = wp.rcNormalPosition.top;
            windowedRect_[2] = wp.rcNormalPosition.right;
            windowedRect_[3] = wp.rcNormalPosition.bottom;
            windowedMaximized_ = (wp.showCmd == SW_SHOWMAXIMIZED);
            hasWindowedRect_ = true;
        }
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            MYE_LOG_WARN("[window] GetMonitorInfo failed (%lu) - staying windowed", GetLastError());
            return;
        }
        // ★スタイルは丸ごと置き換える。WS_MAXIMIZE を残すと、戻したときに最大化の扱いが食い違う
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | visible);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        if (hasWindowedRect_) {
            WINDOWPLACEMENT wp = {};
            wp.length = sizeof(wp);
            wp.rcNormalPosition = { windowedRect_[0], windowedRect_[1], windowedRect_[2], windowedRect_[3] };
            wp.showCmd = !visible ? SW_HIDE : (windowedMaximized_ ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
            SetWindowPlacement(hwnd, &wp);
        }
        // 枠の再計算 (SetWindowLongPtr だけでは非クライアント領域が古いまま)
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    mode_ = mode;
}

bool Win32Window::HasFocus() const
{
    return hwnd_ != nullptr && GetForegroundWindow() == static_cast<HWND>(hwnd_);
}

intptr_t Win32Window::HandleMsg(void* hwndRaw, uint32_t msg, uint64_t wparam, int64_t lparam)
{
    const HWND hwnd = static_cast<HWND>(hwndRaw);

    for (MsgHandler& handler : handlers_) {
        int64_t result = 0;
        if (handler(hwndRaw, msg, wparam, lparam, result)) {
            return result;
        }
    }

    switch (msg) {
    case WM_SIZE: {
        if (wparam == SIZE_MINIMIZED) {
            minimized_ = true;
        } else {
            minimized_ = false;
            const int w = LOWORD(lparam);
            const int h = HIWORD(lparam);
            if (w != width_ || h != height_) {
                width_ = w;
                height_ = h;
                resized_ = true;
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
}

} // namespace mye
