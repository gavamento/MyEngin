//====================================================================================
//                          TerminalVsEnvSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          VS 開発者環境の解析・差分・非同期読み込みとエンジン環境変数の回帰テスト
//====================================================================================
#include "Editor/Tools/Terminal/TerminalVsEnvSelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Editor/Tools/Terminal/TerminalEnvironment.h"
#include "Editor/Tools/Terminal/TerminalVsEnv.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

using std::chrono::milliseconds;
using Clock = TerminalVsEnvLoader::Clock;

const TerminalEnvVar* FindVar(const std::vector<TerminalEnvVar>& env, std::string_view name)
{
    for (const TerminalEnvVar& var : env) {
        if (CompareTerminalEnvName(var.name, name) == 0) {
            return &var;
        }
    }
    return nullptr;
}

bool HasValue(const std::vector<TerminalEnvVar>& env, std::string_view name, std::string_view value)
{
    const TerminalEnvVar* var = FindVar(env, name);
    return var != nullptr && var->value == value;
}

bool IsSorted(const std::vector<TerminalEnvVar>& env)
{
    for (size_t i = 1; i < env.size(); ++i) {
        if (CompareTerminalEnvName(env[i - 1].name, env[i].name) > 0) {
            return false;
        }
    }
    return true;
}

// ---- set 出力の解析 ----

void TestParse(const auto& check)
{
    std::vector<TerminalEnvVar> out;
    const std::string marker(kTerminalVsEnvMarker);

    // バナー (= を含む行も) は目印より前なので読まない。CRLF・目印の行末の空白 (echo MARK && ... の空白) を許す
    const std::string full = "**** banner ****\r\n[note] a=b\r\n" + marker + " \r\nALPHA=1\r\nPath=C:\\a;C:\\b\r\nEQ=x=y=z\r\nEMPTY=\r\n";
    check(ParseTerminalSetOutput(full, marker, out) && out.size() == 4, "parse: only the lines after the marker are read (banner ignored, CRLF, blank after marker)");
    check(out.size() == 4 && out[0].name == "ALPHA" && out[0].value == "1" && out[1].name == "Path" && out[1].value == "C:\\a;C:\\b",
          "parse: NAME=VALUE in output order, CR removed");
    check(HasValue(out, "EQ", "x=y=z"), "parse: '=' inside a value belongs to the value");
    check(FindVar(out, "EMPTY") != nullptr && FindVar(out, "EMPTY")->value.empty(), "parse: an empty value is kept as an empty string");

    check(!ParseTerminalSetOutput("ALPHA=1\r\n", marker, out) && out.empty(), "parse: no marker means failure and no variables");
    check(!ParseTerminalSetOutput("", marker, out) && out.empty(), "parse: empty output with a marker fails");
    check(ParseTerminalSetOutput("", "", out) && out.empty(), "parse: empty output without a marker is an empty list");
    check(ParseTerminalSetOutput("A=1\nB=2", "", out) && out.size() == 2 && out[1].value == "2", "parse: LF only, and the last line without a newline");

    // "=C:" 系の隠し変数: 名前の区切りは 2 文字目以降の最初の '='
    check(ParseTerminalSetOutput("=C:=C:\\Users\\x\r\n=ExitCode=00000000\r\n=D:=\r\nA=1\r\n", "", out) && out.size() == 4 &&
              out[0].name == "=C:" && out[0].value == "C:\\Users\\x" && out[1].name == "=ExitCode" && out[1].value == "00000000" &&
              out[2].name == "=D:" && out[2].value.empty(),
          "parse: hidden '=C:' style variables keep their name and value");
    check(ParseTerminalSetOutput("=\r\n==x\r\nno equals here\r\n\r\n=C:\r\nA=1\r\n", "", out) && out.size() == 1 && out[0].name == "A",
          "parse: lines without a usable name ('=', '==x', no '=', blank, '=C:' without a value separator) are dropped");

    // Unicode: 値・名前のバイト列をそのまま残す
    const std::string japanese = "USERPROFILE=C:\\Users\\\xE7\x8B\x90\xE7\x94\xB0\r\n\xE5\x90\x8D\xE5\x89\x8D=\xF0\x9F\x98\x80 \xE5\x80\xA4\r\n";   // 狐田 / 名前=😀 値
    check(ParseTerminalSetOutput(japanese, "", out) && out.size() == 2 && out[0].value == "C:\\Users\\\xE7\x8B\x90\xE7\x94\xB0" &&
              out[1].name == "\xE5\x90\x8D\xE5\x89\x8D" && out[1].value == "\xF0\x9F\x98\x80 \xE5\x80\xA4",
          "parse: UTF-8 names and values (CJK, emoji, inner space) pass through byte for byte");

    // 値の末尾の空白は値の一部。同名は大文字小文字を無視して先のものを残す
    check(ParseTerminalSetOutput("A=v \r\nB=1\r\nb=2\r\nPath=x\r\nPATH=y\r\n", "", out) && out.size() == 3 && out[0].value == "v " &&
              out[1].name == "B" && out[1].value == "1" && out[2].name == "Path" && out[2].value == "x",
          "parse: trailing blanks of a value are kept; the first of case-insensitive duplicates wins");

    // 目印が 2 回出ても、最初の目印以降を読む (2 つ目は '=' の無い行として捨てる)
    check(ParseTerminalSetOutput(marker + "\r\nA=1\r\n" + marker + "\r\nB=2\r\n", marker, out) && out.size() == 2, "parse: a second marker line is just a dropped line");
}

// ---- 差分と適用 ----

void TestDiff(const auto& check)
{
    const std::vector<TerminalEnvVar> parent = { { "Path", "C:\\Windows" }, { "SAME", "1" }, { "Case", "abc" }, { "OnlyParent", "p" }, { "=C:", "C:\\x" } };
    const std::vector<TerminalEnvVar> vs = { { "PATH", "C:\\VS;C:\\Windows" }, { "SAME", "1" }, { "case", "ABC" }, { "VSCMD_VER", "17.9" },
                                             { "=C:", "C:\\other" }, { "=ExitCode", "00000001" }, { "VSCMD_VER", "dup" }, { "EMPTY", "" } };
    const std::vector<TerminalEnvVar> diff = ComputeTerminalEnvironmentDiff(parent, vs);
    check(diff.size() == 4 && diff[0].name == "PATH" && diff[0].value == "C:\\VS;C:\\Windows" && diff[1].name == "case" && diff[1].value == "ABC" &&
              diff[2].name == "VSCMD_VER" && diff[2].value == "17.9" && diff[3].name == "EMPTY",
          "diff: changed (name matched ignoring case, VS spelling), new and empty-valued variables, in VS order; same value, hidden '=' names and duplicates excluded");
    check(FindVar(diff, "OnlyParent") == nullptr, "diff: a variable only the parent has is not a difference");
    check(ComputeTerminalEnvironmentDiff(parent, parent).empty(), "diff: identical environments (editor started from a developer prompt) give an empty diff");
    check(ComputeTerminalEnvironmentDiff({}, {}).empty() && ComputeTerminalEnvironmentDiff(parent, {}).empty(), "diff: empty inputs");
    const std::vector<TerminalEnvVar> fromEmpty = ComputeTerminalEnvironmentDiff({}, { { "A", "1" }, { "=X", "2" }, { "B", "" } });
    check(fromEmpty.size() == 2 && fromEmpty[0].name == "A" && fromEmpty[1].name == "B", "diff: against an empty parent everything but hidden names is new");

    const std::vector<TerminalEnvVar> applied = ApplyTerminalEnvironmentDiff(parent, diff);
    check(applied.size() == parent.size() + 2 && applied[0].name == "Path" && applied[0].value == "C:\\VS;C:\\Windows" && applied[2].name == "Case" &&
              applied[2].value == "ABC" && applied[1].value == "1" && applied[3].name == "OnlyParent",
          "apply: existing names keep the parent's spelling and position with the new value; the rest is untouched");
    check(HasValue(applied, "VSCMD_VER", "17.9") && FindVar(applied, "EMPTY") != nullptr && applied.back().name == "EMPTY",
          "apply: new variables are appended in diff order");
    check(ApplyTerminalEnvironmentDiff(parent, {}).size() == parent.size() && ApplyTerminalEnvironmentDiff({}, diff).size() == diff.size(),
          "apply: empty diff / empty parent");
    const std::vector<TerminalEnvVar> twice = ApplyTerminalEnvironmentDiff({}, { { "A", "1" }, { "a", "2" } });
    check(twice.size() == 1 && twice[0].value == "2", "apply: the same name twice in a diff ends as one variable with the later value");
}

// ---- エンジンの環境変数 ----

void TestLaunchEnvironment(const auto& check)
{
    const std::vector<TerminalEnvVar> parent = { { "Path", "C:\\Windows" },        { "TERM_PROGRAM", "vscode" }, { "mye_terminal_id", "old" },
                                                 { "COLORTERM", "24bit" },          { "windir", "C:\\Windows" },   { "MYE_PROJECT_ROOT", "C:\\Other" },
                                                 { "=C:", "C:\\Users" } };
    const std::vector<TerminalEnvVar> vsDiff = { { "PATH", "C:\\VS;C:\\Windows" }, { "VSCMD_VER", "17.9" } };

    const std::vector<TerminalEnvVar> env = BuildTerminalLaunchEnvironment(parent, vsDiff, "C:\\Proj\\", "10-20-3");
    check(HasValue(env, "MYE_TERMINAL_ID", "10-20-3") && HasValue(env, "TERM_PROGRAM", "MyEngine") && HasValue(env, "MYE_PROJECT_ROOT", "C:\\Proj"),
          "launch: MYE_TERMINAL_ID / TERM_PROGRAM / MYE_PROJECT_ROOT replace the values inherited from a parent terminal (trailing separator dropped)");
    check(std::count_if(env.begin(), env.end(), [](const TerminalEnvVar& v) { return CompareTerminalEnvName(v.name, "MYE_TERMINAL_ID") == 0; }) == 1,
          "launch: the old differently-cased MYE_TERMINAL_ID is gone (one entry)");
    check(HasValue(env, "COLORTERM", "24bit"), "launch: COLORTERM is added only when the parent has none (an existing value is kept)");
    check(HasValue(env, "Path", "C:\\Proj\\tools;C:\\VS;C:\\Windows") && FindVar(env, "Path")->name == "Path",
          "launch: PATH = <root>\\tools first, then the Visual Studio PATH, under the parent's spelling");
    check(HasValue(env, "VSCMD_VER", "17.9") && HasValue(env, "windir", "C:\\Windows") && HasValue(env, "=C:", "C:\\Users"),
          "launch: the VS diff is applied; other parent variables (hidden ones too) are kept");
    check(IsSorted(env), "launch: the result is in the Windows environment block order (case-insensitive ascending)");

    const std::vector<TerminalEnvVar> bare = BuildTerminalLaunchEnvironment({ { "A", "1" } }, {}, "C:\\Proj", "1-2-3");
    check(HasValue(bare, "COLORTERM", "truecolor") && HasValue(bare, "A", "1") && HasValue(bare, "PATH", "C:\\Proj\\tools") && bare.size() == 6,
          "launch: without a VS diff and a parent PATH, PATH is created and COLORTERM=truecolor is added");

    const std::vector<TerminalEnvVar> noRoot = BuildTerminalLaunchEnvironment({ { "PATH", "x" }, { "MYE_PROJECT_ROOT", "C:\\Stale" } }, {}, "", "1-2-3");
    check(HasValue(noRoot, "PATH", "x") && FindVar(noRoot, "MYE_PROJECT_ROOT") == nullptr, "launch: with no project root, PATH is untouched and no stale MYE_PROJECT_ROOT is left");

    const std::vector<TerminalEnvVar> semicolon = BuildTerminalLaunchEnvironment({ { "PATH", "x" } }, {}, "C:\\a;b", "1-2-3");
    check(HasValue(semicolon, "PATH", "x") && HasValue(semicolon, "MYE_PROJECT_ROOT", "C:\\a;b"),
          "launch: a root containing ';' cannot be written into PATH, so PATH is left alone (the variable still gets the root)");

    const std::vector<TerminalEnvVar> drive = BuildTerminalLaunchEnvironment({ { "PATH", "x" } }, {}, "C:\\", "1-2-3");
    check(HasValue(drive, "MYE_PROJECT_ROOT", "C:\\") && HasValue(drive, "PATH", "C:\\tools;x"), "launch: a drive root keeps its separator (C: alone would be the drive's current directory)");

    const std::vector<TerminalEnvVar> unicodeRoot = BuildTerminalLaunchEnvironment({ { "PATH", "x" } }, {}, "D:/\xE3\x83\x97\xE3\x83\xAD/", "1-2-3");   // D:/プロ/
    check(HasValue(unicodeRoot, "MYE_PROJECT_ROOT", "D:/\xE3\x83\x97\xE3\x83\xAD") && HasValue(unicodeRoot, "PATH", "D:/\xE3\x83\x97\xE3\x83\xAD\\tools;x"),
          "launch: a Unicode root passes through (forward slash input is kept as given)");

    const std::string block = FormatTerminalEnvironmentBlock(env);
    check(block.size() >= 2 && block[block.size() - 1] == '\0' && block[block.size() - 2] == '\0', "launch: the block ends with a double NUL");

    check(FormatTerminalId(1234, 1763500000, 1) == "1234-1763500000-1" && FormatTerminalId(0, 0, 12) == "0-0-12", "id: <pid>-<start unix seconds>-<serial>");
    check(FormatTerminalId(4294967295u, 18446744073709551615ull, 2147483647) == "4294967295-18446744073709551615-2147483647", "id: the largest values do not overflow");
}

// ---- 非同期ローダー ----

// 偽の fetch が共有する状態。ローダーがスレッドを切り離しても生きるよう shared_ptr で持つ
struct FakeFetchState
{
    std::atomic<int> calls{ 0 };
    std::atomic<bool> gate{ false };            // true になるまで fetch は戻らない (cancel でも抜ける)
    std::atomic<bool> sawCancel{ false };
    std::atomic<bool> returned{ false };
    TerminalVsEnvFetchResult result;
    bool honorCancel = true;
    bool throwInstead = false;
    milliseconds sleep{ 0 };
};

TerminalVsEnvFetch MakeFetch(const std::shared_ptr<FakeFetchState>& state)
{
    return [state](const std::atomic<bool>& cancel) {
        ++state->calls;
        if (state->throwInstead) {
            throw 1;
        }
        const Clock::time_point begin = Clock::now();
        while (!state->gate) {
            if (cancel) {
                state->sawCancel = true;
                if (state->honorCancel) {
                    break;
                }
            }
            std::this_thread::sleep_for(milliseconds(1));
            if (state->sleep.count() > 0 && Clock::now() - begin >= state->sleep) {
                break;
            }
        }
        state->returned = true;
        return state->result;
    };
}

// 条件が真になるまで少しずつ待つ (上限 5 秒)
template <class Pred>
bool WaitUntil(Pred pred)
{
    const Clock::time_point limit = Clock::now() + std::chrono::seconds(5);
    while (!pred()) {
        if (Clock::now() > limit) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

void TestLoaderResults(const auto& check)
{
    const Clock::time_point t0 = Clock::now();
    {
        auto state = std::make_shared<FakeFetchState>();
        state->result.status = TerminalVsEnvStatus::Ok;
        state->result.diff = { { "VSCMD_VER", "17.9" } };
        TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(30000));
        check(loader.State() == TerminalVsEnvState::Idle && loader.Poll(t0) == TerminalVsEnvState::Idle && state->calls == 0, "loader: idle until started, Poll alone starts nothing");
        loader.Start(t0);
        loader.Start(t0);
        check(loader.State() == TerminalVsEnvState::Loading, "loader: Start moves to Loading");
        check(loader.Poll(t0) == TerminalVsEnvState::Loading, "loader: still Loading while the fetch has not returned");
        state->gate = true;
        check(WaitUntil([&] { return loader.Poll(t0) == TerminalVsEnvState::Ready; }), "loader: Ready once the fetch returns Ok");
        check(loader.Diff().size() == 1 && loader.Diff()[0].name == "VSCMD_VER", "loader: the diff is exposed when Ready");
        loader.Start(t0 + std::chrono::hours(1));
        loader.Poll(t0 + std::chrono::hours(1));
        check(state->calls == 1 && loader.State() == TerminalVsEnvState::Ready, "loader: runs once per editor session (a later Start / Poll changes nothing)");
    }
    const struct { TerminalVsEnvStatus status; TerminalVsEnvState state; const char* what; } kCases[] = {
        { TerminalVsEnvStatus::NotInstalled, TerminalVsEnvState::NotInstalled, "loader: NotInstalled maps through" },
        { TerminalVsEnvStatus::Failed, TerminalVsEnvState::Failed, "loader: Failed maps through" },
        { TerminalVsEnvStatus::TimedOut, TerminalVsEnvState::TimedOut, "loader: a TimedOut reported by the fetch itself maps through" },
    };
    for (const auto& c : kCases) {
        auto state = std::make_shared<FakeFetchState>();
        state->result.status = c.status;
        state->result.diff = { { "IGNORED", "1" } };
        state->result.detail = "why";
        state->gate = true;
        TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(30000));
        loader.Start(t0);
        check(WaitUntil([&] { return loader.Poll(t0) != TerminalVsEnvState::Loading; }) && loader.State() == c.state && loader.Diff().empty() && loader.Detail() == "why", c.what);
    }
    {
        auto state = std::make_shared<FakeFetchState>();
        state->throwInstead = true;
        TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(30000));
        loader.Start(t0);
        check(WaitUntil([&] { return loader.Poll(t0) != TerminalVsEnvState::Loading; }) && loader.State() == TerminalVsEnvState::Failed,
              "loader: an exception from the fetch becomes Failed (the thread does not terminate the editor)");
    }
    {
        TerminalVsEnvLoader loader(TerminalVsEnvFetch{}, milliseconds(30000));
        loader.Start(t0);
        check(WaitUntil([&] { return loader.Poll(t0) != TerminalVsEnvState::Loading; }) && loader.State() == TerminalVsEnvState::Failed, "loader: an empty fetch function is Failed, not a crash");
    }
}

void TestLoaderTimeoutAndShutdown(const auto& check)
{
    const Clock::time_point t0 = Clock::now();
    {
        auto state = std::make_shared<FakeFetchState>();
        state->result.status = TerminalVsEnvStatus::Ok;
        state->result.diff = { { "LATE", "1" } };
        {
            TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(100));
            loader.Start(t0);
            check(loader.Poll(t0 + milliseconds(99)) == TerminalVsEnvState::Loading, "timeout: one millisecond before the deadline it is still Loading");
            check(loader.Poll(t0 + milliseconds(100)) == TerminalVsEnvState::TimedOut && loader.Detail() == "timed out", "timeout: at the deadline it becomes TimedOut");
            check(WaitUntil([&] { return state->sawCancel.load(); }), "timeout: the fetch is told to cancel");
            check(WaitUntil([&] { return state->returned.load(); }), "timeout: a cooperative fetch returns after the cancel");
            check(loader.Poll(t0 + milliseconds(100)) == TerminalVsEnvState::TimedOut && loader.Diff().empty(),
                  "timeout: a result that arrives after the timeout is not adopted (the decision does not flip later)");
        }
    }
    {
        // 破棄: cancel を立てて協調的な fetch の終了を待つ
        auto state = std::make_shared<FakeFetchState>();
        {
            TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(30000));
            loader.Start(t0);
            WaitUntil([&] { return state->calls.load() == 1; });
        }
        check(state->sawCancel && state->returned, "shutdown: destroying a loading loader cancels the fetch and waits for it");
    }
    {
        // 破棄: cancel を無視する fetch (vswhere のハングなど) は待ちきらずに切り離す。切り離されたスレッドが後で終わっても安全
        auto state = std::make_shared<FakeFetchState>();
        state->honorCancel = false;
        state->sleep = milliseconds(250);
        const Clock::time_point begin = Clock::now();
        {
            TerminalVsEnvLoader loader(MakeFetch(state), milliseconds(30000), milliseconds(10));
            loader.Start(t0);
            WaitUntil([&] { return state->calls.load() == 1; });
        }
        const auto elapsed = std::chrono::duration_cast<milliseconds>(Clock::now() - begin);
        check(elapsed < milliseconds(200) && !state->returned, "shutdown: a fetch that ignores cancel is detached after the short wait instead of blocking the exit");
        check(WaitUntil([&] { return state->returned.load(); }), "shutdown: the detached thread still finishes safely on its own");
    }
    {
        // 一度も Start しない / 完了済みの破棄は即座
        auto state = std::make_shared<FakeFetchState>();
        state->gate = true;
        {
            TerminalVsEnvLoader never(MakeFetch(state));
            TerminalVsEnvLoader done(MakeFetch(state));
            done.Start(t0);
            WaitUntil([&] { return done.Poll(t0) != TerminalVsEnvState::Loading; });
        }
        check(state->calls == 1, "shutdown: a never-started loader runs nothing, a finished one is joined");
    }
}

} // namespace

bool RunTerminalVsEnvSelfTest()
{
    MYE_LOG_INFO("==== TerminalVsEnv self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    TestParse(check);
    TestDiff(check);
    TestLaunchEnvironment(check);
    TestLoaderResults(check);
    TestLoaderTimeoutAndShutdown(check);
    MYE_LOG_INFO("TerminalVsEnv self test: %s (%d failure(s))", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
