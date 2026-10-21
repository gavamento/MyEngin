//====================================================================================
//                          Sprite2DPass.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          Sprite2D (2D スプライト) の描画パス
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Device/RenderTexture.h"
#include "Engine/Renderer/Passes/Sprite2DDrawList.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct ShaderProgram;
class TextureLibrary;

// Sprite2DRenderer が並べた描画リストを、パスがバインド済みの HDR シーン RT へ重ねる (VfxRenderer と同じ位置)。
// ブレンド SRC_ALPHA / INV_SRC_ALPHA、ZTest LEqual・ZWrite off、CULL_NONE。サンプラは point / linear の 2 つを自前で持ち、
// バッチごとに切り替える。バッチが 0 件なら Draw を 1 回も出さない。
// ステンシルを使うバッチ (マスク) があるときだけ、書き込み可能な DSV へ差し替えてステンシルを 0 でクリアし
// (深度は触らない)、終わったら元の RT / DSV に戻す。
// Light2D があるシーンだけ、群ごとのライトバッファを作る。法線マップ付きの lit スプライトがあれば先に法線 RT へ描き、
// shadows のライトごとに影 RT へキャスターの影を描いて、ライトの形状 PS が N·L と影を掛ける。
// レイヤ規約: 生 D3D11 はこの Renderer 層に閉じる。決定論規約: sim には触れない
class Sprite2DPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // RT / ビューポートはバインド済みのものを使う。描画したバッチ数を返す (SRV が引けないバッチは飛ばす)。
    // stencilDsv = ステンシルを書ける DSV (D24S8 の通常ビュー)。null ならマスクは無視され、マスク対象は素通りで描く
    uint32_t Render(GraphicsDevice& device, ShaderManager& shaders, TextureLibrary& textures,
                    const Sprite2DDrawList& list, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
                    ID3D11DepthStencilView* stencilDsv);

private:
    // 1 回の Render で、法線 RT・影・ライトバッファの作成が共有する値 (定義は cpp)
    struct LightContext;

    // スプライト頂点 (+ 法線 RT 用の頂点) を vb_ へ上げる (足りなければ作り直す)
    bool Upload(GraphicsDevice& device, const Sprite2DDrawList& list);
    // ライト頂点を lightVb_ へ上げる (足りなければ作り直す)
    bool UploadLights(GraphicsDevice& device, const Sprite2DDrawList& list);
    // lit スプライトの法線を法線 RT へ描く (クリア = 平ら)。成功なら true。呼ぶ前後の RT / 入力アセンブラは呼び出し側が戻す
    bool BuildNormalBuffer(GraphicsDevice& device, TextureLibrary& textures, const Sprite2DDrawList& list,
                           const LightContext& ctx, ShaderProgram& normalProg);
    // ライト 1 個の影を影 RT へ描く (clear 0 → キャスターの押し出し 1 → selfShadow == false の多角形 0)。
    // 呼んだ後は RT / 入力アセンブラ / シェーダがライト用ではなくなる
    void RenderShadowMap(GraphicsDevice& device, const Sprite2DDrawList& list, const Light2DInstance& light,
                         const LightContext& ctx);
    // 群 1 つ分のライトバッファを作る: グローバル色でクリア → 非 Global のライトを加算。
    // 呼ぶ前後で RT / ビューポート / 入力アセンブラの状態は呼び出し側が戻す
    void BuildLightBuffer(GraphicsDevice& device, TextureLibrary& textures, const Sprite2DDrawList& list,
                          const Sprite2DLightGroup& group, const LightContext& ctx);

    bool ready_ = false;
    AssetID shader_ = {};
    AssetID lightShader_ = {};  // sprite2d_light (ライト形状の描画)
    AssetID shadowShader_ = {}; // sprite2d_shadow (影 RT への書き込み)
    AssetID normalShader_ = {}; // sprite2d_normal (法線 RT への書き込み)
    Microsoft::WRL::ComPtr<ID3D11Buffer> shadowCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> shadowVb_;
    uint32_t shadowVbCapacity_ = 0; // 頂点数
    std::vector<Light2DVertex> shadowScratch_; // 1 ライト分の影の頂点 (毎回作り直す)
    Microsoft::WRL::ComPtr<ID3D11Buffer> normalCb_;
    RenderTexture shadowRt_;       // シーン RT と同サイズの R8。影を落とすライトとキャスターがあるときだけ確保する
    RenderTexture normalRt_;       // シーン RT と同サイズの RGBA8。法線マップ付きの lit スプライトがあるときだけ確保する
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightCb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lightVb_;
    uint32_t lightVbCapacity_ = 0; // 頂点数
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdd_;               // ライト: ONE / ONE
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;        // ライト: 深度もステンシルも使わない
    RenderTexture lightBuffer_;    // シーン RT と同サイズの RGBA16F。Light2D があるときだけ確保する
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> psCb_; // マスクのクリップ指定 (b1)
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0; // 頂点数
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPoint_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendNoColor_; // マスク: 色を書かない
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthMaskWrite_;   // ステンシルへ ref を書く (深度テストなし)
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthTestStencil_; // ステンシル == ref の画素だけ描く
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
};

} // namespace mye
