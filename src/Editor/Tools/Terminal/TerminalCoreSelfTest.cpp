//====================================================================================
//                          TerminalCoreSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルコア (画面モデル・キー変換・シェル解決・環境組立) の回帰テスト
//====================================================================================
#include "Editor/Tools/Terminal/TerminalCoreSelfTest.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "Editor/Tools/Terminal/TerminalEnvironment.h"
#include "Editor/Tools/Terminal/TerminalInput.h"
#include "Editor/Tools/Terminal/TerminalScreen.h"
#include "Editor/Tools/Terminal/TerminalShellResolve.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// ---- 画面モデル ----

void TestScreenBasics(const auto& check)
{
    TerminalScreen screen(80, 24, 100);
    check(screen.Cols() == 80 && screen.Rows() == 24, "screen: initial size");
    screen.Feed("");
    check(TerminalLineText(screen, 0).empty() && screen.TakeOutput().empty(), "screen: empty feed changes nothing");
    const TerminalCursor home = screen.GetCursor();
    check(home.row == 0 && home.col == 0 && home.visible, "screen: cursor starts at origin, visible");

    screen.Feed("hello");
    check(TerminalLineText(screen, 0) == "hello", "screen: ascii text");
    check(screen.GetCursor().col == 5, "screen: cursor advances");
    check(screen.GetCell(0, 0).charCount == 1 && screen.GetCell(0, 0).chars[0] == U'h', "screen: cell codepoint");
    check(screen.GetCell(0, 10).charCount == 0, "screen: blank cell");
    check(screen.GetCell(99, 0).charCount == 0 && screen.GetCell(0, 999).charCount == 0 && screen.GetCell(-5, 0).charCount == 0,
          "screen: out-of-range cells are blank");

    screen.Feed("\r\n\xE3\x81\x82\xE6\x97\xA5x");   // あ日x
    const TerminalCell wide = screen.GetCell(1, 0);
    check(wide.chars[0] == U'\u3042' && wide.width == 2, "screen: fullwidth width 2");
    check(screen.GetCell(1, 1).width == 0 && screen.GetCell(1, 1).charCount == 0, "screen: fullwidth right half is width 0");
    check(screen.GetCell(1, 2).chars[0] == U'\u65E5' && screen.GetCell(1, 4).chars[0] == U'x', "screen: next glyph after 2 cells");
    check(TerminalLineText(screen, 1) == "\xE3\x81\x82\xE6\x97\xA5x", "screen: line text skips right half");
    check(screen.GetCursor().col == 5, "screen: cursor after fullwidth");

    screen.Feed("\x1b[?25l");
    check(!screen.GetCursor().visible, "screen: DECTCEM hide cursor");
    screen.Feed("\x1b[?25h\x1b[5;7H");
    check(screen.GetCursor().visible && screen.GetCursor().row == 4 && screen.GetCursor().col == 6, "screen: CUP and show cursor");

    screen.Feed("\x1b[c");
    const std::string da = screen.TakeOutput();
    check(da.rfind("\x1b[?", 0) == 0 && da.back() == 'c', "screen: DA1 response goes to output");
    check(screen.TakeOutput().empty(), "screen: TakeOutput empties buffer");
}

void TestScreenSplitSequences(const auto& check)
{
    TerminalScreen screen(80, 24, 0);
    // UTF-8 の 3 バイトを 1 バイトずつ
    screen.Feed("\xE3");
    screen.Feed("\x81");
    screen.Feed("\x82");
    check(screen.GetCell(0, 0).chars[0] == U'\u3042' && screen.GetCell(0, 0).width == 2, "split: utf-8 byte by byte");
    // 4 バイト (U+1F600) を 2+2
    screen.Feed("\xF0\x9F");
    screen.Feed("\x98\x80");
    check(screen.GetCell(0, 2).chars[0] == U'\U0001F600', "split: 4-byte utf-8 in two feeds");
    // CSI を途中で切る
    screen.Feed("\x1b");
    screen.Feed("[3");
    screen.Feed("1mR");
    const TerminalCell red = screen.GetCell(0, 4);
    check(red.chars[0] == U'R' && !red.defaultFg && red.fg == 0xE00000u, "split: CSI SGR across feeds");
    // OSC (タイトル) を途中で切っても本文に漏れない
    screen.Feed("\x1b]0;ti");
    screen.Feed("tle\x07Z");
    check(screen.GetCell(0, 5).chars[0] == U'Z' && TerminalLineText(screen, 0).find("title") == std::string::npos,
          "split: OSC across feeds is swallowed");
}

void TestScreenColors(const auto& check)
{
    TerminalScreen screen(80, 24, 0);
    screen.Feed("a\x1b[1;4;7mb\x1b[0mc");
    const TerminalCell plain = screen.GetCell(0, 0);
    const TerminalCell styled = screen.GetCell(0, 1);
    check(plain.defaultFg && plain.defaultBg && !plain.bold, "sgr: default colors");
    check(styled.bold && styled.underline && styled.reverse, "sgr: bold underline reverse");
    check(!screen.GetCell(0, 2).bold && screen.GetCell(0, 2).defaultFg, "sgr: reset");

    screen.Feed("\r\n\x1b[38;5;196mX\x1b[48;5;232mY\x1b[0m");
    check(screen.GetCell(1, 0).fg == 0xFF0000u && !screen.GetCell(1, 0).defaultFg, "sgr: 256-color cube");
    check(screen.GetCell(1, 1).bg == 0x000000u && !screen.GetCell(1, 1).defaultBg, "sgr: 256-color grey bg");
    screen.Feed("\r\n\x1b[38;2;1;2;3;48;2;10;20;30mT\x1b[3;9mS");
    check(screen.GetCell(2, 0).fg == 0x010203u && screen.GetCell(2, 0).bg == 0x0A141Eu, "sgr: truecolor fg/bg");
    check(screen.GetCell(2, 1).italic && screen.GetCell(2, 1).strike, "sgr: italic strike");
    screen.Feed("\x1b[94mB");
    check(!screen.GetCell(2, 2).defaultFg && screen.GetCell(2, 2).fg != screen.GetCell(2, 0).fg, "sgr: bright 16-color");
}

void TestScreenAltScreen(const auto& check)
{
    TerminalScreen screen(40, 10, 100);
    screen.Feed("main");
    check(!screen.IsAltScreen(), "alt: starts on main");
    screen.Feed("\x1b[?1049h");
    check(screen.IsAltScreen() && TerminalLineText(screen, 0).empty(), "alt: 1049h enters cleared alt screen");
    // 代替画面でのスクロールはスクロールバックへ積まない
    for (int i = 0; i < 30; ++i) {
        screen.Feed("alt line\r\n");
    }
    check(screen.ScrollbackCount() == 0, "alt: scrolling on alt screen does not push scrollback");
    screen.Feed("\x1b[?1049l");
    check(!screen.IsAltScreen() && TerminalLineText(screen, 0) == "main", "alt: 1049l restores main screen");
}

void TestScreenScrollback(const auto& check)
{
    TerminalScreen screen(20, 5, 3);
    for (int i = 0; i < 10; ++i) {
        screen.Feed("L" + std::to_string(i) + "\r\n");
    }
    // 10 行 + 改行で 6 行が画面外へ出る (L0..L5)。上限 3 なので L3..L5 が残る
    check(screen.ScrollbackCount() == 3 && screen.ScrollbackLimit() == 3, "scrollback: capped at limit");
    check(TerminalLineText(screen, -1) == "L5" && TerminalLineText(screen, -3) == "L3", "scrollback: newest is -1, oldest kept is L3");
    check(TerminalLineText(screen, -4).empty(), "scrollback: beyond count is blank");
    check(TerminalLineText(screen, 0) == "L6" && TerminalLineText(screen, 3) == "L9", "scrollback: visible rows");

    screen.Feed("\x1b[3J");
    check(screen.ScrollbackCount() == 0, "scrollback: ED 3 clears");

    TerminalScreen none(20, 5, 0);
    for (int i = 0; i < 10; ++i) {
        none.Feed("x\r\n");
    }
    check(none.ScrollbackCount() == 0, "scrollback: limit 0 keeps nothing");

    // 色付きの行がスクロールバックでも色を保つ
    TerminalScreen colored(20, 5, 10);
    colored.Feed("\x1b[38;2;9;8;7mC\x1b[0m\r\n\r\n\r\n\r\n\r\n");
    check(colored.ScrollbackCount() == 1 && colored.GetCell(-1, 0).fg == 0x090807u, "scrollback: keeps colors");
}

void TestScreenResize(const auto& check)
{
    TerminalScreen tiny(1, 1, 10);
    check(tiny.Cols() == kTerminalMinCols && tiny.Rows() == kTerminalMinRows, "resize: constructor clamps to minimum");
    check(!tiny.Resize(5, 2), "resize: below minimum is clamped (no change)");
    check(tiny.Cols() == kTerminalMinCols && tiny.Rows() == kTerminalMinRows, "resize: still minimum");

    TerminalScreen screen(20, 5, 100);
    for (int i = 0; i < 10; ++i) {
        screen.Feed("R" + std::to_string(i) + "\r\n");
    }
    const int before = screen.ScrollbackCount();
    check(screen.Resize(30, 8), "resize: grow returns true");
    check(screen.Cols() == 30 && screen.Rows() == 8, "resize: new size");
    // 行を増やすと libvterm が sb_popline で行を画面へ戻す。総行数は保たれる
    const int popped = before - screen.ScrollbackCount();
    check(popped > 0 && popped <= 3, "resize: grow pops lines back from scrollback");
    // 戻った行は画面の上端に、元の順で並ぶ (R0..R5 が出ていたので、最上段は R(6 - popped))
    check(TerminalLineText(screen, 0) == "R" + std::to_string(6 - popped) && TerminalLineText(screen, popped) == "R6",
          "resize: popped lines restored in order");
    check(!screen.Resize(30, 8), "resize: same size returns false");
    check(screen.Resize(20, 5), "resize: shrink returns true");
    check(screen.Cols() == 20 && screen.Rows() == 5, "resize: shrink size");
    screen.Feed("\x1b[5;20Hz");
    check(screen.GetCell(4, 19).chars[0] == U'z', "resize: writes at the new last cell");
}

void TestScreenBulk(const auto& check)
{
    TerminalScreen screen(80, 24, 5000);
    std::string chunk;
    for (int i = 0; i < 1000; ++i) {
        chunk += "\x1b[32m0123456789012345678901234567890123456789012345678901234567890123456789\x1b[0m\r\n";
    }
    size_t fed = 0;
    while (fed < 1024 * 1024) {
        screen.Feed(chunk);
        fed += chunk.size();
    }
    check(screen.ScrollbackCount() == 5000, "bulk: 1MB output fills scrollback to limit");
    check(TerminalLineText(screen, -1).rfind("0123456789", 0) == 0 && !screen.GetCell(-1, 0).defaultFg, "bulk: newest line intact");
    check(screen.GetCursor().row == 23 && screen.GetCursor().col == 0, "bulk: cursor at bottom");
}

// ---- キー → VT ----

std::string KeyBytes(TerminalScreen& screen, TerminalKey key, uint8_t mods = kTerminalModNone)
{
    TerminalSendKey(screen, key, mods);
    return screen.TakeOutput();
}

std::string CharBytes(TerminalScreen& screen, char32_t cp, uint8_t mods = kTerminalModNone)
{
    TerminalSendChar(screen, cp, mods);
    return screen.TakeOutput();
}

void TestKeys(const auto& check)
{
    TerminalScreen screen(80, 24, 0);
    check(KeyBytes(screen, TerminalKey::Enter) == "\r", "key: Enter");
    check(KeyBytes(screen, TerminalKey::Backspace) == "\x7f", "key: Backspace is DEL");
    check(KeyBytes(screen, TerminalKey::Tab) == "\t", "key: Tab");
    check(KeyBytes(screen, TerminalKey::Tab, kTerminalModShift) == "\x1b[Z", "key: Shift+Tab");
    check(KeyBytes(screen, TerminalKey::Escape) == "\x1b", "key: Escape");
    check(KeyBytes(screen, TerminalKey::Up) == "\x1b[A", "key: Up");
    check(KeyBytes(screen, TerminalKey::Down) == "\x1b[B", "key: Down");
    check(KeyBytes(screen, TerminalKey::Right) == "\x1b[C", "key: Right");
    check(KeyBytes(screen, TerminalKey::Left) == "\x1b[D", "key: Left");
    check(KeyBytes(screen, TerminalKey::Up, kTerminalModCtrl) == "\x1b[1;5A", "key: Ctrl+Up");
    check(KeyBytes(screen, TerminalKey::Home) == "\x1b[H", "key: Home");
    check(KeyBytes(screen, TerminalKey::End) == "\x1b[F", "key: End");
    check(KeyBytes(screen, TerminalKey::PageUp) == "\x1b[5~", "key: PageUp");
    check(KeyBytes(screen, TerminalKey::PageDown) == "\x1b[6~", "key: PageDown");
    check(KeyBytes(screen, TerminalKey::Insert) == "\x1b[2~", "key: Insert");
    check(KeyBytes(screen, TerminalKey::Delete) == "\x1b[3~", "key: Delete");

    const char* const kFunctionBytes[] = {
        "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS", "\x1b[15~", "\x1b[17~",
        "\x1b[18~", "\x1b[19~", "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~",
    };
    bool allFunction = true;
    for (int i = 0; i < 12; ++i) {
        const TerminalKey key = static_cast<TerminalKey>(static_cast<int>(TerminalKey::F1) + i);
        allFunction = allFunction && KeyBytes(screen, key) == kFunctionBytes[i];
    }
    check(allFunction, "key: F1-F12");

    // DECCKM (アプリケーションカーソルキー) では SS3
    screen.Feed("\x1b[?1h");
    check(KeyBytes(screen, TerminalKey::Up) == "\x1bOA" && KeyBytes(screen, TerminalKey::Home) == "\x1bOH", "key: DECCKM uses SS3");
    screen.Feed("\x1b[?1l");
    check(KeyBytes(screen, TerminalKey::Up) == "\x1b[A", "key: DECCKM reset");

    check(CharBytes(screen, U'a') == "a", "char: plain");
    check(CharBytes(screen, U'c', kTerminalModCtrl) == "\x03", "char: Ctrl+C");
    check(CharBytes(screen, U'a', kTerminalModCtrl) == "\x01" && CharBytes(screen, U'z', kTerminalModCtrl) == "\x1a", "char: Ctrl+A / Ctrl+Z");
    check(CharBytes(screen, U'x', kTerminalModAlt) == "\x1bx", "char: Alt+x");
    check(CharBytes(screen, U'\u3042') == "\xE3\x81\x82", "char: fullwidth to utf-8");
    check(CharBytes(screen, U'\U0001F600') == "\xF0\x9F\x98\x80", "char: astral to utf-8");
}

void TestPaste(const auto& check)
{
    check(NormalizeTerminalPasteNewlines("a\r\nb\nc\rd") == "a\rb\rc\rd", "paste: newline normalize");
    check(NormalizeTerminalPasteNewlines("").empty() && NormalizeTerminalPasteNewlines("\r\n\r\n") == "\r\r", "paste: empty / only newlines");

    TerminalScreen screen(80, 24, 0);
    check(!screen.IsBracketedPaste(), "paste: bracketed off by default");
    TerminalSendPaste(screen, "echo 1\r\necho 2\n");
    check(screen.TakeOutput() == "echo 1\recho 2\r", "paste: unwrapped when 2004 off");

    screen.Feed("\x1b[?2004h");
    check(screen.IsBracketedPaste(), "paste: 2004h enables");
    check(screen.TakeOutput().empty(), "paste: probing mode does not leak bytes");
    TerminalSendPaste(screen, "\xE3\x81\x82\n\x1b[x");
    check(screen.TakeOutput() == "\x1b[200~\xE3\x81\x82\r\x1b[x\x1b[201~", "paste: wrapped with 200~/201~");
    TerminalSendPaste(screen, "");
    check(screen.TakeOutput() == "\x1b[200~\x1b[201~", "paste: empty paste still wrapped");
    screen.Feed("\x1b[?2004l");
    check(!screen.IsBracketedPaste(), "paste: 2004l disables");
}

// ---- シェル解決 ----

TerminalExecutableFinder FakeFinder(std::map<std::string, std::string> table)
{
    return [table = std::move(table)](const std::string& name) {
        const auto it = table.find(name);
        return it == table.end() ? std::string() : it->second;
    };
}

void TestShellResolve(const auto& check)
{
    const std::map<std::string, std::string> all = {
        { "pwsh.exe", "C:\\PS7\\pwsh.exe" },
        { "powershell.exe", "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" },
        { "cmd.exe", "C:\\Windows\\System32\\cmd.exe" },
    };
    TerminalShellCommand c = ResolveTerminalShell("", FakeFinder(all));
    check(c.error == TerminalShellError::None && c.path == "C:\\PS7\\pwsh.exe" && c.args.empty(), "shell: auto prefers pwsh");
    c = ResolveTerminalShell("  \t ", FakeFinder(all));
    check(c.path == "C:\\PS7\\pwsh.exe", "shell: whitespace setting is auto");
    c = ResolveTerminalShell("", FakeFinder({ { "powershell.exe", "P" }, { "cmd.exe", "C" } }));
    check(c.path == "P", "shell: auto falls to powershell");
    c = ResolveTerminalShell("", FakeFinder({ { "cmd.exe", "C" } }));
    check(c.path == "C", "shell: auto falls to cmd");
    c = ResolveTerminalShell("", FakeFinder({}));
    check(c.error == TerminalShellError::NoAutoShell && c.path.empty(), "shell: no auto shell");

    c = ResolveTerminalShell("cmd.exe", FakeFinder(all));
    check(c.error == TerminalShellError::None && c.path == "C:\\Windows\\System32\\cmd.exe", "shell: explicit setting wins over auto");
    c = ResolveTerminalShell("bash.exe", FakeFinder(all));
    check(c.error == TerminalShellError::ConfiguredNotFound && c.path.empty(), "shell: explicit missing does not fall back");
    c = ResolveTerminalShell("pwsh.exe -NoLogo  -NoProfile", FakeFinder(all));
    check(c.path == "C:\\PS7\\pwsh.exe" && c.args == std::vector<std::string>{ "-NoLogo", "-NoProfile" }, "shell: unquoted with args");
    c = ResolveTerminalShell("\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -c \"echo a b\"",
                             FakeFinder({ { "C:\\Program Files\\Git\\bin\\bash.exe", "C:\\Program Files\\Git\\bin\\bash.exe" } }));
    check(c.error == TerminalShellError::None && c.path == "C:\\Program Files\\Git\\bin\\bash.exe"
              && c.args == std::vector<std::string>{ "--login", "-c", "echo a b" },
          "shell: quoted path and quoted arg");
    c = ResolveTerminalShell("C:\\Program Files\\PowerShell\\7\\pwsh.exe",
                             FakeFinder({ { "C:\\Program Files\\PowerShell\\7\\pwsh.exe", "C:\\Program Files\\PowerShell\\7\\pwsh.exe" } }));
    check(c.error == TerminalShellError::None && c.args.empty(), "shell: unquoted path with spaces found as a whole");
    c = ResolveTerminalShell("\"C:\\x\\sh.exe", FakeFinder(all));
    check(c.error == TerminalShellError::ConfiguredMalformed, "shell: unterminated quote");
    c = ResolveTerminalShell("cmd.exe /k \"echo", FakeFinder(all));
    check(c.error == TerminalShellError::ConfiguredMalformed, "shell: unterminated quote in args");
    c = ResolveTerminalShell("\"\"", FakeFinder(all));
    check(c.error == TerminalShellError::ConfiguredNotFound, "shell: empty quoted path");
    c = ResolveTerminalShell("C:\\ユーザー\\シェル.exe", FakeFinder({ { "C:\\ユーザー\\シェル.exe", "C:\\ユーザー\\シェル.exe" } }));
    check(c.error == TerminalShellError::None && c.path == "C:\\ユーザー\\シェル.exe", "shell: Japanese path");
}

// ---- 環境組立 ----

bool IsSortedUnique(const std::vector<TerminalEnvVar>& env)
{
    for (size_t i = 1; i < env.size(); ++i) {
        if (CompareTerminalEnvName(env[i - 1].name, env[i].name) >= 0) {
            return false;
        }
    }
    return true;
}

const TerminalEnvVar* Find(const std::vector<TerminalEnvVar>& env, const std::string& name)
{
    for (const TerminalEnvVar& var : env) {
        if (CompareTerminalEnvName(var.name, name) == 0) {
            return &var;
        }
    }
    return nullptr;
}

void TestEnvironment(const auto& check)
{
    const std::vector<TerminalEnvVar> parent = {
        { "windir", "C:\\Windows" },
        { "Path", "C:\\Windows;C:\\bin" },
        { "=C:", "C:\\work" },
        { "PATH", "C:\\dup" },
        { "", "noname" },
        { "A=B", "bad" },
        { "TERM_PROGRAM", "vscode" },
        { "ALLUSERSPROFILE", "C:\\ProgramData" },
        { "_UNDER", "u" },
    };
    const std::vector<TerminalEnvVar> additions = {
        { "MYE_PROJECT_ROOT", "C:\\Proj" },
        { "TERM_PROGRAM", "MyEngine" },
        { "COLORTERM", "truecolor" },
        { "mye_project_root", "dup-addition" },
    };
    const std::vector<TerminalEnvVar> env = BuildTerminalEnvironment(parent, additions, "C:\\Proj\\tools");
    check(IsSortedUnique(env), "env: sorted case-insensitively, no duplicates");
    check(env.size() == 8, "env: invalid / duplicate entries dropped");
    const TerminalEnvVar* path = Find(env, "PATH");
    check(path != nullptr && path->name == "Path" && path->value == "C:\\Proj\\tools;C:\\Windows;C:\\bin",
          "env: PATH prefixed, first spelling kept, duplicate ignored");
    check(Find(env, "TERM_PROGRAM")->value == "vscode", "env: existing variable not overwritten");
    check(Find(env, "MYE_PROJECT_ROOT") != nullptr && Find(env, "MYE_PROJECT_ROOT")->value == "C:\\Proj", "env: addition added once");
    check(env.front().name == "=C:", "env: '=' hidden variable kept and sorts first");
    // 大文字に畳んで比べるので '_' (0x5F) は英字の後
    check(env.back().name == "_UNDER", "env: underscore sorts after letters (uppercase fold)");

    const std::string block = FormatTerminalEnvironmentBlock(env);
    check(block.size() >= 2 && block[block.size() - 1] == '\0' && block[block.size() - 2] == '\0', "env: block ends with double NUL");
    check(static_cast<size_t>(std::count(block.begin(), block.end(), '\0')) == env.size() + 1, "env: one NUL per entry plus terminator");
    check(block.rfind("=C:=C:\\work", 0) == 0, "env: block starts with first entry");
    check(FormatTerminalEnvironmentBlock({}) == std::string("\0\0", 2), "env: empty block is two NULs");

    const std::vector<TerminalEnvVar> noPath = BuildTerminalEnvironment({ { "B", "1" } }, {}, "C:\\t");
    check(noPath.size() == 2 && Find(noPath, "PATH") && Find(noPath, "PATH")->value == "C:\\t", "env: PATH created when absent");
    const std::vector<TerminalEnvVar> emptyPath = BuildTerminalEnvironment({ { "PATH", "" } }, {}, "C:\\t");
    check(emptyPath[0].value == "C:\\t", "env: empty PATH gets prefix only");
    const std::vector<TerminalEnvVar> noPrefix = BuildTerminalEnvironment({ { "PATH", "x" } }, {}, "");
    check(noPrefix[0].value == "x", "env: empty prefix leaves PATH");
    check(BuildTerminalEnvironment({}, {}, "").empty(), "env: all empty");
    const std::vector<TerminalEnvVar> utf8 = BuildTerminalEnvironment({ { "\xC3\x84X", "1" }, { "ZZ", "2" } }, {}, "");
    check(utf8[0].name == "ZZ", "env: non-ascii bytes sort after ascii (unsigned)");
}

} // namespace

bool RunTerminalCoreSelfTest()
{
    MYE_LOG_INFO("==== TerminalCore self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    TestScreenBasics(check);
    TestScreenSplitSequences(check);
    TestScreenColors(check);
    TestScreenAltScreen(check);
    TestScreenScrollback(check);
    TestScreenResize(check);
    TestScreenBulk(check);
    TestKeys(check);
    TestPaste(check);
    TestShellResolve(check);
    TestEnvironment(check);
    MYE_LOG_INFO("TerminalCore self test: %s (%d failure(s))", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
