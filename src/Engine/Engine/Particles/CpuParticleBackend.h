#pragma once
#include <vector>

#include <wrl/client.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Particles/IParticleBackend.h"

namespace mye {

// CPU パーティクル (engine_spec.md 7.3)。
// - SoA レイアウト + SSE 4-wide 更新 (スカラー参照実装を常備 — 端数レーンと検証用)
// - エミッタ別の決定論 RNG ストリーム (ワールドハッシュ対象)
// - 消滅は swap-and-pop
// - アルファブレンドはビュー深度で back-to-front ソート (明示キー、描画専用)
class CpuParticleBackend : public IParticleBackend {
public:
    const char* Name() const override { return "CPU (SIMD)"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Reset() override;
    void Update(World& world, float dt) override;
    void Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders,
                RenderResources& resources, float renderOffsetX) override;
    ParticleStats Stats() const override { return stats_; }

    void SetSimdEnabled(bool enabled) { simd_ = enabled; }
    bool SimdEnabled() const { return simd_; }

    // ワールドハッシュ用 (M6): エミッタ状態を EntityID.index 昇順で列挙
    struct EmitterPool {
        EntityID owner = kNullEntity;
        uint32_t alive = 0;
        float emitAccum = 0.0f;
        int32_t ageTicks = 0; // 放出ウィンドウ内の経過 tick (M32a: burst/duration/loop 用、ハッシュ対象)
        Pcg32 rng;
        ParticleEmitterComponent descCache; // Update 時のコピー (描画属性用)
        // SoA (SSE 4-wide でアクセス)
        std::vector<float> px, py, pz;
        std::vector<float> vx, vy, vz;
        std::vector<float> life;    // 残り秒
        std::vector<float> invLife; // 1/寿命
        std::vector<float> size0;   // 初期サイズ
        // ---- 描画専用バウンズ (Update が毎 tick 再計算。Render のフラスタムカリング用) ----
        // WorldHasher にも SimSnapshot にも**載せない** (両者ともフィールド明示列挙なので
        // ここへの追加はハッシュ/スナップショット版に影響しない)。復元直後は invalid に
        // 戻り「カリングしない」側へ倒れる — 保守方向にしか壊れない設計
        DirectX::XMFLOAT3 boundsMin = {};
        DirectX::XMFLOAT3 boundsMax = {};
        float maxSize0 = 0.0f; // 生存粒子の初期サイズ最大 (ビルボード張り出しの拡張量用)
        bool boundsValid = false;
    };
    const std::vector<EmitterPool>& Pools() const { return pools_; }
    // sim スナップショット (M52d): エミッタ池は WorldHash 対象の sim 状態なので
    // 撮影/復元の対象。**SimSnapshot 以外から書き換えないこと**
    std::vector<EmitterPool>& PoolsForSnapshot() { return pools_; }

private:
    void SyncEmitters(World& world);
    void EmitParticles(EmitterPool& pool, const ParticleEmitterComponent& desc,
                       const DirectX::XMFLOAT3& origin, float dt);
    void Simulate(EmitterPool& pool, const ParticleEmitterComponent& desc, float dt);
    void SimulateScalar(EmitterPool& pool, const DirectX::XMFLOAT3& accel, float dt,
                        uint32_t begin, uint32_t end);
    void KillDead(EmitterPool& pool);

    std::vector<EmitterPool> pools_; // owner.index 昇順 (決定論)
    bool simd_ = true;
    float turb_ = 0.0f; // Simulate 中の乱流係数 (SIMD/スカラー共有)
    ParticleStats stats_;
    std::vector<uint32_t> orderScratch_; // アルファソート用 (描画専用)
    std::vector<uint8_t> visScratch_;    // プール毎の可視フラグ (Render 内のみ有効、描画専用)

    // 描画リソース (全エミッタ共有の動的 structured buffer)
    Microsoft::WRL::ComPtr<ID3D11Buffer> instanceBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> instanceSRV_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> renderCB_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdditive_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthNoWrite_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_; // フリップブックテクスチャ用 (linear clamp)
    uint32_t instanceCapacity_ = 0;
    AssetID shaderId_ = {};
    AssetID distortShaderId_ = {}; // M42d: blendMode=2 用 (particle_distort.hlsl)
};

} // namespace mye
