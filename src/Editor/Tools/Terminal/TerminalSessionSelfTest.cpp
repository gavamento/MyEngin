//====================================================================================
//                          TerminalSessionSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルセッション (受け渡しキュー・セッション状態遷移・ConPTY 往復) の回帰テスト
//====================================================================================
#include "Editor/Tools/Terminal/TerminalSessionSelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Editor/Tools/Terminal/TerminalByteQueue.h"
#include "Editor/Tools/Terminal/TerminalHost.h"
#include "Editor/Tools/Terminal/TerminalSession.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// ---- 受け渡しキュー ----

void TestQueue(const auto& check)
{
    TerminalByteQueue queue(8);
    std::string out;
    check(queue.PopUpTo(out, 100) == 0 && out.empty(), "queue: empty pop");
    queue.Push("hello");
    queue.Push("");
    queue.Push("world");   // 5 < 8 なので待たずに積める (1 回の Push は上限を超えてよい)
    check(queue.Size() == 10, "queue: push may overshoot capacity once");
    check(queue.PopUpTo(out, 3) == 3 && out == "hel", "queue: pop respects max");
    check(queue.PopUpTo(out, 100) == 7 && out == "helloworld" && queue.Size() == 0, "queue: pop appends rest in order");

    // 満杯で Push が待ち、Pop で空くと進む (背圧)
    queue.Push("12345678");
    std::atomic<bool> pushed = false;
    std::thread producer([&] {
        queue.Push("X");
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!pushed, "queue: bounded push waits while full");
    out.clear();
    queue.PopUpTo(out, 4);
    producer.join();
    check(pushed && queue.Size() == 5, "queue: pop releases a waiting push");
    out.clear();
    queue.PopUpTo(out, 100);
    check(out == "5678X", "queue: order kept across the wait");

    // 満杯で待っている Push を Discard が起こし、中身も捨てる (ClosePseudoConsole 前の手順)
    queue.Push("12345678");
    pushed = false;
    std::thread discarded([&] {
        queue.Push("Y");
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.SetMode(TerminalByteQueue::Mode::Discard);
    discarded.join();
    check(pushed && queue.Size() == 0, "queue: discard releases waiter and drops data");
    queue.Push("Z");
    check(queue.Size() == 0, "queue: discard drops later pushes");

    // Unbounded は上限を無視して積む (子が自分で終わったときの最後の出力)
    queue.Reset();
    queue.Push("12345678");
    queue.SetMode(TerminalByteQueue::Mode::Unbounded);
    queue.Push("abc");
    check(queue.Size() == 11, "queue: unbounded ignores capacity");
    queue.Reset();
    check(queue.Size() == 0, "queue: reset clears");

    // 大量の小分け Pop でも順序と総量が保たれる (前半を詰める処理の確認)
    TerminalByteQueue big(1u << 20);
    std::string expected;
    for (int i = 0; i < 1000; ++i) {
        const std::string piece = std::to_string(i) + ",";
        expected += piece;
        big.Push(piece);
    }
    std::string all;
    while (big.PopUpTo(all, 7) > 0) {
    }
    check(all == expected, "queue: many small pops keep bytes");
}

// ---- 偽ホスト ----

// 偽ホストの観測値。セッションが破棄された後も読めるよう共有で持つ
struct FakeHostLog
{
    TerminalHostStartResult nextStart = TerminalHostStartResult::Started;
    int startCount = 0;
    int startCols = 0;
    int startRows = 0;
    std::string startExe;
    std::string pending;          // 子の出力として読ませるバイト
    std::string pendingOnClose;   // Close(true) のときに届く最後の出力
    std::string written;          // 子の入力へ書かれたバイト
    int resizeCount = 0;
    int resizeCols = 0;
    int resizeRows = 0;
    bool exited = false;
    uint32_t exitCode = 0;
    int closeCount = 0;
    int closeKeepCount = 0;
    bool running = false;
};

class FakeHost final : public TerminalHost
{
public:
    explicit FakeHost(std::shared_ptr<FakeHostLog> log)
        : m_log(std::move(log))
    {
    }

    TerminalHostStartResult Start(const TerminalLaunchSpec& spec, int cols, int rows, std::string& errorOut) override
    {
        ++m_log->startCount;
        m_log->startCols = cols;
        m_log->startRows = rows;
        m_log->startExe = spec.exePath;
        if (m_log->nextStart != TerminalHostStartResult::Started) {
            errorOut = "fake failure";
            return m_log->nextStart;
        }
        m_log->running = true;
        m_log->exited = false;
        return TerminalHostStartResult::Started;
    }
    size_t ReadOutput(std::string& out, size_t maxBytes) override
    {
        const size_t n = std::min(maxBytes, m_log->pending.size());
        out.append(m_log->pending, 0, n);
        m_log->pending.erase(0, n);
        return n;
    }
    bool Write(std::string_view bytes) override
    {
        if (!m_log->running) {
            return false;
        }
        m_log->written.append(bytes.data(), bytes.size());
        return true;
    }
    bool Resize(int cols, int rows) override
    {
        ++m_log->resizeCount;
        m_log->resizeCols = cols;
        m_log->resizeRows = rows;
        return m_log->running;
    }
    bool PollExit(uint32_t& exitCode) override
    {
        exitCode = m_log->exitCode;
        return m_log->running && m_log->exited;
    }
    void Close(bool keepPendingOutput) override
    {
        ++m_log->closeCount;
        if (keepPendingOutput) {
            ++m_log->closeKeepCount;
            m_log->pending += m_log->pendingOnClose;
        } else {
            m_log->pending.clear();
        }
        m_log->pendingOnClose.clear();
        m_log->running = false;
    }

private:
    std::shared_ptr<FakeHostLog> m_log;
};

bool ScreenContains(const TerminalScreen& screen, const std::string& text)
{
    for (int line = -screen.ScrollbackCount(); line < screen.Rows(); ++line) {
        if (TerminalLineText(screen, line).find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TerminalLaunchSpec FakeSpec()
{
    TerminalLaunchSpec spec;
    spec.exePath = "C:\\fake\\shell.exe";
    spec.args = { "-x" };
    return spec;
}

void TestSessionLifecycle(const auto& check)
{
    auto log = std::make_shared<FakeHostLog>();
    {
        TerminalSession session(std::make_unique<FakeHost>(log), 80, 24, 100);
        check(session.State() == TerminalSessionState::NotStarted, "session: initial state");
        check(session.Pump() == 0, "session: pump before start does nothing");
        session.SendChar(U'a', kTerminalModNone);
        check(log->written.empty(), "session: input before start is dropped");

        check(session.Start(FakeSpec()), "session: start succeeds");
        check(session.State() == TerminalSessionState::Running && log->startCount == 1, "session: running after start");
        check(log->startCols == 80 && log->startRows == 24 && log->startExe == "C:\\fake\\shell.exe", "session: host gets screen size and spec");
        check(!session.Start(FakeSpec()) && log->startCount == 1, "session: start while running is refused");

        // 入力の送信
        session.SendChar(U'a', kTerminalModNone);
        session.SendKey(TerminalKey::Enter, kTerminalModNone);
        check(log->written == "a\r", "session: keys go to host");
        log->written.clear();
        session.SendPaste("x\ny");
        check(log->written == "x\ry", "session: paste goes to host (newline normalized)");
        log->written.clear();

        // 出力の投入と DA 応答の返送 (Pump の中で、キー入力が無くても返す)
        log->pending = "hello\x1b[c";
        session.Pump();
        check(ScreenContains(session.Screen(), "hello"), "session: output reaches screen");
        check(log->written.rfind("\x1b[?", 0) == 0, "session: DA reply is written back in pump");
        log->written.clear();

        // 1 フレームの投入上限
        log->pending.assign(kTerminalFeedBytesPerFrame * 2 + 100, 'z');
        check(session.Pump() == kTerminalFeedBytesPerFrame, "session: pump caps at per-frame limit");
        check(session.Pump() == kTerminalFeedBytesPerFrame, "session: rest goes to next frame");
        check(session.Pump() == 100 && log->pending.empty(), "session: last remainder");
        check(session.Pump(10) == 0, "session: pump with nothing pending");
        log->pending = "abcdef";
        check(session.Pump(4) == 4 && log->pending == "ef", "session: explicit max honored");
        session.Pump();

        // リサイズ: 変化時だけホストへ、最小サイズへ切り上げ
        check(session.Resize(100, 30) && log->resizeCount == 1 && log->resizeCols == 100 && log->resizeRows == 30,
              "session: resize forwarded to host");
        check(!session.Resize(100, 30) && log->resizeCount == 1, "session: same size is not forwarded");
        check(session.Resize(1, 1) && log->resizeCols == kTerminalMinCols && log->resizeRows == kTerminalMinRows,
              "session: resize clamps to minimum before host");

        // 履歴の消去: 自分の画面のスクロールバックだけを捨てる。子へは何も送らず、見えている画面も変えない
        // (上の大量出力 z... で画面は 'z' の行で埋まり、スクロールバックが溜まっている)
        check(session.Screen().ScrollbackCount() > 0, "session: scrollback has lines before clearing");
        const std::string visibleBefore = TerminalLineText(session.Screen(), 0);
        log->written.clear();
        session.ClearScrollback();
        check(session.Screen().ScrollbackCount() == 0 && TerminalLineText(session.Screen(), 0) == visibleBefore && log->written.empty(),
              "session: ClearScrollback drops the history only (screen kept, nothing sent to the shell)");

        // 子の終了: 終了コードを取り、Close(true) で最後の出力を残し、次の Pump で画面へ出す
        log->exited = true;
        log->exitCode = 3;
        log->pendingOnClose = "\r\nfinal-line";
        session.Pump();
        check(session.State() == TerminalSessionState::Exited && session.ExitCode() == 3, "session: exit detected with code");
        check(log->closeKeepCount == 1, "session: host closed keeping pending output");
        session.Pump();
        check(ScreenContains(session.Screen(), "final-line"), "session: output after exit still reaches screen");
        session.SendChar(U'q', kTerminalModNone);
        log->pending = "\x1b[c";
        session.Pump();
        check(log->written.empty(), "session: nothing written after exit (input, DA reply)");
        check(session.Resize(90, 25) && log->resizeCount == 2, "session: resize after exit changes screen only");

        // 再起動: 前の残りを捨てて Close(false)、画面を空にし、同じ spec で起こし直す
        const int closeBefore = log->closeCount;
        log->pending = "stale";
        check(session.Restart(), "session: restart succeeds");
        check(log->closeCount == closeBefore + 1 && log->startCount == 2 && session.State() == TerminalSessionState::Running,
              "session: restart closes and starts again");
        session.Pump();
        check(!ScreenContains(session.Screen(), "final-line") && !ScreenContains(session.Screen(), "stale"),
              "session: restart clears screen and drops stale output");
        check(session.Screen().Cols() == 90 && session.Screen().Rows() == 25 && log->startCols == 90,
              "session: restart keeps current size");
        check(session.ExitCode() == 0, "session: exit code reset on restart");
    }
    check(log->closeCount >= 3 && !log->running, "session: destructor closes host");
}

void TestSessionFailures(const auto& check)
{
    auto log = std::make_shared<FakeHostLog>();
    log->nextStart = TerminalHostStartResult::Failed;
    TerminalSession failed(std::make_unique<FakeHost>(log), 80, 24, 0);
    check(!failed.Restart(), "session: restart before any start is refused");
    check(!failed.Start(FakeSpec()) && failed.State() == TerminalSessionState::Failed && !failed.ErrorMessage().empty(),
          "session: failed start reports error");
    check(failed.Pump() == 0, "session: pump after failure is harmless");
    log->nextStart = TerminalHostStartResult::Started;
    check(failed.Restart() && failed.State() == TerminalSessionState::Running && failed.ErrorMessage().empty(),
          "session: restart retries after failure");

    auto unsupportedLog = std::make_shared<FakeHostLog>();
    unsupportedLog->nextStart = TerminalHostStartResult::Unsupported;
    TerminalSession unsupported(std::make_unique<FakeHost>(unsupportedLog), 80, 24, 0);
    check(!unsupported.Start(FakeSpec()) && unsupported.State() == TerminalSessionState::Unsupported,
          "session: unsupported state");
    check(!unsupported.Restart() && unsupportedLog->startCount == 1, "session: restart does not retry unsupported");
}

// ---- 実 ConPTY ----

constexpr auto kConPtyTimeout = std::chrono::seconds(15);

// 条件が立つまで Pump を回す。時間切れなら false
template <typename Pred>
bool PumpUntil(TerminalSession& session, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + kConPtyTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        session.Pump();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void TestConPty(const auto& check)
{
    // ConPTY が無い OS の擬似 (関数ポインタ null)
    {
        TerminalSession session(CreateConPtyTerminalHost(TerminalConPtyApiSource::Missing), 80, 24, 100);
        TerminalLaunchSpec spec;
        spec.exePath = FindTerminalExecutable("cmd.exe");
        check(!session.Start(spec) && session.State() == TerminalSessionState::Unsupported && !session.ErrorMessage().empty(),
              "conpty: missing api gives Unsupported");
    }

    const std::string cmd = FindTerminalExecutable("cmd.exe");
    check(!cmd.empty(), "conpty: cmd.exe found by FindTerminalExecutable");
    check(FindTerminalExecutable("mye_no_such_shell_8841.exe").empty(), "conpty: missing exe gives empty");
    if (cmd.empty()) {
        return;
    }

    // cmd /c echo のマーカーが画面に出て、終了コード 0
    const std::string marker = "MYE_TERMINAL_MARKER_7310";
    {
        TerminalSession session(CreateConPtyTerminalHost(), 80, 24, 100);
        TerminalLaunchSpec spec;
        spec.exePath = cmd;
        spec.args = { "/d", "/c", "echo", marker };
        check(session.Start(spec) && session.State() == TerminalSessionState::Running, "conpty: cmd /c echo starts");
        const bool done = PumpUntil(session, [&] {
            return session.State() == TerminalSessionState::Exited && ScreenContains(session.Screen(), marker);
        });
        check(done, "conpty: marker shown and process exited");
        check(session.ExitCode() == 0, "conpty: exit code 0");
    }

    // 対話シェルを動かしたままリサイズ・入力・再起動・破棄
    {
        auto host = CreateConPtyTerminalHost();
        TerminalLaunchSpec spec;
        spec.exePath = cmd;
        spec.args = { "/d", "/k" };
        std::string error;
        check(host->Start(spec, 80, 24, error) == TerminalHostStartResult::Started, "conpty: interactive cmd starts");
        check(host->Resize(100, 30), "conpty: ResizePseudoConsole succeeds while running");
        uint32_t code = 0;
        check(!host->PollExit(code), "conpty: interactive cmd still running");
        const auto closeBegin = std::chrono::steady_clock::now();
        host->Close(false);
        check(std::chrono::steady_clock::now() - closeBegin < kConPtyTimeout, "conpty: close returns");
        check(!host->Resize(80, 24) && !host->Write("x"), "conpty: closed host refuses resize and write");
    }
    {
        TerminalSession session(CreateConPtyTerminalHost(), 80, 24, 100);
        TerminalLaunchSpec spec;
        spec.exePath = cmd;
        spec.args = { "/d", "/k" };
        check(session.Start(spec), "conpty: session interactive start");
        const std::string echoMarker = "MYE_TERMINAL_INPUT_5521";
        session.SendPaste("echo " + echoMarker + "\r");
        check(PumpUntil(session, [&] { return ScreenContains(session.Screen(), echoMarker); }),
              "conpty: input reaches shell and echoes");
        check(session.Resize(100, 30), "conpty: session resize");
        check(session.Restart() && session.State() == TerminalSessionState::Running, "conpty: restart while running");
        session.SendPaste("exit 7\r");
        check(PumpUntil(session, [&] { return session.State() == TerminalSessionState::Exited; }) && session.ExitCode() == 7,
              "conpty: exit code from shell");
    }
}

bool Run(const char* title, void (*body)(const std::function<void(bool, const char*)>&))
{
    MYE_LOG_INFO("==== %s self test ====", title);
    int failCount = 0;
    const std::function<void(bool, const char*)> check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    body(check);
    MYE_LOG_INFO("%s self test: %s (%d failure(s))", title, failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace

bool RunTerminalSessionSelfTest()
{
    return Run("TerminalSession", [](const std::function<void(bool, const char*)>& check) {
        TestQueue(check);
        TestSessionLifecycle(check);
        TestSessionFailures(check);
    });
}

bool RunTerminalConPtySelfTest()
{
    return Run("TerminalConPty", [](const std::function<void(bool, const char*)>& check) { TestConPty(check); });
}

} // namespace mye
