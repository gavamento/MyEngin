//====================================================================================
//                          HzbDebugPass.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          HZB ピラミッドの全画面可視化 (--hzb-debug)
//====================================================================================
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class HzbPass;
struct RenderView;

// HZB (min-Z / max-Z どちらも) の選んだミップを view.rtv へ最近傍で引き伸ばす。
// Deferred の min-Z 表示と、オクルージョン用 max-Z の表示 (Deferred / Forward) が共有する。
// 描画後は RTV + DSV を view の物へ戻し、深度・ブレンドを既定へ戻す。
// 表示の規約は debug_hzb.hlsl が正本
class HzbDebugPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    // hzbDebug = RenderView::hzbDebug (N: ミップ N-1。範囲外は端で頭打ち)
    void Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                const HzbPass& pyramid, int hzbDebug);

private:
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOpaque_;
};

} // namespace mye
