#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Vfx/VfxGeometry.h"

namespace mye {

class World;
class GraphicsDevice;
class ShaderManager;
class UIRenderer;
struct RenderResources;
struct RenderView;

// TrailRenderer の点列ストア (M29c)。点列は sim 非対象 (描画専用 = NoHash) なので
// コンポーネント外のここに常駐する。**蓄積は tick 側** (EngineLoop) — Render 側で
// 蓄積すると SceneView/GameView/backbuffer の多重描画で多重サンプルされるため。
// D3D 非依存 (VfxSelfTest がヘッドレスで検証する)。
class TrailStore {
public:
    static constexpr int kMaxPoints = 256; // 1 トレイルの点数上限 (超過は最古を落とす)

    struct Buffer {
        EntityID owner;
        std::vector<vfx::TrailPoint> pts; // 追加順 (先頭=最古 → 末尾=最新)
    };

    // 毎 tick: 現存する {TrailRenderer + WorldMatrix} エンティティと同期し、
    // 点の追加 (emitting かつ minVertexDistance 以上の移動) と寿命失効を処理する。
    // 消えた/無効化されたエンティティのバッファは除去。
    void Update(World& world, uint64_t tick);
    void Reset() { buffers_.clear(); }
    const std::vector<Buffer>& Buffers() const { return buffers_; }

private:
    std::vector<Buffer> buffers_; // owner.index 昇順
};

// M57追補: VfxCB のフォグ / フロクセル部分の素材。**D3D デバイス非依存**にしてあるのは
// VfxSelfTest がヘッドレスで検査できるようにするため (描画そのものの担保は golden の担当)。
// ★存在理由は「VFX が forward_lit とまったく同じ RenderView フィールドを読んでいる」ことを
//   機械検査できるようにすること。M32c の手書きフォグは M29d の 5 本しか読んでおらず、
//   **M43a の 6 本 (ハイトフォグ + 太陽インスキャッタ) を落としていた** — 同じシーンで
//   メッシュと VFX の霧の濃さが食い違っていて、それに気づく仕掛けがどこにも無かった
struct VfxFogParams {
    int32_t fogMode = -1;
    DirectX::XMFLOAT3 fogColor = {};
    float fogDensity = 0.0f;
    float fogStart = 0.0f;
    float fogEnd = 0.0f;
    float heightFalloff = 0.0f;
    float baseHeight = 0.0f;
    float inscatterIntensity = 0.0f;
    float inscatterPower = 8.0f;
    DirectX::XMFLOAT3 sunDirection = {};
    DirectX::XMFLOAT3 sunColor = {};
    int32_t froxelEnabled = 0; // **FroxelIsBound 1 本**で決める (自作ゲート禁止)
    float froxelNearZ = 0.0f;
    float froxelFarZ = 0.0f;
    float froxelSlices = 0.0f;
    float screenW = 0.0f;
    float screenH = 0.0f;
};
VfxFogParams BuildVfxFogParams(const RenderView& view);

// Sprite / Trail / TextMesh のワールド空間 VFX 描画 (M29c)。
// RenderSystem が path.Render (不透明+透明メッシュ) の後・パーティクルの前に呼ぶ —
// HDR 中間に描かれ postfx を通る。RT/ビューポートはパスがバインド済み前提 (particles と同じ)。
// vfx アイテム同士は viewZ 降順 (back-to-front)、透明メッシュとの相互ソートはしない (制限)。
// レイヤ規約: 生 D3D11 はこのクラスに閉じる。描画専用 = sim/hash 非干渉。
class VfxRenderer {
public:
    // ui は TextMesh のフォントアトラス共有元 (null なら TextMesh は描かない)
    bool Init(GraphicsDevice& device, ShaderManager& shaders, UIRenderer* ui);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // tick 側 (EngineLoop フェーズ 4 の particles 直後、simulateScripts 中のみ)
    void UpdateTrails(World& world, uint64_t tick) { trails_.Update(world, tick); }
    // シーン遷移時 (ResetParticles の隣)
    void Reset() { trails_.Reset(); }
    TrailStore& Trails() { return trails_; }

    void Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                RenderResources& resources, const RenderView& view);

private:
    struct Batch {
        ID3D11ShaderResourceView* srv = nullptr;
        uint32_t start = 0;
        uint32_t count = 0;
        bool pointSample = false; // フォントアトラスは POINT (crisp)、他は LINEAR
    };

    void PushVerts(ID3D11ShaderResourceView* srv, bool pointSample, const VfxVertex* v, int count);

    bool ready_ = false;
    AssetID shader_ = {};
    UIRenderer* ui_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPoint_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthReadOnly_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    TrailStore trails_;

    // フレームごとの作業領域
    std::vector<VfxVertex> verts_;
    std::vector<Batch> batches_;
    std::vector<VfxVertex> scratch_; // テキストのローカル座標構築用
};

} // namespace mye
