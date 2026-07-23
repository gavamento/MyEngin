#pragma once
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/RenderTexture.h"
#include "Engine/Renderer/SkyboxPass.h"

namespace mye {

// Deferred レンダリング (engine_spec.md 6.1 Option B / M6.5)。
//   1. ジオメトリパス: opaque → GBuffer (albedo RGBA8 + 法線 R10G10B10A2 + 共有深度)
//   2. ライティングパス: フルスクリーン解決 (Forward と同じ common.hlsli の関数)
//   3. 透明後段: transparent はマテリアルの Forward シェーダで上描き
//      (パーティクルはさらにその後、RenderSystem が共通の Forward 後段として描く)
// ライトは Forward と同じ LightList データを使うため、切替で見た目が一致する
class DeferredPath : public IRenderPath {
public:
    const char* Name() const override { return "Deferred"; }
    bool Init(GraphicsDevice& device, ShaderManager& shaders) override;
    void Shutdown() override;
    void Render(GraphicsDevice& device, const RenderView& view, const RenderQueue& queue,
                const SceneLightData& lights, RenderResources& resources,
                ShaderManager& shaders) override;

private:
    RenderTexture gbAlbedo_;   // a=1 でジオメトリ有りマーク
    RenderTexture gbNormal_;   // ワールド法線 *0.5+0.5
    RenderTexture gbPosition_; // ワールド座標 (Point/Spot ライティング用)
    RenderTexture gbMaterial_; // r=metallic g=roughness (PBR、M17)

    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> materialCB_; // PBR パラメータ
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> shadowSampler_; // 比較サンプラ (PCF)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> iblSampler_;    // LINEAR/CLAMP (M38c)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerWire_; // SceneView Wireframe (M40b)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOpaque_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTransparent_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAlpha_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> boneCB_; // ボーンパレット (b3、スキニング、M18)
    AssetID gbufferShader_ = {};
    AssetID gbufferSkinnedShader_ = {}; // deferred_gbuffer_skinned (スキンメッシュ用に差替)
    AssetID lightShader_ = {};
    // ---- インスタンシング (M38f)。非スキン opaque の連続 run を一括描画 ----
    AssetID gbufferInstancedShader_ = {};
    MeshInstanceBuffer instanceBuf_;
    std::vector<uint8_t> canInstance_; // フレーム毎スクラッチ
    std::vector<MeshInstanceRun> runs_;
    std::vector<DirectX::XMFLOAT4X4> worlds_;
    SkyboxPass skybox_; // ライトパス後・透明前に空を塗る (M29d)

    // ---- SSAO (M38e、半解像度) ----
    RenderTexture ssaoRaw_;
    RenderTexture ssaoBlur_;
    AssetID ssaoShader_ = {};
    AssetID ssaoBlurShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> ssaoCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> ssaoBlurCB_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointClamp_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointWrap_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> noiseTex_; // 4x4 ランダム回転
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> noiseSrv_;
};

} // namespace mye
