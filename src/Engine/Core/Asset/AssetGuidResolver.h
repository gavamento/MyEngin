#pragma once
#include <cstdint>
#include <string>

namespace mye {
namespace assetguid {

// GUID → 現在パス解決のグローバルフック (M39a、assetkey::Resolve の逆方向)。
// .mat.json / .controller.json のサブ参照 GUID 化で、Renderer/Engine のローダが
// AssetDatabase (Engine 層) を参照せずに GUID からファイルパスを引くために使う。
// 既定 (フック未設定) = 空文字列 — 呼び出し側は「未解決」としてフォールバックする。
// 層規約: Core は Engine を知らない — 関数ポインタ注入で逆依存を回避 (assetkey:: と同じ流儀)。
// AssetDatabase::InstallAsKeyResolver が assetkey と同時に Install/Uninstall する。
// スレッド規約: Install/ResolvePath はメインスレッド専用。
using ResolvePathFn = std::wstring (*)(void* user, uint64_t guid);

void Install(ResolvePathFn fn, void* user); // fn=null で既定 (常に空文字列) に戻す
std::wstring ResolvePath(uint64_t guid);    // 未解決 = 空文字列

} // namespace assetguid
} // namespace mye
