//====================================================================================
//                          DeviceLostRescue.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失時のシーン退避保存
//====================================================================================
#include "Editor/App/DeviceLostRescue.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

std::string RescueTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

std::wstring RescueSceneName(const std::wstring& scenePath)
{
    const std::wstring file = std::filesystem::path(scenePath).filename().wstring();
    const std::wstring name = file.substr(0, file.find(L'.'));
    return name.empty() ? std::wstring(L"scene") : name;
}

RescueSaveResult SaveRescueScene(const nlohmann::json& sceneJson, const std::wstring& rootDir,
                                 const std::wstring& sceneName, const std::string& stamp)
{
    RescueSaveResult result;
    namespace fs = std::filesystem;
    const fs::path dir = fs::absolute(fs::path(rootDir) / L"crash" / (L"device_lost_" + Utf8ToWide(stamp)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        result.error = "cannot create " + WideToUtf8(dir.wstring()) + ": " + ec.message();
        return result;
    }
    const fs::path file = dir / (sceneName + L".scene.json");
    std::string text;
    try {
        text = sceneJson.dump(2);
    } catch (const nlohmann::json::exception& ex) {
        result.error = std::string("cannot serialize the scene: ") + ex.what();
        return result;
    }
    if (!WriteFileReplacing(file.wstring(), text)) {
        result.error = "cannot write " + WideToUtf8(file.wstring());
        return result;
    }
    result.ok = true;
    result.path = file.wstring();
    MYE_LOG_INFO("[device] scene rescued: %s", WideToUtf8(result.path).c_str());
    return result;
}

} // namespace mye
