#pragma once
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TerrainPass.h"

namespace mye {

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
    // ★M57d の時点では false だった — 合成が Deferred の光パスにしか無く、true に
    //   すると「ゴッドレイだけ消えて霧が増えない」= 霧が減るだけになったため。
    //   M57e で Forward 側の合成が入ったので true にできる
    bool AppliesFroxel() const override { return true; }

private:
    // runs 非 null = opaque のインスタンス run 一括描画を併用 (M38f)。transparent は nullptr
    void DrawItems(GraphicsDevice& device, const std::vector<RenderItem>& items,
                   const RenderView& view, RenderResources& resources, ShaderManager& shaders,
                   const std::vector<MeshInstanceRun>* runs);

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
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_; // 書き込みなし
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    SkyboxPass skybox_; // 不透明後・透明前に空を塗る (M29d)
    // 地形 (M58c)。不透明メッシュの直後・スカイボックスの前に描く (深度を書くため)
    TerrainPass terrain_;
};

} // namespace mye
