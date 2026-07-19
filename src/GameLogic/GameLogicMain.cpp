// GameLogic.dll — ホットリロード対象のユーザースクリプト DLL (engine_spec.md 8.4)。
//
// M4 で Shared\ScriptAPI.h / EngineAPI.h に基づく本実装に置き換える。
// 現状はビルドパイプライン (DLL 生成 + PDBALTPATH) の検証用プレースホルダ。
//
// DLL 境界規則:
//   - extern "C" の C ABI のみ境界を越える (C++ クラス / STL / 例外は禁止)
//   - メモリ確保は必ずエンジン側アロケータを使う (DLL 側 CRT ヒープ依存禁止)

extern "C" __declspec(dllexport) unsigned int GameLogic_ApiVersion(void)
{
    return 0; // M4 で Shared\EngineAPI.h の MYE_API_VERSION を返すようにする
}
