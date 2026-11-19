//====================================================================================
//                          TerminalScreen.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの画面モデル (libvterm の所有・セル読み出し・スクロールバック)
//====================================================================================
#include "Editor/Tools/Terminal/TerminalScreen.h"

#include <algorithm>
#include <deque>
#include <vector>

#include <vterm.h>

static_assert(mye::kTerminalMaxCharsPerCell == VTERM_MAX_CHARS_PER_CELL, "TerminalCell の文字数上限が libvterm と食い違う");

namespace mye {

struct TerminalScreen::Impl
{
    VTerm* vt = nullptr;
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    // スクロールバックは libvterm の生セルのまま持つ。sb_popline (行数を増やすリサイズ) で
    // 既定色の印などを失わずに画面へ戻すため。front が最も古い行
    std::deque<std::vector<VTermScreenCell>> scrollback;
    int scrollbackLimit = 0;
    std::string output;
    bool cursorVisible = true;
    bool altScreen = false;
};

namespace {

void OnOutput(const char* bytes, size_t len, void* user)
{
    static_cast<TerminalScreen::Impl*>(user)->output.append(bytes, len);
}

int OnSetTermProp(VTermProp prop, VTermValue* val, void* user)
{
    auto* impl = static_cast<TerminalScreen::Impl*>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        impl->cursorVisible = val->boolean != 0;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        impl->altScreen = val->boolean != 0;
    }
    return 1;
}

int OnPushLine(int cols, const VTermScreenCell* cells, void* user)
{
    auto* impl = static_cast<TerminalScreen::Impl*>(user);
    if (impl->scrollbackLimit <= 0) {
        return 0;
    }
    impl->scrollback.emplace_back(cells, cells + cols);
    while (static_cast<int>(impl->scrollback.size()) > impl->scrollbackLimit) {
        impl->scrollback.pop_front();
    }
    return 1;
}

// 行数を増やすリサイズで libvterm が画面上端へ戻す行を求める。列数が違えば切るか空白で埋める
int OnPopLine(int cols, VTermScreenCell* cells, void* user)
{
    auto* impl = static_cast<TerminalScreen::Impl*>(user);
    if (impl->scrollback.empty()) {
        return 0;
    }
    const std::vector<VTermScreenCell>& line = impl->scrollback.back();
    const int copyCount = std::min(cols, static_cast<int>(line.size()));
    std::copy(line.begin(), line.begin() + copyCount, cells);
    for (int col = copyCount; col < cols; ++col) {
        VTermScreenCell blank = {};
        blank.width = 1;
        // 空白の色は既定色にする (直前セルの色を引き継ぐと背景色の帯が出る)
        vterm_state_get_default_colors(impl->state, &blank.fg, &blank.bg);
        cells[col] = blank;
    }
    impl->scrollback.pop_back();
    return 1;
}

// ED 3 (CSI 3 J: スクロールバック消去)
int OnClearScrollback(void* user)
{
    static_cast<TerminalScreen::Impl*>(user)->scrollback.clear();
    return 1;
}

const VTermScreenCallbacks kScreenCallbacks = {
    nullptr,             // damage: 毎フレーム全セルを読むので差分は使わない
    nullptr,             // moverect
    nullptr,             // movecursor: 位置は vterm_state_get_cursorpos で読む
    &OnSetTermProp,
    nullptr,             // bell
    nullptr,             // resize
    &OnPushLine,
    &OnPopLine,
    &OnClearScrollback,
};

uint32_t ToRgb(const VTermScreen* screen, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(screen, &color);
    return (static_cast<uint32_t>(color.rgb.red) << 16) | (static_cast<uint32_t>(color.rgb.green) << 8) | color.rgb.blue;
}

TerminalCell ConvertCell(const VTermScreen* screen, const VTermScreenCell& src)
{
    TerminalCell cell;
    // libvterm は全角の右半分のセルを chars[0] = (uint32_t)-1 で表す (screen.c の putglyph)
    if (src.chars[0] == static_cast<uint32_t>(-1)) {
        cell.width = 0;
        cell.defaultFg = VTERM_COLOR_IS_DEFAULT_FG(&src.fg);
        cell.defaultBg = VTERM_COLOR_IS_DEFAULT_BG(&src.bg);
        cell.fg = ToRgb(screen, src.fg);
        cell.bg = ToRgb(screen, src.bg);
        return cell;
    }
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && src.chars[i] != 0; ++i) {
        cell.chars[i] = static_cast<char32_t>(src.chars[i]);
        cell.charCount = i + 1;
    }
    cell.width = src.width;
    // 印は RGB 変換で消えるので先に読む
    cell.defaultFg = VTERM_COLOR_IS_DEFAULT_FG(&src.fg);
    cell.defaultBg = VTERM_COLOR_IS_DEFAULT_BG(&src.bg);
    cell.fg = ToRgb(screen, src.fg);
    cell.bg = ToRgb(screen, src.bg);
    cell.bold = src.attrs.bold != 0;
    cell.italic = src.attrs.italic != 0;
    cell.underline = src.attrs.underline != VTERM_UNDERLINE_OFF;
    cell.reverse = src.attrs.reverse != 0;
    cell.strike = src.attrs.strike != 0;
    return cell;
}

void AppendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

TerminalScreen::TerminalScreen(int cols, int rows, int scrollbackLimit)
    : m_impl(std::make_unique<Impl>())
{
    cols = std::max(cols, kTerminalMinCols);
    rows = std::max(rows, kTerminalMinRows);
    m_impl->scrollbackLimit = std::max(scrollbackLimit, 0);
    m_impl->vt = vterm_new(rows, cols);
    vterm_set_utf8(m_impl->vt, 1);
    vterm_output_set_callback(m_impl->vt, &OnOutput, m_impl.get());
    m_impl->state = vterm_obtain_state(m_impl->vt);
    m_impl->screen = vterm_obtain_screen(m_impl->vt);
    vterm_screen_set_callbacks(m_impl->screen, &kScreenCallbacks, m_impl.get());
    // 代替画面 (DECSET 1049) を有効にする。無効だと全画面 CLI の終了後に元の画面へ戻らない
    vterm_screen_enable_altscreen(m_impl->screen, 1);
    vterm_screen_reset(m_impl->screen, 1);
}

TerminalScreen::~TerminalScreen()
{
    vterm_free(m_impl->vt);
}

void TerminalScreen::Feed(std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }
    vterm_input_write(m_impl->vt, bytes.data(), bytes.size());
}

bool TerminalScreen::Resize(int cols, int rows)
{
    cols = std::max(cols, kTerminalMinCols);
    rows = std::max(rows, kTerminalMinRows);
    if (cols == Cols() && rows == Rows()) {
        return false;
    }
    vterm_set_size(m_impl->vt, rows, cols);
    return true;
}

int TerminalScreen::Cols() const
{
    int rows = 0;
    int cols = 0;
    vterm_get_size(m_impl->vt, &rows, &cols);
    return cols;
}

int TerminalScreen::Rows() const
{
    int rows = 0;
    int cols = 0;
    vterm_get_size(m_impl->vt, &rows, &cols);
    return rows;
}

TerminalCell TerminalScreen::GetCell(int line, int col) const
{
    if (col < 0 || col >= Cols()) {
        return TerminalCell{};
    }
    if (line >= 0) {
        if (line >= Rows()) {
            return TerminalCell{};
        }
        VTermScreenCell raw = {};
        vterm_screen_get_cell(m_impl->screen, VTermPos{ line, col }, &raw);
        return ConvertCell(m_impl->screen, raw);
    }
    const int index = static_cast<int>(m_impl->scrollback.size()) + line;
    if (index < 0) {
        return TerminalCell{};
    }
    const std::vector<VTermScreenCell>& stored = m_impl->scrollback[static_cast<size_t>(index)];
    if (col >= static_cast<int>(stored.size())) {
        return TerminalCell{};
    }
    return ConvertCell(m_impl->screen, stored[static_cast<size_t>(col)]);
}

int TerminalScreen::ScrollbackCount() const
{
    return static_cast<int>(m_impl->scrollback.size());
}

int TerminalScreen::ScrollbackLimit() const
{
    return m_impl->scrollbackLimit;
}

TerminalCursor TerminalScreen::GetCursor() const
{
    VTermPos pos = {};
    vterm_state_get_cursorpos(m_impl->state, &pos);
    TerminalCursor cursor;
    cursor.row = pos.row;
    cursor.col = pos.col;
    cursor.visible = m_impl->cursorVisible;
    return cursor;
}

bool TerminalScreen::IsAltScreen() const
{
    return m_impl->altScreen;
}

bool TerminalScreen::IsBracketedPaste() const
{
    // libvterm 0.3.3 は mode.bracketpaste を公開していない。vterm_keyboard_start_paste はその印が立つときだけ
    // CSI 200~ を出し、他の状態を変えないので、出力先を一時的に差し替えて呼び、出たかどうかで判定する。
    // 却下: state の内部構造体を直接読む (同梱版の非公開ヘッダに依存する)、自前で DECSET 2004 を解析する (二重管理)
    bool emitted = false;
    vterm_output_set_callback(m_impl->vt, [](const char*, size_t len, void* user) {
        if (len > 0) {
            *static_cast<bool*>(user) = true;
        }
    }, &emitted);
    vterm_keyboard_start_paste(m_impl->vt);
    vterm_output_set_callback(m_impl->vt, &OnOutput, m_impl.get());
    return emitted;
}

std::string TerminalScreen::TakeOutput()
{
    std::string taken;
    taken.swap(m_impl->output);
    return taken;
}

void TerminalScreen::AppendOutput(std::string_view bytes)
{
    m_impl->output.append(bytes.data(), bytes.size());
}

VTerm* TerminalScreen::RawVTerm() const
{
    return m_impl->vt;
}

std::string TerminalLineText(const TerminalScreen& screen, int line)
{
    std::string text;
    size_t keep = 0;
    for (int col = 0; col < screen.Cols(); ++col) {
        const TerminalCell cell = screen.GetCell(line, col);
        if (cell.width == 0) {
            continue;
        }
        if (cell.charCount == 0) {
            text += ' ';
            continue;
        }
        for (int i = 0; i < cell.charCount; ++i) {
            AppendUtf8(text, cell.chars[i]);
        }
        keep = text.size();
    }
    text.resize(keep);
    return text;
}

} // namespace mye
