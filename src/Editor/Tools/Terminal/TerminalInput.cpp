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

std::string SanitizeTerminalPaste(std::string_view utf8)
{
    // 貼付本文はそのままシェルや CLI の入力になる。bracketed paste 中でも本文に ESC[201~ があれば貼付を抜けられ、
    // 無効時も ^C や ESC 列として解釈されるので、有効・無効によらず制御文字を除く (置換文字にはしない)
    constexpr unsigned char kTab = 0x09;
    constexpr unsigned char kDel = 0x7F;
    constexpr unsigned char kC1Lead = 0xC2;   // U+0080-U+009F は UTF-8 で 0xC2 0x80-0x9F
    constexpr unsigned char kC1TrailFirst = 0x80;
    constexpr unsigned char kC1TrailLast = 0x9F;

    std::string out;
    out.reserve(utf8.size());
    bool afterCr = false;   // 直前の入力バイトが CR のとき、続く LF を CRLF の一部として畳む
    for (const char ch : utf8) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool wasAfterCr = afterCr;
        afterCr = false;
        if (c == '\r') {
            out += '\r';
            afterCr = true;
        } else if (c == '\n') {
            // 端末の Enter は CR。Windows の貼付は CRLF、他所からの文字列は LF のことがあり、そのまま送ると
            // シェルが空行を 2 回受け取ったり LF を改行と見なさなかったりする (Windows Terminal も同じく CR へ揃える)
            if (!wasAfterCr) {
                out += '\r';
            }
        } else if (c == kTab || (c >= 0x20 && c != kDel)) {
            out += ch;
            // 0xC2 0x9B (8 ビット CSI) などを取り除く。出力側で判定するので、間にあった制御文字を除いた結果として
            // 0xC2 と 0x80-0x9F が隣り合う場合 ("\xC2\x1b\x9B"、"\xC2\xC2\x9B\x9B") も残さない
            if (c >= kC1TrailFirst && c <= kC1TrailLast && out.size() >= 2 &&
                static_cast<unsigned char>(out[out.size() - 2]) == kC1Lead) {
                out.resize(out.size() - 2);
            }
        }
        // それ以外 (HT 以外の C0、ESC を含む。DEL) は捨てる。不正な UTF-8 のバイトは 0x80 以上なので上の分岐でそのまま通る
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
    screen.AppendOutput(SanitizeTerminalPaste(utf8));
    vterm_keyboard_end_paste(vt);
}

} // namespace mye
