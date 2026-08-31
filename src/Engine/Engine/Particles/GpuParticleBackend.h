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
// - M42追補: alpha (blendMode==1) はビットニックソートで back-to-front に並べ替える。
//   CPU バックエンドの std::sort と同じ比較規則 (viewZ 降順 → 添字昇順) を GPU 上で写す —
//   ここが無かった間、加算はビット一致するのに alpha だけ 610 画素割れていた (M57追補の申し送り)
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
        // M42追補: CPU が作った 1/lifetime をそのまま渡す。emit CS が 1/life から作り直すと
        // subframe の寿命前倒し (life = lifetime + f*dt) の分だけ age 曲線が前へずれ、
        // **alpha のフェードが CPU と食い違う** (差は最大 1 tick だが色は毎フレーム効く)。
        // ステージングは 32B -> 48B になる (1M バーストで 32MB -> 48MB。ClampGpuEmitCount の
        // コメント参照 — burst した tick だけの一過性)
        float invLife;
        // M63a: 旧 _pad の 12B を意味づけし直した (48B のまま)。CPU バックエンドと**同じゲート・
        // 同じ消費順** (rot0 → rotVel → flipU) で Pcg32 から引いた値を GPU へ運ぶ。
        // 乱数を GPU で作らない契約 (particle_emit.cs.hlsl 冒頭) の下では、per-particle の
        // ランダム属性を渡す口はここしかない
        float rot0;
        float rotVel;
        float flipU;
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
        // M42追補: プリウォーム済みフラグ (CPU 側 EmitterPool.prewarmed のミラー)。
        // GPU はハッシュ/スナップショット非対象なので、ここに置くだけでよい
        uint32_t prewarmed = 0;
        // M61g: エミッタのワールド行列のキャッシュ (CPU 側 EmitterPool.renderWorld のミラー)。
        // simulationSpace=1 のとき Render がこれを GpuRenderCB へ載せ、VS が pos を変換する
        DirectX::XMFLOAT4X4 renderWorld = { 1.0f, 0.0f, 0.0f, 0.0f,
                                            0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f,
                                            0.0f, 0.0f, 0.0f, 1.0f };
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
        // [0]=deadCount [1]=aliveInCount [2]=aliveOutCount (M42追補: alpha ソートの
        // setup CS が「どれだけ働くか」をここから決める。CPU は生存数を readback しない)
        Microsoft::WRL::ComPtr<ID3D11Buffer> counts;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> countsSRV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> emitBuffer; // 動的 (CPU 生成の初期値)
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> emitSRV;
        uint32_t emitCapacity = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indirectArgs; // DrawInstancedIndirect
        // ---- M42追補: alpha ソート (blendMode==1 のエミッタだけが持つ。遅延確保) ----
        // 0 = 未確保。プール容量が変わったら作り直す (M61f の容量追従と対で動く)
        uint32_t sortCapacity = 0;
        bool sortValid = false; // この Render でソート済み (false = 従来どおり alive list を描く)
        Microsoft::WRL::ComPtr<ID3D11Buffer> sortKeys;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sortKeysUAV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> sortIdx;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sortIdxUAV;
        // ★これが描画へ渡る成果物。型も意味も alive list と同じ StructuredBuffer<uint> なので、
        //   particle_render_gpu.hlsl は 1 行も変えずに t1 の中身だけが差し替わる
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sortIdxSRV;
        Microsoft::WRL::ComPtr<ID3D11Buffer> sortArgs; // DispatchIndirect 引数 (16B × パス数)
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sortArgsUAV; // RAW
    };

    bool CreateEmitterResources(GraphicsDevice& device, GpuEmitter& em, uint32_t capacity);
    void SyncEmitters(World& world, GraphicsDevice& device);
    // M42追補: 描画順ソート。資源は初めて並べる必要が出た tick に遅延確保する
    // (歪み専用のエミッタは 1 バイトも余分に確保しない)
    bool EnsureSortResources(GraphicsDevice& device, GpuEmitter& em);
    void SortEmittersForDraw(GraphicsDevice& device, const RenderView& view);

    std::vector<GpuEmitter> emitters_; // owner.index 昇順
    GraphicsDevice* device_ = nullptr;
    ShaderManager* shaders_ = nullptr;
    GpuTimer timer_;
    ParticleStats stats_;

    AssetID emitCS_ = {};
    AssetID simCS_ = {};
    AssetID renderShader_ = {};
    // M42追補: alpha ソートの 3 パス (キー生成 / ブロック内 LDS / 全域マージ)
    AssetID sortSetupCS_ = {};
    AssetID sortLdsCS_ = {};
    AssetID sortMergeCS_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> simCB_;    // GpuParticleCB (b0)
    Microsoft::WRL::ComPtr<ID3D11Buffer> renderCB_; // GpuRenderCB (b1)
    Microsoft::WRL::ComPtr<ID3D11Buffer> sortCB_;   // ParticleSortCB (b1、ソートパス専用)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdditive_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthNoWrite_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_; // M42c: フリップブック (linear clamp)
    // ---- M63d: ライティング (b2 / s1)。lights が null のビューでは 1 度も張られない ----
    // ★CB は**ビューにつき 1 回**しか上げない (エミッタループの外)
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_; // CSM の PCF 比較サンプラ

    // M42e: 深度衝突の入力 (SetSceneDepth で更新。valid=false なら衝突無効)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> collDepthSRV_;
    DirectX::XMFLOAT4X4 collViewProj_ = {};    // transpose 済み
    DirectX::XMFLOAT4X4 collInvViewProj_ = {}; // transpose 済み
    float collScreen_[4] = { 0, 0, 0.1f, 1000.0f }; // w, h, nearZ, farZ
    bool collValid_ = false;
};

} // namespace mye
