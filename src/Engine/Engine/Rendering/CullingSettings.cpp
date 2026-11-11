//====================================================================================
//                          CullingSettings.cpp
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          カリング設定の読み書きと CLI との合成の実装
//====================================================================================
#include "Engine/Engine/Rendering/CullingSettings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"
#include "Engine/Engine/Scene/TagNames.h"

namespace mye {

static_assert(kCullingTagCount == kMaxTags, "kCullingTagCount must match kMaxTags");

namespace {

using json = nlohmann::json;

// 数値として読めて有限なら out へ。型違いは false
bool ReadNumber(const json& v, float& out)
{
    if (!v.is_number()) {
        return false;
    }
    const double d = v.get<double>();
    if (!std::isfinite(d)) {
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

// ファイルに書く値。float をそのまま double にすると 0.005f が 0.004999999888... になるので、
// float へ丸め戻して同じになる最短の桁数で一度文字列にしてから double に直す
double CleanDouble(float v)
{
    char buf[32];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buf, sizeof(buf), "%.*g", digits, static_cast<double>(v));
        if (static_cast<float>(std::strtod(buf, nullptr)) == v) {
            break;
        }
    }
    return std::strtod(buf, nullptr);
}

// "rendering" オブジェクトを用意して mutate を当てる (他のキーは残す)
bool UpdateRendering(const std::wstring& assetsRoot, const std::function<void(json&)>& mutate)
{
    return UpdateProjectSettingsFile(ProjectSettingsPath(assetsRoot), [&](json& j) {
        if (!j.contains("rendering") || !j["rendering"].is_object()) {
            j["rendering"] = json::object();
        }
        mutate(j["rendering"]);
    });
}

const char* FlagKey(CullingFlag flag)
{
    switch (flag) {
    case CullingFlag::Frustum: return "frustumCulling";
    case CullingFlag::Occlusion: return "occlusionCulling";
    case CullingFlag::Distance: return "distanceCulling";
    case CullingFlag::Size: return "sizeCulling";
    }
    return "";
}

} // namespace

bool IsValidMinScreenSize(float f)
{
    return std::isfinite(f) && f > 0.0f && f <= 1.0f;
}

bool IsValidCullDistance(float d)
{
    return std::isfinite(d) && d >= 0.0f;
}

CullingSettings LoadCullingSettings(const std::wstring& assetsRoot)
{
    CullingSettings s;
    json root;
    if (ReadProjectSettingsFile(ProjectSettingsPath(assetsRoot), root) != ProjectSettingsRead::Ok) {
        return s; // 無い / 読めない = 既定値
    }
    s.occlusion = LoadOcclusionCullingSetting(assetsRoot);
    if (!root.contains("rendering") || !root["rendering"].is_object()) {
        return s;
    }
    const json& r = root["rendering"];

    auto readBool = [&](const char* key, bool& out) {
        if (!r.contains(key)) {
            return;
        }
        if (r[key].is_boolean()) {
            out = r[key].get<bool>();
        } else {
            MYE_LOG_WARN("[culling] rendering.%s is not a boolean: using the default", key);
        }
    };
    readBool("frustumCulling", s.frustum);
    readBool("distanceCulling", s.distance);
    readBool("sizeCulling", s.size);

    if (r.contains("minScreenSize")) {
        float v = 0.0f;
        if (ReadNumber(r["minScreenSize"], v) && IsValidMinScreenSize(v)) {
            s.minScreenSize = v;
        } else {
            MYE_LOG_WARN("[culling] rendering.minScreenSize must be a number in (0, 1]: using the default");
        }
    }
    if (r.contains("defaultMaxDrawDistance")) {
        float v = 0.0f;
        if (ReadNumber(r["defaultMaxDrawDistance"], v) && IsValidCullDistance(v)) {
            s.defaultMaxDrawDistance = v;
        } else {
            MYE_LOG_WARN("[culling] rendering.defaultMaxDrawDistance must be a number >= 0: using the default");
        }
    }
    if (r.contains("tagCullDistances")) {
        const json& arr = r["tagCullDistances"];
        if (!arr.is_array()) {
            MYE_LOG_WARN("[culling] rendering.tagCullDistances is not an array: ignored");
        } else {
            for (const json& e : arr) {
                float dist = 0.0f;
                const bool ok = e.is_object() && e.contains("tag") && e["tag"].is_number_integer()
                    && e.contains("distance") && ReadNumber(e["distance"], dist) && dist > 0.0f;
                const int64_t tag = ok ? e["tag"].get<int64_t>() : -1;
                if (!ok || tag < 0 || tag >= kCullingTagCount) {
                    MYE_LOG_WARN("[culling] rendering.tagCullDistances: an invalid entry was skipped");
                    continue;
                }
                s.tagDistance[tag] = dist; // 同じタグが重なれば後の行が勝つ
            }
        }
    }
    return s;
}

bool SaveCullingFlag(const std::wstring& assetsRoot, CullingFlag flag, bool value)
{
    if (flag == CullingFlag::Occlusion) {
        return SaveOcclusionCullingSetting(assetsRoot, value); // 既存の保存と同じ経路
    }
    const char* key = FlagKey(flag);
    return UpdateRendering(assetsRoot, [&](json& r) { r[key] = value; });
}

bool SaveMinScreenSize(const std::wstring& assetsRoot, float value)
{
    if (!IsValidMinScreenSize(value)) {
        return false;
    }
    return UpdateRendering(assetsRoot, [&](json& r) { r["minScreenSize"] = CleanDouble(value); });
}

bool SaveDefaultMaxDrawDistance(const std::wstring& assetsRoot, float value)
{
    if (!IsValidCullDistance(value)) {
        return false;
    }
    return UpdateRendering(assetsRoot, [&](json& r) { r["defaultMaxDrawDistance"] = CleanDouble(value); });
}

bool SaveTagCullDistances(const std::wstring& assetsRoot, const float (&distances)[kCullingTagCount])
{
    json arr = json::array();
    for (int tag = 0; tag < kCullingTagCount; ++tag) {
        if (distances[tag] > 0.0f && std::isfinite(distances[tag])) {
            json e = json::object();
            e["tag"] = tag;
            e["distance"] = CleanDouble(distances[tag]);
            arr.push_back(std::move(e));
        }
    }
    return UpdateRendering(assetsRoot, [&](json& r) { r["tagCullDistances"] = std::move(arr); });
}

CullingSettings ApplyCullingCli(const CullingSettings& file, const CullingCliOverrides& cli)
{
    CullingSettings s = file;
    s.frustum = file.frustum && cli.frustum;
    s.occlusion = file.occlusion && cli.occlusion;
    s.distance = file.distance && cli.distance;
    if (cli.maxDrawDistance >= 0.0f) {
        s.defaultMaxDrawDistance = cli.maxDrawDistance;
    }
    if (cli.minScreenSize > 0.0f) {
        s.minScreenSize = cli.minScreenSize;
    }
    // 閾値を CLI で渡したら、ファイルが OFF でもその起動中は ON。ただし --no-size-culling が最優先
    s.size = (file.size || cli.minScreenSize > 0.0f) && cli.size;
    return s;
}

CullingSettings ResolveCullingSettings(const std::wstring& assetsRoot, const CullingCliOverrides& cli)
{
    return ApplyCullingCli(LoadCullingSettings(assetsRoot), cli);
}

} // namespace mye
