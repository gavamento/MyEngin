//====================================================================================
//                          LocalHosting.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          ローカル実行用のホスティングの実装
//====================================================================================
#include "Server/Hosting/LocalHosting.h"

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

bool LocalHosting::Init()
{
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
}

} // namespace mye
