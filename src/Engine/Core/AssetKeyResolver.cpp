#include "Engine/Core/AssetKeyResolver.h"

#include "Engine/Core/Hash.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace assetkey {
namespace {

ResolverFn g_fn = nullptr;
void* g_user = nullptr;

} // namespace

void Install(ResolverFn fn, void* user)
{
    g_fn = fn;
    g_user = user;
}

uint64_t Resolve(const std::wstring& normalizedPath)
{
    if (g_fn) {
        return g_fn(g_user, normalizedPath);
    }
    return HashStr(WideToUtf8(normalizedPath)); // 既定 = 従来の path-hash
}

} // namespace assetkey
} // namespace mye
