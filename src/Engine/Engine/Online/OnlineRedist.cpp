//====================================================================================
//                          OnlineRedist.cpp
//  MyEngin/ 秋田蓮音                                                     11/25/2026
//                                          オンライン実装がゲームの配布物に要求する実行時ファイル
//====================================================================================
#include "Engine/Engine/Online/OnlineRedist.h"

#include "Engine/Engine/Online/Eos/EosRedist.h"

namespace mye {

std::span<const OnlineRedistFile> OnlineRedistFiles(OnlineBackend backend)
{
    switch (backend) {
    case OnlineBackend::Eos:
        return EosRedistFiles();
    case OnlineBackend::Local:
    case OnlineBackend::None:
        break;
    }
    return {};
}

} // namespace mye
