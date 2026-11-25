//====================================================================================
//                          EosRedist.h
//  MyEngin/ 秋田蓮音                                                     11/25/2026
//                                          EOS を使うゲームが exe の隣に置く実行時 DLL
//====================================================================================
#pragma once
#include <span>

#include "Engine/Engine/Online/OnlineRedist.h"

namespace mye {

// SDK 本体。exe は遅延ロードで参照する (build\Common.props)
inline constexpr wchar_t kEosSdkDll[] = L"EOSSDK-Win64-Shipping.dll";
// ボイス (RTC) の再生に SDK が読む XAudio2.9 の再配布 DLL。無ければボイスだけが無効になる
inline constexpr wchar_t kEosXAudioRedistDll[] = L"xaudio2_9redist.dll";

// EOS を使うゲームが同梱するファイル。SDK 無しのビルドでは空。
// ★ここで SDK の関数を参照しない (Editor がこのヘッダだけを引いても SDK の import が入らないようにする)
inline std::span<const OnlineRedistFile> EosRedistFiles()
{
#if defined(MYE_HAS_EOS)
    static constexpr OnlineRedistFile kFiles[] = {
        { kEosSdkDll, true },
        { kEosXAudioRedistDll, false },
    };
    return kFiles;
#else
    return {};
#endif
}

} // namespace mye
