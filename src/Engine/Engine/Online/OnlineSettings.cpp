//====================================================================================
//                          OnlineSettings.cpp
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          project_settings.json の "online" 節
//====================================================================================
#include "Engine/Engine/Online/OnlineSettings.h"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"

namespace mye {

namespace {

std::string StringOr(const nlohmann::json& obj, const char* key)
{
    if (obj.contains(key) && obj[key].is_string()) {
        return obj[key].get<std::string>();
    }
    return std::string();
}

bool BoolOr(const nlohmann::json& obj, const char* key, bool fallback)
{
    if (obj.contains(key) && obj[key].is_boolean()) {
        return obj[key].get<bool>();
    }
    return fallback;
}

} // namespace

const char* OnlineBackendName(OnlineBackend backend)
{
    switch (backend) {
    case OnlineBackend::Eos:
        return "eos";
    case OnlineBackend::Local:
        return "local";
    case OnlineBackend::None:
        break;
    }
    return "none";
}

bool ParseOnlineBackend(const std::string& name, OnlineBackend& out)
{
    if (name == "eos") {
        out = OnlineBackend::Eos;
    } else if (name == "local") {
        out = OnlineBackend::Local;
    } else if (name == "none" || name.empty()) {
        out = OnlineBackend::None;
    } else {
        return false;
    }
    return true;
}

bool LoadOnlineSettings(const std::wstring& assetsRoot, OnlineSettings& out)
{
    out = OnlineSettings{};
    nlohmann::json root;
    if (ReadProjectSettingsFile(ProjectSettingsPath(assetsRoot), root) != ProjectSettingsRead::Ok) {
        return false;
    }
    if (!root.contains("online") || !root["online"].is_object()) {
        return false;
    }
    const nlohmann::json& online = root["online"];
    const std::string backend = StringOr(online, "backend");
    if (!ParseOnlineBackend(backend, out.backend)) {
        // 綴り違いを黙って「オフライン」にしない (気づかないまま配布される)
        out.backend = OnlineBackend::None;
        MYE_LOG_WARN("[online] project_settings.json: unknown online.backend \"%s\" - online features are off",
                     backend.c_str());
    }
    out.displayName = StringOr(online, "displayName");
    if (online.contains("eos") && online["eos"].is_object()) {
        const nlohmann::json& eos = online["eos"];
        out.productId = StringOr(eos, "productId");
        out.sandboxId = StringOr(eos, "sandboxId");
        out.deploymentId = StringOr(eos, "deploymentId");
        out.clientId = StringOr(eos, "clientId");
        out.clientSecret = StringOr(eos, "clientSecret");
    }
    if (online.contains("voice") && online["voice"].is_object()) {
        const nlohmann::json& voice = online["voice"];
        out.voiceEnabled = BoolOr(voice, "enabled", out.voiceEnabled);
        out.pushToTalk = BoolOr(voice, "pushToTalk", out.pushToTalk);
        const std::string key = StringOr(voice, "pushToTalkKey");
        if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= 'a' && key[0] <= 'z'))) {
            out.pushToTalkKey = static_cast<char>(key[0] & ~0x20); // 大文字へ (仮想キーコードと同じ値)
        } else if (!key.empty()) {
            MYE_LOG_WARN("[online] project_settings.json: voice.pushToTalkKey must be one letter (got \"%s\") - using T",
                         key.c_str());
        }
    }
    return true;
}

} // namespace mye
