//====================================================================================
//                          TerminalLayoutSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナル窓の純関数 (セル→描画ラン・選択→文字列・ピクセル→行列・スクロール・キー振り分け) の回帰テスト
//====================================================================================
#include "Editor/Tools/Terminal/TerminalLayoutSelfTest.h"

#include <string>
#include <vector>

#include "Editor/Tools/Terminal/TerminalLayout.h"
#include "Editor/Tools/Terminal/TerminalScreen.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

constexpr TerminalPalette kPalette = { 0xD4D4D4, 0x1E1E1E };

// 画面の 1 行 (line) のセルを集めて描画ランにする
TerminalRowRuns RunsOfLine(const TerminalScreen& screen, int line, bool asciiRunsOnGrid = true)
{
    std::vector<TerminalCell> cells;
    for (int col = 0; col < screen.Cols(); ++col) {
        cells.push_back(screen.GetCell(line, col));
    }
    TerminalRowRuns runs;
    BuildTerminalRowRuns(cells.data(), static_cast<int>(cells.size()), kPalette, asciiRunsOnGrid, runs);
    return runs;
}

void TestClamp(const auto& check)
{
    check(ClampTerminalFontSize(0) == kTerminalMinFontSize && ClampTerminalFontSize(14) == 14 &&
              ClampTerminalFontSize(999) == kTerminalMaxFontSize && ClampTerminalFontSize(-5) == kTerminalMinFontSize,
          "clamp: font size");
    check(ClampTerminalScrollback(0) == kTerminalMinScrollback && ClampTerminalScrollback(5000) == 5000 &&
              ClampTerminalScrollback(100) == 100 && ClampTerminalScrollback(100000) == 100000 &&
              ClampTerminalScrollback(1000000) == kTerminalMaxScrollback && ClampTerminalScrollback(-1) == kTerminalMinScrollback,
          "clamp: scrollback 100..100000 including both edges");
    check(kTerminalDefaultFontSize == 14 && kTerminalDefaultScrollback == 5000, "clamp: defaults match spec 4.2");
}

void TestRuns(const auto& check)
{
    {
        TerminalScreen screen(80, 24, 0);
        TerminalRowRuns blank = RunsOfLine(screen, 0);
        check(blank.backgrounds.empty() && blank.texts.empty(), "runs: an empty line makes no runs");

        screen.Feed("hello world");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 1 && r.texts[0].utf8 == "hello world" && r.texts[0].col == 0 && r.texts[0].cells == 11 &&
                  r.texts[0].rgb == kPalette.defaultFg && r.backgrounds.empty(),
              "runs: same-style ASCII (with inner space) is one text run in the theme foreground, no background run");
        const TerminalRowRuns single = RunsOfLine(screen, 0, false);
        check(single.texts.size() == 10 && single.texts[0].utf8 == "h" && single.texts[9].col == 10,
              "runs: a non-monospace font gets one run per cell (inner space makes none)");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("ab   ");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 1 && r.texts[0].utf8 == "ab" && r.texts[0].cells == 2, "runs: trailing blanks are trimmed");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("a\x1b[31mb\x1b[0mc");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 3 && r.texts[0].utf8 == "a" && r.texts[1].utf8 == "b" && r.texts[2].utf8 == "c" &&
                  r.texts[1].col == 1 && r.texts[1].rgb == screen.GetCell(0, 1).fg && r.texts[1].rgb != kPalette.defaultFg &&
                  r.texts[0].rgb == kPalette.defaultFg && r.texts[2].rgb == kPalette.defaultFg,
              "runs: a color change splits the run and keeps the right columns");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("\x1b[1mB\x1b[22m\x1b[4mU\x1b[24m\x1b[9mS\x1b[0m");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 3 && r.texts[0].bold && !r.texts[0].underline && r.texts[1].underline && !r.texts[1].bold &&
                  r.texts[2].strike,
              "runs: bold / underline / strike each split and are carried");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("\x1b[44mab\x1b[0m  \x1b[44mc\x1b[41md");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.backgrounds.size() == 3 && r.backgrounds[0].col == 0 && r.backgrounds[0].cells == 2 &&
                  r.backgrounds[1].col == 4 && r.backgrounds[1].cells == 1 && r.backgrounds[2].col == 5 &&
                  r.backgrounds[2].cells == 1 && r.backgrounds[0].rgb == r.backgrounds[1].rgb &&
                  r.backgrounds[0].rgb != r.backgrounds[2].rgb,
              "runs: equal adjacent backgrounds merge, the default-background gap and a different color split");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("\x1b[7mR\x1b[0m");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 1 && r.texts[0].rgb == kPalette.defaultBg && r.backgrounds.size() == 1 &&
                  r.backgrounds[0].rgb == kPalette.defaultFg,
              "runs: reverse swaps the theme colors (text in the background color on a foreground-colored rectangle)");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("日本a");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 3 && r.texts[0].utf8 == "日" && r.texts[0].col == 0 && r.texts[0].cells == 2 &&
                  r.texts[1].utf8 == "本" && r.texts[1].col == 2 && r.texts[1].cells == 2 && r.texts[2].utf8 == "a" &&
                  r.texts[2].col == 4,
              "runs: a wide character takes 2 cells, is its own run and the right half draws nothing");
        screen.Feed("\x1b[44m日");
        const TerminalRowRuns wide = RunsOfLine(screen, 0);
        check(wide.backgrounds.size() == 1 && wide.backgrounds[0].col == 5 && wide.backgrounds[0].cells == 2,
              "runs: a wide character's background covers both cells once");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("ae\xcc\x81z");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 3 && r.texts[1].utf8 == "e\xcc\x81" && r.texts[1].cells == 1 && r.texts[2].col == 2,
              "runs: a combining sequence stays in one cell and does not merge into the ASCII run");
    }
    {
        TerminalScreen screen(80, 24, 0);
        screen.Feed("\x1b[4m  \x1b[0m");
        const TerminalRowRuns r = RunsOfLine(screen, 0);
        check(r.texts.size() == 1 && r.texts[0].underline && r.texts[0].cells == 2 && r.texts[0].utf8 == "  ",
              "runs: underlined blanks keep a run (the line is visible)");
    }
}

void TestSelection(const auto& check)
{
    TerminalScreen screen(20, 5, 100);
    screen.Feed("line one\r\nline two   \r\n日本語abc");
    TerminalSelection sel;
    check(TerminalSelectionText(screen, sel).empty(), "selection: inactive selection has no text");

    sel = { true, 0, 5, 1, 3 };
    check(TerminalSelectionText(screen, sel) == "one\nline", "selection: spans lines in reading order, inclusive of both ends");
    sel = { true, 1, 3, 0, 5 };
    check(TerminalSelectionText(screen, sel) == "one\nline", "selection: dragging backwards gives the same text");
    sel = { true, 1, 0, 1, 19 };
    check(TerminalSelectionText(screen, sel) == "line two", "selection: trailing blanks of a line are dropped");
    sel = { true, 0, 2, 0, 2 };
    check(TerminalSelectionText(screen, sel) == "n", "selection: a single cell");
    sel = { true, 2, 1, 2, 3 };
    check(TerminalSelectionText(screen, sel) == "日本", "selection: starting on the right half of a wide character includes the whole character");
    sel = { true, 0, 0, 3, 19 };
    check(TerminalSelectionText(screen, sel) == "line one\nline two\n日本語abc\n", "selection: blank rows give empty lines, middle rows are whole");

    sel = { true, 1, 4, 2, 2 };
    TerminalColSpan span = TerminalSelectionSpanOnRow(sel, 0, 20);
    check(span.Empty(), "span: rows before the selection are empty");
    span = TerminalSelectionSpanOnRow(sel, 1, 20);
    check(!span.Empty() && span.first == 4 && span.last == 19, "span: the first row runs from the start column to the right edge");
    span = TerminalSelectionSpanOnRow(sel, 2, 20);
    check(!span.Empty() && span.first == 0 && span.last == 2, "span: the last row runs from the left edge to the end column");
    check(TerminalSelectionSpanOnRow(sel, 3, 20).Empty(), "span: rows after the selection are empty");
    TerminalSelection middle = { true, 0, 7, 2, 1 };
    span = TerminalSelectionSpanOnRow(middle, 1, 20);
    check(span.first == 0 && span.last == 19, "span: a middle row is whole");
    span = TerminalSelectionSpanOnRow({ true, 4, 3, 4, 8 }, 4, 20);
    check(span.first == 3 && span.last == 8, "span: a single-row selection");
    span = TerminalSelectionSpanOnRow({ true, 4, 3, 4, 99 }, 4, 20);
    check(span.last == 19, "span: an end column past the width is clipped (the screen got narrower)");
    sel.active = false;
    check(TerminalSelectionSpanOnRow(sel, 1, 20).Empty(), "span: inactive selection has none");

    // スクロールバックの行を含む選択 (絶対行 = 画面の行 + スクロールバックの行数)
    TerminalScreen scrolled(20, 5, 100);
    for (int i = 0; i < 10; ++i) {
        scrolled.Feed("L" + std::to_string(i) + "\r\n");
    }
    const int sb = scrolled.ScrollbackCount();
    check(sb >= 6, "selection: the screen pushed lines to the scrollback");
    sel = { true, 0, 0, 1, 19 };
    check(TerminalSelectionText(scrolled, sel) == "L0\nL1", "selection: absolute row 0 is the oldest scrollback line");
    sel = { true, sb - 1, 0, sb, 19 };
    check(TerminalSelectionText(scrolled, sel) == std::string("L") + std::to_string(sb - 1) + "\nL" + std::to_string(sb),
          "selection: crosses the scrollback / screen boundary");
    scrolled.Feed("\x1b[3J");
    check(scrolled.ScrollbackCount() == 0 && TerminalLineText(scrolled, 0) != "", "selection: ED 3 empties the scrollback but not the visible screen");
    sel = { true, 500, 0, 501, 3 };
    check(TerminalSelectionText(scrolled, sel) == "\n", "selection: rows beyond the screen are empty lines, not a crash");
}

void TestGeometry(const auto& check)
{
    TerminalGridSize grid = TerminalGridFromPixels(800.0f, 400.0f, 8.0f, 16.0f);
    check(grid.cols == 100 && grid.rows == 25, "grid: pixels divide into whole cells");
    grid = TerminalGridFromPixels(807.0f, 415.9f, 8.0f, 16.0f);
    check(grid.cols == 100 && grid.rows == 25, "grid: the remainder is dropped");
    grid = TerminalGridFromPixels(10.0f, 10.0f, 8.0f, 16.0f);
    check(grid.cols == kTerminalMinCols && grid.rows == kTerminalMinRows, "grid: tiny areas round up to the minimum size");
    grid = TerminalGridFromPixels(800.0f, 400.0f, 0.0f, 0.0f);
    check(grid.cols == kTerminalMinCols && grid.rows == kTerminalMinRows, "grid: zero cell size does not divide by zero");
    grid = TerminalGridFromPixels(-50.0f, -50.0f, 8.0f, 16.0f);
    check(grid.cols == kTerminalMinCols && grid.rows == kTerminalMinRows, "grid: negative area gives the minimum");

    TerminalCellPos pos = TerminalCellFromPixels(17.0f, 33.0f, 8.0f, 16.0f, 100, 25);
    check(pos.col == 2 && pos.row == 2, "cell: pixel to cell");
    pos = TerminalCellFromPixels(-5.0f, -5.0f, 8.0f, 16.0f, 100, 25);
    check(pos.col == 0 && pos.row == 0, "cell: before the origin clamps to 0 (dragging out of the panel)");
    pos = TerminalCellFromPixels(99999.0f, 99999.0f, 8.0f, 16.0f, 100, 25);
    check(pos.col == 99 && pos.row == 24, "cell: past the end clamps to the last cell");
    pos = TerminalCellFromPixels(7.9f, 15.9f, 8.0f, 16.0f, 100, 25);
    check(pos.col == 0 && pos.row == 0, "cell: just inside the first cell");
    pos = TerminalCellFromPixels(8.0f, 16.0f, 8.0f, 16.0f, 100, 25);
    check(pos.col == 1 && pos.row == 1, "cell: exactly on a boundary belongs to the next cell");
    pos = TerminalCellFromPixels(10.0f, 10.0f, 0.0f, 16.0f, 100, 25);
    check(pos.col == 0 && pos.row == 0, "cell: zero cell width gives (0, 0)");
}

void TestScroll(const auto& check)
{
    check(ScrollTerminalView(0, 1.0f, 3, 100, false) == 3, "scroll: one notch up goes 3 lines into the past");
    check(ScrollTerminalView(5, -1.0f, 3, 100, false) == 2, "scroll: one notch down comes back");
    check(ScrollTerminalView(1, -1.0f, 3, 100, false) == 0, "scroll: cannot go below the bottom");
    check(ScrollTerminalView(98, 1.0f, 3, 100, false) == 100, "scroll: cannot pass the oldest line");
    check(ScrollTerminalView(0, 1.0f, 3, 0, false) == 0, "scroll: no scrollback means no scrolling");
    check(ScrollTerminalView(10, 1.0f, 3, 100, true) == 0, "scroll: the alternate screen never shows scrollback");
    check(ScrollTerminalView(0, 0.1f, 3, 100, false) == 1 && ScrollTerminalView(5, -0.1f, 3, 100, false) == 4,
          "scroll: a tiny touchpad delta still moves one line");
    check(ScrollTerminalView(7, 0.0f, 3, 100, false) == 7, "scroll: zero wheel changes nothing");
    check(ClampTerminalViewOffset(50, 10) == 10 && ClampTerminalViewOffset(-3, 10) == 0 && ClampTerminalViewOffset(5, -1) == 0,
          "scroll: the view offset is clamped to the scrollback");
}

void TestChords(const auto& check)
{
    check(ClassifyTerminalCtrlLetter('c', false, true) == TerminalChordAction::Copy, "chord: Ctrl+C with a selection copies");
    check(ClassifyTerminalCtrlLetter('c', false, false) == TerminalChordAction::SendToShell, "chord: Ctrl+C without a selection interrupts the shell");
    check(ClassifyTerminalCtrlLetter('c', true, true) == TerminalChordAction::Copy, "chord: Ctrl+Shift+C with a selection copies");
    check(ClassifyTerminalCtrlLetter('c', true, false) == TerminalChordAction::None, "chord: Ctrl+Shift+C without a selection does nothing (never sends ^C)");
    check(ClassifyTerminalCtrlLetter('v', false, false) == TerminalChordAction::Paste && ClassifyTerminalCtrlLetter('v', true, true) == TerminalChordAction::Paste,
          "chord: Ctrl+V and Ctrl+Shift+V paste");
    check(ClassifyTerminalCtrlLetter('d', false, true) == TerminalChordAction::SendToShell && ClassifyTerminalCtrlLetter('z', true, false) == TerminalChordAction::SendToShell,
          "chord: other Ctrl+letters go to the shell, selection or not");
}

} // namespace

bool RunTerminalLayoutSelfTest()
{
    MYE_LOG_INFO("==== TerminalLayout self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    TestClamp(check);
    TestRuns(check);
    TestSelection(check);
    TestGeometry(check);
    TestScroll(check);
    TestChords(check);
    MYE_LOG_INFO("TerminalLayout self test: %s (%d failure(s))", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
