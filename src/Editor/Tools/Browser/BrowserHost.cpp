//====================================================================================
//                          BrowserHost.cpp
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          別プロセスのブラウザ窓の起動・特定・埋め込みの実装
//====================================================================================
#include "Editor/Tools/Browser/BrowserHost.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <unordered_map>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

std::wstring ToLower(std::wstring s)
{
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

std::wstring NormalizeSlashes(std::wstring s)
{
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return ToLower(std::move(s));
}

// "C:\a\b\c.exe" → "c.exe" / "c:\a\b" (小文字・区切りは '\\')
void SplitPath(const std::wstring& path, std::wstring& dir, std::wstring& name)
{
    const std::wstring p = NormalizeSlashes(path);
    const size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) {
        dir.clear();
        name = p;
        return;
    }
    dir = p.substr(0, pos);
    name = p.substr(pos + 1);
}

std::string ErrorText(const char* what, DWORD err)
{
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf), "%s (error %lu)", what, static_cast<unsigned long>(err));
    return buf;
}

HWND AsHwnd(void* p) { return static_cast<HWND>(p); }

BrowserRect ToBrowserRect(const RECT& r) { return BrowserRect{ r.left, r.top, r.right, r.bottom }; }

// 窓の所有プロセスの exe (取れなければ空)。cache = 1 回の列挙の間だけ使う pid → パス
std::wstring ProcessImageOf(DWORD pid, std::unordered_map<DWORD, std::wstring>& cache)
{
    const auto it = cache.find(pid);
    if (it != cache.end()) {
        return it->second;
    }
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD len = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
            path.assign(buf, len);
        }
        CloseHandle(h);
    }
    cache.emplace(pid, path);
    return path;
}

struct EnumContext {
    std::vector<BrowserWindowObservation>* out = nullptr;
    const std::vector<void*>* snapshot = nullptr; // nullptr = snapshot 条件なし
    HWND main = nullptr;
    bool resolveImage = false;                    // false = 可視・所有者なしの窓だけ控える (snapshot 取得用)
    std::unordered_map<DWORD, std::wstring> imageCache;
};

BOOL CALLBACK EnumTopLevel(HWND hwnd, LPARAM lp)
{
    EnumContext& ctx = *reinterpret_cast<EnumContext*>(lp);
    BrowserWindowObservation o;
    o.hwnd = hwnd;
    o.visible = IsWindowVisible(hwnd) != FALSE;
    o.hasOwner = GetWindow(hwnd, GW_OWNER) != nullptr;
    o.toolWindow = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    o.width = rc.right - rc.left;
    o.height = rc.bottom - rc.top;
    // 実行ファイルの引き当ては、他の条件を満たしたものだけ (全窓の OpenProcess を毎回やらない)
    if (ctx.resolveImage && hwnd != ctx.main && o.visible && !o.hasOwner && !o.toolWindow
        && o.width >= kBrowserMinWindowWidth && o.height >= kBrowserMinWindowHeight
        && (ctx.snapshot == nullptr
            || std::find(ctx.snapshot->begin(), ctx.snapshot->end(), static_cast<void*>(hwnd)) == ctx.snapshot->end())) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        o.imagePath = ProcessImageOf(pid, ctx.imageCache);
    }
    ctx.out->push_back(std::move(o));
    return TRUE;
}

std::vector<BrowserWindowObservation> ObserveTopLevelWindows(HWND main, const std::vector<void*>* snapshot, bool resolveImage)
{
    std::vector<BrowserWindowObservation> out;
    EnumContext ctx;
    ctx.out = &out;
    ctx.snapshot = snapshot;
    ctx.main = main;
    ctx.resolveImage = resolveImage;
    EnumWindows(EnumTopLevel, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::wstring WindowTitleNoBlock(HWND hwnd)
{
    // GetWindowTextW は他プロセスの窓へ WM_GETTEXT を送らず、窓のキャプションを内部から読む (公式の Remarks:
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextw)。
    // 応答なし・忙しいブラウザ窓でも UI スレッドが待たされない。WM_GETTEXT を自前で送る SendMessageTimeout は待ちうるので使わない
    wchar_t buf[256] = {};
    const int len = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
    return std::wstring(buf, len > 0 ? static_cast<size_t>(len) : 0);
}

// 窓の画面座標の矩形をメイン窓のクライアント座標へ
BrowserRect ClientRectOf(HWND main, HWND child)
{
    RECT rc = {};
    GetWindowRect(child, &rc);
    POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
    MapWindowPoints(nullptr, main, pts, 2);
    return BrowserRect{ pts[0].x, pts[0].y, pts[1].x, pts[1].y };
}

constexpr LONG_PTR kTopLevelStyleBits = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

} // namespace

// ---- 純関数 ----

BrowserRect FitBrowserRectToWorkArea(const BrowserRect& r, const BrowserRect& work)
{
    if (work.Empty()) {
        return r;
    }
    const int width = (std::min)((std::max)(r.Width(), 0), work.Width());
    const int height = (std::min)((std::max)(r.Height(), 0), work.Height());
    // 右下を先に押し戻し、次に左上を押し戻す (左上 = タイトルバー側を必ず作業領域の中に置く)
    const int left = (std::max)((std::min)(r.left, work.right - width), work.left);
    const int top = (std::max)((std::min)(r.top, work.bottom - height), work.top);
    return BrowserRect{ left, top, left + width, top + height };
}

bool BrowserImageMatchesRegistered(const std::wstring& registeredExe, const std::wstring& processImage, BrowserImageMatch match)
{
    if (registeredExe.empty() || processImage.empty()) {
        return false;
    }
    std::wstring regDir, regName, imgDir, imgName;
    SplitPath(registeredExe, regDir, regName);
    SplitPath(processImage, imgDir, imgName);
    if (match == BrowserImageMatch::SameFileName && regName != imgName) {
        return false;
    }
    if (imgDir == regDir) {
        return true;
    }
    // 配下 = 登録ディレクトリ + '\\' で始まる (ディレクトリ名の前方一致 "Opera" と "Opera GX" を混同しない)
    return imgDir.size() > regDir.size() && imgDir.compare(0, regDir.size(), regDir) == 0 && imgDir[regDir.size()] == L'\\';
}

std::vector<size_t> FilterBrowserWindows(const std::vector<BrowserWindowObservation>& zOrdered,
                                         const std::vector<void*>* snapshot, const std::wstring& registeredExe,
                                         BrowserImageMatch match)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < zOrdered.size(); ++i) {
        const BrowserWindowObservation& o = zOrdered[i];
        if (snapshot != nullptr && std::find(snapshot->begin(), snapshot->end(), o.hwnd) != snapshot->end()) {
            continue;
        }
        if (!o.visible || o.hasOwner || o.toolWindow) {
            continue;
        }
        if (o.width < kBrowserMinWindowWidth || o.height < kBrowserMinWindowHeight) {
            continue;
        }
        if (!BrowserImageMatchesRegistered(registeredExe, o.imagePath, match)) {
            continue;
        }
        out.push_back(i);
    }
    return out;
}

int PickBrowserWindow(const std::vector<BrowserWindowObservation>& zOrdered, const std::vector<void*>* snapshot,
                      const std::wstring& registeredExe, BrowserImageMatch match)
{
    const std::vector<size_t> all = FilterBrowserWindows(zOrdered, snapshot, registeredExe, match);
    return all.empty() ? -1 : static_cast<int>(all.front());
}

BrowserStatus StepBrowserState(const BrowserStatus& cur, const BrowserObservation& obs)
{
    BrowserStatus next = cur;
    switch (cur.state) {
    case BrowserState::Launching:
        if (obs.candidateFound) {
            next.state = obs.attachFailed ? BrowserState::Failed : BrowserState::Embedded;
            next.hung = false;
        } else if (obs.launchElapsedMs >= kBrowserLaunchTimeoutMs) {
            next.state = BrowserState::TimedOut;
        }
        break;
    case BrowserState::Embedded:
        if (obs.windowAlive && obs.parentLost) {
            next.state = BrowserState::Failed;
            next.hung = false;
        } else if (obs.windowAlive) {
            next.hung = obs.windowHung; // 戻れば消す
        } else if (obs.ownerExited && obs.ownerExitNonZero) {
            next.state = BrowserState::Lost;
            next.hung = false;
        } else if (obs.ownerExited || obs.goneForMs >= kBrowserGoneGraceMs) {
            next.state = BrowserState::Closed;
            next.hung = false;
        }
        // 窓が消えて猶予の間は Embedded のまま (プロセスの終了コードが見えるのを待つ)
        break;
    default:
        break;
    }
    return next;
}

BrowserHideReason EvaluateBrowserVisibility(const BrowserVisibilityInput& in)
{
    if (!in.panelDrawn) {
        return BrowserHideReason::PanelNotDrawn;
    }
    if (in.panel.Empty()) {
        return BrowserHideReason::EmptyRect;
    }
    if (in.mainMinimized) {
        return BrowserHideReason::MainMinimized;
    }
    if (in.windowDragging) {
        return BrowserHideReason::WindowDragging;
    }
    if (in.modalOpen) {
        return BrowserHideReason::ModalOpen;
    }
    for (const BrowserRect& o : in.overlays) {
        if (!o.Empty() && in.panel.Overlaps(o)) {
            return BrowserHideReason::Overlapped;
        }
    }
    return BrowserHideReason::None;
}

bool ShouldFocusBrowserChild(bool shown, bool hung, const BrowserRect& childScreen, int cursorX, int cursorY)
{
    if (!shown || hung || childScreen.Empty()) {
        return false;
    }
    return childScreen.left <= cursorX && cursorX < childScreen.right && childScreen.top <= cursorY && cursorY < childScreen.bottom;
}

bool BrowserRateLimiter::TryAcquire(int64_t nowMs)
{
    stamps_.erase(std::remove_if(stamps_.begin(), stamps_.end(), [&](int64_t t) { return nowMs - t >= windowMs_; }),
                  stamps_.end());
    if (static_cast<int>(stamps_.size()) >= maxCount_) {
        return false;
    }
    stamps_.push_back(nowMs);
    return true;
}

int64_t BrowserNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::vector<BrowserWindowInfo> EnumerateBrowserWindows(const std::wstring& registeredExe, BrowserImageMatch match)
{
    const std::vector<BrowserWindowObservation> obs = ObserveTopLevelWindows(nullptr, nullptr, true);
    std::vector<BrowserWindowInfo> out;
    for (size_t i : FilterBrowserWindows(obs, nullptr, registeredExe, match)) {
        BrowserWindowInfo info;
        info.hwnd = obs[i].hwnd;
        info.title = WindowTitleNoBlock(AsHwnd(obs[i].hwnd));
        DWORD pid = 0;
        GetWindowThreadProcessId(AsHwnd(obs[i].hwnd), &pid);
        info.pid = pid;
        out.push_back(std::move(info));
    }
    return out;
}

// ---- BrowserHost ----

BrowserHost::~BrowserHost()
{
    // 通常は EditorApp::OnShutdown が先に Detach する。ここは取りこぼし用の保険
    if (status_.state == BrowserState::Embedded) {
        Detach();
    }
    ReleaseChild();
}

void BrowserHost::SetResult(BrowserState state, std::string message)
{
    status_.state = state;
    status_.hung = false;
    message_ = std::move(message);
    if (state != BrowserState::Failed) {
        failReason_ = BrowserFailReason::None; // Failed の理由は呼び出し側が SetResult の前に入れる
    }
}

bool BrowserHost::Launch(const std::wstring& exePath, const std::wstring& args, int64_t nowMs, BrowserImageMatch match)
{
    if (status_.state == BrowserState::Launching || status_.state == BrowserState::Embedded) {
        return false;
    }
    ReleaseChild();
    launchExe_ = exePath;
    launchMatch_ = match;
    findElapsedMs_ = -1;
    // 起動の直前に、可視・所有者なしのトップレベル窓を控える (差分で新しい窓を特定する)
    snapshot_.clear();
    for (const BrowserWindowObservation& o : ObserveTopLevelWindows(AsHwnd(mainHwnd_), nullptr, false)) {
        if (o.visible && !o.hasOwner) {
            snapshot_.push_back(o.hwnd);
        }
    }

    std::wstring cmd = L"\"" + exePath + L"\"";
    if (!args.empty()) {
        cmd += L" " + args;
    }
    std::wstring workDir = std::filesystem::path(exePath).parent_path().wstring();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.empty() ? nullptr : workDir.c_str(),
                        &si, &pi)) {
        failReason_ = BrowserFailReason::LaunchFailed;
        SetResult(BrowserState::Failed, ErrorText("CreateProcess failed", GetLastError()));
        MYE_LOG_WARN("[browser] %s", message_.c_str());
        return false;
    }
    launchPid_ = pi.dwProcessId;
    // 窓の持ち主はこのプロセスとは限らない (既存プロセスへ依頼して即終了する / ランチャー) ので、ハンドルは持たない
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    launchedAtMs_ = nowMs;
    nextPollMs_ = nowMs;
    SetResult(BrowserState::Launching, "launching");
    return true;
}

bool BrowserHost::AttachExisting(void* hwnd)
{
    if (status_.state == BrowserState::Launching || status_.state == BrowserState::Embedded) {
        return false;
    }
    ReleaseChild();
    if (AttachWindow(hwnd)) {
        SetResult(BrowserState::Embedded, "attached");
        return true;
    }
    SetResult(BrowserState::Failed, message_);
    return false;
}

void BrowserHost::CancelLaunch()
{
    if (status_.state == BrowserState::Launching) {
        SetResult(BrowserState::Idle, "launch canceled");
    }
}

bool BrowserHost::AttachWindow(void* hwndRaw)
{
    HWND child = AsHwnd(hwndRaw);
    HWND main = AsHwnd(mainHwnd_);
    lastTargetHwnd_ = hwndRaw;
    if (main == nullptr || !IsWindow(main)) {
        message_ = "main window is not set";
        failReason_ = BrowserFailReason::MainWindowMissing;
        return false;
    }
    if (!IsWindow(child) || child == main) {
        message_ = "target window does not exist";
        failReason_ = BrowserFailReason::TargetGone;
        return false;
    }
    if (IsHungAppWindow(child)) {
        message_ = "target window is not responding";
        failReason_ = BrowserFailReason::NotResponding;
        return false;
    }
    // 親子にするとブラウザ側の DPI 認識が強制的に直されることがあるため、違うなら外に置いたままにする
    if (!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(child), GetWindowDpiAwarenessContext(main))) {
        message_ = "DPI awareness differs from the editor";
        failReason_ = BrowserFailReason::DpiMismatch;
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);

    SavedWindow saved;
    saved.style = GetWindowLongPtrW(child, GWL_STYLE);
    saved.exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
    saved.wasMaximized = IsZoomed(child) != FALSE;
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(child, &wp)) {
        saved.normalRect = ToBrowserRect(wp.rcNormalPosition);
    } else {
        RECT rc = {};
        GetWindowRect(child, &rc);
        saved.normalRect = ToBrowserRect(rc);
    }
    if (saved.wasMaximized) {
        ShowWindow(child, SW_RESTORE); // 最大化のまま子にすると WS_MAXIMIZE が残る。先に戻す
    }

    // 子窓化のスタイルは SetParent の前に当てる (SetParent の公式の注意: WS_POPUP を外して WS_CHILD を足してから親を替える)
    const LONG_PTR childStyle = (saved.style & ~kTopLevelStyleBits) | WS_CHILD;
    SetWindowLongPtrW(child, GWL_STYLE, childStyle);
    SetLastError(0);
    if (SetParent(child, main) == nullptr && GetLastError() != 0) {
        const DWORD err = GetLastError();
        SetWindowLongPtrW(child, GWL_STYLE, saved.style);
        if (proc != nullptr) {
            CloseHandle(proc);
        }
        message_ = ErrorText("SetParent failed", err);
        failReason_ = BrowserFailReason::SetParentFailed;
        return false;
    }
    if (clipChildren_) {
        const LONG_PTR mainStyle = GetWindowLongPtrW(main, GWL_STYLE);
        if ((mainStyle & WS_CLIPCHILDREN) == 0) {
            SetWindowLongPtrW(main, GWL_STYLE, mainStyle | WS_CLIPCHILDREN);
        }
    }
    // 画面上の元の位置に置いたままだと子窓の座標系 (親クライアント) とずれる。次の Update が内容矩形へ合わせる
    SetWindowPos(child, nullptr, 0, 0, saved.normalRect.Width(), saved.normalRect.Height(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
    // SetParent の公式の注意: UISTATE を同期する
    PostMessageW(child, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);

    // ブラウザ側がすぐ親を外すことがある (Opera GX)。WS_CHILD のままデスクトップに取り残さず、元の窓へ戻して失敗にする
    if (GetParent(child) != main) {
        RestoreTopLevel(child, saved);
        if (proc != nullptr) {
            CloseHandle(proc);
        }
        message_ = "browser did not accept the embedding (parent was removed)";
        failReason_ = BrowserFailReason::ParentRemoved;
        return false;
    }

    saved_ = saved;
    childHwnd_ = child;
    ownerProcess_ = proc;
    childPid_ = pid;
    shown_ = true;
    goneSinceMs_ = -1;
    parentLostSinceMs_ = -1;
    rectLimiter_ = BrowserRateLimiter();
    return true;
}

void BrowserHost::ReleaseChild()
{
    if (ownerProcess_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(ownerProcess_));
        ownerProcess_ = nullptr;
    }
    childHwnd_ = nullptr;
    childPid_ = 0;
    goneSinceMs_ = -1;
    parentLostSinceMs_ = -1;
    focusChildPending_ = false;
}

void BrowserHost::RestoreTopLevel(void* childHwnd, const SavedWindow& saved)
{
    HWND child = AsHwnd(childHwnd);
    // 子窓のうちに、今ある画面上の位置を控える (パネルがあった場所)。親がデスクトップなら窓の座標がそのまま画面座標
    RECT screen = {};
    GetWindowRect(child, &screen);
    BrowserRect target{ screen.left, screen.top, screen.left + saved.normalRect.Width(),
                        screen.top + saved.normalRect.Height() };
    // 背の高い窓をパネルの位置に置くと画面の下へはみ出す (Edge で 320 px を実測)。乗るモニタの作業領域
    // (タスクバーを除く) に収める。モニタの判定は置こうとする矩形で行い、どのモニタにも掛からなければ最寄りのモニタ
    const RECT targetRc{ target.left, target.top, target.right, target.bottom };
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromRect(&targetRc, MONITOR_DEFAULTTONEAREST), &mi)) {
        target = FitBrowserRectToWorkArea(target, ToBrowserRect(mi.rcWork));
    }
    // 公式の注意: SetParent(NULL) の後で WS_CHILD を外し WS_POPUP 等を戻す。親が既にデスクトップなら付け替えない
    if (GetAncestor(child, GA_PARENT) != GetDesktopWindow()) {
        SetParent(child, nullptr);
    }
    SetWindowLongPtrW(child, GWL_STYLE, saved.style & ~static_cast<std::intptr_t>(WS_VISIBLE));
    SetWindowLongPtrW(child, GWL_EXSTYLE, saved.exStyle);
    SetWindowPos(child, HWND_TOP, target.left, target.top, target.Width(), target.Height(),
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (saved.wasMaximized) {
        ShowWindow(child, SW_MAXIMIZE);
    }
}

bool BrowserHost::Detach()
{
    if (status_.state != BrowserState::Embedded || childHwnd_ == nullptr) {
        return true;
    }
    HWND child = AsHwnd(childHwnd_);
    if (!IsWindow(child)) {
        return true; // 消えているなら次の Update が Closed / Lost にする
    }
    if (IsHungAppWindow(child)) {
        message_ = "browser is not responding; detach skipped";
        return false; // SetParent / SetWindowPos は同期で待たされる
    }
    HWND focus = GetFocus();
    const bool hadFocus = focus != nullptr && (focus == child || IsChild(child, focus));
    RestoreTopLevel(child, saved_);
    if (hadFocus && mainHwnd_ != nullptr) {
        SetFocus(AsHwnd(mainHwnd_));
    }
    ReleaseChild();
    SetResult(BrowserState::Idle, "detached");
    return true;
}

void BrowserHost::RequestClose()
{
    if (status_.state == BrowserState::Embedded && childHwnd_ != nullptr) {
        PostMessageW(AsHwnd(childHwnd_), WM_CLOSE, 0, 0);
    }
}

void BrowserHost::ReturnFocusToMain()
{
    if (status_.state != BrowserState::Embedded || childHwnd_ == nullptr || mainHwnd_ == nullptr) {
        return;
    }
    HWND child = AsHwnd(childHwnd_);
    // SetFocus は旧フォーカス窓へ WM_KILLFOCUS を同期で送る。応答なしの窓には触らない (D12)
    if (!IsWindow(child) || IsHungAppWindow(child)) {
        return;
    }
    HWND focus = GetFocus();
    if (focus != nullptr && (focus == child || IsChild(child, focus))) {
        SetFocus(AsHwnd(mainHwnd_));
    }
}

void BrowserHost::NoteMouseDownAt(int screenX, int screenY)
{
    if (status_.state != BrowserState::Embedded || childHwnd_ == nullptr) {
        return;
    }
    HWND child = AsHwnd(childHwnd_);
    // WindowFromPoint は他スレッドの窓へ WM_NCHITTEST を送りうるので使わない。矩形とカーソルだけで決める (同期メッセージなし)
    RECT rc = {};
    if (!IsWindow(child) || !GetWindowRect(child, &rc)) {
        return;
    }
    if (ShouldFocusBrowserChild(shown_, status_.hung, ToBrowserRect(rc), screenX, screenY)) {
        focusChildPending_ = true;
    }
}

void BrowserHost::Update(const BrowserFrameInput& in)
{
    switch (status_.state) {
    case BrowserState::Launching:
        UpdateLaunching(in.nowMs);
        break;
    case BrowserState::Embedded:
        UpdateEmbedded(in);
        break;
    default:
        break;
    }
}

void BrowserHost::UpdateLaunching(int64_t nowMs)
{
    BrowserObservation obs;
    obs.launchElapsedMs = nowMs - launchedAtMs_;
    if (nowMs >= nextPollMs_) {
        nextPollMs_ = nowMs + kBrowserLaunchPollMs;
        const std::vector<BrowserWindowObservation> windows =
            ObserveTopLevelWindows(AsHwnd(mainHwnd_), &snapshot_, true);
        const int idx = PickBrowserWindow(windows, &snapshot_, launchExe_, launchMatch_);
        if (idx >= 0) {
            obs.candidateFound = true;
            obs.attachFailed = !AttachWindow(windows[static_cast<size_t>(idx)].hwnd);
            findElapsedMs_ = obs.launchElapsedMs;
        }
    }
    const BrowserStatus next = StepBrowserState(status_, obs);
    if (next.state == status_.state) {
        return;
    }
    switch (next.state) {
    case BrowserState::Embedded:
        SetResult(BrowserState::Embedded, "attached");
        break;
    case BrowserState::Failed:
        SetResult(BrowserState::Failed, message_); // AttachWindow が理由を入れている
        MYE_LOG_WARN("[browser] attach failed: %s", message_.c_str());
        break;
    case BrowserState::TimedOut:
        SetResult(BrowserState::TimedOut, "no browser window found within the time limit");
        break;
    default:
        break;
    }
}

void BrowserHost::UpdateEmbedded(const BrowserFrameInput& in)
{
    HWND child = AsHwnd(childHwnd_);
    HWND main = AsHwnd(mainHwnd_);
    // 予約はこのフレームで必ず消費する。渡せない状態 (隠す・応答なし・消えた) で早く抜けても持ち越さない
    const bool focusRequested = focusChildPending_;
    focusChildPending_ = false;

    BrowserObservation obs;
    obs.windowAlive = IsWindow(child) != FALSE;
    if (obs.windowAlive) {
        obs.windowHung = IsHungAppWindow(child) != FALSE;
        // 同期メッセージではない。確定は一定時間続いたときだけ (壊れかけの窓の一瞬の空振りを Failed にしない)
        if (GetParent(child) != main) {
            if (parentLostSinceMs_ < 0) {
                parentLostSinceMs_ = in.nowMs;
            }
            obs.parentLost = in.nowMs - parentLostSinceMs_ >= kBrowserParentLostConfirmMs;
        } else {
            parentLostSinceMs_ = -1;
        }
        goneSinceMs_ = -1;
    } else {
        if (goneSinceMs_ < 0) {
            goneSinceMs_ = in.nowMs;
        }
        obs.goneForMs = in.nowMs - goneSinceMs_;
        if (ownerProcess_ != nullptr && WaitForSingleObject(static_cast<HANDLE>(ownerProcess_), 0) == WAIT_OBJECT_0) {
            obs.ownerExited = true;
            DWORD code = 0;
            if (GetExitCodeProcess(static_cast<HANDLE>(ownerProcess_), &code)) {
                obs.ownerExitNonZero = code != 0;
            }
        }
    }

    const BrowserStatus next = StepBrowserState(status_, obs);
    if (next.state == BrowserState::Failed) {
        // 親を外された窓は WS_CHILD のままデスクトップに残っている。応答なしなら同期で待たされるので触らず、理由だけ残す
        if (!obs.windowHung) {
            RestoreTopLevel(child, saved_);
        }
        ReleaseChild();
        failReason_ = BrowserFailReason::ParentRemoved;
        SetResult(BrowserState::Failed, "browser removed the embedded window from the editor (parent was removed)");
        MYE_LOG_WARN("[browser] %s", message_.c_str());
        return;
    }
    if (next.state != BrowserState::Embedded) {
        const bool lost = next.state == BrowserState::Lost;
        ReleaseChild();
        SetResult(next.state, lost ? "browser process exited abnormally" : "browser window closed");
        return;
    }
    status_ = next;
    if (!obs.windowAlive) {
        return; // 消えた直後の猶予中
    }
    if (status_.hung) {
        return; // SetWindowPos 等は同期で待たされる。応答が戻るまで触らない
    }

    // 表示切替 (状態が変わったときだけ。ShowWindowAsync は待たない)
    if (in.show != shown_) {
        if (!in.show) {
            // 隠す窓にキーを打たせない: フォーカスが子窓にあればメイン窓へ戻す
            HWND focus = GetFocus();
            if (focus != nullptr && (focus == child || IsChild(child, focus))) {
                SetFocus(main);
            }
        }
        ShowWindowAsync(child, in.show ? SW_SHOWNA : SW_HIDE);
        shown_ = in.show;
    }
    if (!in.show || in.contentRect.Empty()) {
        return;
    }
    // 位置合わせ: 子窓の今の矩形が内容矩形と違うときだけ (ブラウザ側が自分でサイズを変えた場合も含む)。1 秒 10 回まで
    const bool minimized = IsIconic(child) != FALSE;
    const bool maximized = IsZoomed(child) != FALSE;
    if (minimized || maximized || !(ClientRectOf(main, child) == in.contentRect)) {
        if (rectLimiter_.TryAcquire(in.nowMs)) {
            if (minimized || maximized) {
                ShowWindowAsync(child, SW_RESTORE);
            }
            SetWindowPos(child, nullptr, in.contentRect.left, in.contentRect.top, in.contentRect.Width(),
                         in.contentRect.Height(), SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
    }
    // ブラウザ上で押された: キーボードフォーカスを子窓へ渡す。WS_CHILD にした窓は WM_ACTIVATE を受けないので、
    // ブラウザは自分ではフォーカスを取り直さない (エディタ側を一度クリックすると打てなくなる)。
    // 親子にした時点で入力キューは結合しているので、他プロセスの窓へも SetFocus が効く
    if (focusRequested && parentLostSinceMs_ < 0) {
        HWND focus = GetFocus();
        if (focus != child && !IsChild(child, focus)) {
            SetFocus(child);
        }
    }
}

} // namespace mye
