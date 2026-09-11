#include "Engine/Core/AssetKeyResolver.h"

#include <cstdio>

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

std::string SubAssetKeyPrefix(const std::wstring& modelPath)
{
    const uint64_t guid = Resolve(NormalizePathKey(modelPath));
    char buf[32];
    // ★16 桁固定・小文字。.meta の "guid" と同じ綴りにしておくと、ログのキーから .meta を grep できる
    std::snprintf(buf, sizeof(buf), "%.*s%016llx", static_cast<int>(kSubAssetKeyScheme.size()),
                  kSubAssetKeyScheme.data(), static_cast<unsigned long long>(guid));
    return std::string(buf);
}

bool ParseSubAssetKey(std::string_view key, uint64_t& guidOut)
{
    const size_t n = kSubAssetKeyScheme.size();
    if (key.size() < n + 16 || key.substr(0, n) != kSubAssetKeyScheme) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = n; i < n + 16; ++i) {
        const char c = key[i];
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) {
            return false;
        }
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    // 接頭辞の直後はキーの終端か '#' (サフィックス) だけ — "guid://<16hex>xyz" を GUID と誤読しない
    if (key.size() > n + 16 && key[n + 16] != '#') {
        return false;
    }
    guidOut = v;
    return true;
}

} // namespace assetkey
} // namespace mye
