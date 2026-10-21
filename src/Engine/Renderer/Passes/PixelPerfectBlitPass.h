//====================================================================================
//                          PixelPerfectBlitPass.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          低解像度 RT を整数倍・点サンプルで最終ターゲットへ貼るパス
//====================================================================================
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// PixelPerfectCamera2D の低解像度 RT (src) を、zoom 倍の矩形にして dst の中央へ貼る。
// 矩形が dst より大きい分は両端で切る (RT は画面を覆う偶数サイズなので黒帯は出ない)。
// 遅延 Init: PixelPerfect が現れるまで何も確保しない。生 D3D11 はこの Renderer 層に閉じる
class PixelPerfectBlitPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    bool IsReady() const { return ready_; }

    // dst の RT / ビューポート / 入力アセンブラは上書きされる (呼び出し側は戻さない前提の最終段で使う)
    bool Blit(GraphicsDevice& device, ShaderManager& shaders, ID3D11ShaderResourceView* src, int32_t srcW,
              int32_t srcH, ID3D11RenderTargetView* dst, int32_t dstW, int32_t dstH, int32_t zoom);

    void Release();

private:
    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerPoint_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
};

} // namespace mye
