//====================================================================================
//                          TerminalSession.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの 1 セッション (画面モデルと子プロセスのホストを束ね、毎フレーム進める)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalSession.h"

#include <utility>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

TerminalSession::TerminalSession(std::unique_ptr<TerminalHost> host, int cols, int rows, int scrollbackLimit)
    : m_host(std::move(host))
    , m_screen(std::make_unique<TerminalScreen>(cols, rows, scrollbackLimit))
    , m_scrollbackLimit(scrollbackLimit)
{
}

TerminalSession::~TerminalSession()
{
    // 画面より先にホストを閉じる。ホストの Close は読み取りスレッドの join まで済ませるので、以後は誰も触らない
    if (m_host) {
        m_host->Close(false);
    }
}

bool TerminalSession::Start(const TerminalLaunchSpec& spec)
{
    if (m_state == TerminalSessionState::Running || !m_host) {
        return false;
    }
    m_spec = spec;
    m_hasSpec = true;
    m_exitCode = 0;
    m_error.clear();
    const TerminalHostStartResult result = m_host->Start(spec, m_screen->Cols(), m_screen->Rows(), m_error);
    switch (result) {
    case TerminalHostStartResult::Started:
        m_state = TerminalSessionState::Running;
        return true;
    case TerminalHostStartResult::Unsupported:
        m_state = TerminalSessionState::Unsupported;
        break;
    case TerminalHostStartResult::Failed:
        m_state = TerminalSessionState::Failed;
        break;
    }
    MYE_LOG_WARN("[terminal] could not start shell: %s", m_error.c_str());
    return false;
}

bool TerminalSession::Restart()
{
    if (!m_hasSpec || m_state == TerminalSessionState::Unsupported || !m_host) {
        return false;
    }
    // 前の子の残り出力は新しい画面へ混ぜない (false = 捨てる)
    m_host->Close(false);
    m_screen = std::make_unique<TerminalScreen>(m_screen->Cols(), m_screen->Rows(), m_scrollbackLimit);
    m_state = TerminalSessionState::NotStarted;
    const TerminalLaunchSpec spec = m_spec;
    return Start(spec);
}

size_t TerminalSession::Pump(size_t maxBytes)
{
    if (m_state == TerminalSessionState::NotStarted || m_state == TerminalSessionState::Unsupported) {
        return 0;
    }
    // Exited / Failed でも読む: 子の最後の出力 (終了直前の echo など) はホストの Close 後に届き切る
    m_feedScratch.clear();
    const size_t fed = m_host->ReadOutput(m_feedScratch, maxBytes);
    m_screen->Feed(m_feedScratch);
    FlushScreenOutput();

    uint32_t code = 0;
    if (m_state == TerminalSessionState::Running && m_host->PollExit(code)) {
        m_exitCode = code;
        m_state = TerminalSessionState::Exited;
        // 疑似コンソールを閉じて最後の出力を吐かせる (true = 残す)。残りは次フレーム以降の Pump で投入される
        m_host->Close(true);
        MYE_LOG_INFO("[terminal] shell exited with code %u", static_cast<unsigned>(code));
    }
    return fed;
}

void TerminalSession::SendKey(TerminalKey key, uint8_t modifiers)
{
    TerminalSendKey(*m_screen, key, modifiers);
    FlushScreenOutput();
}

void TerminalSession::SendChar(char32_t codepoint, uint8_t modifiers)
{
    TerminalSendChar(*m_screen, codepoint, modifiers);
    FlushScreenOutput();
}

void TerminalSession::SendPaste(std::string_view utf8)
{
    TerminalSendPaste(*m_screen, utf8);
    FlushScreenOutput();
}

bool TerminalSession::Resize(int cols, int rows)
{
    if (!m_screen->Resize(cols, rows)) {
        return false;
    }
    if (m_state == TerminalSessionState::Running && !m_host->Resize(m_screen->Cols(), m_screen->Rows())) {
        MYE_LOG_WARN("[terminal] resize to %dx%d failed", m_screen->Cols(), m_screen->Rows());
    }
    return true;
}

void TerminalSession::FlushScreenOutput()
{
    std::string bytes = m_screen->TakeOutput();
    if (bytes.empty() || m_state != TerminalSessionState::Running) {
        return;
    }
    if (!m_host->Write(bytes)) {
        MYE_LOG_WARN("[terminal] write of %zu byte(s) to the shell failed", bytes.size());
    }
}

} // namespace mye
