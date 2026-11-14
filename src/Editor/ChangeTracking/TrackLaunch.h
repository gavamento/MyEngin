//====================================================================================
//                          TrackLaunch.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスの探索と切り離し起動 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

namespace mye {

// サービスの exe を上書きする環境変数。設定されていればそのファイルだけを使い、無ければ他を探さない (空は未設定)
inline constexpr const wchar_t* kTrackExeEnvVar = L"MYE_TRACK_EXE";

// 起動する mye_track.exe を探す。MYE_TRACK_EXE があればそれだけ、無ければ <exeDir>\mye_track.exe。
// 見つからなければ空 (探し直しは呼び出し側が決める)。正本: ipc-contract.md §9.5
std::wstring FindTrackExe(const std::wstring& exeDir);

// CreateProcess の引数 1 つを MSVC の規則で引用する (末尾の '\' と '"' の前の '\' を二重にする)。
// プロジェクトのルートが "C:\Proj\" のように '\' で終わっても、閉じの '"' を壊さない
std::wstring QuoteCommandLineArg(const std::wstring& arg);

// `serve --project <root>` を切り離して起動する。子は待たず、ハンドルは返さない。
// ★StartChildProcess は使わない: あれは bInheritHandles = TRUE でログのハンドルを子へ渡す。
//   常駐するサービスがそれを掴むと、ログを消せない・Editor が終わっても親のハンドルが残る
// 失敗なら false (errorOut に GetLastError)
bool LaunchTrackService(const std::wstring& exePath, const std::wstring& projectRoot, uint32_t* errorOut);

} // namespace mye
