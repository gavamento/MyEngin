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
//
// ★永続化するのは **particleBackend の 1 キーだけ** (M66h、決定 8)。
//   比較モード / 比較オフセット / CPU の SIMD は「その人のデバッグ表示」であって
//   プロジェクトの内容ではない — チームで共有される project_settings.json に置くと、
//   比較を 1 回入れただけで「全員の画面に粒子が 2 重に出る」差分が push される。
//   置き場は Editor 側の個人設定 (<project>\.mye\editor_settings.json) で、
//   起動時と変更時に Editor が下の setter へ流し込む (Engine 層は EditorSettings を知らない)。
//   旧 project_settings.json に残っている 3 キーは**読み飛ばす** (害の無い死んだ文字列)。
class ParticleSystem {
public:
    // backendOverride / compareOverride: -1 = 未指定 (project_settings.json に従う) /
    // 0 = CPU / 1 = GPU (compare は 0 = off / 1 = on)。CLI (--particle-backend /
    // --particle-compare) 用の M57追補。
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

    // ★以下の setter は**セッション上書き**で、どれも設定ファイルを書かない。
    //   永続化するのは SaveSettings() を明示的に呼んだときだけ (Project Settings 窓の
    //   「プロジェクト既定にする」1 箇所)。ここを「触ったら保存」に戻すと、
    //   スクショ 1 枚や比較の一瞬の確認で共有ファイルが書き換わる (M57追補で踏んだ罠)
    ParticleBackendKind ActiveKind() const { return active_; }
    void SetActiveKind(ParticleBackendKind kind); // 切替 (Reset のみ。保存しない)
    bool CompareMode() const { return compareMode_; }
    void SetCompareMode(bool enabled);
    float CompareOffsetX() const { return compareOffsetX_; }
    void SetCompareOffsetX(float x) { compareOffsetX_ = x; }
    // --particle-compare が指定されて起動したか (M66h)。Editor はこれが true のとき
    // 個人設定の比較モードを**流し込まない** — CLI 固定の撮影が機体の設定で割れると、
    // golden スクショが「撮った人によって違う」ものになる
    bool CompareOverriddenByCli() const { return compareFromCli_; }

    CpuParticleBackend& Cpu() { return cpu_; }
    GpuParticleBackend& Gpu() { return gpu_; }
    IParticleBackend& Active() { return active_ == ParticleBackendKind::Cpu
                                     ? static_cast<IParticleBackend&>(cpu_)
                                     : static_cast<IParticleBackend&>(gpu_); }

    // project_settings.json の **particleBackend だけ**を読む / 書く。
    // ★Load は Init が呼ぶが public なのは「Init を通さずに永続化だけ検査する」
    //   セルフテストの唯一の入口だから (D3D デバイスが要らない部分をテストできる形にしておく)。
    //   Save の呼び出し元は Project Settings 窓 1 箇所だけに保つこと
    void LoadSettings(const std::wstring& settingsPath);
    void SaveSettings() const;

private:
    // スクリプト/エディタ起因の pendingBurst を全エミッタでクリアする (両バックエンドが読んだ後)
    static void ClearPendingBursts(World& world);

    CpuParticleBackend cpu_;
    GpuParticleBackend gpu_;
    ParticleBackendKind active_ = ParticleBackendKind::Cpu;
    bool compareMode_ = false;
    float compareOffsetX_ = 4.0f;
    bool compareFromCli_ = false;
    std::wstring settingsPath_;
};

} // namespace mye
