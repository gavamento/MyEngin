//====================================================================================
//                          TerminalSession.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの 1 セッション (画面モデルと子プロセスのホストを束ね、毎フレーム進める)
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Editor/Tools/Terminal/TerminalHost.h"
#include "Editor/Tools/Terminal/TerminalInput.h"
#include "Editor/Tools/Terminal/TerminalScreen.h"

namespace mye {

// 1 フレームに libvterm へ投入する出力の上限 (spec 4.4)。1MB を流しても UI が固まらないよう、残りは次フレームへ回す
inline constexpr size_t kTerminalFeedBytesPerFrame = 256u * 1024u;

enum class TerminalSessionState
{
    NotStarted,
    Running,
    Exited,        // 子が終わった (ExitCode が有効)。Restart で起こし直せる
    Failed,        // 起動失敗 (ErrorMessage)。Restart で再試行できる
    Unsupported,   // ConPTY が無い OS。Restart しても変わらない
};

// 1 つのシェルと 1 つの画面。所有者は Terminal 窓 (sub-04)。UI スレッドだけが触る (スレッド安全ではない)。
// 呼ぶ順: Start → 毎フレーム Pump (窓が隠れていても) → 必要に応じて Send* / Resize / Restart → 破棄。
// 破棄 (デストラクタ) で子孫ごと終わらせる。エディタ終了時は所有者が確実に破棄すること
class TerminalSession
{
public:
    TerminalSession(std::unique_ptr<TerminalHost> host, int cols, int rows, int scrollbackLimit);
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    // 子を起動する。spec は Restart のために保持する。Running 中に呼んだら何もせず false
    bool Start(const TerminalLaunchSpec& spec);
    // 今の子を捨てて (Running なら終わらせて) 同じ spec で画面を空にして起こし直す。Unsupported / 未起動なら false
    bool Restart();

    // 1 フレーム分進める: 出力を最大 maxBytes 投入 → 画面が作った応答 (DA 等) を子へ返す → 子の終了を見る。
    // 投入したバイト数を返す。キー入力が無いフレームでも毎フレーム呼ぶ (応答の返送が遅れると CLI が待つ)
    size_t Pump(size_t maxBytes = kTerminalFeedBytesPerFrame);

    // 画面モデルのスクロールバックだけを捨てる (CSI 3 J を自分の画面へ流す。子には何も送らず、今見えている画面も変えない)
    void ClearScrollback();

    // 入力を変換して子へ送る (Running 以外では捨てる)
    void SendKey(TerminalKey key, uint8_t modifiers);
    void SendChar(char32_t codepoint, uint8_t modifiers);
    void SendPaste(std::string_view utf8);

    // 画面の列数・行数を変える (最小サイズへ切り上げ)。実際に変わり、Running ならホストにも伝える。変わったら true
    bool Resize(int cols, int rows);

    TerminalSessionState State() const { return m_state; }
    uint32_t ExitCode() const { return m_exitCode; }
    const std::string& ErrorMessage() const { return m_error; }
    const TerminalScreen& Screen() const { return *m_screen; }

private:
    // 画面が溜めた「子へ送るバイト」を Running なら書く。それ以外では捨てる (終わった子へは送れない)
    void FlushScreenOutput();

    std::unique_ptr<TerminalHost> m_host;
    std::unique_ptr<TerminalScreen> m_screen;
    int m_scrollbackLimit = 0;
    TerminalLaunchSpec m_spec;
    bool m_hasSpec = false;
    TerminalSessionState m_state = TerminalSessionState::NotStarted;
    uint32_t m_exitCode = 0;
    std::string m_error;
    std::string m_feedScratch;   // Pump で使い回す読み取りバッファ (毎フレームの確保を避ける)
};

} // namespace mye
