//====================================================================================
//                          LocalHosting.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ローカル実行用のホスティングの実装
//====================================================================================
#include "Server/Hosting/LocalHosting.h"

#include <atomic>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// コンソールのハンドラは別スレッドから呼ばれる。立てるのはこのフラグだけで、処理は Poll (メインスレッド) で行う
std::atomic<bool> g_terminateRequested{ false };

BOOL WINAPI ConsoleHandler(DWORD)
{
    g_terminateRequested.store(true);
    return TRUE; // 既定の即時終了を止める (記録 .rep を閉じてから自分で抜ける)
}

} // namespace

LocalHosting::~LocalHosting()
{
    Shutdown();
}

bool LocalHosting::Init()
{
    g_terminateRequested.store(false);
    handlerInstalled_ = SetConsoleCtrlHandler(ConsoleHandler, TRUE) != FALSE;
    if (!handlerInstalled_) {
        MYE_LOG_WARN("[hosting] could not install the console handler; Ctrl+C will end the process abruptly");
    }
    return true;
}

bool LocalHosting::NotifyReady(uint16_t port, const std::vector<std::wstring>&)
{
    ready_ = true;
    MYE_LOG_INFO("[hosting] local: ready on UDP port %u (a session starts immediately)", port);
    return true;
}

void LocalHosting::Poll(std::vector<HostingEvent>& out)
{
    if (ready_ && !startSent_) {
        startSent_ = true;
        HostingEvent e;
        e.kind = HostingEventKind::StartSession;
        out.push_back(e);
    }
    if (g_terminateRequested.load() && !terminateSent_) {
        terminateSent_ = true;
        HostingEvent e;
        e.kind = HostingEventKind::Terminate;
        out.push_back(e);
    }
}

void LocalHosting::Shutdown()
{
    if (handlerInstalled_) {
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        handlerInstalled_ = false;
    }
}

} // namespace mye
