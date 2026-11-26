//====================================================================================
//                          StopRequest.cpp
//  MyEngin/ 秋田蓮音                                                     11/27/2026
//                                          サーバの停止要求の受け口の実装
//====================================================================================
#include "Server/StopRequest.h"

#include <atomic>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// コンソールのハンドラは別スレッドから呼ばれる。立てるのはこのフラグだけで、処理はメインスレッドが行う
std::atomic<bool> g_consoleStop{ false };

BOOL WINAPI ConsoleHandler(DWORD)
{
    g_consoleStop.store(true);
    return TRUE; // 既定の即時終了を止める (記録 .rep を閉じてから自分で抜ける)
}

} // namespace

StopRequest::~StopRequest()
{
    if (handlerInstalled_) {
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    }
    if (event_ != nullptr) {
        CloseHandle(event_);
    }
}

bool StopRequest::InstallConsoleHandler()
{
    if (handlerInstalled_) {
        return true;
    }
    g_consoleStop.store(false);
    handlerInstalled_ = SetConsoleCtrlHandler(ConsoleHandler, TRUE) != FALSE;
    if (!handlerInstalled_) {
        MYE_LOG_WARN("[server] could not install the console handler; Ctrl+C will end the process abruptly");
    }
    return handlerInstalled_;
}

bool StopRequest::OpenEvent(const std::wstring& name)
{
    if (event_ != nullptr || name.empty()) {
        return false;
    }
    // 待つだけなので SYNCHRONIZE があればよい (立てる側は別プロセス)
    event_ = ::OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    return event_ != nullptr;
}

bool StopRequest::Requested() const
{
    if (!latched_) {
        latched_ = (handlerInstalled_ && g_consoleStop.load()) || (event_ != nullptr && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0);
    }
    return latched_;
}

void StopRequest::Request()
{
    latched_ = true;
}

} // namespace mye
