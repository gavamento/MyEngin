//====================================================================================
//                          TerminalInput.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルのキー入力・貼付を VT バイト列へ変換する
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace mye {

class TerminalScreen;

// 窓側 (ImGui) のキーをこの列挙へ写して渡す。文字キーは TerminalSendChar で送る
enum class TerminalKey : uint8_t
{
    Enter,
    Backspace,
    Tab,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

// 修飾キーのビット和
enum TerminalModifier : uint8_t
{
    kTerminalModNone = 0,
    kTerminalModShift = 1 << 0,
    kTerminalModAlt = 1 << 1,
    kTerminalModCtrl = 1 << 2,
};

// 特殊キーを送る。結果のバイトは screen の出力 (TakeOutput) に積まれる。
// カーソルキーの CSI / SS3 の切り替え (DECCKM) など端末状態による違いは libvterm の keyboard API に任せる
void TerminalSendKey(TerminalScreen& screen, TerminalKey key, uint8_t modifiers);
// 文字を 1 つ送る (Ctrl+文字 は制御文字、Alt+文字 は ESC 前置になる)
void TerminalSendChar(TerminalScreen& screen, char32_t codepoint, uint8_t modifiers);
// 貼付文字列 (UTF-8) を送る。本文は SanitizeTerminalPaste を通し、bracketed paste 有効時は CSI 200~ / 201~ で包む
void TerminalSendPaste(TerminalScreen& screen, std::string_view utf8);

// 貼付本文の消毒 (Win32 非依存の純関数。bracketed paste の有効・無効によらず TerminalSendPaste が通す):
//  - 改行は CR に揃える (CRLF と単独 LF → CR、単独 CR はそのまま)
//  - HT (0x09) と CR 以外の C0 制御文字 (ESC を含む)、DEL、UTF-8 の C1 制御文字 (U+0080-U+009F) は取り除く
//  - 日本語・絵文字などそれ以外の文字は残す。不正な UTF-8 / 途中で切れた列のバイトは (0x80 以上なので) そのまま通すが、
//    取り除いた結果として 0xC2 + 0x80-0x9F が隣り合う場合は C1 として取り除く
std::string SanitizeTerminalPaste(std::string_view utf8);

} // namespace mye
