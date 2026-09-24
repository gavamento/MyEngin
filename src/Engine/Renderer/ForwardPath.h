#pragma once
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TerrainPass.h"
#include "Engine/Renderer/WaterPass.h"

namespace mye {

struct Material;             // GpuResources.h
struct Mesh;                 // GpuResources.h
struct SurfaceMaterialState; // GpuResources.h (M79 sub-02)

// Forward レンダリング (engine_spec.md 6.1 Option A)。
// opaque を手前順 → transparent を奥順で 1 パス描画する
class ForwardPath : public IRenderPath {
public:
    const char* Name() const override { return "Forward"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                const SceneLightData& lights, RenderResources& resources,
                ShaderManager& shaders) override;
    // M57e: Forward も t7 でフロクセルを合成する (不透明 / 透明 / 地形 + スカイ)。
    // ★合成を外すなら false に戻すこと — true のまま合成が無いと「ゴッドレイだけ消えて
    //   霧が増えない」= 霧が減るだけになる
    bool AppliesFroxel() const override { return true; }

private:
    // runs 非 null = opaque のインスタンス run 一括描画を併用 (M38f)。transparent は nullptr
    void DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                   const RenderView& view, RenderResources& resources, ShaderManager& shaders,
                   const std::vector<MeshInstanceRun>* runs);
    // M79 sub-02: shader が "*.surface" のアイテムを 1 個描く (色エントリのみ。
    // 深度書き込み/ブレンドは呼び出し元が既に設定済みの opaque/transparent ステートに従う)。
    // 描画後に IA/VS/PS/CB/SRV/サンプラの一部が forward_lit の前提と食い違うので、
    // 呼び出し側 (DrawItems) が続けて RestoreFixedBindings 相当を行うこと
    void DrawSurfaceItem(GraphicsDevice& device, const RenderItem& item, const Material& mat,
                         const Mesh& mesh, SurfaceMaterialState& surf, ShaderManager& shaders,
                         RenderResources& resources, const RenderView& view);

    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialCB_; // PBR パラメータ (metallic/roughness)
    Microsoft::WRL::ComPtr<ID3D11Buffer> boneCB_;     // ボーンパレット (b3、スキニング、M18)
    AssetID skinnedShader_ = {};                      // forward_skinned (スキンメッシュ用に差替)
    // ---- インスタンシング (M38f)。forward_lit マテリアルの opaque 連続 run のみ対象 ----
    AssetID litShader_ = {};          // forward_lit (run 判定: mat->shader がこれと一致する時のみ)
    AssetID litInstancedShader_ = {}; // forward_lit_instanced
    MeshInstanceBuffer instanceBuf_;
    std::vector<uint8_t> canInstance_;        // フレーム毎スクラッチ
    std::vector<MeshInstanceRun> runs_;
    std::vector<DirectX::XMFLOAT4X4> worlds_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_; // 比較サンプラ (PCF)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> iblSampler_;    // LINEAR/CLAMP (s2、M38c)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerWire_; // SceneView Wireframe (M40b)
    // M79 sub-06: doubleSided なサーフェス用 (Cull None、Solid)。DrawSurfaceItem が描画直前に
    // 張り、直後に restoreForwardLitBindings が rasterizer_/rasterizerWire_ へ戻す
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerCullNone_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_; // 書き込みなし
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    // ---- M79 sub-02: サーフェスシェーダーの予約 CB (forward_lit の b0-b2 とは別バッファ) ----
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerFrameCB_;   // MyEnginePerFrame
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceFrameCB_;      // MyEngineSurfaceFrame
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfacePerObjectCB_;  // MyEnginePerObject
    Microsoft::WRL::ComPtr<ID3D11Buffer> surfaceWaterCB_;      // MyEngineWater (sub-05 まで 0 埋め)
    AssetID surfaceErrorId_ = {}; // "surface_error" (失敗時のマゼンタ代替)
    std::unordered_set<uint64_t> skinnedSurfaceWarned_; // スキン+サーフェスの WARN はマテリアル毎に 1 回
    SkyboxPass skybox_; // 不透明後・透明前に空を塗る (M29d)
    // 地形 (M58c)。不透明メッシュの直後・スカイボックスの前に描く (深度を書くため)
    TerrainPass terrain_;
    // 水面 (スカイボックス後・透明メッシュ前)
    WaterPass water_;
};

} // namespace mye
