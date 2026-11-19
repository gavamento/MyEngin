//====================================================================================
//                          TerminalScreen.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの画面モデル (libvterm の所有・セル読み出し・スクロールバック)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct VTerm;

namespace mye {

// 1 セルに入る結合文字を含むコードポイントの上限 (libvterm の VTERM_MAX_CHARS_PER_CELL と同じ値)
inline constexpr int kTerminalMaxCharsPerCell = 6;
// 窓をどれだけ小さくしても保つ最小の列数・行数 (spec 4.1)
inline constexpr int kTerminalMinCols = 20;
inline constexpr int kTerminalMinRows = 5;

// 描画側へ渡すセル。色は RGB に解決済み (0xRRGGBB)。既定色かどうかは別の印で持ち、描画側のテーマ色に差し替えられるようにする
struct TerminalCell
{
    char32_t chars[kTerminalMaxCharsPerCell] = {};
    int charCount = 0;     // 0 = 空白セル
    int width = 1;         // 全角は 2。全角の右半分のセルは 0 (描画しない)
    uint32_t fg = 0;
    uint32_t bg = 0;
    bool defaultFg = true;
    bool defaultBg = true;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool reverse = false;
    bool strike = false;
};

struct TerminalCursor
{
    int row = 0;
    int col = 0;
    bool visible = true;
};

// libvterm の VTerm / VTermScreen を所有する画面モデル。UI スレッドだけが触る (スレッド安全ではない)。
// 子プロセスの出力を Feed で投入し、セル・カーソル・モードを読み出す。キー入力や DA 応答など
// 「端末から子へ送るバイト」は内部のバッファに溜まり、TakeOutput で取り出す
class TerminalScreen
{
public:
    // scrollbackLimit: スクロールバックに保つ最大行数 (古い行から捨てる)。0 なら保たない
    TerminalScreen(int cols, int rows, int scrollbackLimit);
    ~TerminalScreen();
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;

    // 子の出力バイトを投入する。UTF-8 やエスケープ列が途中で切れていても、続きは次の Feed で繋がる
    void Feed(std::string_view bytes);
    // 列数・行数を変える (最小 kTerminalMinCols x kTerminalMinRows に切り上げる)。実際に変わったら true
    bool Resize(int cols, int rows);

    int Cols() const;
    int Rows() const;
    // line: 0..Rows()-1 が現在の画面、-1..-ScrollbackCount() がスクロールバック (-1 が最も新しい)。範囲外は空白セル
    TerminalCell GetCell(int line, int col) const;
    int ScrollbackCount() const;
    int ScrollbackLimit() const;
    TerminalCursor GetCursor() const;
    bool IsAltScreen() const;
    // DECSET 2004 (bracketed paste) が有効か
    bool IsBracketedPaste() const;

    // 子へ送るバイト (キー変換の結果、DA などの応答) を取り出して空にする
    std::string TakeOutput();
    // 子へ送るバイトを libvterm を通さずに積む (貼付の本文用。TerminalSendPaste が使う)
    void AppendOutput(std::string_view bytes);

    // TerminalInput が libvterm の keyboard API を呼ぶための口。所有権は渡さない
    VTerm* RawVTerm() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

// コードポイント 1 つを UTF-8 にして out の末尾へ足す
void AppendTerminalUtf8(std::string& out, char32_t codepoint);

// 1 行分のセルを UTF-8 文字列にする (選択コピー用・テスト用)。末尾の空白は落とす。全角の右半分は飛ばす
std::string TerminalLineText(const TerminalScreen& screen, int line);
// 同じく列 colBegin..colEnd (両端を含む。範囲外は切る) だけ。colBegin が全角の右半分なら左半分から含める
std::string TerminalLineText(const TerminalScreen& screen, int line, int colBegin, int colEnd);

} // namespace mye
