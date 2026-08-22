#pragma once
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/RenderTexture.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/TerrainPass.h"

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
    // M55c/M55d: GBuffer RT4 = 画面速度。Deferred だけが書ける (Forward に MRT は無い) ので、
    // TAA / モーションブラー v2 / RT の物体モーションは **Deferred 限定**の機能になる
    bool WritesVelocity() const override { return true; }
    ID3D11ShaderResourceView* VelocitySRV() const override { return gbVelocity_.SRV(); }

private:
    // M56a: デカール (投影ボックス)。ジオメトリパス直後・SSAO 前に albedo を上描きする。
    // view.decals が null / 空なら 1 命令も発行せずに return する
    void RenderDecals(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                      RenderResources& resources, const DirectX::XMFLOAT4X4& viewProjT);

    RenderTexture gbAlbedo_;   // a=1 でジオメトリ有りマーク
    RenderTexture gbNormal_;   // ワールド法線 *0.5+0.5
    RenderTexture gbPosition_; // ワールド座標 (Point/Spot ライティング用)
    RenderTexture gbMaterial_; // r=metallic g=roughness (PBR、M17)
    // M55c: 画面速度 (R16G16_FLOAT)。**このサブでは誰も読まない** — 消費者は
    // M55d (TAA) / M55e (モーションブラー v2) / M55f (RT の物体モーション)。
    // 読む側は自分で SRV を bind する (光パスの t0-t11 の並びは 1 つも動かさない)
    RenderTexture gbVelocity_;

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
    // M55c: worlds_ と**同じ並び**の「前フレームに描いた world」(VS t1)。
    // BuildInstanceRuns は共有 (Forward/Shadow も呼ぶ) なので触らず、runs_ から組み直す
    std::vector<DirectX::XMFLOAT4X4> prevWorlds_;
    MeshInstanceBuffer prevInstanceBuf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> velocityCB_; // b4 (GBuffer パス専用)
    // M55c: velocity の可視化 (RenderView::velocityDebug != 0 のときだけ)
    AssetID velocityDebugShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> velocityDebugCB_;
    // ---- M56a: デカール ----
    AssetID decalShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> decalCB_;
    // 投影ボックス専用のラスタライザ。CULL_FRONT (裏面を描く = カメラが箱に入っても消えない)
    // + DepthClipEnable=FALSE (箱が near/far を跨いでも欠けない)
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerDecal_;
    SkyboxPass skybox_; // ライトパス後・透明前に空を塗る (M29d)
    // 地形 (M58c)。GBuffer へ専用シェーダで書く — 不透明パスは material->shader を
    // 見ないのでマテリアル経由では通せない (TerrainPass.h の頭のコメント参照)
    TerrainPass terrain_;

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
