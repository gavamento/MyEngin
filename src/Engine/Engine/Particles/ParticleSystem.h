#pragma once
#include <memory>
#include <string>

#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Engine/Particles/GpuParticleBackend.h"

namespace mye {

enum class ParticleBackendKind { Cpu = 0, Gpu = 1 };

// パーティクルシステムの司令塔 (engine_spec.md 7 章)。
// - CPU / GPU バックエンドを保持し、エディタ GUI から実行時切替 (spec 7.4)
// - 切替時は生存パーティクルを破棄して再スタート (初期実装の仕様)
// - 比較モード: 同一エミッタ定義を両バックエンドで並走させ、GPU 側を
//   横にオフセットして並べて表示 + 両者の更新時間を計測 (spec 7.4)
// - 選択状態は project_settings.json に永続化
class ParticleSystem {
public:
    // backendOverride / compareOverride: -1 = 未指定 (project_settings.json に従う) /
    // 0 = CPU / 1 = GPU (compare は 0 = off / 1 = on)。CLI (--particle-backend /
    // --particle-compare) 用の M57追補。
    // ★適用は **LoadSettings() の直後に active_ / compareMode_ へ直接代入**する。
    //   SetActiveKind() 経由にするとあちらが SaveSettings() を呼ぶので、撮影 1 回で
    //   開発者の project_settings.json が書き換わる
    bool Init(GraphicsDevice& device, ShaderManager& shaders, const std::wstring& assetsRoot,
              int backendOverride = -1, int compareOverride = -1);
    void Shutdown();

    void Update(World& world, float dt);                       // tick フェーズ 4
    void Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders,
                RenderResources& resources);

    // シーン遷移 (M19.4): 生存パーティクルを破棄する (古いシーンの粒子を残さない)
    void ResetParticles()
    {
        cpu_.Reset();
        gpu_.Reset();
    }

    ParticleBackendKind ActiveKind() const { return active_; }
    void SetActiveKind(ParticleBackendKind kind); // 切替 (Reset + 設定保存)
    bool CompareMode() const { return compareMode_; }
    void SetCompareMode(bool enabled);
    float CompareOffsetX() const { return compareOffsetX_; }

    CpuParticleBackend& Cpu() { return cpu_; }
    GpuParticleBackend& Gpu() { return gpu_; }
    IParticleBackend& Active() { return active_ == ParticleBackendKind::Cpu
                                     ? static_cast<IParticleBackend&>(cpu_)
                                     : static_cast<IParticleBackend&>(gpu_); }

    void SaveSettings() const;

private:
    void LoadSettings();
    // スクリプト/エディタ起因の pendingBurst を全エミッタでクリアする (両バックエンドが読んだ後)
    static void ClearPendingBursts(World& world);

    CpuParticleBackend cpu_;
    GpuParticleBackend gpu_;
    ParticleBackendKind active_ = ParticleBackendKind::Cpu;
    bool compareMode_ = false;
    float compareOffsetX_ = 4.0f;
    std::wstring settingsPath_;
};

} // namespace mye
