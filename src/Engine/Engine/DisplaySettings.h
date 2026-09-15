//====================================================================================
//                          DisplaySettings.h
//  三校/ 秋田蓮音                                                          09/15/2026
//                                  ウィンドウの表示モードを起動をまたいで覚える
//====================================================================================
#pragma once
// ABI v19 SetWindowMode の保存先と、起動時にどのモードで開くかの決定。
// ★sim の外。<saveDir>\display.json はセーブスロットと同じディレクトリに置くが、PersistStore にも
//   .rep にも SimSnapshot にも載らない = 記録/検証の結果を 1 bit も変えない
// 起動時のモード = display.json の windowMode → 無ければ project_settings.json の window.defaultMode
//                → 無ければウィンドウ (どちらも "windowed" | "borderless")
#include <string>

#include "Engine/Platform/Win32Window.h"

namespace mye {
namespace display {

// "windowed" / "borderless" (JSON とログの表記)
const char* WindowModeName(WindowMode mode);

std::wstring DisplaySettingsPath(const std::wstring& saveDir);

// 読めたら out へ書いて true。壊れている・キーが無い・値が不明なら false で **out に触らない**
bool ParseDisplaySettings(const std::string& jsonText, WindowMode& out);
bool ParseProjectDefaultWindowMode(const std::string& jsonText, WindowMode& out);

WindowMode LoadStartupWindowMode(const std::wstring& assetsRoot, const std::wstring& saveDir);

// saveDir が無ければ作ってから書く (テンポラリ → 置き換え)
bool SaveWindowMode(const std::wstring& saveDir, WindowMode mode);

} // namespace display
} // namespace mye
