#pragma once
#include <vector>

#include <wrl/client.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Particles/GpuAliveEstimator.h"
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

    // M42e: 深度衝突用にシーンカメラの深度を渡す (RenderSystem がシーンカメラ描画後に呼ぶ)。
    // sim は tick フェーズで走るため衝突相手は前フレームの深度 = 1 フレーム遅延 (仕様)。
    // ComPtr 保持なのでリサイズで元 RenderTexture が破棄されても stale-but-safe。
    void SetSceneDepth(ID3D11ShaderResourceView* depthSRV, const DirectX::XMFLOAT4X4& view,
                       const DirectX::XMFLOAT4X4& proj, int width, int height, float nearZ,
                       float farZ);

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
        GpuAliveEstimator aliveEst; // 生存数の CPU 側推定 (表示用。readback しない)
        int32_t idleTicks = 0; // 「放出0 + 推定生存0」の連続 tick (空 Dispatch 回避の猶予計数)
        bool gpuIdle = false;  // true = 今 tick は GPU 作業を丸ごと省いた (Render もスキップ)
        // M61c: 原点履歴 (CPU 側 EmitterPool の prevOrigin ミラー)。GPU バックエンドは
        // 表示用ベストエフォート = ハッシュ/スナップショット非対象なのでここに置くだけでよい
        DirectX::XMFLOAT3 prevOrigin = {};
        uint32_t prevOriginValid = 0;
        // M61e: owner 非アクティブによる凍結。gpuIdle が「D3D 作業だけ省いて放出計画と
        // 推定器の記帳は続ける」のに対し、凍結は「時が止まる」— 記帳ごと全部止める
        bool frozen = false;
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
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_; // M42c: フリップブック (linear clamp)

    // M42e: 深度衝突の入力 (SetSceneDepth で更新。valid=false なら衝突無効)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> collDepthSRV_;
    DirectX::XMFLOAT4X4 collViewProj_ = {};    // transpose 済み
    DirectX::XMFLOAT4X4 collInvViewProj_ = {}; // transpose 済み
    float collScreen_[4] = { 0, 0, 0.1f, 1000.0f }; // w, h, nearZ, farZ
    bool collValid_ = false;
};

} // namespace mye
