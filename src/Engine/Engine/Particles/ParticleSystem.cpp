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
    const bool cpuOk = cpu_.Init(device, shaders);
    const bool gpuOk = gpu_.Init(device, shaders);
    if (!cpuOk || !gpuOk) {
        MYE_LOG_ERROR("ParticleSystem: backend init failed (cpu=%d gpu=%d)", cpuOk, gpuOk);
    }
    LoadSettings(assetsRoot + L"\\project_settings.json");
    // M57追補: CLI (--particle-backend / --particle-compare) は設定ファイルより優先する。
    // ★M66h 以降、この 2 つの setter は保存しない (書き戻しは Project Settings 窓だけ) ので
    //   経路の選択は自由になったが、Reset とログを走らせない直接代入のままにしてある —
    //   ここは LoadSettings() の直後で「読んだ値を差し替える」以上のことをする必要が無い。
    // ★compare が CLI 由来かどうかは覚えておく: Editor が個人設定 (editor_settings.json) の
    //   比較モードを流し込む前に見て、CLI を勝たせるため (撮影の再現性が個人設定で壊れると
    //   golden が「撮った人によって違う」ものになる)
    if (backendOverride >= 0) {
        active_ = (backendOverride == 1) ? ParticleBackendKind::Gpu : ParticleBackendKind::Cpu;
    }
    if (compareOverride >= 0) {
        compareMode_ = (compareOverride != 0);
        compareFromCli_ = true;
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
}

void ParticleSystem::LoadSettings(const std::wstring& settingsPath)
{
    settingsPath_ = settingsPath;
    std::ifstream f(std::filesystem::path(settingsPath_), std::ios::binary);
    if (!f) {
        return;
    }
    try {
        nlohmann::json root;
        f >> root;
        const std::string backend = root.value("particleBackend", std::string("cpu"));
        active_ = (backend == "gpu") ? ParticleBackendKind::Gpu : ParticleBackendKind::Cpu;
        // ★particleCompareMode / particleCompareOffsetX / particleCpuSimd は**読まない** (M66h)。
        //   既存プロジェクトの JSON には残っているが、個人設定の正本は
        //   <project>\.mye\editor_settings.json に移った。ここで読むと
        //   「共有ファイルの古い値が、あとから個人設定を上書きする」順序問題が生まれる
        //   (移行は行わない = 既定値から始まる。compare は本来 OFF が既定の調査用表示なので、
        //    共有ファイルに ON が焼かれていた場合はむしろ消える方が正しい)
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("project_settings.json parse error: %s", ex.what());
    }
}

void ParticleSystem::SaveSettings() const
{
    if (settingsPath_.empty()) {
        return; // Init も LoadSettings も通っていない (ヘッドレスのテスト等)
    }
    // 既存の設定を保持しつつ particleBackend のみ更新 (マージ保存)。
    // ★旧 3 キーが残っていても**消さない** — 消すと「Project Settings を一度開いて
    //   保存しただけ」で共有ファイルに 3 行の削除差分が出る。読む人が誰もいない
    //   死んだキーなので、放っておくのが最も安い
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

    std::ofstream f(std::filesystem::path(settingsPath_), std::ios::binary);
    if (f) {
        const std::string text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace mye
