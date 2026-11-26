//====================================================================================
//                          ServerPackage.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 用サーバ版の組み立てと局所起動確認の実装
//====================================================================================
#include "Editor/Tools/GameLift/ServerPackage.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include <winsock2.h>
#include <Windows.h>

#include "Editor/App/ChildProcess.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Session/Provenance.h"
#include "Engine/Platform/PathUtil.h"
#include "nlohmann/json.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

namespace mye {
namespace {

constexpr const char* kInstallBatText =
    "vc_redist.x64.exe /install /quiet /norestart /log C:\\game\\install_vcredist.log\r\n"
    "exit /b 0\r\n";

constexpr DWORD kVswhereTimeoutMs = 20000;
constexpr int kLocalCheckServerTimeoutSec = 3;     // Server.exe の --server-timeout (exit 5 になる)
constexpr uint64_t kLocalCheckHardTimeoutMs = 60000; // これを過ぎても終わらなければ子を終了して失敗
constexpr int kLogTailLines = 8;
constexpr const char* kReadyLine = "ready on UDP port";
constexpr uint32_t kServerTimeoutExitCode = 5;

std::string Utf8(const fs::path& p)
{
    return WideToUtf8(p.wstring());
}

// 小文字・'\' 区切り・末尾の '\' なしの比較用キー
std::wstring PathKey(const fs::path& p)
{
    std::wstring key = NormalizePathKey(p.wstring());
    while (key.size() > 1 && key.back() == L'\\') {
        key.pop_back();
    }
    return key;
}

bool IsSameOrUnder(const fs::path& child, const fs::path& parent)
{
    const std::wstring c = PathKey(child);
    const std::wstring p = PathKey(parent);
    return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == L'\\');
}

std::string HexHash(uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string UtcNowText()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc = {};
    gmtime_s(&utc, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

// "14.51.36231" → {14, 51, 36231}。数字とドットだけでなければ false
bool ParseDottedVersion(const std::wstring& text, std::vector<uint64_t>& out)
{
    out.clear();
    uint64_t current = 0;
    bool haveDigit = false;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            current = current * 10 + static_cast<uint64_t>(c - L'0');
            haveDigit = true;
        } else if (c == L'.' && haveDigit) {
            out.push_back(current);
            current = 0;
            haveDigit = false;
        } else {
            return false;
        }
    }
    if (!haveDigit) {
        return false;
    }
    out.push_back(current);
    return true;
}

// 前回の組み立て結果 (mye_server_build.json を持つ) か、空のフォルダだけを入れ替えの対象にする
bool IsReplaceableOutput(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return true;
    }
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    if (fs::is_regular_file(dir / kServerBuildInfoName, ec)) {
        return true;
    }
    return fs::is_empty(dir, ec) && !ec;
}

// root 以下の全ファイル・全フォルダの相対パス (小文字・'\' 区切り、整列済み)
std::vector<std::wstring> CollectRelativePaths(const fs::path& root)
{
    std::vector<std::wstring> paths;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::wstring rel = it->path().lexically_relative(root).wstring();
        for (wchar_t& c : rel) {
            c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));
        }
        paths.push_back(std::move(rel));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string ReadWholeFileShared(const fs::path& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        text.append(buf, got);
    }
    CloseHandle(h);
    return text;
}

std::string LastLines(const std::string& text, int count)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    std::string out;
    const size_t begin = lines.size() > static_cast<size_t>(count) ? lines.size() - static_cast<size_t>(count) : 0;
    for (size_t i = begin; i < lines.size(); ++i) {
        out += "  | " + lines[i] + "\n";
    }
    return out;
}

// bind(0) で OS に空きポートを選ばせて返す。失敗は 0 (閉じてから使うので取り合いは起こりうるが、起動確認は 1 回で足りる)
uint16_t FindFreeUdpPort()
{
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }
    uint16_t port = 0;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        int len = sizeof(addr);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
            && getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}

std::wstring QuoteArg(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

} // namespace

std::string MakeServerInstallBat()
{
    return kInstallBatText;
}

bool IsDebugBuildDir(const fs::path& exeDir)
{
    std::wstring leaf = exeDir.has_filename() ? exeDir.filename().wstring() : exeDir.parent_path().filename().wstring();
    for (wchar_t& c : leaf) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return leaf == L"debug";
}

fs::path FindVcRedistIn(const fs::path& vsInstallDir)
{
    std::error_code ec;
    const fs::path root = vsInstallDir / L"VC" / L"Redist" / L"MSVC";
    fs::path bestNumeric;
    std::vector<uint64_t> bestVersion;
    fs::path bestNamed; // v145 のように数値でない名前 (数値の版が無いときだけ使う)
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const fs::path candidate = it->path() / kVcRedistFileName;
        if (!fs::is_regular_file(candidate, ec)) {
            continue;
        }
        std::vector<uint64_t> version;
        if (ParseDottedVersion(it->path().filename().wstring(), version)) {
            if (bestNumeric.empty() || version > bestVersion) {
                bestNumeric = candidate;
                bestVersion = version;
            }
        } else if (bestNamed.empty() || candidate.wstring() > bestNamed.wstring()) {
            bestNamed = candidate;
        }
    }
    return bestNumeric.empty() ? bestNamed : bestNumeric;
}

fs::path FindVsInstallDirViaVswhere()
{
    wchar_t pf86[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    const fs::path vswhere = fs::path(pf86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) {
        return {};
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        return {};
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); // 子へ渡すのは書き込み側だけ
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nulIn;
    std::wstring cmdline = QuoteArg(vswhere.wstring()) + L" -latest -property installationPath";
    std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
    buffer.push_back(L'\0');
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (nulIn != INVALID_HANDLE_VALUE) {
        CloseHandle(nulIn);
    }
    std::string text;
    if (started) {
        CloseHandle(pi.hThread);
        // 出力は 1 行なのでパイプの容量 (4 KB) を超えない。終わるまで待ってから読む
        if (WaitForSingleObject(pi.hProcess, kVswhereTimeoutMs) == WAIT_OBJECT_0) {
            char buf[1024];
            DWORD got = 0;
            while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
                text.append(buf, got);
            }
        } else {
            TerminateProcess(pi.hProcess, 1);
            MYE_LOG_WARN("[gamelift] vswhere did not finish within %lu ms", kVswhereTimeoutMs);
        }
        CloseHandle(pi.hProcess);
    }
    CloseHandle(readEnd);

    const size_t eol = text.find_first_of("\r\n");
    if (eol != std::string::npos) {
        text.resize(eol);
    }
    return text.empty() ? fs::path() : fs::path(Utf8ToWide(text));
}

ServerPackageResult BuildServerPackage(const ServerPackageInput& in)
{
    ServerPackageResult r;
    r.outputDir = in.outputDir;
    std::error_code ec;
    const auto problem = [&](std::string text) { r.problems.push_back(std::move(text)); };
    const auto requireFile = [&](const fs::path& p, const char* what) -> bool {
        if (fs::is_regular_file(p, ec)) {
            return true;
        }
        problem(std::string("missing ") + what + ": " + Utf8(p));
        return false;
    };

    // ---- 1. 入力の点検 (足りないものを全部挙げる) ----
    const fs::path distAssets = in.distDir / L"assets";
    const fs::path distManifest = distAssets / kContentManifestName;
    if (in.distDir.empty() || !fs::is_directory(in.distDir, ec)) {
        problem("exported folder not found: " + Utf8(in.distDir));
    } else {
        requireFile(in.distDir / L"Runtime.exe", "Runtime.exe in the exported folder (is this a BuildSettings output?)");
        requireFile(in.distDir / L"GameLogic.dll", "GameLogic.dll in the exported folder");
        if (!fs::is_directory(distAssets, ec)) {
            problem("missing assets folder in the exported folder: " + Utf8(distAssets));
        } else if (requireFile(distManifest, "content_manifest.json in the exported assets folder")) {
            uint64_t hash = 0;
            if (ReadContentManifest(distManifest.wstring(), hash)) {
                r.contentHash = hash;
            } else {
                problem("content_manifest.json is unreadable or inconsistent: " + Utf8(distManifest));
            }
        }
    }
    requireFile(in.exeDir / L"Server.exe", "Server.exe next to Editor.exe");
    requireFile(in.exeDir / L"libssl-3-x64.dll", "libssl-3-x64.dll next to Editor.exe");
    requireFile(in.exeDir / L"libcrypto-3-x64.dll", "libcrypto-3-x64.dll next to Editor.exe");

    fs::path vcRedist;
    {
        const fs::path vsDir = in.vsInstallDirOverride.empty() ? FindVsInstallDirViaVswhere() : in.vsInstallDirOverride;
        if (vsDir.empty()) {
            problem("Visual Studio not found (vswhere): vc_redist.x64.exe cannot be located");
        } else {
            vcRedist = FindVcRedistIn(vsDir);
            if (vcRedist.empty()) {
                problem("vc_redist.x64.exe not found under " + Utf8(vsDir / L"VC" / L"Redist" / L"MSVC"));
            }
        }
    }

    if (in.outputDir.empty()) {
        problem("output folder is not specified");
    } else if (!in.distDir.empty() && (IsSameOrUnder(in.outputDir, in.distDir) || IsSameOrUnder(in.distDir, in.outputDir))) {
        problem("output folder and exported folder overlap: " + Utf8(in.outputDir));
    } else if (!IsReplaceableOutput(in.outputDir)) {
        problem("output folder exists and is not a previous server build (refusing to replace it): " + Utf8(in.outputDir));
    }
    if (!r.problems.empty()) {
        return r;
    }

    // ---- 2. 一時フォルダへ組む ----
    static std::atomic<uint64_t> nextStage{ 0 };
    const std::wstring suffix = L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++nextStage);
    const fs::path staging = fs::path(in.outputDir.wstring() + L".staging" + suffix);
    const fs::path retired = fs::path(in.outputDir.wstring() + L".old" + suffix);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        problem("cannot create staging folder " + Utf8(staging) + ": " + ec.message());
        return r;
    }

    bool copied = true;
    const auto copyFile = [&](const fs::path& from, const fs::path& to) {
        std::error_code e;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
        if (e) {
            problem("copy failed: " + Utf8(from) + " (" + e.message() + ")");
            copied = false;
        }
    };
    const auto copyTree = [&](const fs::path& from, const fs::path& to) {
        std::error_code e;
        fs::create_directories(to.parent_path(), e);
        if (!e) {
            fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, e);
        }
        if (e) {
            problem("copy failed: " + Utf8(from) + " (" + e.message() + ")");
            copied = false;
        }
    };

    copyFile(in.exeDir / L"Server.exe", staging / L"Server.exe");
    copyFile(in.exeDir / L"libssl-3-x64.dll", staging / L"libssl-3-x64.dll");
    copyFile(in.exeDir / L"libcrypto-3-x64.dll", staging / L"libcrypto-3-x64.dll");
    copyFile(in.distDir / L"GameLogic.dll", staging / L"GameLogic.dll");
    copyFile(vcRedist, staging / kVcRedistFileName);
    // assets\ は manifest を含めそのまま写す (焼き直さない = 書き出しと contentHash が一致する)
    copyTree(distAssets, staging / L"assets");
    const fs::path distCooked = in.distDir / L"cache" / L"cooked";
    if (fs::is_directory(distCooked, ec)) {
        copyTree(distCooked, staging / L"cache" / L"cooked");
    }

    r.gameLogicHash = HashFileBytes((in.distDir / L"GameLogic.dll").wstring());
    r.serverIsDebug = IsDebugBuildDir(in.exeDir);
    if (copied) {
        std::ofstream bat(staging / kServerBuildInstallBatName, std::ios::binary | std::ios::trunc);
        bat << MakeServerInstallBat();
        bat.close();
        const nlohmann::json info = {
            { "version", 1 },
            { "sourceDist", Utf8(fs::absolute(in.distDir, ec)) },
            { "builtAtUtc", UtcNowText() },
            { "serverConfig", r.serverIsDebug ? "Debug" : "Release" },
            { "contentHash", HexHash(r.contentHash) },
            { "gameLogicHash", HexHash(r.gameLogicHash) },
        };
        std::ofstream json(staging / kServerBuildInfoName, std::ios::binary | std::ios::trunc);
        json << info.dump(1, '\t') << '\n';
        json.close();
        if (!bat || !json) {
            problem("cannot write install.bat / mye_server_build.json into " + Utf8(staging));
            copied = false;
        }
    }
    if (!copied) {
        fs::remove_all(staging, ec);
        return r;
    }

    for (fs::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (it->is_regular_file(e)) {
            ++r.fileCount;
            r.totalBytes += it->file_size(e);
        }
    }

    // ---- 3. 入れ替え (旧を退避 → 新を置く。どちらかで失敗したら元へ戻す) ----
    bool retiredOld = false;
    if (fs::exists(in.outputDir, ec)) {
        fs::rename(in.outputDir, retired, ec);
        if (ec) {
            problem("cannot replace the output folder (is a file in it in use?): " + Utf8(in.outputDir) + " (" + ec.message() + ")");
            fs::remove_all(staging, ec);
            return r;
        }
        retiredOld = true;
    }
    fs::rename(staging, in.outputDir, ec);
    if (ec) {
        const std::string why = ec.message();
        if (retiredOld) {
            fs::rename(retired, in.outputDir, ec);
        }
        fs::remove_all(staging, ec);
        problem("cannot move the new build into place: " + Utf8(in.outputDir) + " (" + why + ")");
        return r;
    }
    if (retiredOld) {
        fs::remove_all(retired, ec);
        if (ec) {
            r.warnings.push_back("could not delete the previous build: " + Utf8(retired));
        }
    }

    if (r.serverIsDebug) {
        r.warnings.push_back("Server.exe is a Debug build (" + Utf8(in.exeDir) + "): it runs but slowly; use Release for a real fleet");
    }
    r.ok = true;
    return r;
}

// ---- ServerLocalCheck ----

ServerLocalCheck::~ServerLocalCheck()
{
    Cancel();
}

void ServerLocalCheck::Finish(ServerLocalCheckState state, std::string summary)
{
    state_ = state;
    summary_ = std::move(summary);
    if (state == ServerLocalCheckState::Failed) {
        MYE_LOG_ERROR("[gamelift] local check failed: %s", summary_.c_str());
    } else {
        MYE_LOG_INFO("[gamelift] local check: %s", summary_.c_str());
    }
}

bool ServerLocalCheck::Start(const fs::path& buildDir, const fs::path& logPath)
{
    Cancel();
    state_ = ServerLocalCheckState::Idle;
    summary_.clear();
    buildDir_ = fs::absolute(buildDir);
    logPath_ = fs::absolute(logPath);

    std::error_code ec;
    if (!fs::is_regular_file(buildDir_ / L"Server.exe", ec)) {
        Finish(ServerLocalCheckState::Failed, "Server.exe not found in " + Utf8(buildDir_));
        return false;
    }
    if (IsSameOrUnder(logPath_, buildDir_)) {
        Finish(ServerLocalCheckState::Failed, "the log file must be outside the build folder: " + Utf8(logPath_));
        return false;
    }
    const uint16_t port = FindFreeUdpPort();
    if (port == 0) {
        Finish(ServerLocalCheckState::Failed, "no free UDP port");
        return false;
    }
    fs::create_directories(logPath_.parent_path(), ec);
    existingPaths_ = CollectRelativePaths(buildDir_);

    const std::wstring cmdline = QuoteArg((buildDir_ / L"Server.exe").wstring()) + L" --hosting local --port "
        + std::to_wstring(port) + L" --server-timeout " + std::to_wstring(kLocalCheckServerTimeoutSec);
    process_ = StartChildProcess(cmdline, buildDir_.wstring(), logPath_.wstring(), "[gamelift]", " (local check)");
    if (process_ == nullptr) {
        Finish(ServerLocalCheckState::Failed, "could not start Server.exe in " + Utf8(buildDir_));
        return false;
    }
    startedMs_ = GetTickCount64();
    state_ = ServerLocalCheckState::Running;
    MYE_LOG_INFO("[gamelift] local check started: Server.exe --hosting local --port %u (cwd %s)",
                 static_cast<unsigned>(port), Utf8(buildDir_).c_str());
    return true;
}

ServerLocalCheckState ServerLocalCheck::Poll()
{
    if (state_ != ServerLocalCheckState::Running) {
        return state_;
    }
    uint32_t exitCode = 0;
    if (PollChildProcess(process_, exitCode)) {
        CloseChildProcess(process_);
        process_ = nullptr;
        const std::string log = ReadWholeFileShared(logPath_);
        const bool ready = log.find(kReadyLine) != std::string::npos;
        RemoveCreatedFiles();
        if (exitCode == kServerTimeoutExitCode && ready) {
            Finish(ServerLocalCheckState::Passed, "Server.exe exited with 5 (--server-timeout) after 'ready on UDP port'");
        } else {
            Finish(ServerLocalCheckState::Failed,
                   "Server.exe exited with " + std::to_string(exitCode) + (ready ? " (ready line seen)" : " (no 'ready on UDP port' line)")
                       + ", expected exit 5 after the ready line. Log tail (" + Utf8(logPath_) + "):\n" + LastLines(log, kLogTailLines));
        }
    } else if (GetTickCount64() - startedMs_ > kLocalCheckHardTimeoutMs) {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 5000);
        CloseChildProcess(process_);
        process_ = nullptr;
        const std::string log = ReadWholeFileShared(logPath_);
        RemoveCreatedFiles();
        Finish(ServerLocalCheckState::Failed,
               "Server.exe did not exit within " + std::to_string(kLocalCheckHardTimeoutMs / 1000) + " s. Log tail:\n"
                   + LastLines(log, kLogTailLines));
    }
    return state_;
}

void ServerLocalCheck::Cancel()
{
    if (process_ == nullptr) {
        return;
    }
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 5000);
    CloseChildProcess(process_);
    process_ = nullptr;
    RemoveCreatedFiles();
    state_ = ServerLocalCheckState::Idle;
    summary_ = "cancelled";
}

// 子 (Server.exe) は作業ディレクトリの下へ cache\ や .meta を作る。そのまま AWS へ上げると混ざるので、
// 起動前に無かったものだけを消して組み立て直後の中身へ戻す
void ServerLocalCheck::RemoveCreatedFiles()
{
    std::vector<fs::path> created;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(buildDir_, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::wstring rel = it->path().lexically_relative(buildDir_).wstring();
        for (wchar_t& c : rel) {
            c = (c == L'/') ? L'\\' : static_cast<wchar_t>(towlower(c));
        }
        if (!std::binary_search(existingPaths_.begin(), existingPaths_.end(), rel)) {
            created.push_back(it->path());
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
        }
    }
    for (const fs::path& p : created) {
        fs::remove_all(p, ec);
        if (ec) {
            MYE_LOG_WARN("[gamelift] could not remove %s created by the local check: %s", Utf8(p).c_str(),
                         ec.message().c_str());
        }
    }
}

// ---- CLI ----

int RunServerBuildCli(const std::wstring& projectDir, const std::wstring& distDir)
{
    constexpr const char* kTag = "[gamelift-server-build]";
    std::error_code ec;
    if (projectDir.empty() || !fs::is_directory(projectDir, ec)) {
        std::fprintf(stderr, "%s FAILED: --project <dir> is required and must exist\n", kTag);
        return 1;
    }
    const fs::path project = fs::absolute(projectDir, ec);
    ServerPackageInput input;
    input.distDir = fs::absolute(distDir, ec);
    input.exeDir = fs::path(GetExecutableDir());
    input.outputDir = project / L"cache" / L"gamelift" / L"server-build";

    const ServerPackageResult built = BuildServerPackage(input);
    for (const std::string& warning : built.warnings) {
        std::fprintf(stderr, "%s WARN: %s\n", kTag, warning.c_str());
    }
    if (!built.ok) {
        std::string joined;
        for (const std::string& p : built.problems) {
            std::fprintf(stderr, "%s   - %s\n", kTag, p.c_str());
            joined += (joined.empty() ? "" : "; ") + p;
        }
        std::printf("%s FAILED: %zu problem(s): %s\n", kTag, built.problems.size(), joined.c_str());
        return 1;
    }

    ServerLocalCheck check;
    const fs::path logPath = project / L"cache" / L"gamelift" / L"server-localcheck.log";
    if (check.Start(input.outputDir, logPath)) {
        while (check.Poll() == ServerLocalCheckState::Running) {
            Sleep(50);
        }
    }
    if (check.State() != ServerLocalCheckState::Passed) {
        std::fprintf(stderr, "%s   - local check: %s\n", kTag, check.Summary().c_str());
        std::printf("%s FAILED: built %s but the local start check failed\n", kTag, Utf8(input.outputDir).c_str());
        return 1;
    }
    std::printf("%s OK: %s (%u files, %.1f MB, contentHash %s, local start check passed)\n", kTag,
                Utf8(input.outputDir).c_str(), built.fileCount,
                static_cast<double>(built.totalBytes) / (1024.0 * 1024.0), HexHash(built.contentHash).c_str());
    return 0;
}

} // namespace mye
