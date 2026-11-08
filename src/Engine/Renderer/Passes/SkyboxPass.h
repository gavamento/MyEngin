#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/Pipeline/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// スカイボックス描画 (M29d gradient / M38b cubemap)。フルスクリーン三角形を z=1 (far)・
// 深度 LESS_EQUAL・書き込み無しで描き、ジオメトリの無いピクセルだけを空の色で塗る。
// 挿入点はパス毎: Forward = 不透明後・透明前 / Deferred = ライトパス後・透明前。
// CB は PS の b3 (b0-b2 はメッシュ描画が使用中のため衝突しないスロット)。
// cubemap (view.skyMode==1 + skyCubemap SRV) は t0/s0 でサンプル。
class SkyboxPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown(); // GPU オブジェクトを手放して Init 前へ戻す (デバイス復旧用、M88)
    // view.skyMode < 0 なら何もしない。RT/ビューポートは設定済み前提だが、
    // 深度テストが要るため RTV+DSV を自前で再バインドする (deferred のライトパス後は DSV 無し)
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view);

    // Forward のスカイ無しの背景 (clearColor のまま残った画素) に、地面の霧とフロクセルを掛ける全画面パス
    // (Deferred の光パスの背景分岐と同じ式)。条件が欠けたら何も描かない = 絵はビット不変。
    // スカイを描く経路 (Render) とは排他: skyMode >= 0 のビューでは false。
    static bool NeedsBackgroundFog(const RenderView& view);
    void RenderBackgroundFog(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view);

private:
    bool ready_ = false;
    AssetID shader_ = {};     // gradient (skybox.hlsl)
    AssetID shaderCube_ = {}; // cubemap (skybox_cubemap.hlsl、M38b)
    AssetID shaderPano_ = {}; // panoramic (skybox_panoramic.hlsl)
    AssetID shaderBgFog_ = {}; // Forward の背景の霧 (forward_background_fog.hlsl)
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> bgFogCb_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthReadOnly_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;     // cubemap 用 LINEAR/CLAMP
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPano_; // panoramic 用 LINEAR/WRAP(U)+CLAMP(V)
};

} // namespace mye
