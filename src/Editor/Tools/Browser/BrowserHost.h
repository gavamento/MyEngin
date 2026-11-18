//====================================================================================
//                          BrowserHost.h
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          別プロセスのブラウザ窓の起動・特定・埋め込み (中核)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// 矩形 (px)。座標系は使う側が決める (メイン窓のクライアント座標 / 画面座標)
struct BrowserRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return Width() < 1 || Height() < 1; }
    bool operator==(const BrowserRect& o) const
    {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
    // 辺が触れるだけ (面積 0) は重ならない
    bool Overlaps(const BrowserRect& o) const
    {
        return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
    }
};

// ---- 窓の特定 (Win32 を呼ばない純関数) ----

// トップレベル窓 1 枚の観測。EnumWindows の列挙順 (Z 順の上から) で並べて渡す
struct BrowserWindowObservation {
    void* hwnd = nullptr;
    bool visible = false;
    bool hasOwner = false;
    bool toolWindow = false;     // WS_EX_TOOLWINDOW
    int width = 0;
    int height = 0;
    std::wstring imagePath;      // 所有プロセスの実行ファイル (取れなければ空)
};

constexpr int kBrowserMinWindowWidth = 200;
constexpr int kBrowserMinWindowHeight = 150;

// 所有プロセスの exe が登録 exe と同じファイル名で、登録 exe のディレクトリの配下か (大文字小文字無視)。
// Opera GX のランチャー (...\Opera GX\opera.exe) の実体 (...\Opera GX\<版>\opera.exe) を同一視するための配下判定
bool BrowserImageMatchesRegistered(const std::wstring& registeredExe, const std::wstring& processImage);

// 窓の候補条件 (snapshot に無い / 可視・所有者なし・ツール窓でない / 200x150 以上 / exe 一致) を全部満たす観測の添字を、
// 入力順 (= Z 順の上から) に返す。snapshot が nullptr のとき 1 つ目の条件は見ない (既存の窓を取り込む用)
std::vector<size_t> FilterBrowserWindows(const std::vector<BrowserWindowObservation>& zOrdered,
                                         const std::vector<void*>* snapshot, const std::wstring& registeredExe);

// FilterBrowserWindows の先頭 (Z 順で最上位)。候補なしは -1
int PickBrowserWindow(const std::vector<BrowserWindowObservation>& zOrdered, const std::vector<void*>* snapshot,
                      const std::wstring& registeredExe);

// ---- 状態機械 (純関数) ----

enum class BrowserState { Idle, Launching, Embedded, Closed, Lost, TimedOut, Failed };

struct BrowserStatus {
    BrowserState state = BrowserState::Idle;
    bool hung = false; // Embedded の間だけ意味がある (戻れば消える)
};

constexpr int64_t kBrowserLaunchTimeoutMs = 15000;
constexpr int64_t kBrowserLaunchPollMs = 100;
// 窓が消えてから、所有プロセスの終了 (落ちた) を待つ猶予。TerminateProcess 直後は窓が先に消えて終了コードが遅れることがある
constexpr int64_t kBrowserGoneGraceMs = 400;
// 親の喪失をこの間続けて観測したら確定する。強制終了で壊れかけの窓は一瞬 GetParent が空になるため (すぐ IsWindow が偽になる)
constexpr int64_t kBrowserParentLostConfirmMs = 150;

// 1 回の観測。Win32 を叩く側が集めて StepBrowserState へ渡す
struct BrowserObservation {
    // Launching
    int64_t launchElapsedMs = 0;
    bool candidateFound = false;   // 窓の候補が見つかり、Attach を試した
    bool attachFailed = false;     // その Attach が失敗した
    // Embedded
    bool windowAlive = true;       // IsWindow
    bool windowHung = false;       // IsHungAppWindow
    bool parentLost = false;       // 窓は生きているが親がメイン窓でなくなった (ブラウザ側が親を外した。Opera GX で観測)
    bool ownerExited = false;      // 窓の所有プロセスが終了済み
    bool ownerExitNonZero = false; // その終了コードが 0 以外
    int64_t goneForMs = 0;         // 窓が消えてからの経過 (windowAlive == true なら 0)
};

// 遷移関数。親の喪失 (windowAlive かつ parentLost) は Failed。Closed (閉じた) と Lost (落ちた) の区別: 窓が消え、所有プロセスが 0 以外の終了コードで終わっていれば Lost、
// 0 で終わった・猶予 (kBrowserGoneGraceMs) の間に終わらなかったなら Closed。Idle / Closed / Lost / TimedOut / Failed は動かない
BrowserStatus StepBrowserState(const BrowserStatus& cur, const BrowserObservation& obs);

// ---- 見せる条件 (純関数) ----

enum class BrowserHideReason {
    None,           // 見せる
    PanelNotDrawn,  // 1: パネルがこのフレーム描かれていない
    EmptyRect,      // 2: 内容矩形が 1 px 未満
    MainMinimized,  // 3
    WindowDragging, // 4: ImGui の窓の移動・ドッキングのドラッグ中
    ModalOpen,      // 5
    Overlapped,     // 6: 手前に描かれる窓と重なる
};

struct BrowserVisibilityInput {
    bool panelDrawn = false;
    BrowserRect panel;                  // 内容矩形 (メイン窓のクライアント座標)
    bool mainMinimized = false;
    bool windowDragging = false;
    bool modalOpen = false;
    std::vector<BrowserRect> overlays;  // パネルより手前の窓の矩形 (パネル自身の子・ツールチップは呼び出し側で除く)
};

BrowserHideReason EvaluateBrowserVisibility(const BrowserVisibilityInput& in);

// 時間窓あたりの回数上限 (矩形の取り合いで点滅させない用。既定 1 秒 10 回)
class BrowserRateLimiter {
public:
    explicit BrowserRateLimiter(int maxCount = 10, int64_t windowMs = 1000) : maxCount_(maxCount), windowMs_(windowMs) {}
    // 通れるなら true を返して 1 回と数える
    bool TryAcquire(int64_t nowMs);

private:
    int maxCount_;
    int64_t windowMs_;
    std::vector<int64_t> stamps_;
};

// ---- ホスト ----

struct BrowserFrameInput {
    int64_t nowMs = 0;
    bool show = false;       // EvaluateBrowserVisibility が None
    BrowserRect contentRect; // メイン窓のクライアント座標 (px)
};

// 取り込み候補 (既存の窓)
struct BrowserWindowInfo {
    void* hwnd = nullptr;
    std::wstring title;
    uint32_t pid = 0;
};

// 単調増加の現在時刻 (ms)。Editor の UI 状態にだけ使う (sim には持ち込まない)
int64_t BrowserNowMs();

// 登録 exe の今ある窓 (取り込み用。snapshot 条件なし)
std::vector<BrowserWindowInfo> EnumerateBrowserWindows(const std::wstring& registeredExe);

// 別プロセスのブラウザ窓を 1 枚だけメイン窓の子にする。UI スレッド専用。
// SendMessage 系の同期呼び出しをブラウザ窓へ投げない (入力キューが結合するため。応答なしなら SetParent / SetWindowPos も呼ばない)
class BrowserHost {
public:
    BrowserHost() = default;
    ~BrowserHost();
    BrowserHost(const BrowserHost&) = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    // 子窓の親 (メイン窓の HWND)。Launch / AttachExisting より前に 1 回
    void SetMainWindow(void* mainHwnd) { mainHwnd_ = mainHwnd; }
    // true = 埋め込み時に親へ WS_CLIPCHILDREN を足す (既定 true。要否の観測用に切れる)
    void SetClipChildren(bool on) { clipChildren_ = on; }

    // exe を args (exe を除く) で起動し、窓の特定を始める。Idle / Closed / Lost / TimedOut / Failed からだけ
    bool Launch(const std::wstring& exePath, const std::wstring& args, int64_t nowMs);
    // 既存の窓を Attach する。Idle / Closed / Lost / TimedOut / Failed からだけ
    bool AttachExisting(void* hwnd);
    // 元のスタイル・画面上の今の位置に戻して外へ出す。応答なしなら何もせず false
    bool Detach();
    // WM_CLOSE を Post する (閉じたかどうかは Update が窓の消滅で見る)
    void RequestClose();
    // 子窓 (その子孫を含む) がキーボードフォーカスを持っていればメイン窓へ戻す。応答なしなら何もしない。UI スレッド専用
    void ReturnFocusToMain();
    // 待つのをやめる (起動したブラウザは閉じない)
    void CancelLaunch();
    // 毎フレーム (全 ImGui 窓の提出が終わった後に呼ぶ)
    void Update(const BrowserFrameInput& in);

    const BrowserStatus& Status() const { return status_; }
    BrowserState State() const { return status_.state; }
    bool Hung() const { return status_.hung; }
    // 直前の結果の理由 (英語。UI の文字列は sub-02)
    const std::string& Message() const { return message_; }
    void* ChildHwnd() const { return childHwnd_; }
    // 観測・検証用
    uint32_t LastLaunchPid() const { return launchPid_; }
    uint32_t ChildPid() const { return childPid_; }
    // 直近に Attach を試した窓 (Failed のあと、窓がどう戻ったかを調べる検証用)
    void* LastTargetHwnd() const { return lastTargetHwnd_; }
    int64_t FindElapsedMs() const { return findElapsedMs_; }
    bool ChildShown() const { return shown_; }

private:
    // Attach 前の窓の状態 (Detach で戻す)
    struct SavedWindow {
        std::intptr_t style = 0;
        std::intptr_t exStyle = 0;
        BrowserRect normalRect;     // 画面座標。最大化中は通常時の矩形
        bool wasMaximized = false;
    };

    void UpdateLaunching(int64_t nowMs);
    void UpdateEmbedded(const BrowserFrameInput& in);
    bool AttachWindow(void* hwnd);
    // 子窓化した窓を元のスタイルの普通のトップレベル窓へ戻す (Detach と、親を失ったときの Failed で共有)。
    // 親がデスクトップのまま (ブラウザ側が外した) なら SetParent はしない。位置は今の画面位置、大きさは saved の通常時
    static void RestoreTopLevel(void* childHwnd, const SavedWindow& saved);
    void ReleaseChild();
    void SetResult(BrowserState state, std::string message);

    void* mainHwnd_ = nullptr;
    void* childHwnd_ = nullptr;
    void* lastTargetHwnd_ = nullptr;
    void* ownerProcess_ = nullptr; // 窓の所有プロセスのハンドル (生死の観測用。ReleaseChild で閉じる)
    uint32_t childPid_ = 0;
    uint32_t launchPid_ = 0;
    bool clipChildren_ = true;
    BrowserStatus status_;
    std::string message_;
    std::wstring launchExe_;
    std::vector<void*> snapshot_;
    int64_t launchedAtMs_ = 0;
    int64_t nextPollMs_ = 0;
    int64_t findElapsedMs_ = -1;
    int64_t goneSinceMs_ = -1;
    int64_t parentLostSinceMs_ = -1;
    bool shown_ = true;
    BrowserRateLimiter rectLimiter_;
    SavedWindow saved_;
};

} // namespace mye
