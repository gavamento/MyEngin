#include "Engine/Core/Asset/ImportMetaResolver.h"

namespace mye {
namespace importmeta {
namespace {

ResolveFn g_fn = nullptr;
void* g_user = nullptr;
ModelLodResolveFn g_lodFn = nullptr;
void* g_lodUser = nullptr;

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
    return false; // 既定 = 未解決 (呼び出し側が既定値を使う)
}

void InstallModelLod(ModelLodResolveFn fn, void* user)
{
    g_lodFn = fn;
    g_lodUser = user;
}

bool ResolveModelLod(const std::wstring& path, ModelLodSettings& out)
{
    out = ModelLodSettings{};
    if (g_lodFn && g_lodFn(g_lodUser, path, out)) {
        out.Normalize();
        return out.levels > 0;
    }
    return false;
}

} // namespace importmeta
} // namespace mye
