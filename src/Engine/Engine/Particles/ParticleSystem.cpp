#include "Engine/Engine/Particles/ParticleSystem.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

namespace mye {

bool ParticleSystem::Init(GraphicsDevice& device, ShaderManager& shaders,
                          const std::wstring& assetsRoot)
{
    settingsPath_ = assetsRoot + L"\\project_settings.json";
    const bool cpuOk = cpu_.Init(device, shaders);
    const bool gpuOk = gpu_.Init(device, shaders);
    if (!cpuOk || !gpuOk) {
        MYE_LOG_ERROR("ParticleSystem: backend init failed (cpu=%d gpu=%d)", cpuOk, gpuOk);
    }
    LoadSettings();
    MYE_LOG_INFO("ParticleSystem: active backend = %s%s", Active().Name(),
                 compareMode_ ? " (+compare)" : "");
    return cpuOk && gpuOk;
}

void ParticleSystem::Shutdown()
{
    cpu_.Shutdown();
    gpu_.Shutdown();
}

void ParticleSystem::Update(World& world, float dt)
{
    if (compareMode_) {
        // 比較モード: 同一定義・同一シードで両バックエンドを並走 (spec 7.4)
        cpu_.Update(world, dt);
        gpu_.Update(world, dt);
        return;
    }
    Active().Update(world, dt);
}

void ParticleSystem::Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders)
{
    if (compareMode_) {
        cpu_.Render(device, view, shaders, 0.0f);
        gpu_.Render(device, view, shaders, compareOffsetX_); // 横に並べて表示
        return;
    }
    Active().Render(device, view, shaders, 0.0f);
}

void ParticleSystem::SetActiveKind(ParticleBackendKind kind)
{
    if (kind == active_) {
        return;
    }
    // 切替時は生存パーティクルを破棄して再スタート (spec 7.4 初期実装)
    cpu_.Reset();
    gpu_.Reset();
    active_ = kind;
    MYE_LOG_INFO("[particles] backend switched to %s (particles restarted)", Active().Name());
    SaveSettings();
}

void ParticleSystem::SetCompareMode(bool enabled)
{
    if (compareMode_ == enabled) {
        return;
    }
    compareMode_ = enabled;
    cpu_.Reset();
    gpu_.Reset();
    MYE_LOG_INFO("[particles] compare mode %s", enabled ? "ON" : "OFF");
    SaveSettings();
}

void ParticleSystem::LoadSettings()
{
    std::ifstream f(std::filesystem::path(settingsPath_), std::ios::binary);
    if (!f) {
        return;
    }
    try {
        nlohmann::json root;
        f >> root;
        const std::string backend = root.value("particleBackend", std::string("cpu"));
        active_ = (backend == "gpu") ? ParticleBackendKind::Gpu : ParticleBackendKind::Cpu;
        compareMode_ = root.value("particleCompareMode", false);
        compareOffsetX_ = root.value("particleCompareOffsetX", 4.0f);
        cpu_.SetSimdEnabled(root.value("particleCpuSimd", true));
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("project_settings.json parse error: %s", ex.what());
    }
}

void ParticleSystem::SaveSettings() const
{
    // 既存の設定を保持しつつパーティクル関連キーのみ更新
    nlohmann::json root;
    {
        std::ifstream f(std::filesystem::path(settingsPath_), std::ios::binary);
        if (f) {
            try {
                f >> root;
            } catch (...) {
                root = nlohmann::json::object();
            }
        }
    }
    root["particleBackend"] = (active_ == ParticleBackendKind::Gpu) ? "gpu" : "cpu";
    root["particleCompareMode"] = compareMode_;
    root["particleCompareOffsetX"] = compareOffsetX_;
    root["particleCpuSimd"] = cpu_.SimdEnabled();

    std::ofstream f(std::filesystem::path(settingsPath_), std::ios::binary);
    if (f) {
        const std::string text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace mye
