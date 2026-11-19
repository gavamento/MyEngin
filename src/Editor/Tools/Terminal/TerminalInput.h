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
// 貼付文字列 (UTF-8) を送る。改行は CR に揃え、bracketed paste 有効時は CSI 200~ / 201~ で包む
void TerminalSendPaste(TerminalScreen& screen, std::string_view utf8);

// 貼付の改行正規化: CRLF と単独 LF を CR にする (単独 CR はそのまま)
std::string NormalizeTerminalPasteNewlines(std::string_view utf8);

} // namespace mye
