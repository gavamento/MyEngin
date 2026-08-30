#include "Engine/Engine/Particles/ParticleSystem.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

namespace mye {

bool ParticleSystem::Init(GraphicsDevice& device, ShaderManager& shaders,
                          const std::wstring& assetsRoot, int backendOverride,
                          int compareOverride)
{
    settingsPath_ = assetsRoot + L"\\project_settings.json";
    const bool cpuOk = cpu_.Init(device, shaders);
    const bool gpuOk = gpu_.Init(device, shaders);
    if (!cpuOk || !gpuOk) {
        MYE_LOG_ERROR("ParticleSystem: backend init failed (cpu=%d gpu=%d)", cpuOk, gpuOk);
    }
    LoadSettings();
    // M57追補: CLI (--particle-backend / --particle-compare) は設定ファイルより優先する。
    // ★SetActiveKind() / SetCompareMode() を**呼んではいけない** — あちらは SaveSettings() を
    //   呼ぶので、スクショ 1 枚のために開発者の project_settings.json が書き換わる。
    //   ここは LoadSettings() の直後なので、直接代入で「読んだ値を上書きする」だけで足りる。
    // ※ 上書き中に GUI (ParticleSettingsWindow) でバックエンドを触ると保存されるが、それは
    //    明示的なユーザー操作の結果なので許容する (ヘッドレス撮影では GUI が無いので起きない)
    if (backendOverride >= 0) {
        active_ = (backendOverride == 1) ? ParticleBackendKind::Gpu : ParticleBackendKind::Cpu;
    }
    if (compareOverride >= 0) {
        compareMode_ = (compareOverride != 0);
    }
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
    } else {
        Active().Update(world, dt);
    }
    // 即時バースト (script/editor) は両バックエンドが読み終えた後にクリアする (M32a)。
    // これで tick 末ハッシュ前に pendingBurst=0 が保証され、compare でも両者が同値を見る。
    ClearPendingBursts(world);
}

void ParticleSystem::ClearPendingBursts(World& world)
{
    const ComponentTypeId req[] = { ParticleEmitterComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (auto* e = world.GetComponent<ParticleEmitterComponent>(arch.EntityAt(row))) {
                e->pendingBurst = 0;
            }
        }
    });
}

void ParticleSystem::Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders,
                            RenderResources& resources)
{
    if (compareMode_) {
        cpu_.Render(device, view, shaders, resources, 0.0f);
        gpu_.Render(device, view, shaders, resources, compareOffsetX_); // 横に並べて表示
        return;
    }
    Active().Render(device, view, shaders, resources, 0.0f);
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
