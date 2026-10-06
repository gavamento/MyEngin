//====================================================================================
//                          DeviceLostRescue.h
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失時のシーン退避保存
//====================================================================================
#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace mye {

struct RescueSaveResult {
    bool ok = false;
    std::wstring path; // 書いたファイルの絶対パス (ok のとき)
    std::string error; // 失敗の理由 (ok でないとき)
};

// 退避ファイル名に使う現在のローカル時刻 "YYYYMMDD-HHMMSS"
std::string RescueTimestamp();

// シーンのパスから退避ファイル名の基部を作る (最初の '.' の手前。空なら "scene")
std::wstring RescueSceneName(const std::wstring& scenePath);

// シーン文書を <rootDir>\crash\device_lost_<stamp>\<sceneName>.scene.json へ書く。
// 元のシーンファイルには触れない (別ファイルへ書く)。通常のシーン保存と同じ形式なので
// そのままエディタで開ける
RescueSaveResult SaveRescueScene(const nlohmann::json& sceneJson, const std::wstring& rootDir,
                                 const std::wstring& sceneName, const std::string& stamp);

} // namespace mye
