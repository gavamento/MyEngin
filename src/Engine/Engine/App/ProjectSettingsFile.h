//====================================================================================
//                          ProjectSettingsFile.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          project_settings.json の共有の読み書き
//====================================================================================
#pragma once

#include <functional>
#include <string>

#include "nlohmann/json.hpp"

namespace mye {

// project_settings.json を読んだ結果
enum class ProjectSettingsRead {
    Missing, // ファイルが無い
    Ok,
    Corrupt, // 開けない / JSON でない / ルートがオブジェクトでない
};

// assetsRoot 直下の project_settings.json のパス
std::wstring ProjectSettingsPath(const std::wstring& assetsRoot);

// 丸ごと読む。Ok 以外では out は空オブジェクト。空白だけのファイルは空オブジェクトの Ok
ProjectSettingsRead ReadProjectSettingsFile(const std::wstring& path, nlohmann::json& out);

// 既存のキーを保ったまま mutate を適用して書き戻す (ファイルが無ければ作る)。
// 既存ファイルが Corrupt のときは触らずに false を返す。空から書き直すと他の機能のキーが全部消える。
// 改行コードは既存ファイルに合わせる (保存のたびに全行の差分を出さない)
bool UpdateProjectSettingsFile(const std::wstring& path, const std::function<void(nlohmann::json&)>& mutate);

} // namespace mye
