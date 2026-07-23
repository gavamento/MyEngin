#pragma once
#include <cstdint>
#include <string>

namespace mye {
namespace importmeta {

// テクスチャのインポート設定 (.meta v2、M39b)。
// 既定値 = 従来挙動 (srgb はロードサイトのヒント / mips 有 / Cook は BCn 自動)。
// wrap (サンプラアドレス) はサンプラがパス単位のため v1 では見送り。
struct TextureImportSettings {
    int32_t srgb = 0;         // 0=auto (ロードサイトのヒント) / 1=on (_SRGB) / 2=off (UNORM)
    int32_t generateMips = 1; // 0=off (mip0 のみ) / 1=on (フルチェーン)
    int32_t compress = 0;     // Cook to DDS: 0=auto (BC1/BC3 自動) / 1=none (RGBA8 非圧縮)
};

// パス → インポート設定解決のグローバルフック (assetkey::/assetguid:: と同じ流儀)。
// TextureLibrary (Renderer 層) が AssetDatabase (Engine 層) を参照せずに .meta の
// 設定を引くための関数ポインタ注入。AssetDatabase::InstallAsKeyResolver が接続する。
// 戻り値 false = .meta 無し等で未解決 — 呼び出し側は既定値 (= 従来挙動) を使う。
// スレッド規約: Install/Resolve はメインスレッド専用 (LoadFile/PollAsyncLoads と同じ)。
using ResolveFn = bool (*)(void* user, const std::wstring& path, TextureImportSettings& out);

void Install(ResolveFn fn, void* user); // fn=null で既定 (常に未解決) に戻す
bool Resolve(const std::wstring& path, TextureImportSettings& out);

} // namespace importmeta
} // namespace mye
