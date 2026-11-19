//====================================================================================
//                          TerminalInput.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルのキー入力・貼付を VT バイト列へ変換する
//====================================================================================
#include "Editor/Tools/Terminal/TerminalInput.h"

#include <vterm.h>

#include "Editor/Tools/Terminal/TerminalScreen.h"

namespace mye {

namespace {

VTermModifier ToVTermModifier(uint8_t modifiers)
{
    int mod = VTERM_MOD_NONE;
    if ((modifiers & kTerminalModShift) != 0) {
        mod |= VTERM_MOD_SHIFT;
    }
    if ((modifiers & kTerminalModAlt) != 0) {
        mod |= VTERM_MOD_ALT;
    }
    if ((modifiers & kTerminalModCtrl) != 0) {
        mod |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(mod);
}

VTermKey ToVTermKey(TerminalKey key)
{
    switch (key) {
    case TerminalKey::Enter:     return VTERM_KEY_ENTER;
    case TerminalKey::Backspace: return VTERM_KEY_BACKSPACE;
    case TerminalKey::Tab:       return VTERM_KEY_TAB;
    case TerminalKey::Escape:    return VTERM_KEY_ESCAPE;
    case TerminalKey::Up:        return VTERM_KEY_UP;
    case TerminalKey::Down:      return VTERM_KEY_DOWN;
    case TerminalKey::Left:      return VTERM_KEY_LEFT;
    case TerminalKey::Right:     return VTERM_KEY_RIGHT;
    case TerminalKey::Home:      return VTERM_KEY_HOME;
    case TerminalKey::End:       return VTERM_KEY_END;
    case TerminalKey::PageUp:    return VTERM_KEY_PAGEUP;
    case TerminalKey::PageDown:  return VTERM_KEY_PAGEDOWN;
    case TerminalKey::Insert:    return VTERM_KEY_INS;
    case TerminalKey::Delete:    return VTERM_KEY_DEL;
    default:
        break;
    }
    // F1..F12 は列挙の並びが連続している
    const int functionIndex = static_cast<int>(key) - static_cast<int>(TerminalKey::F1);
    return static_cast<VTermKey>(VTERM_KEY_FUNCTION(1 + functionIndex));
}

} // namespace

void TerminalSendKey(TerminalScreen& screen, TerminalKey key, uint8_t modifiers)
{
    vterm_keyboard_key(screen.RawVTerm(), ToVTermKey(key), ToVTermModifier(modifiers));
}

void TerminalSendChar(TerminalScreen& screen, char32_t codepoint, uint8_t modifiers)
{
    vterm_keyboard_unichar(screen.RawVTerm(), static_cast<uint32_t>(codepoint), ToVTermModifier(modifiers));
}

std::string NormalizeTerminalPasteNewlines(std::string_view utf8)
{
    // 端末の Enter は CR。Windows の貼付は CRLF、他所からの文字列は LF のことがあり、そのまま送ると
    // シェルが空行を 2 回受け取ったり LF を改行と見なさなかったりする (Windows Terminal も同じく CR へ揃える)
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (c == '\r') {
            out += '\r';
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') {
                ++i;
            }
        } else if (c == '\n') {
            out += '\r';
        } else {
            out += c;
        }
    }
    return out;
}

void TerminalSendPaste(TerminalScreen& screen, std::string_view utf8)
{
    // 本文は vterm_keyboard_unichar を通さず生のまま積む: 1 文字ずつ通すと Ctrl 修飾の解釈や CSI u の経路に
    // 乗りうる上、UTF-8 の分解・再符号化も無駄になる。libvterm の出力コールバックは同期なので、
    // start → 本文 → end の順に同じバッファへ並ぶ
    VTerm* vt = screen.RawVTerm();
    vterm_keyboard_start_paste(vt);
    screen.AppendOutput(NormalizeTerminalPasteNewlines(utf8));
    vterm_keyboard_end_paste(vt);
}

} // namespace mye
