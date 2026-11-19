//====================================================================================
//                          TerminalLayout.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル窓の描画・選択・寸法の計算 (ImGui にも Win32 にも依存しない純関数)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Editor/Tools/Terminal/TerminalScreen.h"

namespace mye {

// ---- 設定値の範囲 (spec 4.2)。クランプは窓 / 設定側の責務 (画面モデルは構築時の値を信じる) ----
inline constexpr int kTerminalDefaultFontSize = 14;
inline constexpr int kTerminalMinFontSize = 8;
inline constexpr int kTerminalMaxFontSize = 32;
inline constexpr int kTerminalDefaultScrollback = 5000;
inline constexpr int kTerminalMinScrollback = 100;
inline constexpr int kTerminalMaxScrollback = 100000;

int ClampTerminalFontSize(int pixels);
int ClampTerminalScrollback(int lines);

// ---- セル → 描画ラン ----

// 既定色のセルへ当てるテーマ色 (0xRRGGBB)。窓が ImGui のテーマから作って渡す
struct TerminalPalette
{
    uint32_t defaultFg = 0xD4D4D4;
    uint32_t defaultBg = 0x1E1E1E;
};

// 背景の矩形 1 枚分。同じ色が続くセルを 1 枚にまとめる (既定背景と同じ色の区間は作らない = 窓が一括で塗る)
struct TerminalBgRun
{
    int col = 0;
    int cells = 0;     // 覆う列数 (全角は 2 を数える)
    uint32_t rgb = 0;
};

// 文字列 1 本分。col から cells 列の幅に、同じ前景色・同じ属性の文字を UTF-8 で並べる
struct TerminalTextRun
{
    int col = 0;
    int cells = 0;
    std::string utf8;
    uint32_t rgb = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
};

struct TerminalRowRuns
{
    std::vector<TerminalBgRun> backgrounds;
    std::vector<TerminalTextRun> texts;
};

// 1 行分のセル (cols 個) を描画ランへ変換する。反転 (reverse) は前景と背景を入れ替えて解決済みの色で返す。
// 全角の右半分 (width 0) は左半分に含めて数え、単独では何も作らない。
// asciiRunsOnGrid: 描く書体の ASCII の送り幅がセル幅と等しい (等幅フォント) とき true。
//   true なら同じ見た目の ASCII を 1 本の文字列にまとめる (全角・結合文字は常に 1 セル 1 本: 日本語フォントの送り幅は
//   セル幅の整数倍と限らず、まとめると以降の桁がずれるため)。false (等幅でない代替フォント) なら全セルを 1 本ずつ作り、桁を保つ
void BuildTerminalRowRuns(const TerminalCell* cells, int cols, const TerminalPalette& palette, bool asciiRunsOnGrid,
                          TerminalRowRuns& out);

// ---- 選択 (行方向。端末の通常のテキスト選択と同じ「読む順」の範囲) ----

// 行は「絶対行」= 画面上の行番号 + そのときのスクロールバック行数 (新しい出力でスクロールバックが増えても同じ内容は同じ番号)。
// スクロールバックが上限で古い行を捨て始めると、選択は 1 行ずつ上へずれる (許容: 上限は 100 行以上)
struct TerminalSelection
{
    bool active = false;
    int anchorRow = 0;
    int anchorCol = 0;
    int headRow = 0;
    int headCol = 0;
};

// 読む順で整えた範囲 (両端を含む)
struct TerminalSelectionRange
{
    int startRow = 0;
    int startCol = 0;
    int endRow = 0;
    int endCol = 0;
};

TerminalSelectionRange NormalizeTerminalSelection(const TerminalSelection& selection);
// 選択のうち絶対行 row にかかる列の範囲 (両端を含む)。かからない行・選択が無いときは Empty()
struct TerminalColSpan
{
    int first = 0;
    int last = -1;
    bool Empty() const { return last < first; }
};
TerminalColSpan TerminalSelectionSpanOnRow(const TerminalSelection& selection, int row, int cols);
// 選択範囲を UTF-8 にする。各行の末尾の空白は落とし、行の間は LF。全角の右半分は飛ばす。範囲外の行は空行になる。
// 選択が空 (active でない) なら空文字列
std::string TerminalSelectionText(const TerminalScreen& screen, const TerminalSelection& selection);

// ---- ピクセル ↔ セル ----

struct TerminalGridSize
{
    int cols = kTerminalMinCols;
    int rows = kTerminalMinRows;
};

// 描画領域 (px) に入る列数・行数。端数は切り捨て、最小 kTerminalMinCols x kTerminalMinRows に切り上げる
TerminalGridSize TerminalGridFromPixels(float width, float height, float cellWidth, float cellHeight);

struct TerminalCellPos
{
    int col = 0;
    int row = 0;
};

// 描画領域の左上からの位置 (px) を 0..cols-1 / 0..rows-1 へ収めたセルにする。セル寸法が 0 以下なら (0, 0)
TerminalCellPos TerminalCellFromPixels(float x, float y, float cellWidth, float cellHeight, int cols, int rows);

// ---- スクロール ----

// 表示のずれ (0 = 最下部、正 = 過去へ何行戻っているか) を 0..scrollbackCount に収める
int ClampTerminalViewOffset(int offset, int scrollbackCount);
// ホイール 1 刻み (wheel の正 = 上へ = 過去へ) を linesPerNotch 行として offset に足し、収める。
// 代替画面 (altScreen) ではスクロールバックを見せないので常に 0
int ScrollTerminalView(int offset, float wheel, int linesPerNotch, int scrollbackCount, bool altScreen);

// ---- キーの振り分け ----

enum class TerminalChordAction
{
    None,          // 何もしない
    Copy,          // 選択をクリップボードへ
    Paste,         // クリップボードから貼る
    SendToShell,   // 修飾付きの文字として子へ送る
};

// Ctrl+文字 の扱い (letter は 'a'..'z')。Ctrl+C は選択があればコピー、無ければ割り込み (^C) を子へ。
// Ctrl+Shift+C は常にコピー (選択が無ければ何も起きない)、Ctrl+V と Ctrl+Shift+V は貼付、それ以外は子へ
TerminalChordAction ClassifyTerminalCtrlLetter(char letter, bool shift, bool hasSelection);

} // namespace mye
