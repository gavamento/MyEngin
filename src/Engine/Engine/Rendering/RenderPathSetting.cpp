//====================================================================================
//                          RenderPathSetting.cpp
//  MyEngin/ 秋田蓮音                                                       11/25/2026
//                                          起動時のレンダーパスの読み書きの実装
//====================================================================================
#include "Engine/Engine/Rendering/RenderPathSetting.h"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"

namespace mye {

namespace {

using json = nlohmann::json;

constexpr const char* kKey = "renderPath";
constexpr const char* kForwardName = "forward";
constexpr const char* kDeferredName = "deferred";

} // namespace

RenderPathKind LoadRenderPathSetting(const std::wstring& assetsRoot)
{
    json root;
    if (ReadProjectSettingsFile(ProjectSettingsPath(assetsRoot), root) != ProjectSettingsRead::Ok) {
        return RenderPathKind::Forward; // 無い / 読めない = 既定
    }
    if (!root.contains("rendering") || !root["rendering"].is_object() || !root["rendering"].contains(kKey)) {
        return RenderPathKind::Forward;
    }
    const json& v = root["rendering"][kKey];
    if (v.is_string()) {
        const std::string name = v.get<std::string>();
        if (name == kDeferredName) {
            return RenderPathKind::Deferred;
        }
        if (name == kForwardName) {
            return RenderPathKind::Forward;
        }
    }
    MYE_LOG_WARN("[render] rendering.renderPath must be \"forward\" or \"deferred\": using forward");
    return RenderPathKind::Forward;
}

bool SaveRenderPathSetting(const std::wstring& assetsRoot, RenderPathKind kind)
{
    // rendering 節の用意は CullingSettings.cpp の UpdateRendering と同じ (rendering が非オブジェクトなら作り直す)
    return UpdateProjectSettingsFile(ProjectSettingsPath(assetsRoot), [&](json& j) {
        if (!j.contains("rendering") || !j["rendering"].is_object()) {
            j["rendering"] = json::object();
        }
        j["rendering"][kKey] = (kind == RenderPathKind::Deferred) ? kDeferredName : kForwardName;
    });
}

} // namespace mye
