//====================================================================================
//                          TerminalByteQueue.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル出力の読み取りスレッド → UI スレッドの受け渡しキュー (ロック付き)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalByteQueue.h"

#include <algorithm>

namespace mye {

TerminalByteQueue::TerminalByteQueue(size_t capacity)
    : m_capacity(capacity)
{
}

void TerminalByteQueue::Push(std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    // 1 回の Push は上限を少し超えてよい (待つのは「既に満杯」のときだけ)。読み取りの塊を分けずに済む
    m_spaceAvailable.wait(lock, [this] {
        return m_mode != Mode::Bounded || m_buffer.size() - m_head < m_capacity;
    });
    if (m_mode == Mode::Discard) {
        return;
    }
    m_buffer.append(bytes.data(), bytes.size());
}

size_t TerminalByteQueue::PopUpTo(std::string& out, size_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t taken = std::min(maxBytes, m_buffer.size() - m_head);
    out.append(m_buffer, m_head, taken);
    m_head += taken;
    if (m_head == m_buffer.size()) {
        m_buffer.clear();
        m_head = 0;
    } else if (m_head > m_buffer.size() / 2) {
        // 読み終えた前半を詰める。毎回は消さず、半分を超えたときだけにして移動量を償却 O(1) にする
        m_buffer.erase(0, m_head);
        m_head = 0;
    }
    if (taken > 0) {
        m_spaceAvailable.notify_all();
    }
    return taken;
}

void TerminalByteQueue::SetMode(Mode mode)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mode = mode;
        if (mode == Mode::Discard) {
            m_buffer.clear();
            m_head = 0;
        }
    }
    m_spaceAvailable.notify_all();
}

void TerminalByteQueue::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.clear();
    m_head = 0;
    m_mode = Mode::Bounded;
}

size_t TerminalByteQueue::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffer.size() - m_head;
}

} // namespace mye
