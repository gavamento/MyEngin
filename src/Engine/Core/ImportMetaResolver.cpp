#include "Engine/Core/ImportMetaResolver.h"

namespace mye {
namespace importmeta {
namespace {

ResolveFn g_fn = nullptr;
void* g_user = nullptr;

} // namespace

void Install(ResolveFn fn, void* user)
{
    g_fn = fn;
    g_user = user;
}

bool Resolve(const std::wstring& path, TextureImportSettings& out)
{
    if (g_fn) {
        return g_fn(g_user, path, out);
    }
    return false; // 既定 = 未解決 (呼び出し側が既定値 = 従来挙動)
}

} // namespace importmeta
} // namespace mye
