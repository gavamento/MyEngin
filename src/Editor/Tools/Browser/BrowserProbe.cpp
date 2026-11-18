//====================================================================================
//                          BrowserProbe.cpp
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          ブラウザ埋め込みの検証用: ダミー窓プロセスと実ブラウザのプローブの実装
//====================================================================================
#include "Editor/Tools/Browser/BrowserProbe.h"

#include <cstdio>
#include <vector>

#include <Windows.h>

#include "Editor/Tools/Browser/BrowserCatalog.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Win32Window.h"

namespace mye {
namespace {

constexpr UINT_PTR kDummyHangTimer = 1;
constexpr UINT_PTR kDummyUnparentTimer = 2;
constexpr int kProbeWarmupFrames = 60;
constexpr int64_t kProbeOverallTimeoutMs = 60000;

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == kDummyHangTimer) {
            KillTimer(hwnd, kDummyHangTimer);
            Sleep(kBrowserDummyHangMs); // UI スレッドを止める (応答なしの再現)
            return 0;
        }
        if (wp == kDummyUnparentTimer) {
            KillTimer(hwnd, kDummyUnparentTimer);
            SetParent(hwnd, nullptr); // スタイルは WS_CHILD のまま。親を外す窓の再現
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        // 検証キャプチャで見分けられるよう単色で塗る
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(40, 160, 90));
        FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring SelfExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

} // namespace

int RunBrowserDummyWindow(int hangAfterMs, int unparentAfterMs)
{
    EnsureProcessDpiAwareness(); // エディタ (Per-Monitor V2) と DPI 認識を揃える
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.lpszClassName = L"MyEngineBrowserDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kBrowserDummyTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 800, 600,
                                nullptr, nullptr, inst, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }
    if (hangAfterMs > 0) {
        SetTimer(hwnd, kDummyHangTimer, static_cast<UINT>(hangAfterMs), nullptr);
    }
    if (unparentAfterMs > 0) {
        SetTimer(hwnd, kDummyUnparentTimer, static_cast<UINT>(unparentAfterMs), nullptr);
    }
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

BrowserEmbedProbe::BrowserEmbedProbe(const BrowserProbeOptions& options) : options_(options)
{
    host_.SetClipChildren(options.clipChildren);
}

BrowserEmbedProbe::~BrowserEmbedProbe() = default;

void BrowserEmbedProbe::Finish(bool ok, const char* why)
{
    if (!ok && host_.State() == BrowserState::Failed && host_.LastTargetHwnd() != nullptr) {
        // 失敗した窓が元のスタイルの普通の窓として残っているか (WS_CHILD のまま取り残されていないか)
        HWND w = static_cast<HWND>(host_.LastTargetHwnd());
        if (IsWindow(w)) {
            RECT rc = {};
            GetWindowRect(w, &rc);
            std::printf("[browser-probe] failed window: ancestor_is_desktop=%d child_bit=%d visible=%d style=0x%llx rect_screen=(%ld,%ld,%ld,%ld)\n",
                        GetAncestor(w, GA_PARENT) == GetDesktopWindow() ? 1 : 0, (GetWindowLongPtrW(w, GWL_STYLE) & WS_CHILD) ? 1 : 0,
                        IsWindowVisible(w) ? 1 : 0, static_cast<unsigned long long>(GetWindowLongPtrW(w, GWL_STYLE)), rc.left, rc.top,
                        rc.right, rc.bottom);
        }
    }
    exitCode_ = ok ? 0 : 1;
    phase_ = Phase::Done;
    std::printf("[browser-probe] %s: %s\n", ok ? "PASS" : "FAIL", why);
    std::fflush(stdout);
    MYE_LOG_INFO("[browser-probe] %s: %s", ok ? "PASS" : "FAIL", why);
}

void BrowserEmbedProbe::LogEmbedded(void* mainHwnd)
{
    HWND main = static_cast<HWND>(mainHwnd);
    HWND child = static_cast<HWND>(host_.ChildHwnd());
    const bool parentIsMain = GetParent(child) == main;
    std::printf("[browser-probe] O1 find_ms=%lld launch_pid=%u window_pid=%u parent_is_main=%d dpi_equal=%d clip_children=%d\n",
                static_cast<long long>(host_.FindElapsedMs()), host_.LastLaunchPid(), host_.ChildPid(), parentIsMain ? 1 : 0,
                AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(child), GetWindowDpiAwarenessContext(main)) ? 1 : 0,
                (GetWindowLongPtrW(main, GWL_STYLE) & WS_CLIPCHILDREN) ? 1 : 0);
    embeddedOk_ = parentIsMain;
    BringWindowToTop(main); // 他の窓 (ブラウザの元の窓) に隠れていると検証キャプチャに写らない
    std::fflush(stdout);
}

void BrowserEmbedProbe::SampleHold(void* mainHwnd, const char* tag)
{
    HWND main = static_cast<HWND>(mainHwnd);
    HWND child = static_cast<HWND>(host_.ChildHwnd());
    if (child == nullptr || !IsWindow(child)) {
        std::printf("[browser-probe] %s child window is gone\n", tag);
        return;
    }
    RECT rc = {};
    GetWindowRect(child, &rc);
    POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
    MapWindowPoints(nullptr, main, pts, 2);
    const bool rectMatch = pts[0].x == wanted_.left && pts[0].y == wanted_.top && pts[1].x == wanted_.right && pts[1].y == wanted_.bottom;
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    HWND focus = GetFocus();
    std::printf("[browser-probe] %s parent_is_main=%d style=0x%llx exstyle=0x%llx visible=%d hung=%d rect_client=(%ld,%ld,%ld,%ld) "
                "rect_match=%d focus_in_child=%d raw_input_total=%llu\n",
                tag, GetParent(child) == main ? 1 : 0, static_cast<unsigned long long>(style),
                static_cast<unsigned long long>(GetWindowLongPtrW(child, GWL_EXSTYLE)), IsWindowVisible(child) ? 1 : 0,
                IsHungAppWindow(child) ? 1 : 0, pts[0].x, pts[0].y, pts[1].x, pts[1].y, rectMatch ? 1 : 0,
                (focus == child || (focus != nullptr && IsChild(child, focus))) ? 1 : 0,
                static_cast<unsigned long long>(rawInputCount_));
    if (!rectMatch || GetParent(child) != main) {
        embeddedOk_ = false;
        wchar_t cls[128] = {};
        GetClassNameW(GetParent(child), cls, 128);
        std::printf("[browser-probe] %s parent=%p main=%p parent_class='%s' owner=%p\n", tag, static_cast<void*>(GetParent(child)),
                    static_cast<void*>(main), WideToUtf8(cls).c_str(), static_cast<void*>(GetWindow(child, GW_OWNER)));
    }
    std::fflush(stdout);
}

void BrowserEmbedProbe::Frame(void* mainHwnd, int64_t nowMs)
{
    if (phase_ == Phase::Done) {
        return;
    }
    ++frames_;
    if (phase_ == Phase::Warmup) {
        if (frames_ < kProbeWarmupFrames) {
            return;
        }
        host_.SetMainWindow(mainHwnd);
        std::wstring exe;
        std::wstring args;
        if (options_.id == L"dummy") {
            exe = SelfExePath();
            args = L"--browser-dummy-window";
        } else {
            for (const BrowserCandidate& c : DetectInstalledBrowsers()) {
                if (c.id == options_.id) {
                    exe = c.exePath;
                    args = BuildBrowserLaunchArgs(c.family, options_.url);
                    break;
                }
            }
        }
        if (exe.empty()) {
            Finish(false, "unknown browser id");
            return;
        }
        std::printf("[browser-probe] launch exe='%s' args='%s'\n", WideToUtf8(exe).c_str(), WideToUtf8(args).c_str());
        std::fflush(stdout);
        startMs_ = nowMs;
        if (!host_.Launch(exe, args, nowMs)) {
            Finish(false, host_.Message().c_str());
            return;
        }
        phase_ = Phase::Launching;
        return;
    }

    BrowserFrameInput in;
    in.nowMs = nowMs;
    in.show = true;
    in.contentRect = wanted_;
    const int64_t frameStart = BrowserNowMs();
    host_.Update(in);
    const int64_t frameCost = BrowserNowMs() - frameStart;
    if (frameCost >= 50) {
        std::printf("[browser-probe] E2? BrowserHost::Update took %lld ms\n", static_cast<long long>(frameCost));
    }

    if (phase_ == Phase::Launching) {
        if (host_.State() == BrowserState::Embedded) {
            embeddedAtMs_ = nowMs;
            rawInputAtHoldStart_ = rawInputCount_;
            LogEmbedded(mainHwnd);
            phase_ = Phase::Holding;
        } else if (host_.State() != BrowserState::Launching) {
            Finish(false, host_.Message().c_str());
        } else if (nowMs - startMs_ > kProbeOverallTimeoutMs) {
            Finish(false, "overall timeout");
        }
        return;
    }

    // Holding
    if (host_.State() != BrowserState::Embedded) {
        Finish(false, host_.Message().c_str()); // 保持中に Closed / Lost になった
        return;
    }
    const int64_t held = nowMs - embeddedAtMs_;
    if (held > 800 && !sampledMid_) {
        sampledMid_ = true;
        SampleHold(mainHwnd, "O1 sample");
    }
    if (held < options_.holdMs) {
        return;
    }
    SampleHold(mainHwnd, "O1 final");
    std::printf("[browser-probe] O8 wm_input_during_hold=%llu\n",
                static_cast<unsigned long long>(rawInputCount_ - rawInputAtHoldStart_));
    HWND child = static_cast<HWND>(host_.ChildHwnd());
    const bool detached = host_.Detach();
    if (!detached) {
        Finish(false, "detach refused");
        return;
    }
    const bool top = GetParent(child) == nullptr;
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    RECT rc = {};
    GetWindowRect(child, &rc);
    std::printf("[browser-probe] O5 detached parent_null=%d style=0x%llx child_bit=%d visible=%d rect_screen=(%ld,%ld,%ld,%ld)\n",
                top ? 1 : 0, static_cast<unsigned long long>(style), (style & WS_CHILD) ? 1 : 0, IsWindowVisible(child) ? 1 : 0,
                rc.left, rc.top, rc.right, rc.bottom);
    std::fflush(stdout);
    Finish(embeddedOk_ && top && (style & WS_CHILD) == 0 && IsWindowVisible(child),
           "embed / hold / detach finished");
}

void BrowserEmbedProbe::Shutdown()
{
    if (host_.State() == BrowserState::Embedded) {
        host_.Detach();
    }
}

} // namespace mye
