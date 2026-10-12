//====================================================================================
//                          ComputeSettings.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          project_settings.json の "compute" 節の読み書き
//====================================================================================
#include "Engine/Engine/Compute/ComputeSettings.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"

namespace mye::compute {
namespace {

// 非負整数で max 以下なら採る。それ以外は WARN を出して false (呼び出し側が既定値のまま)
bool ReadUnsigned(const nlohmann::json& node, const char* key, uint64_t maxValue, uint64_t& out)
{
    if (!node.contains(key)) {
        return false;
    }
    const nlohmann::json& v = node[key];
    if (!v.is_number_unsigned()) { // 負数・小数・文字列は弾く
        MYE_LOG_WARN("project_settings.json: compute.%s must be a non-negative integer - using the default", key);
        return false;
    }
    const uint64_t value = v.get<uint64_t>();
    if (value > maxValue) {
        MYE_LOG_WARN("project_settings.json: compute.%s is out of range (%llu > %llu) - using the default", key,
                     static_cast<unsigned long long>(value), static_cast<unsigned long long>(maxValue));
        return false;
    }
    out = value;
    return true;
}

} // namespace

bool ParseComputeSettings(const std::string& jsonText, ComputeProjectSettings& out)
{
    out = ComputeProjectSettings{};
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("project_settings.json parse error: %s", ex.what());
        return false;
    }
    if (!root.is_object() || !root.contains("compute")) {
        return false;
    }
    const nlohmann::json& node = root["compute"];
    if (!node.is_object()) {
        MYE_LOG_WARN("project_settings.json: compute must be an object - using the defaults");
        return false;
    }
    uint64_t v = 0;
    if (ReadUnsigned(node, "gpuMinElements", kGpuMinElementsMax, v)) {
        out.config.gpuMinElements = static_cast<uint32_t>(v);
    }
    if (ReadUnsigned(node, "cpuMaxChunks", kCpuMaxChunksMax, v)) {
        out.config.cpuMaxChunks = static_cast<uint32_t>(v);
    }
    if (ReadUnsigned(node, "fxUploadBytesPerTick", kFxUploadBytesMax, v)) {
        out.config.fxUploadBytesPerTick = v;
    }
    if (ReadUnsigned(node, "fxDispatchPerTick", kFxDispatchMax, v)) {
        out.config.fxDispatchPerTick = static_cast<uint32_t>(v);
    }
    if (ReadUnsigned(node, "stateTotalBytes", kStateTotalBytesMax, v)) {
        out.config.stateTotalBytes = v;
    }
    if (node.contains("kernelOverrides")) {
        const nlohmann::json& ov = node["kernelOverrides"];
        if (!ov.is_object()) {
            MYE_LOG_WARN("project_settings.json: compute.kernelOverrides must be an object - ignored");
        } else {
            for (auto it = ov.begin(); it != ov.end(); ++it) {
                if (it.value().is_string() && it.value().get<std::string>() == "cpu") {
                    out.kernelOverridesCpu.insert(it.key());
                } else if (!(it.value().is_string() && it.value().get<std::string>() == "auto")) {
                    MYE_LOG_WARN("project_settings.json: compute.kernelOverrides.%s must be \"cpu\" or \"auto\" - ignored",
                                 it.key().c_str());
                }
            }
        }
    }
    return true;
}

ComputeProjectSettings LoadComputeSettings(const std::wstring& assetsRoot)
{
    ComputeProjectSettings s;
    if (assetsRoot.empty()) {
        return s;
    }
    std::ifstream f(std::filesystem::path(ProjectSettingsPath(assetsRoot)), std::ios::binary);
    if (!f) {
        return s;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    ParseComputeSettings(ss.str(), s);
    return s;
}

void ApplyComputeSettings(ComputeService& service, const ComputeProjectSettings& settings)
{
    ComputeConfig config = settings.config;
    config.forceCpu = service.Config().forceCpu;
    config.alwaysWaitGpu = service.Config().alwaysWaitGpu;
    service.SetConfig(config);
    // 設定ファイルが持つ上書きを入れ直す (セッションで足した分は設定ファイルの内容に置き換わる)
    const std::set<std::string> current = service.KernelOverridesCpu();
    for (const std::string& name : current) {
        service.SetKernelOverrideCpu(name, false);
    }
    for (const std::string& name : settings.kernelOverridesCpu) {
        service.SetKernelOverrideCpu(name, true);
    }
}

bool SaveComputeKernelOverrides(const std::wstring& assetsRoot, const std::set<std::string>& cpuOnlyKernels)
{
    if (assetsRoot.empty()) {
        return false;
    }
    return UpdateProjectSettingsFile(ProjectSettingsPath(assetsRoot), [&cpuOnlyKernels](nlohmann::json& root) {
        // "compute" がオブジェクト以外で置かれていると、下の添字アクセスが例外を投げる
        if (!root.contains("compute") || !root["compute"].is_object()) {
            root["compute"] = nlohmann::json::object();
        }
        nlohmann::json overrides = nlohmann::json::object();
        for (const std::string& name : cpuOnlyKernels) {
            overrides[name] = "cpu";
        }
        if (overrides.empty()) {
            root["compute"].erase("kernelOverrides");
            if (root["compute"].empty()) {
                root.erase("compute"); // 何も無い節は残さない (保存のたびに空の節の差分を出さない)
            }
        } else {
            root["compute"]["kernelOverrides"] = overrides;
        }
    });
}

} // namespace mye::compute
