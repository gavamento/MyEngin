//====================================================================================
//                          TerminalLayout.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル窓の描画・選択・寸法の計算 (ImGui にも Win32 にも依存しない純関数)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalLayout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mye {

int ClampTerminalFontSize(int pixels)
{
    return std::clamp(pixels, kTerminalMinFontSize, kTerminalMaxFontSize);
}

int ClampTerminalScrollback(int lines)
{
    return std::clamp(lines, kTerminalMinScrollback, kTerminalMaxScrollback);
}

namespace {

struct ResolvedColors
{
    uint32_t fg = 0;
    uint32_t bg = 0;
};

// 既定色の印をテーマ色へ差し替え、反転なら前景と背景を入れ替える
ResolvedColors ResolveColors(const TerminalCell& cell, const TerminalPalette& palette)
{
    ResolvedColors colors;
    colors.fg = cell.defaultFg ? palette.defaultFg : cell.fg;
    colors.bg = cell.defaultBg ? palette.defaultBg : cell.bg;
    if (cell.reverse) {
        std::swap(colors.fg, colors.bg);
    }
    return colors;
}

// 結合文字なしの印字可能な ASCII 1 文字のセルか (等幅フォントなら送り幅がセル幅と等しいので、並べて 1 本にまとめてよい)
bool IsPlainAscii(const TerminalCell& cell)
{
    return cell.width == 1 && cell.charCount == 1 && cell.chars[0] >= 0x20 && cell.chars[0] < 0x7F;
}

// 字を描かないセル (空白・制御文字)。下線や取り消し線だけは見えるので、呼び出し側が別に扱う
bool IsBlank(const TerminalCell& cell)
{
    return cell.charCount == 0 || cell.chars[0] < 0x20 || cell.chars[0] == 0x7F;
}

bool SameTextStyle(const TerminalTextRun& run, const TerminalCell& cell, uint32_t fg)
{
    return run.rgb == fg && run.bold == cell.bold && run.italic == cell.italic && run.underline == cell.underline &&
           run.strike == cell.strike;
}

// 組み立て中の文字列。閉じるとき、線の無い run は末尾の空白を落とす (描いても見えず、桁だけが余分に伸びる)
class RunBuilder
{
public:
    explicit RunBuilder(std::vector<TerminalTextRun>& out) : m_out(out) {}

    bool IsOpen() const { return m_open; }
    const TerminalTextRun& Current() const { return m_run; }

    void Open(int col, int cells, const TerminalCell& cell, uint32_t fg)
    {
        Close();
        m_open = true;
        m_run = TerminalTextRun{};
        m_run.col = col;
        m_run.cells = cells;
        m_run.rgb = fg;
        m_run.bold = cell.bold;
        m_run.italic = cell.italic;
        m_run.underline = cell.underline;
        m_run.strike = cell.strike;
        AppendCell(cell);
    }

    // 今の run の末尾へ cells 列ぶんのセルを足す (空白セルは ' ')
    void Extend(int cells, const TerminalCell& cell)
    {
        m_run.cells += cells;
        AppendCell(cell);
    }

    void Close()
    {
        if (!m_open) {
            return;
        }
        m_open = false;
        if (!m_run.underline && !m_run.strike) {
            while (!m_run.utf8.empty() && m_run.utf8.back() == ' ') {
                m_run.utf8.pop_back();
                --m_run.cells;
            }
        }
        if (!m_run.utf8.empty()) {
            m_out.push_back(std::move(m_run));
        }
    }

private:
    void AppendCell(const TerminalCell& cell)
    {
        if (IsBlank(cell)) {
            m_run.utf8 += ' ';
            return;
        }
        for (int i = 0; i < cell.charCount; ++i) {
            AppendTerminalUtf8(m_run.utf8, cell.chars[i]);
        }
    }

    std::vector<TerminalTextRun>& m_out;
    TerminalTextRun m_run;
    bool m_open = false;
};

} // namespace

void BuildTerminalRowRuns(const TerminalCell* cells, int cols, const TerminalPalette& palette, bool asciiRunsOnGrid,
                          TerminalRowRuns& out)
{
    out.backgrounds.clear();
    out.texts.clear();
    RunBuilder builder(out.texts);
    bool openIsAscii = false;   // 今の run は ASCII を並べているか (全角・結合文字の run へは足さない)

    int col = 0;
    while (col < cols) {
        const TerminalCell& cell = cells[col];
        if (cell.width <= 0) {
            ++col;   // 全角の右半分は左半分が覆う。孤立していても何も描かない
            continue;
        }
        const int span = std::min(cell.width, cols - col);
        const ResolvedColors colors = ResolveColors(cell, palette);

        if (colors.bg != palette.defaultBg) {
            if (!out.backgrounds.empty() && out.backgrounds.back().rgb == colors.bg &&
                out.backgrounds.back().col + out.backgrounds.back().cells == col) {
                out.backgrounds.back().cells += span;
            } else {
                out.backgrounds.push_back(TerminalBgRun{ col, span, colors.bg });
            }
        }

        const bool blank = IsBlank(cell);
        const bool decorated = cell.underline || cell.strike;
        if (blank && !decorated) {
            // 空白が既存の run と同じ見た目で続くなら run を伸ばす (間の空白を含む 1 本にして呼び出し数を減らす)。
            // そうでなければ run を閉じる
            if (asciiRunsOnGrid && builder.IsOpen() && openIsAscii && SameTextStyle(builder.Current(), cell, colors.fg)) {
                builder.Extend(span, cell);
            } else {
                builder.Close();
            }
            col += span;
            continue;
        }

        const bool plain = IsPlainAscii(cell) || (blank && span == 1);
        if (asciiRunsOnGrid && plain && builder.IsOpen() && openIsAscii && SameTextStyle(builder.Current(), cell, colors.fg)) {
            builder.Extend(span, cell);
        } else {
            builder.Open(col, span, cell, colors.fg);
            openIsAscii = asciiRunsOnGrid && plain;
            if (!openIsAscii) {
                builder.Close();   // 1 セル 1 本。以降のセルを足さない
            }
        }
        col += span;
    }
    builder.Close();
}

TerminalSelectionRange NormalizeTerminalSelection(const TerminalSelection& selection)
{
    const bool anchorFirst = selection.anchorRow < selection.headRow ||
                             (selection.anchorRow == selection.headRow && selection.anchorCol <= selection.headCol);
    TerminalSelectionRange range;
    if (anchorFirst) {
        range = { selection.anchorRow, selection.anchorCol, selection.headRow, selection.headCol };
    } else {
        range = { selection.headRow, selection.headCol, selection.anchorRow, selection.anchorCol };
    }
    return range;
}

TerminalColSpan TerminalSelectionSpanOnRow(const TerminalSelection& selection, int row, int cols)
{
    TerminalColSpan span;
    if (!selection.active || cols <= 0) {
        return span;
    }
    const TerminalSelectionRange r = NormalizeTerminalSelection(selection);
    if (row < r.startRow || row > r.endRow) {
        return span;
    }
    span.first = row == r.startRow ? r.startCol : 0;
    span.last = row == r.endRow ? r.endCol : cols - 1;
    span.first = std::max(span.first, 0);
    span.last = std::min(span.last, cols - 1);
    return span;
}

std::string TerminalSelectionText(const TerminalScreen& screen, const TerminalSelection& selection)
{
    if (!selection.active) {
        return {};
    }
    const TerminalSelectionRange r = NormalizeTerminalSelection(selection);
    const int scrollback = screen.ScrollbackCount();
    const int lastCol = screen.Cols() - 1;
    std::string text;
    for (int row = r.startRow; row <= r.endRow; ++row) {
        const int firstCol = row == r.startRow ? r.startCol : 0;
        const int endCol = row == r.endRow ? r.endCol : lastCol;
        std::string line = TerminalLineText(screen, row - scrollback, firstCol, endCol);
        // シェルが明示的に書いた空白 (プロンプトの後ろの余白など) も、コピーでは行末から落とす (Windows Terminal と同じ)
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        text += line;
        if (row != r.endRow) {
            text += '\n';
        }
    }
    return text;
}

TerminalGridSize TerminalGridFromPixels(float width, float height, float cellWidth, float cellHeight)
{
    TerminalGridSize grid;
    if (cellWidth > 0.0f && cellHeight > 0.0f) {
        grid.cols = std::max(static_cast<int>(std::floor(width / cellWidth)), kTerminalMinCols);
        grid.rows = std::max(static_cast<int>(std::floor(height / cellHeight)), kTerminalMinRows);
    }
    return grid;
}

TerminalCellPos TerminalCellFromPixels(float x, float y, float cellWidth, float cellHeight, int cols, int rows)
{
    TerminalCellPos pos;
    if (cellWidth <= 0.0f || cellHeight <= 0.0f || cols <= 0 || rows <= 0) {
        return pos;
    }
    pos.col = std::clamp(static_cast<int>(std::floor(x / cellWidth)), 0, cols - 1);
    pos.row = std::clamp(static_cast<int>(std::floor(y / cellHeight)), 0, rows - 1);
    return pos;
}

int ClampTerminalViewOffset(int offset, int scrollbackCount)
{
    return std::clamp(offset, 0, std::max(scrollbackCount, 0));
}

int ScrollTerminalView(int offset, float wheel, int linesPerNotch, int scrollbackCount, bool altScreen)
{
    if (altScreen) {
        return 0;
    }
    int delta = static_cast<int>(std::lround(wheel * static_cast<float>(linesPerNotch)));
    if (delta == 0 && wheel != 0.0f) {
        delta = wheel > 0.0f ? 1 : -1;   // タッチパッドの細かい値でも 1 行は動く
    }
    return ClampTerminalViewOffset(offset + delta, scrollbackCount);
}

TerminalChordAction ClassifyTerminalCtrlLetter(char letter, bool shift, bool hasSelection)
{
    if (letter == 'c') {
        if (shift) {
            return hasSelection ? TerminalChordAction::Copy : TerminalChordAction::None;
        }
        return hasSelection ? TerminalChordAction::Copy : TerminalChordAction::SendToShell;
    }
    if (letter == 'v') {
        return TerminalChordAction::Paste;
    }
    return TerminalChordAction::SendToShell;
}

} // namespace mye
