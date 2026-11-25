//====================================================================================
//                          OnlineRedist.h
//  MyEngin/ 秋田蓮音                                                     11/25/2026
//                                          オンライン実装がゲームの配布物に要求する実行時ファイル
//====================================================================================
#pragma once
#include <span>

#include "Engine/Engine/Online/OnlineSettings.h"

namespace mye {

// ゲームの配布物で exe の隣に置く実行時ファイル
struct OnlineRedistFile {
    const wchar_t* fileName = nullptr;
    bool required = false; // false = 無くても一部の機能 (ボイスなど) が無効になるだけ
};

// backend を使うゲームが同梱するファイル。None / Local と、SDK 無しのビルドの Eos は空。
// SDK の関数を参照しないので、呼んだ exe に SDK の import は入らない
std::span<const OnlineRedistFile> OnlineRedistFiles(OnlineBackend backend);

} // namespace mye
