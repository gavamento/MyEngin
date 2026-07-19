// GameLogic.dll — ホットリロード対象のユーザースクリプト DLL (engine_spec.md 8.4)。
//
// DLL 境界規則:
//   - extern "C" の C ABI のみ境界を越える (C++ クラス / STL / 例外は禁止)
//   - スクリプト状態はエンジン側 ECS に保持され、この DLL はロジックのみを持つ
//   - global / static 変数の永続は保証しない (プロジェクト規約で使用禁止)

#include "Shared/ScriptAPI.h"

extern "C" __declspec(dllexport) const MyeScriptModule* GameLogic_GetModule(const MyeEngineApi* api)
{
    (void)api; // 必要ならここで保持できるが、規約上 ctx.api を使うこと
    static MyeScriptModule mod = {};
    mod.apiVersion = MYE_API_VERSION;
    mod.scripts = mye_script_detail::Registry().data();
    mod.scriptCount = static_cast<uint32_t>(mye_script_detail::Registry().size());
    return &mod;
}

extern "C" __declspec(dllexport) unsigned int GameLogic_ApiVersion(void)
{
    return MYE_API_VERSION;
}
