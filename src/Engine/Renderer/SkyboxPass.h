#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// スカイボックス描画 (M29d、gradient)。フルスクリーン三角形を z=1 (far)・深度 LESS_EQUAL・
// 書き込み無しで描き、ジオメトリの無いピクセルだけを空の色で塗る。
// 挿入点はパス毎: Forward = 不透明後・透明前 / Deferred = ライトパス後・透明前。
// CB は PS の b3 (b0-b2 はメッシュ描画が使用中のため衝突しないスロット)。
class SkyboxPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    // view.skyMode < 0 なら何もしない。RT/ビューポートは設定済み前提だが、
    // 深度テストが要るため RTV+DSV を自前で再バインドする (deferred のライトパス後は DSV 無し)
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view);

private:
    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthReadOnly_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
};

} // namespace mye
