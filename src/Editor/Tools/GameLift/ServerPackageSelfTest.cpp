//====================================================================================
//                          ServerPackageSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 用サーバ版の組み立ての回帰テスト実装
//====================================================================================
#include "Editor/Tools/GameLift/ServerPackageSelfTest.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>

#include "Editor/Tools/GameLift/ServerPackage.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Session/Provenance.h"
#include "Engine/Platform/PathUtil.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace mye {
namespace {

struct Counter {
    int failCount = 0;
    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }
};

void WriteFile(const fs::path& path, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadFileBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// root 以下の全ファイルの相対パス (小文字・'\' 区切り)
std::set<std::wstring> ListFiles(const fs::path& root)
{
    std::set<std::wstring> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        std::wstring rel = it->path().lexically_relative(root).wstring();
        for (wchar_t& c : rel) {
            c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));
        }
        files.insert(rel);
    }
    return files;
}

// 偽の書き出しフォルダ・exeDir・VS を作る。exeDir は bin\x64\<exeConfig>
struct Fixture {
    fs::path root;
    fs::path dist;
    fs::path exeDir;
    fs::path vs;
    fs::path outParent;
    fs::path out;
    std::string vcRedistNewest;
};

Fixture MakeFixture(const fs::path& root, const wchar_t* exeConfig)
{
    Fixture f;
    f.root = root;
    f.dist = root / L"dist";
    f.exeDir = root / L"exe" / L"bin" / L"x64" / exeConfig;
    f.vs = root / L"vs";
    f.outParent = root / L"proj" / L"cache" / L"gamelift";
    f.out = f.outParent / L"server-build";
    std::error_code ec;
    fs::remove_all(root, ec);

    // 書き出し物: サーバに要るもの + 要らないもの (Runtime / .NET / C# ホスト / EOS / pdb)
    WriteFile(f.dist / L"Runtime.exe", "runtime");
    WriteFile(f.dist / L"GameLogic.dll", "gamelogic-v1");
    WriteFile(f.dist / L"nethost.dll", "nethost");
    WriteFile(f.dist / L"MyeScripting.dll", "scripting");
    WriteFile(f.dist / L"MyeScripting.deps.json", "{}");
    WriteFile(f.dist / L"Microsoft.CodeAnalysis.dll", "roslyn");
    WriteFile(f.dist / L"OnlineRuntime.dll", "eos");
    WriteFile(f.dist / L"Runtime.pdb", "pdb");
    WriteFile(f.dist / L"dotnet" / L"host" / L"fxr" / L"8.0.0" / L"hostfxr.dll", "fxr");
    WriteFile(f.dist / L"assets" / L"scenes" / L"main.scene.json", "{\"version\":4,\"entities\":[]}\n");
    WriteFile(f.dist / L"assets" / L"scenes" / L"main.scene.json.meta", "{\"guid\":\"x\"}\n");
    // 0 と CRLF を含む (テキスト変換されたらバイトが変わる)
    WriteFile(f.dist / L"assets" / L"data" / L"blob.bin", std::string("a\0b\r\nc\r\n\xff\x00", 10));
    WriteFile(f.dist / L"assets" / L"data" / L"deep" / L"er" / L"note.txt", "deep file\n");
    WriteFile(f.dist / L"cache" / L"cooked" / L"model.mmdl", "cooked-model");
    WriteFile(f.dist / L"cache" / L"cooked" / L".sealed", "sealed\n");
    WriteFile(f.dist / L"cache" / L"other" / L"junk.bin", "not cooked");
    WriteContentManifest((f.dist / L"assets" / kContentManifestName).wstring(), (f.dist / L"assets").wstring());

    WriteFile(f.exeDir / L"Server.exe", "server");
    WriteFile(f.exeDir / L"libssl-3-x64.dll", "ssl");
    WriteFile(f.exeDir / L"libcrypto-3-x64.dll", "crypto");
    WriteFile(f.exeDir / L"Editor.exe", "editor");
    WriteFile(f.exeDir / L"Runtime.exe", "runtime-from-exedir");
    WriteFile(f.exeDir / L"OnlineRuntime.dll", "eos");
    WriteFile(f.exeDir / L"Server.pdb", "pdb");

    // 14.9.1 と 14.51.x: 文字列順なら 14.9.1 が勝ってしまう。v145 は数値の版が無いときだけ使う
    WriteFile(f.vs / L"VC" / L"Redist" / L"MSVC" / L"14.9.1" / L"vc_redist.x64.exe", "vc-old");
    WriteFile(f.vs / L"VC" / L"Redist" / L"MSVC" / L"14.51.36231" / L"vc_redist.x64.exe", "vc-newest");
    WriteFile(f.vs / L"VC" / L"Redist" / L"MSVC" / L"v145" / L"vc_redist.x64.exe", "vc-named");
    f.vcRedistNewest = "vc-newest";
    return f;
}

ServerPackageInput InputOf(const Fixture& f)
{
    ServerPackageInput in;
    in.distDir = f.dist;
    in.exeDir = f.exeDir;
    in.outputDir = f.out;
    in.vsInstallDirOverride = f.vs;
    return in;
}

bool AnyContains(const std::vector<std::string>& lines, const char* needle)
{
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

std::set<std::wstring> ExpectedOutputFiles(const Fixture& f)
{
    std::set<std::wstring> expected = {
        L"server.exe", L"libssl-3-x64.dll", L"libcrypto-3-x64.dll", L"gamelogic.dll", L"vc_redist.x64.exe",
        L"install.bat", L"mye_server_build.json",
        L"cache\\cooked\\model.mmdl", L"cache\\cooked\\.sealed",
    };
    for (const std::wstring& file : ListFiles(f.dist / L"assets")) {
        expected.insert(L"assets\\" + file);
    }
    return expected;
}

// outParent の中に out 以外 (.staging / .old) が残っていないか
bool OnlyOutputRemains(const Fixture& f)
{
    std::error_code ec;
    int count = 0;
    for (const auto& e : fs::directory_iterator(f.outParent, ec)) {
        (void)e;
        ++count;
    }
    return count == 1 && fs::is_directory(f.out, ec);
}

fs::path TempRoot(const wchar_t* leaf)
{
    return fs::temp_directory_path() / (std::wstring(leaf) + L"_" + std::to_wstring(GetCurrentProcessId()));
}

} // namespace

bool RunServerPackageSelfTest()
{
    MYE_LOG_INFO("==== ServerPackage (M107b) self test ====");
    Counter c;
    std::error_code ec;
    const fs::path root = TempRoot(L"mye_gl_pkg");

    // ---- 1. 正常: 入れるものだけが入り、assets がバイト一致する ----
    {
        const Fixture f = MakeFixture(root, L"Release");
        const ServerPackageResult r = BuildServerPackage(InputOf(f));
        c.Check(r.ok && r.problems.empty(), "fake export + fake exeDir + fake VS builds");
        c.Check(ListFiles(f.out) == ExpectedOutputFiles(f),
                "output holds exactly Server.exe / OpenSSL / GameLogic.dll / assets / cooked / vc_redist / install.bat / info json");
        c.Check(!fs::exists(f.out / L"Runtime.exe", ec) && !fs::exists(f.out / L"dotnet", ec)
                    && !fs::exists(f.out / L"nethost.dll", ec) && !fs::exists(f.out / L"OnlineRuntime.dll", ec)
                    && !fs::exists(f.out / L"cache" / L"other", ec),
                "Runtime / dotnet / nethost / EOS / non-cooked cache are not copied");
        bool bytesMatch = true;
        for (const std::wstring& rel : ListFiles(f.dist / L"assets")) {
            bytesMatch &= ReadFileBytes(f.dist / L"assets" / rel) == ReadFileBytes(f.out / L"assets" / rel);
        }
        c.Check(bytesMatch, "every file under assets\\ (manifest included) is byte-identical to the export");
        c.Check(ReadFileBytes(f.out / L"vc_redist.x64.exe") == f.vcRedistNewest,
                "vc_redist is the newest numeric version (14.51 beats 14.9 and v145)");
        c.Check(ReadFileBytes(f.out / L"install.bat")
                    == "vc_redist.x64.exe /install /quiet /norestart /log C:\\game\\install_vcredist.log\r\nexit /b 0\r\n",
                "install.bat content matches the spec");
        const nlohmann::json info = nlohmann::json::parse(ReadFileBytes(f.out / kServerBuildInfoName), nullptr, false);
        uint64_t manifestHash = 0;
        ReadContentManifest((f.dist / L"assets" / kContentManifestName).wstring(), manifestHash);
        char want[24];
        std::snprintf(want, sizeof(want), "0x%016llx", static_cast<unsigned long long>(manifestHash));
        c.Check(info.is_object() && info.value("contentHash", "") == want && r.contentHash == manifestHash,
                "info json carries the manifest's contentHash");
        char wantGame[24];
        std::snprintf(wantGame, sizeof(wantGame), "0x%016llx",
                      static_cast<unsigned long long>(HashFileBytes((f.dist / L"GameLogic.dll").wstring())));
        c.Check(info.value("gameLogicHash", "") == wantGame && info.value("serverConfig", "") == "Release"
                    && info.value("sourceDist", "") == WideToUtf8(fs::absolute(f.dist).wstring())
                    && !info.value("builtAtUtc", "").empty(),
                "info json carries gameLogicHash / serverConfig / sourceDist / builtAtUtc");
        c.Check(r.warnings.empty() && !r.serverIsDebug, "Release exeDir gives no warning");
        c.Check(OnlyOutputRemains(f), "no .staging / .old folder is left behind");

        // ---- 2. 作り直し: 前回の残りが消え、中身が新しくなる ----
        WriteFile(f.out / L"stale.txt", "left from the last build");
        WriteFile(f.dist / L"assets" / L"data" / L"deep" / L"er" / L"note.txt", "changed\n");
        WriteContentManifest((f.dist / L"assets" / kContentManifestName).wstring(), (f.dist / L"assets").wstring());
        const ServerPackageResult again = BuildServerPackage(InputOf(f));
        c.Check(again.ok && !fs::exists(f.out / L"stale.txt", ec)
                    && ReadFileBytes(f.out / L"assets" / L"data" / L"deep" / L"er" / L"note.txt") == "changed\n"
                    && again.contentHash != r.contentHash,
                "rebuilding replaces the previous build (stale file gone, new content and hash)");
        c.Check(OnlyOutputRemains(f), "rebuilding leaves no .staging / .old folder");
    }

    // ---- 3. 足りない入力: 全部挙げて失敗し、出力が変わらない ----
    {
        const Fixture f = MakeFixture(root, L"Release");
        const ServerPackageResult good = BuildServerPackage(InputOf(f)); // 前回の組み立て結果を作る
        c.Check(good.ok, "setup: previous build exists");
        const auto before = ListFiles(f.out);
        const std::string infoBefore = ReadFileBytes(f.out / kServerBuildInfoName);
        fs::remove(f.dist / L"assets" / kContentManifestName, ec);
        fs::remove(f.exeDir / L"Server.exe", ec);
        fs::remove(f.exeDir / L"libssl-3-x64.dll", ec);
        fs::remove(f.dist / L"Runtime.exe", ec);
        ServerPackageInput in = InputOf(f);
        in.vsInstallDirOverride = f.root / L"no-such-vs";
        const ServerPackageResult r = BuildServerPackage(in);
        c.Check(!r.ok && r.problems.size() >= 5, "missing inputs fail with every problem listed (>= 5)");
        c.Check(AnyContains(r.problems, "content_manifest.json") && AnyContains(r.problems, "Server.exe")
                    && AnyContains(r.problems, "libssl-3-x64.dll") && AnyContains(r.problems, "Runtime.exe")
                    && AnyContains(r.problems, "vc_redist.x64.exe"),
                "problems name manifest / Server.exe / libssl / Runtime.exe / vc_redist");
        c.Check(ListFiles(f.out) == before && ReadFileBytes(f.out / kServerBuildInfoName) == infoBefore,
                "the previous output is untouched after a failure");
        c.Check(OnlyOutputRemains(f), "a failure leaves no staging folder");
    }
    {
        // VS が見つからない (vswhere も無い想定) だけでも失敗として挙がる / 出力先が無ければ作られない
        const Fixture f = MakeFixture(root, L"Release");
        ServerPackageInput in = InputOf(f);
        in.vsInstallDirOverride = f.root / L"no-such-vs";
        const ServerPackageResult r = BuildServerPackage(in);
        c.Check(!r.ok && r.problems.size() == 1 && AnyContains(r.problems, "vc_redist.x64.exe") && !fs::exists(f.out, ec)
                    && !fs::exists(f.outParent, ec),
                "only vc_redist missing: one problem, nothing is created");
    }

    // ---- 4. 入れ替えの安全側 ----
    {
        const Fixture f = MakeFixture(root, L"Release");
        WriteFile(f.out / L"keep.txt", "someone else's file");
        const ServerPackageResult r = BuildServerPackage(InputOf(f));
        c.Check(!r.ok && AnyContains(r.problems, "refusing") && ReadFileBytes(f.out / L"keep.txt") == "someone else's file",
                "a non-empty folder that is not a previous build is not replaced");
        fs::remove_all(f.out, ec);
        fs::create_directories(f.out, ec);
        c.Check(BuildServerPackage(InputOf(f)).ok, "an empty existing output folder is accepted");

        ServerPackageInput inside = InputOf(f);
        inside.outputDir = f.dist / L"server-build";
        const ServerPackageResult nested = BuildServerPackage(inside);
        c.Check(!nested.ok && AnyContains(nested.problems, "overlap") && !fs::exists(inside.outputDir, ec),
                "an output folder inside the export is rejected");
        ServerPackageInput parent = InputOf(f);
        parent.outputDir = f.root;
        c.Check(!BuildServerPackage(parent).ok && fs::exists(f.dist / L"Runtime.exe", ec),
                "an output folder that contains the export is rejected and the export survives");
    }

    // ---- 5. Debug の Server.exe は警告して続ける ----
    {
        const Fixture f = MakeFixture(root, L"Debug");
        const ServerPackageResult r = BuildServerPackage(InputOf(f));
        const nlohmann::json info = nlohmann::json::parse(ReadFileBytes(f.out / kServerBuildInfoName), nullptr, false);
        c.Check(r.ok && r.serverIsDebug && AnyContains(r.warnings, "Debug") && info.value("serverConfig", "") == "Debug",
                "a Debug exeDir builds with a warning and records Debug");
    }

    // ---- 6. 純関数 ----
    {
        const Fixture f = MakeFixture(root, L"Release");
        c.Check(FindVcRedistIn(f.vs) == f.vs / L"VC" / L"Redist" / L"MSVC" / L"14.51.36231" / L"vc_redist.x64.exe",
                "FindVcRedistIn picks the numerically newest version");
        fs::remove_all(f.vs / L"VC" / L"Redist" / L"MSVC" / L"14.9.1", ec);
        fs::remove_all(f.vs / L"VC" / L"Redist" / L"MSVC" / L"14.51.36231", ec);
        c.Check(FindVcRedistIn(f.vs) == f.vs / L"VC" / L"Redist" / L"MSVC" / L"v145" / L"vc_redist.x64.exe",
                "FindVcRedistIn falls back to a named folder (v145) when no numeric version exists");
        c.Check(FindVcRedistIn(f.root / L"nothing").empty(), "FindVcRedistIn returns empty when the folder does not exist");
        c.Check(IsDebugBuildDir(L"C:\\x\\bin\\x64\\Debug") && IsDebugBuildDir(L"C:\\x\\bin\\x64\\DEBUG\\")
                    && !IsDebugBuildDir(L"C:\\x\\bin\\x64\\Release") && !IsDebugBuildDir(L"C:\\x\\Debugger"),
                "IsDebugBuildDir looks at the folder name only");
    }

    fs::remove_all(root, ec);
    if (c.failCount == 0) {
        MYE_LOG_INFO("==== ServerPackage self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== ServerPackage self test: %d FAILED ====", c.failCount);
    }
    return c.failCount == 0;
}

bool RunServerPackageLocalSelfTest()
{
    MYE_LOG_INFO("==== ServerPackage local start check (M107b) self test ====");
    Counter c;
    std::error_code ec;
    const fs::path root = TempRoot(L"mye_gl_local");
    const fs::path realExeDir = fs::path(GetExecutableDir());
    const wchar_t* const needed[] = { L"Server.exe", L"libssl-3-x64.dll", L"libcrypto-3-x64.dll", L"GameLogic.dll" };
    bool haveReal = true;
    for (const wchar_t* name : needed) {
        haveReal &= fs::is_regular_file(realExeDir / name, ec);
    }

    // 失敗系は偽の Server.exe (cmd.exe) で: 起動はできるが exit 5 にも ready の行にもならない
    {
        fs::remove_all(root, ec);
        const fs::path fakeBuild = root / L"fake";
        const fs::path logPath = root / L"logs" / L"fake.log";
        fs::create_directories(fakeBuild, ec);
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        fs::copy_file(fs::path(sysDir) / L"cmd.exe", fakeBuild / L"Server.exe", ec);
        ServerLocalCheck check;
        c.Check(check.State() == ServerLocalCheckState::Idle, "a new check is Idle");
        const bool started = check.Start(fakeBuild, logPath);
        c.Check(started && check.State() == ServerLocalCheckState::Running, "a fake Server.exe starts (Running)");
        const ULONGLONG t0 = GetTickCount64();
        while (check.Poll() == ServerLocalCheckState::Running && GetTickCount64() - t0 < 60000) {
            Sleep(20);
        }
        c.Check(check.State() == ServerLocalCheckState::Failed && check.Summary().find("expected exit 5") != std::string::npos,
                "a Server.exe that neither exits 5 nor prints the ready line fails with the log tail");

        ServerLocalCheck missing;
        c.Check(!missing.Start(root / L"nothing", logPath) && missing.State() == ServerLocalCheckState::Failed,
                "a folder without Server.exe fails at Start");
        ServerLocalCheck inside;
        c.Check(!inside.Start(fakeBuild, fakeBuild / L"x.log") && inside.State() == ServerLocalCheckState::Failed,
                "a log path inside the build folder is rejected");

        ServerLocalCheck cancelled;
        if (cancelled.Start(fakeBuild, logPath)) {
            cancelled.Cancel();
        }
        c.Check(cancelled.State() == ServerLocalCheckState::Idle, "Cancel returns to Idle");
    }

    if (!haveReal) {
        MYE_LOG_WARN("  SKIP: real Server.exe / OpenSSL / GameLogic.dll are not next to Editor.exe (%s)",
                     WideToUtf8(realExeDir.wstring()).c_str());
    } else {
        // 実物の Server.exe + 小さな assets (シーン 1 枚 + manifest) + 実物の GameLogic.dll
        fs::remove_all(root, ec);
        const fs::path dist = root / L"dist";
        const fs::path vs = root / L"vs";
        WriteFile(dist / L"Runtime.exe", "runtime");
        fs::copy_file(realExeDir / L"GameLogic.dll", dist / L"GameLogic.dll", ec);
        WriteFile(dist / L"assets" / L"scenes" / L"main.scene.json", "{\"version\":4,\"entities\":[]}\n");
        WriteContentManifest((dist / L"assets" / kContentManifestName).wstring(), (dist / L"assets").wstring());
        WriteFile(vs / L"VC" / L"Redist" / L"MSVC" / L"14.0.0" / L"vc_redist.x64.exe", "vc");

        ServerPackageInput in;
        in.distDir = dist;
        in.exeDir = realExeDir;
        in.outputDir = root / L"proj" / L"cache" / L"gamelift" / L"server-build";
        in.vsInstallDirOverride = vs;
        const ServerPackageResult built = BuildServerPackage(in);
        c.Check(built.ok, "real Server.exe + tiny assets assemble");
        const auto filesBefore = ListFiles(in.outputDir);

        ServerLocalCheck check;
        const bool started = check.Start(in.outputDir, root / L"proj" / L"cache" / L"gamelift" / L"server-localcheck.log");
        c.Check(started, "the assembled Server.exe starts");
        const ULONGLONG t0 = GetTickCount64();
        while (check.Poll() == ServerLocalCheckState::Running && GetTickCount64() - t0 < 120000) {
            Sleep(20);
        }
        if (check.State() != ServerLocalCheckState::Passed) {
            MYE_LOG_ERROR("  local check summary: %s", check.Summary().c_str());
        }
        c.Check(check.State() == ServerLocalCheckState::Passed, "local start check passes (exit 5 + 'ready on UDP port')");
        c.Check(ListFiles(in.outputDir) == filesBefore, "the check leaves the build folder exactly as assembled (cache\\ etc. removed)");
    }

    fs::remove_all(root, ec);
    if (c.failCount == 0) {
        MYE_LOG_INFO("==== ServerPackage local self test: ALL PASS%s ====", haveReal ? "" : " (real-process part SKIPPED)");
    } else {
        MYE_LOG_ERROR("==== ServerPackage local self test: %d FAILED ====", c.failCount);
    }
    return c.failCount == 0;
}

} // namespace mye
