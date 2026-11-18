//====================================================================================
//                          BrowserEmbedSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          ブラウザ埋め込みの回帰テスト (純関数 + ダミー窓プロセス)
//====================================================================================
#include "Editor/Tools/Browser/BrowserEmbedSelfTest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <Windows.h>

#include "Editor/Tools/Browser/BrowserCatalog.h"
#include "Editor/Tools/Browser/BrowserHost.h"
#include "Editor/Tools/Browser/BrowserProbe.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// ---- Fast ----

void TestExtractAndFamily(const auto& check)
{
    check(ExtractBrowserExePath(L"\"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe\"")
              == L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "extract: quoted");
    check(ExtractBrowserExePath(L"\"C:\\a b\\x.exe\" --flag \"%1\"") == L"C:\\a b\\x.exe", "extract: quoted with args and %1");
    check(ExtractBrowserExePath(L"C:\\Program\\iexplore.exe") == L"C:\\Program\\iexplore.exe", "extract: unquoted");
    check(ExtractBrowserExePath(L"C:\\Prog\\ff.exe -url %1") == L"C:\\Prog\\ff.exe", "extract: unquoted with args");
    check(ExtractBrowserExePath(L"  \"C:\\p\\a.exe\"") == L"C:\\p\\a.exe", "extract: leading spaces");
    check(ExtractBrowserExePath(L"C:\\Prog\\A.EXE /x") == L"C:\\Prog\\A.EXE", "extract: upper-case extension");
    check(ExtractBrowserExePath(L"").empty(), "extract: empty");
    check(ExtractBrowserExePath(L"\"C:\\unterminated.exe").empty(), "extract: unterminated quote");
    check(ExtractBrowserExePath(L"notepad").empty(), "extract: no .exe");
    check(ExtractBrowserExePath(L"\"C:\\ユーザー\\あ い\\ブラウザ.exe\" %1") == L"C:\\ユーザー\\あ い\\ブラウザ.exe", "extract: Japanese path");

    check(ClassifyBrowserFamily(L"C:\\x\\Chrome.EXE") == BrowserFamily::Chromium, "family: chrome");
    check(ClassifyBrowserFamily(L"C:\\x\\opera.exe") == BrowserFamily::Chromium, "family: opera");
    check(ClassifyBrowserFamily(L"C:\\x\\firefox.exe") == BrowserFamily::Firefox, "family: firefox");
    check(ClassifyBrowserFamily(L"C:\\x\\mybrowser.exe") == BrowserFamily::Unknown, "family: unknown");
    check(BuildBrowserLaunchArgs(BrowserFamily::Chromium, L"") == L"--new-window", "args: chromium without url");
    check(BuildBrowserLaunchArgs(BrowserFamily::Chromium, L"https://a/?b=1&c=2") == L"--new-window \"https://a/?b=1&c=2\"",
          "args: chromium with url");
    check(BuildBrowserLaunchArgs(BrowserFamily::Firefox, L"") == L"-new-window", "args: firefox without url");
    check(BuildBrowserLaunchArgs(BrowserFamily::Firefox, L"x") == L"-new-window \"x\"", "args: firefox with url");
    check(BuildBrowserLaunchArgs(BrowserFamily::Unknown, L"").empty(), "args: unknown without url");
    check(BuildBrowserLaunchArgs(BrowserFamily::Unknown, L"x") == L"\"x\"", "args: unknown with url");
}

void TestCandidates(const auto& check)
{
    auto raw = [](const wchar_t* id, const wchar_t* name, const wchar_t* cmd, const wchar_t* prog) {
        BrowserRawEntry e;
        e.id = id;
        e.displayName = name;
        e.command = cmd;
        e.httpsProgId = prog;
        return e;
    };
    // 優先順 (HKCU → HKLM → WOW) に並べた入力。Chrome / Edge / IE は HKLM と WOW の二重登録
    const std::vector<BrowserRawEntry> entries = {
        raw(L"OperaGX", L"Opera GX Stable", L"\"C:\\Users\\あ\\AppData\\Local\\Programs\\Opera GX\\opera.exe\"", L"OperaProg"),
        raw(L"Google Chrome", L"Google Chrome", L"\"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe\"", L"ChromeHTML"),
        raw(L"Microsoft Edge", L"Microsoft Edge", L"\"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe\" %1", L"MSEdgeHTM"),
        raw(L"IEXPLORE.EXE", L"Internet Explorer", L"C:\\Program Files\\Internet Explorer\\iexplore.exe", L""),
        raw(L"Ghost", L"Ghost Browser", L"\"C:\\missing\\ghost.exe\"", L""),
        raw(L"NoName", L"", L"\"C:\\Tools\\apple.exe\"", L""),
        raw(L"Google Chrome", L"Google Chrome (WOW)", L"\"C:\\PROGRAM FILES\\google\\chrome\\application\\CHROME.EXE\"", L"ChromeHTML"),
        raw(L"Microsoft Edge", L"Microsoft Edge", L"\"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe\"", L"MSEdgeHTM"),
        raw(L"IEXPLORE.EXE", L"Internet Explorer", L"C:\\Program Files\\Internet Explorer\\iexplore.exe", L""),
        raw(L"Broken", L"Broken", L"not-a-command", L""),
    };
    const auto exists = [](const std::wstring& exe) { return exe.find(L"missing") == std::wstring::npos; };
    const std::vector<BrowserCandidate> list = BuildBrowserCandidates(entries, L"MSEdgeHTM", exists);

    check(list.size() == 4, "candidates: 4 survive (dup / IE / missing exe / broken command dropped)");
    if (list.size() != 4) {
        return;
    }
    // 並び: 表示名 (大文字小文字無視) の昇順。空の表示名は id を使う ("NoName")
    check(list[0].displayName == L"Google Chrome", "candidates: sorted [0] Google Chrome");
    check(list[1].displayName == L"Microsoft Edge", "candidates: sorted [1] Microsoft Edge");
    check(list[2].displayName == L"NoName", "candidates: sorted [2] empty display name falls back to id");
    check(list[3].displayName == L"Opera GX Stable", "candidates: sorted [3] Opera GX Stable");
    check(list[0].displayName != L"Google Chrome (WOW)", "candidates: first-read entry wins on duplicate exe");
    check(list[3].exePath == L"C:\\Users\\あ\\AppData\\Local\\Programs\\Opera GX\\opera.exe", "candidates: Japanese path kept");
    check(list[1].isDefault && !list[0].isDefault && !list[2].isDefault && !list[3].isDefault, "candidates: default mark only on Edge");
    check(list[3].family == BrowserFamily::Chromium && list[2].family == BrowserFamily::Unknown, "candidates: family set");
    // 既定の ProgId が空なら印は付かない
    const std::vector<BrowserCandidate> none = BuildBrowserCandidates(entries, L"", exists);
    check(std::none_of(none.begin(), none.end(), [](const BrowserCandidate& c) { return c.isDefault; }), "candidates: no default mark when ProgId empty");
    // 同名は exe パス順
    const std::vector<BrowserRawEntry> same = { raw(L"b", L"Same", L"\"C:\\b\\x.exe\"", L""), raw(L"a", L"same", L"\"C:\\a\\x.exe\"", L"") };
    const std::vector<BrowserCandidate> ties = BuildBrowserCandidates(same, L"", [](const std::wstring&) { return true; });
    check(ties.size() == 2 && ties[0].exePath == L"C:\\a\\x.exe", "candidates: same display name ordered by exe path");
}

BrowserWindowObservation Win(void* h, int w, int hgt, const wchar_t* image, bool visible = true, bool owner = false, bool tool = false)
{
    BrowserWindowObservation o;
    o.hwnd = h;
    o.width = w;
    o.height = hgt;
    o.imagePath = image;
    o.visible = visible;
    o.hasOwner = owner;
    o.toolWindow = tool;
    return o;
}

void TestWindowPicker(const auto& check)
{
    const std::wstring reg = L"C:\\Users\\a\\AppData\\Local\\Programs\\Opera GX\\opera.exe";
    check(BrowserImageMatchesRegistered(reg, reg), "image: same exe");
    check(BrowserImageMatchesRegistered(reg, L"c:\\users\\A\\appdata\\local\\programs\\opera gx\\139.0.1\\OPERA.exe"),
          "image: version directory under the registered one (Opera GX launcher)");
    check(!BrowserImageMatchesRegistered(reg, L"C:\\Users\\a\\AppData\\Local\\Programs\\Opera GX\\139.0.1\\other.exe"), "image: other file name");
    check(!BrowserImageMatchesRegistered(reg, L"C:\\Users\\a\\AppData\\Local\\Programs\\Opera\\opera.exe"), "image: other browser with the same file name");
    check(!BrowserImageMatchesRegistered(reg, L"C:\\Users\\a\\AppData\\Local\\Programs\\Opera GX2\\opera.exe"), "image: directory prefix is not a subdirectory");
    check(!BrowserImageMatchesRegistered(reg, L""), "image: unknown process image");
    check(!BrowserImageMatchesRegistered(L"", reg), "image: empty registered exe");

    const std::wstring chrome = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
    const std::wstring edge = L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";
    void* const a = reinterpret_cast<void*>(0x10);
    void* const b = reinterpret_cast<void*>(0x20);
    void* const c = reinterpret_cast<void*>(0x30);
    void* const d = reinterpret_cast<void*>(0x40);
    const std::vector<BrowserWindowObservation> z = {
        Win(a, 1000, 700, chrome.c_str()),               // 0: snapshot にある
        Win(b, 1000, 700, edge.c_str()),                 // 1: 別ブラウザ
        Win(c, 800, 600, chrome.c_str()),                // 2: 候補 (Z 順の先頭)
        Win(d, 900, 650, chrome.c_str()),                // 3: 候補 (下)
    };
    const std::vector<void*> snapshot = { a };
    check(PickBrowserWindow(z, &snapshot, chrome) == 2, "pick: snapshot excluded, other browser skipped, top of Z order wins");
    check(PickBrowserWindow(z, nullptr, chrome) == 0, "pick: without snapshot the topmost matching window wins");
    check(FilterBrowserWindows(z, nullptr, chrome).size() == 3, "filter: all matching windows (existing-window attach)");
    check(PickBrowserWindow(z, &snapshot, edge) == 1, "pick: Edge window");

    const std::vector<BrowserWindowObservation> bad = {
        Win(a, 1000, 700, chrome.c_str(), /*visible=*/false),
        Win(b, 1000, 700, chrome.c_str(), true, /*owner=*/true),
        Win(c, 1000, 700, chrome.c_str(), true, false, /*tool=*/true),
        Win(d, 199, 700, chrome.c_str()),
        Win(reinterpret_cast<void*>(0x50), 1000, 149, chrome.c_str()),
    };
    check(PickBrowserWindow(bad, nullptr, chrome) == -1, "pick: invisible / owned / tool / small windows excluded");
    const std::vector<BrowserWindowObservation> edge200 = { Win(a, 200, 150, chrome.c_str()) };
    check(PickBrowserWindow(edge200, nullptr, chrome) == 0, "pick: 200x150 is accepted (boundary)");
    check(PickBrowserWindow({}, nullptr, chrome) == -1, "pick: empty list");
}

void TestStateMachine(const auto& check)
{
    BrowserStatus idle;
    BrowserObservation obs;
    check(StepBrowserState(idle, obs).state == BrowserState::Idle, "state: Idle stays");

    BrowserStatus launching;
    launching.state = BrowserState::Launching;
    obs = BrowserObservation();
    obs.launchElapsedMs = 14999;
    check(StepBrowserState(launching, obs).state == BrowserState::Launching, "state: Launching waits before 15 s");
    obs.launchElapsedMs = 15000;
    check(StepBrowserState(launching, obs).state == BrowserState::TimedOut, "state: Launching -> TimedOut at 15 s");
    obs = BrowserObservation();
    obs.candidateFound = true;
    check(StepBrowserState(launching, obs).state == BrowserState::Embedded, "state: Launching -> Embedded when found");
    obs.attachFailed = true;
    check(StepBrowserState(launching, obs).state == BrowserState::Failed, "state: Launching -> Failed when attach fails");
    obs = BrowserObservation();
    obs.candidateFound = true;
    obs.launchElapsedMs = 20000;
    check(StepBrowserState(launching, obs).state == BrowserState::Embedded, "state: found wins over the timeout");

    BrowserStatus embedded;
    embedded.state = BrowserState::Embedded;
    obs = BrowserObservation();
    check(StepBrowserState(embedded, obs).state == BrowserState::Embedded && !StepBrowserState(embedded, obs).hung, "state: Embedded stays");
    obs.windowHung = true;
    BrowserStatus s = StepBrowserState(embedded, obs);
    check(s.state == BrowserState::Embedded && s.hung, "state: hung mark rises, state stays Embedded");
    obs.windowHung = false;
    check(!StepBrowserState(s, obs).hung, "state: hung mark falls when the window answers again");

    // 親の喪失 (ブラウザ側が親を外した): 窓が生きていれば Failed。応答なしの印も消える。窓が消えていれば Closed / Lost の判定が先
    obs = BrowserObservation();
    obs.parentLost = true;
    s = StepBrowserState(embedded, obs);
    check(s.state == BrowserState::Failed && !s.hung, "state: Embedded + parent lost -> Failed");
    BrowserStatus hungEmbedded = embedded;
    hungEmbedded.hung = true;
    obs.windowHung = true;
    check(StepBrowserState(hungEmbedded, obs).state == BrowserState::Failed && !StepBrowserState(hungEmbedded, obs).hung,
          "state: parent lost while hung -> Failed, hung mark cleared");
    obs = BrowserObservation();
    obs.windowAlive = false;
    obs.parentLost = true;
    obs.ownerExited = true;
    check(StepBrowserState(embedded, obs).state == BrowserState::Closed, "state: window gone wins over parent lost");
    BrowserObservation lostObs;
    lostObs.parentLost = true;
    check(StepBrowserState(launching, lostObs).state == BrowserState::Launching, "state: parent lost is ignored while Launching");
    s = embedded;
    s.hung = true;

    obs = BrowserObservation();
    obs.windowAlive = false;
    obs.ownerExited = true;
    obs.ownerExitNonZero = true;
    check(StepBrowserState(s, obs).state == BrowserState::Lost && !StepBrowserState(s, obs).hung, "state: window gone + non-zero exit -> Lost");
    obs.ownerExitNonZero = false;
    check(StepBrowserState(s, obs).state == BrowserState::Closed, "state: window gone + exit code 0 -> Closed");
    obs = BrowserObservation();
    obs.windowAlive = false;
    obs.goneForMs = kBrowserGoneGraceMs - 1;
    check(StepBrowserState(s, obs).state == BrowserState::Embedded, "state: window gone, process still alive -> wait inside the grace");
    obs.goneForMs = kBrowserGoneGraceMs;
    check(StepBrowserState(s, obs).state == BrowserState::Closed, "state: window gone, process alive after the grace -> Closed");

    for (BrowserState terminal : { BrowserState::Closed, BrowserState::Lost, BrowserState::TimedOut, BrowserState::Failed }) {
        BrowserStatus t;
        t.state = terminal;
        BrowserObservation o;
        o.candidateFound = true;
        o.windowAlive = false;
        o.ownerExited = true;
        check(StepBrowserState(t, o).state == terminal, "state: terminal states do not move by themselves");
    }
}

void TestVisibility(const auto& check)
{
    BrowserVisibilityInput in;
    in.panelDrawn = true;
    in.panel = { 100, 100, 500, 400 };
    check(EvaluateBrowserVisibility(in) == BrowserHideReason::None, "visible: all conditions met");

    BrowserVisibilityInput v = in;
    v.panelDrawn = false;
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::PanelNotDrawn, "visible 1: panel not drawn");
    v = in;
    v.panel = { 100, 100, 100, 400 };
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::EmptyRect, "visible 2: zero width");
    v.panel = { 100, 100, 500, 100 };
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::EmptyRect, "visible 2: zero height");
    v = in;
    v.mainMinimized = true;
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::MainMinimized, "visible 3: main window minimized");
    v = in;
    v.windowDragging = true;
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::WindowDragging, "visible 4: window drag");
    v = in;
    v.modalOpen = true;
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::ModalOpen, "visible 5: modal");
    v = in;
    v.overlays = { { 0, 0, 100, 100 }, { 500, 400, 600, 500 }, { 0, 0, 50, 50 } };
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::None, "visible 6: touching edges / far windows do not overlap");
    v.overlays.push_back({ 450, 350, 700, 700 });
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::Overlapped, "visible 6: an overlapping window hides it");
    v.overlays = { { 120, 120, 120, 300 } };
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::None, "visible 6: an empty overlay rect is ignored");
    // 優先順は 1 → 6 (理由の表示用)
    v = in;
    v.panelDrawn = false;
    v.modalOpen = true;
    check(EvaluateBrowserVisibility(v) == BrowserHideReason::PanelNotDrawn, "visible: reasons are reported in condition order");
}

void TestRateLimiter(const auto& check)
{
    BrowserRateLimiter lim(10, 1000);
    int ok = 0;
    for (int i = 0; i < 25; ++i) {
        ok += lim.TryAcquire(100 + i) ? 1 : 0;
    }
    check(ok == 10, "limiter: 10 per second");
    check(!lim.TryAcquire(1099), "limiter: still limited inside the window");
    check(lim.TryAcquire(1100), "limiter: frees after the first stamp leaves the window");
}

// ---- Full ----

HWND CreateTestParent()
{
    static const wchar_t kClass[] = L"MyEngineBrowserTestParent";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // 2 回目は失敗するが、登録済みなら構わない
    // 窓の DPI 認識は作成時のスレッドの文脈で決まる。ダミー窓プロセス (Per-Monitor V2) に合わせる (プロセス全体は変えない)
    const DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND hwnd = CreateWindowExW(0, kClass, L"browser test parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1000, 700, nullptr, nullptr,
                                wc.hInstance, nullptr);
    SetThreadDpiAwarenessContext(old);
    return hwnd;
}

void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

std::wstring SelfExe()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

struct ProcDriver {
    BrowserHost& host;
    BrowserRect rect;
    int64_t maxUpdateMs = 0;

    // pred が真になるまで 1 フレームずつ回す。戻り値 = 真になるまでの ms (間に合わなければ -1)
    template <typename Pred>
    int64_t Run(int64_t timeoutMs, Pred pred, bool show = true)
    {
        const int64_t start = BrowserNowMs();
        for (;;) {
            Pump();
            BrowserFrameInput in;
            in.nowMs = BrowserNowMs();
            in.show = show;
            in.contentRect = rect;
            host.Update(in);
            maxUpdateMs = (std::max)(maxUpdateMs, BrowserNowMs() - in.nowMs);
            const int64_t elapsed = BrowserNowMs() - start;
            if (pred()) {
                return elapsed;
            }
            if (elapsed >= timeoutMs) {
                return -1;
            }
            Sleep(16);
        }
    }
};

void TerminatePid(uint32_t pid)
{
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

} // namespace

bool RunBrowserEmbedSelfTest()
{
    MYE_LOG_INFO("==== BrowserEmbed self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    TestExtractAndFamily(check);
    TestCandidates(check);
    TestWindowPicker(check);
    TestStateMachine(check);
    TestVisibility(check);
    TestRateLimiter(check);
    MYE_LOG_INFO("BrowserEmbed self test: %s (%d failure(s))", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

bool RunBrowserEmbedProcSelfTest()
{
    MYE_LOG_INFO("==== BrowserEmbedProc self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    HWND parent = CreateTestParent();
    check(parent != nullptr, "test parent window created");
    if (parent == nullptr) {
        return false;
    }
    Pump();
    const std::wstring exe = SelfExe();
    const BrowserRect want{ 10, 10, 410, 310 };
    const auto ClientRectOfChild = [&](HWND child) {
        RECT rc = {};
        GetWindowRect(child, &rc);
        POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
        MapWindowPoints(nullptr, parent, pts, 2);
        return BrowserRect{ pts[0].x, pts[0].y, pts[1].x, pts[1].y };
    };

    {
        BrowserHost host;
        host.SetMainWindow(parent);
        ProcDriver drv{ host, want };

        // 1. 起動 → 差分で特定 → Attach
        check(host.Launch(exe, L"--browser-dummy-window", BrowserNowMs()), "launch dummy window process");
        const int64_t found = drv.Run(15000, [&] { return host.State() != BrowserState::Launching; });
        check(found >= 0 && host.State() == BrowserState::Embedded, "dummy window identified and embedded");
        HWND child = static_cast<HWND>(host.ChildHwnd());
        if (host.State() == BrowserState::Embedded && child != nullptr) {
            MYE_LOG_INFO("  info: found in %lld ms", static_cast<long long>(found));
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            check(pid == host.LastLaunchPid(), "identified window belongs to the launched process");
            drv.Run(500, [] { return false; }); // 矩形合わせの 1 フレーム目以降を回す
            check(GetParent(child) == parent, "GetParent == test parent");
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            check((style & WS_CHILD) != 0 && (style & (WS_CAPTION | WS_THICKFRAME | WS_POPUP)) == 0, "style: WS_CHILD, no caption / thickframe / popup");
            check(ClientRectOfChild(child) == want, "child rect matches the content rect");
            check(IsWindowVisible(child) != FALSE, "child is visible");

            // 2. 隠す → 見せる (状態が変わったときだけ ShowWindowAsync)
            drv.Run(400, [&] { return IsWindowVisible(child) == FALSE; }, /*show=*/false);
            check(IsWindowVisible(child) == FALSE && !host.ChildShown(), "hidden when show=false");
            drv.Run(400, [&] { return IsWindowVisible(child) != FALSE; }, /*show=*/true);
            check(IsWindowVisible(child) != FALSE && host.ChildShown(), "visible again when show=true");

            // 3. 矩形の追従
            drv.rect = BrowserRect{ 30, 40, 530, 400 };
            drv.Run(500, [&] { return ClientRectOfChild(child) == drv.rect; });
            check(ClientRectOfChild(child) == drv.rect, "child follows a changed content rect");

            // 4. Detach: 元のスタイル・親なし・可視・サイズ
            const bool detached = host.Detach();
            check(detached && host.State() == BrowserState::Idle, "Detach succeeds");
            const LONG_PTR after = GetWindowLongPtrW(child, GWL_STYLE);
            check(GetParent(child) == nullptr, "after Detach: no parent");
            check((after & WS_CHILD) == 0 && (after & WS_CAPTION) == WS_CAPTION && (after & WS_THICKFRAME) != 0 && (after & WS_SYSMENU) != 0,
                  "after Detach: original top-level style restored");
            RECT rc = {};
            GetWindowRect(child, &rc);
            check(rc.right - rc.left == 800 && rc.bottom - rc.top == 600, "after Detach: original size restored");
            check(IsWindowVisible(child) != FALSE, "after Detach: visible");

            // 5. 再 Attach
            check(host.AttachExisting(child) && host.State() == BrowserState::Embedded, "AttachExisting re-embeds the detached window");
            drv.Run(300, [] { return false; });
            check(GetParent(child) == parent && ClientRectOfChild(child) == drv.rect, "re-attached window follows the content rect");

            // 6. WM_CLOSE → Closed (1 秒以内)
            host.RequestClose();
            const int64_t closed = drv.Run(1500, [&] { return host.State() != BrowserState::Embedded; });
            check(closed >= 0 && closed <= 1000 && host.State() == BrowserState::Closed, "WM_CLOSE -> Closed within 1 s");
        }
    }

    {
        // 7. 強制終了 → Lost (1 秒以内)
        BrowserHost host;
        host.SetMainWindow(parent);
        ProcDriver drv{ host, want };
        check(host.Launch(exe, L"--browser-dummy-window", BrowserNowMs()), "launch second dummy");
        drv.Run(15000, [&] { return host.State() != BrowserState::Launching; });
        check(host.State() == BrowserState::Embedded, "second dummy embedded");
        if (host.State() == BrowserState::Embedded) {
            TerminatePid(host.ChildPid());
            const int64_t lost = drv.Run(1500, [&] { return host.State() != BrowserState::Embedded; });
            check(lost >= 0 && lost <= 1000 && host.State() == BrowserState::Lost, "TerminateProcess -> Lost within 1 s");
        }
    }

    {
        // 8. 応答なし: 1.5 秒後に UI スレッドを 30 秒止めるダミー。Attach はその前に済ませる
        BrowserHost host;
        host.SetMainWindow(parent);
        ProcDriver drv{ host, want };
        check(host.Launch(exe, L"--browser-dummy-window --browser-dummy-hang-after-ms 1500", BrowserNowMs()), "launch hanging dummy");
        drv.Run(15000, [&] { return host.State() != BrowserState::Launching; });
        check(host.State() == BrowserState::Embedded, "hanging dummy embedded before it hangs");
        if (host.State() == BrowserState::Embedded) {
            const uint32_t pid = host.ChildPid();
            const int64_t hung = drv.Run(12000, [&] { return host.Hung(); });
            check(hung >= 0 && host.State() == BrowserState::Embedded, "hung mark rises while Embedded");
            // 応答なしの間の 1 フレームは 50 ms 以内 (表示切替・矩形変更を含めて)
            drv.maxUpdateMs = 0;
            drv.rect = BrowserRect{ 20, 20, 300, 200 };
            drv.Run(600, [] { return false; }, /*show=*/false);
            drv.Run(600, [] { return false; }, /*show=*/true);
            check(drv.maxUpdateMs <= 50, "BrowserHost::Update <= 50 ms per frame while the browser hangs");
            MYE_LOG_INFO("  info: max Update while hung = %lld ms", static_cast<long long>(drv.maxUpdateMs));
            const int64_t t0 = BrowserNowMs();
            check(!host.Detach() && BrowserNowMs() - t0 <= 50, "Detach refuses (and returns fast) while the browser hangs");
            check(host.State() == BrowserState::Embedded, "refused Detach keeps Embedded");
            TerminatePid(pid);
            const int64_t lost = drv.Run(1500, [&] { return host.State() != BrowserState::Embedded; });
            check(lost >= 0 && host.State() == BrowserState::Lost, "hung dummy killed -> Lost");
        }
    }

    {
        // 9. ダミー窓が自分で親を外す (WS_CHILD のまま SetParent(nullptr))。Opera GX と同じ。Failed になり、窓は普通の窓へ戻る
        BrowserHost host;
        host.SetMainWindow(parent);
        ProcDriver drv{ host, want };
        check(host.Launch(exe, L"--browser-dummy-window --browser-dummy-unparent-after-ms 1500", BrowserNowMs()), "launch unparenting dummy");
        drv.Run(15000, [&] { return host.State() != BrowserState::Launching; });
        check(host.State() == BrowserState::Embedded, "unparenting dummy embedded first");
        HWND child = static_cast<HWND>(host.ChildHwnd());
        if (host.State() == BrowserState::Embedded && child != nullptr) {
            const uint32_t pid = host.ChildPid();
            const int64_t failed = drv.Run(6000, [&] { return host.State() != BrowserState::Embedded; });
            check(failed >= 0 && host.State() == BrowserState::Failed, "parent removed by the browser -> Failed");
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            check((style & WS_CHILD) == 0 && (style & WS_CAPTION) == WS_CAPTION, "after parent loss: WS_CHILD removed, original style restored");
            check(GetParent(child) == nullptr && GetAncestor(child, GA_PARENT) == GetDesktopWindow(), "after parent loss: no parent");
            check(IsWindowVisible(child) != FALSE, "after parent loss: visible");
            RECT rc = {};
            GetWindowRect(child, &rc);
            check(rc.right - rc.left == 800 && rc.bottom - rc.top == 600, "after parent loss: original size restored");
            check(host.ChildHwnd() == nullptr, "after parent loss: host lets go of the window");
            TerminatePid(pid);
        }
    }

    DestroyWindow(parent);
    Pump();
    MYE_LOG_INFO("BrowserEmbedProc self test: %s (%d failure(s))", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
