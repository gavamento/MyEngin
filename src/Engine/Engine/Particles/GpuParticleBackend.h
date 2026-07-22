#pragma once
#include <vector>

#include <wrl/client.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Particles/IParticleBackend.h"
#include "Engine/Renderer/GpuTimer.h"

namespace mye {

// GPU パーティクル (engine_spec.md 7.3)。
// - プール + dead list (append/consume) + alive list A/B ピンポン (圧縮)
// - InstanceCount は CopyStructureCount → DrawInstancedIndirect (CPU リードバックなし)
// - 乱数は CPU 側 (エンジンの決定論 RNG) が生成した放出バッファを消費 (spec 7.3)
// - アルファソートなし (additive 前提。Bitonic Sort はストレッチ)
class GpuParticleBackend : public IParticleBackend {
public:
    const char* Name() const override { return "GPU (Compute)"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Reset() override;
    void Update(World& world, float dt) override;
    void Render(GraphicsDevice& device, const RenderView& view, ShaderManager& shaders,
                RenderResources& resources, float renderOffsetX) override;
    ParticleStats Stats() const override { return stats_; }

private:
    struct EmitData { // particle_emit.cs.hlsl の EmitData と一致
        DirectX::XMFLOAT3 pos;
        float life;
        DirectX::XMFLOAT3 vel;
        float size;
    };
    struct GpuEmitter {
        EntityID owner = kNullEntity;
        uint32_t capacity = 0;
        float emitAccum = 0.0f;
        int32_t ageTicks = 0; // M32a: 放出ウィンドウ経過 tick (CPU 側で管理、表示用)
        Pcg32 rng;
        ParticleEmitterComponent descCache;
        bool firstDispatch = true; // 初回のみ dead list カウンタを capacity で初期化
        Microsoft::WRL::ComPtr<ID3D11Buffer> pool;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> poolUAV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> poolSRV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> deadList;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> deadUAV; // APPEND flag
        Microsoft::WRL::ComPtr<ID3D11Buffer> alive[2];
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> aliveUAV[2]; // COUNTER flag
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> aliveSRV[2];
        int aliveCurrent = 0; // 描画に使う側 (= 直近 sim の出力)
        Microsoft::WRL::ComPtr<ID3D11Buffer> counts;   // [0]=deadCount [1]=aliveInCount
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> countsSRV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> emitBuffer; // 動的 (CPU 生成の初期値)
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> emitSRV;
        uint32_t emitCapacity = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indirectArgs; // DrawInstancedIndirect
    };

    bool CreateEmitterResources(GraphicsDevice& device, GpuEmitter& em, uint32_t capacity);
    void SyncEmitters(World& world, GraphicsDevice& device);

    std::vector<GpuEmitter> emitters_; // owner.index 昇順
    GraphicsDevice* device_ = nullptr;
    ShaderManager* shaders_ = nullptr;
    GpuTimer timer_;
    ParticleStats stats_;

    AssetID emitCS_ = {};
    AssetID simCS_ = {};
    AssetID renderShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> simCB_;    // GpuParticleCB (b0)
    Microsoft::WRL::ComPtr<ID3D11Buffer> renderCB_; // GpuRenderCB (b1)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdditive_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthNoWrite_;
};

} // namespace mye
