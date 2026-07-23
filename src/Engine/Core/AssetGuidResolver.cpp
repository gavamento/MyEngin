#include "Engine/Core/AssetGuidResolver.h"

namespace mye {
namespace assetguid {
namespace {

ResolvePathFn g_fn = nullptr;
void* g_user = nullptr;

} // namespace

void Install(ResolvePathFn fn, void* user)
{
    g_fn = fn;
    g_user = user;
}

std::wstring ResolvePath(uint64_t guid)
{
    if (g_fn && guid != 0) {
        return g_fn(g_user, guid);
    }
    return {}; // 既定 = 未解決 (呼び出し側がフォールバック)
}

} // namespace assetguid
} // namespace mye
