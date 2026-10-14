//====================================================================================
//                          OnlineFactory.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          設定に応じたオンラインサービスの実装を作る
//====================================================================================
#include "Engine/Engine/Online/IOnlineService.h"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Online/Eos/EosOnlineService.h"
#include "Engine/Engine/Online/LocalOnlineService.h"

namespace mye {

std::unique_ptr<IOnlineService> CreateOnlineService(OnlineBackend backend)
{
    switch (backend) {
    case OnlineBackend::None:
        return nullptr;
    case OnlineBackend::Local:
        return std::make_unique<LocalOnlineService>();
    case OnlineBackend::Eos:
        // SDK の有無は build\Common.props の MyeHasEos が external\eos-sdk を見て決める (MYE_HAS_EOS)
        if (EosAvailableInThisBuild()) {
            return CreateEosOnlineService();
        }
        MYE_LOG_WARN("[online] online.backend is \"eos\" but this build has no EOS SDK "
                     "(put the SDK in external\\eos-sdk and rebuild; see external\\eos-sdk\\BUILD.md)");
        return nullptr;
    }
    return nullptr;
}

} // namespace mye
