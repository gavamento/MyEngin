//====================================================================================
//                          TerminalByteQueue.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル出力の読み取りスレッド → UI スレッドの受け渡しキュー (ロック付き)
//====================================================================================
#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace mye {

// 読み取りスレッドが積み、UI スレッドが取り出すバイトのキュー。
// 上限を超えると Push が待つ (子の出力パイプが詰まり、子自身が書き込みで待つ = 背圧)。
// ★ClosePseudoConsole は出力パイプが読まれるまで返らないことがある。閉じる前に必ず
//   SetMode(Unbounded / Discard) にして、読み取りスレッドが Push で待ち続けないようにする
class TerminalByteQueue
{
public:
    enum class Mode
    {
        Bounded,     // 上限で Push が待つ (通常運転)
        Unbounded,   // 上限を無視して積む (閉じる間に最後の出力を取りこぼさないため)
        Discard,     // 積まずに捨てる。既存の中身も消す (破棄時)
    };

    explicit TerminalByteQueue(size_t capacity);
    TerminalByteQueue(const TerminalByteQueue&) = delete;
    TerminalByteQueue& operator=(const TerminalByteQueue&) = delete;

    // 読み取りスレッドから呼ぶ。Bounded で満杯なら空くかモードが変わるまで待つ
    void Push(std::string_view bytes);
    // UI スレッドから呼ぶ。先頭から最大 maxBytes を out の末尾へ移し、移したバイト数を返す
    size_t PopUpTo(std::string& out, size_t maxBytes);
    // 待っている Push を起こす
    void SetMode(Mode mode);
    // 中身を空にして Bounded へ戻す (再起動時。読み取りスレッドが居ないときだけ呼ぶ)
    void Reset();
    size_t Size() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_spaceAvailable;
    std::string m_buffer;
    size_t m_head = 0;   // m_buffer の未読の先頭。PopUpTo のたびに先頭を消すと O(n^2) になるので位置で持つ
    size_t m_capacity = 0;
    Mode m_mode = Mode::Bounded;
};

} // namespace mye
