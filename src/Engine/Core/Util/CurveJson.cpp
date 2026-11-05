//====================================================================================
//                          CurveJson.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          MyeCurve (Shared/FeelMath.h) の JSON 表現 (M94、ADR-035) の実装
//====================================================================================
#include "Engine/Core/Util/CurveJson.h"

#include <string>

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

using nlohmann::json;

namespace {

float ReadFloat(const json& j, const char* key, float def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

int32_t ReadEase(const json& j, const char* key, int32_t def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) {
        return def;
    }
    const std::string name = j[key].get<std::string>();
    const int32_t e = MyeEaseFromName(name.c_str());
    if (e < 0) {
        MYE_LOG_WARN("[curve] unknown ease '%s' (Linear is used)", name.c_str());
        return MYE_EASE_LINEAR;
    }
    return e;
}

} // namespace

json CurveToJson(const MyeCurve& c)
{
    json j;
    if (c.mode == MYE_CURVE_CUSTOM) {
        j["mode"] = "custom";
        json keys = json::array();
        for (int32_t i = 0; i < c.keyCount && i < MYE_CURVE_MAX_KEYS; ++i) {
            const MyeCurveKey& k = c.keys[i];
            keys.push_back(json::array({ k.t, k.value, k.inTangent, k.outTangent }));
        }
        j["keys"] = keys;
    } else {
        j["mode"] = "preset";
        j["ease"] = MyeEaseName(c.ease);
        j["from"] = c.from;
        j["to"] = c.to;
        if (c.param != 0.0f) {
            j["param"] = c.param;
        }
    }
    return j;
}

void CurveFromJson(const json& j, MyeCurve& out)
{
    if (!j.is_object()) {
        return;
    }
    const std::string mode = (j.contains("mode") && j["mode"].is_string()) ? j["mode"].get<std::string>() : "preset";
    if (mode == "custom") {
        out.mode = MYE_CURVE_CUSTOM;
        out.keyCount = 0;
        if (j.contains("keys") && j["keys"].is_array()) {
            for (const json& k : j["keys"]) {
                if (out.keyCount >= MYE_CURVE_MAX_KEYS) {
                    MYE_LOG_WARN("[curve] a curve has more than %d keys (the rest are ignored)", MYE_CURVE_MAX_KEYS);
                    break;
                }
                if (!k.is_array() || k.size() < 2) {
                    continue;
                }
                MyeCurveKey key;
                float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (size_t i = 0; i < 4 && i < k.size(); ++i) {
                    v[i] = k[i].is_number() ? k[i].get<float>() : 0.0f;
                }
                key.t = v[0];
                key.value = v[1];
                key.inTangent = v[2];
                key.outTangent = v[3];
                out.keys[out.keyCount++] = key;
            }
        }
        return;
    }
    out.mode = MYE_CURVE_PRESET;
    out.ease = ReadEase(j, "ease", out.ease);
    out.from = ReadFloat(j, "from", out.from);
    out.to = ReadFloat(j, "to", out.to);
    out.param = ReadFloat(j, "param", out.param);
}

} // namespace mye
