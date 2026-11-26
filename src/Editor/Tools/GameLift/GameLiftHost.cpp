//====================================================================================
//                          GameLiftHost.cpp
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          GameLift 窓の手順が外界に触る口の実装 (aws CLI・クライアント起動・aws login)
//====================================================================================
#include "Editor/Tools/GameLift/GameLiftHost.h"

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// CreateProcessW で独立したプロセスを起こす。ハンドルは継がせず、起動したら閉じる
bool SpawnDetached(std::wstring commandLine, const std::wstring& workDir, DWORD flags, std::string& error)
{
    commandLine.push_back(L'\0'); // CreateProcessW は書き換えうるので可変バッファで渡す
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &startup, &info)) {
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

} // namespace

bool ProcessAwsCliRunner::Locate(std::wstring& awsPath)
{
    awsPath_ = FindAwsExecutable(std::wstring());
    awsPath = awsPath_;
    return !awsPath_.empty();
}

bool ProcessAwsCliRunner::Start(const AwsCall& call, uint32_t timeoutMs, std::string& error)
{
    if (awsPath_.empty()) {
        error = "aws.exe was not found";
        return false;
    }
    CliRunSpec spec;
    spec.exePath = awsPath_;
    spec.args = call.args;
    spec.timeoutMs = timeoutMs;
    spec.secret = call.secret;
    return process_.Start(spec, error);
}

bool LocalProcessHost::LaunchClient(const ClientLaunchSpec& spec, std::string& error)
{
    if (spec.exePath.empty()) {
        error = "exe path is empty";
        return false;
    }
    const bool ok = SpawnDetached(BuildCommandLine(spec.exePath, spec.args), spec.workDir, 0, error);
    if (ok) {
        MYE_LOG_INFO("[gamelift] client launched (%s)", WideToUtf8(spec.exePath).c_str());
    }
    return ok;
}

bool OpenAwsLoginConsole(const std::wstring& awsPath, std::string& error)
{
    if (awsPath.empty()) {
        error = "aws.exe was not found";
        return false;
    }
    // cmd /s /c "<外側の引用符を剥がす> "aws.exe" login & pause" — pause で結果の表示を読ませてから閉じる
    const std::wstring commandLine = L"cmd.exe /d /s /c \"\"" + awsPath + L"\" login & pause\"";
    return SpawnDetached(commandLine, std::wstring(), CREATE_NEW_CONSOLE, error);
}

} // namespace mye
