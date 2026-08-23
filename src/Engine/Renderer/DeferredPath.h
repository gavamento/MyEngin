#pragma once
#include <vector>
#include <wrl/client.h>

#include "Engine/Renderer/HzbPass.h"
#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/RenderPath.h"
#include "Engine/Renderer/RenderTexture.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/SsrPass.h"
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
    // M56c: HZB を組んだ GPU 時間 (ProfilerWindow 表示用)。組まないフレームは前の値が残る
    float HzbGpuMs() const override { return hzb_.GpuMs(); }
    // M56d: SSR (コピー + 階層 Z トレース + 加算合成) の GPU 時間。同上
    float SsrGpuMs() const override { return ssr_.GpuMs(); }
    // M57d: 光パス (t15) で不透明ピクセルへ合成する。M57e で背景ピクセルと透明後段
    // (Forward t7) も受け持つようになった。スカイとパーティクルは SkyboxPass /
    // ParticleSystem が view.froxelSRV を直接読む。ここが true になった時点で
    // ゴッドレイは自動 off になる (三重計上の解消)
    bool AppliesFroxel() const override { return true; }

private:
    // M56a: デカール (投影ボックス)。ジオメトリパス直後・SSAO 前に albedo を上描きする。
    // view.decals が null / 空なら 1 命令も発行せずに return する
    void RenderDecals(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                      RenderResources& resources, const DirectX::XMFLOAT4X4& viewProjT);
    // M56b: RT1 (法線) の読み取り用コピーを用意する (無ければ作る / サイズが変わったら作り直す)。
    // 戻り値 false = 確保できなかった → そのフレームは albedo だけの M56a 相当へ縮退する
    bool EnsureNormalCopy(GraphicsDevice& device);

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
    // ---- M56b: 法線 / roughness の上描き ----
    // MRT ごとに別の書込マスクとブレンド係数を持つ独立ブレンド (Init のコメント参照)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendDecal_;
    // ★**RT1 (法線) のコピー**。デカールは受け面の法線を角度フェードのために読むので、
    //   RT1 を RTV として bind するフレームは同じリソースを SRV でも読めない
    //   (同一リソースの読み書き二重バインドは D3D が禁じている)。RenderTexture を
    //   使わないのは RTV / DSV が要らないため — SRV だけの素の Texture2D で足りる。
    //   **法線も roughness も書かないフレームでは 1 バイトも確保しない** (遅延生成)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gbNormalCopy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gbNormalCopySrv_;
    int normalCopyW_ = 0;
    int normalCopyH_ = 0;
    // ---- M56c: HZB (min-Z ピラミッド) ----
    // 本番の消費者 (SSR) は M56d。**このサブでは view.hzbDebug != 0 のときしか組まない**ので、
    // 既定の絵は 1 命令も増えない。可視化シェーダは velocityDebugShader_ と同じ立ち位置
    HzbPass hzb_;
    AssetID hzbDebugShader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> hzbDebugCB_;
    // ---- M56d: SSR (スクリーンスペース反射) ----
    // HZB の唯一の本番消費者。**view.ssrEnabled が HZB を組む条件に or で入る** —
    // 忘れると SSR が null のピラミッドを見て何も映らない (M56c からの申し送り)
    SsrPass ssr_;
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
