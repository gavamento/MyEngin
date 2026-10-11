//====================================================================================
//                          KernelHash.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネルのソースハッシュの実装
//====================================================================================
#include "Engine/Engine/Compute/KernelHash.h"

#include "Engine/Core/Util/Hash.h"

namespace mye::kernel {

uint64_t HashKernelSource(std::string_view text)
{
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (text.substr(0, kBom.size()) == kBom) {
        text.remove_prefix(kBom.size());
    }
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        h ^= static_cast<uint8_t>(text[i]);
        h *= kFnvPrime;
    }
    return h;
}

} // namespace mye::kernel
