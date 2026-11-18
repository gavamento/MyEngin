//====================================================================================
//                          BrowserProbe.h
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          ブラウザ埋め込みの検証用: ダミー窓プロセスと実ブラウザのプローブ
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "Editor/Tools/Browser/BrowserHost.h"

namespace mye {
class BrowserWindow;
}

namespace mye {

// --browser-dummy-window [--browser-dummy-hang-after-ms N] [--browser-dummy-unparent-after-ms N]。タイトル固定の普通のトップレベル窓を出してメッセージを回すだけの
// プロセス (selftest 専用)。hangAfterMs > 0 なら、その ms 後に UI スレッドを 30 秒止める (応答なしの再現)。
// unparentAfterMs > 0 なら、その ms 後に自分で SetParent(自分, nullptr) する (WS_CHILD のまま親を外すブラウザの再現)。
// WM_CLOSE で終了コード 0 で抜ける。エンジンは初期化しない
int RunBrowserDummyWindow(int hangAfterMs, int unparentAfterMs);

constexpr const wchar_t* kBrowserDummyTitle = L"MyEngine Browser Dummy Window";
constexpr int kBrowserDummyHangMs = 30000;
// ダミー窓プロセスの寿命の上限。テストが失敗経路で後始末できなくても、自分で終了して Editor.exe を掴み続けない
constexpr int kBrowserDummyLifetimeMs = 120000;

struct BrowserProbeOptions {
    std::wstring id;           // 候補の id (BrowserCatalog) / "dummy" = この exe 自身をダミー窓として起動
    std::wstring url;          // 空 = ブラウザの既定
    int holdMs = 5000;         // 埋め込んでから切り離すまで
    bool clipChildren = true;  // false = 親に WS_CLIPCHILDREN を足さない (O6 の観測)
};

// --browser-embed-probe。ブラウザ窓 (BrowserWindow) を開いたエディタで、id のブラウザを窓の「起動」と同じ経路で起動して
// そのパネルの内容矩形へ埋め込み、観測をログに出し、holdMs 保持してから切り離す。
// UI スレッドで毎フレーム、BrowserWindow::ApplyToHost の後に Frame を呼ぶ
class BrowserEmbedProbe {
public:
    BrowserEmbedProbe(const BrowserProbeOptions& options, BrowserWindow& window);
    ~BrowserEmbedProbe();

    // OnImGui の末尾から毎フレーム (BrowserWindow::ApplyToHost の後)
    void Frame(void* mainHwnd, int64_t nowMs);
    // メイン窓の WM_INPUT を数える (Win32Window のメッセージハンドラから呼ぶ)
    void OnRawInput() { ++rawInputCount_; }

    bool Finished() const { return phase_ == Phase::Done; }
    int ExitCode() const { return exitCode_; }

private:
    enum class Phase { Warmup, Launching, Holding, Done };

    void Finish(bool ok, const char* why);
    void LogEmbedded(void* mainHwnd);
    void SampleHold(void* mainHwnd, const char* tag);

    BrowserProbeOptions options_;
    BrowserWindow& window_;
    Phase phase_ = Phase::Warmup;
    int exitCode_ = 1;
    int frames_ = 0;
    int64_t startMs_ = 0;
    int64_t embeddedAtMs_ = 0;
    bool sampledMid_ = false;
    bool embeddedOk_ = false;
    uint64_t rawInputCount_ = 0;
    uint64_t rawInputAtHoldStart_ = 0;
};

} // namespace mye
