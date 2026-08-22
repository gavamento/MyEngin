#pragma once
#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct RenderView;

// M55d: テンポラル AA。PostProcess::Resolve のチェーン先頭 (DoF より前) で走り、
// 「今フレームの HDR シーン」に「前フレームの TAA 結果を velocity で再投影したもの」を
// 混ぜる。数式は assets\shaders\postfx_taa.hlsl (CPU ミラーは PostFxMath.h の mye::taa)。
//
// 成立の条件 (どれか 1 つでも欠けたら Run は nullptr を返し、絵は 1 ビットも変わらない):
//   ① RenderView::taaEnabled != 0 (CameraPostFx の taaOn / グローバル設定 / --taa)
//   ② RenderView::velocitySRV != nullptr = **Deferred パスのみ** (v1 制限)
//   ③ viewKey が 1..3 (AssetPreview は履歴を持たない)
//   ④ 前フレームも同じビューが同じ解像度で描かれた (描画通番が 1 つ違い)
//
// ★履歴は **(w,h) キーではなく viewKey キー**で持つ。PostProcess::Target の LRU に
//   相乗りすると、SceneView と GameView が同サイズのときに互いの履歴を食い合って
//   混線する。viewKey 別に分ける設計は RtPasses::kHistorySlots (M46d) が前例で、
//   通番の連続性判定とリサイズ破棄もそちらに倣っている。
class TaaPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // sceneSRV (今フレームの HDR) を解決して履歴テクスチャへ書き、その SRV を返す。
    // 戻り値 nullptr = 走らせなかった (呼び出し側は sceneSRV をそのまま使い続ける)。
    // feedback は履歴の残し率 [0,1) — 0 で「履歴を混ぜない」= 恒等
    ID3D11ShaderResourceView* Run(GraphicsDevice& device, ShaderManager& shaders,
                                  const RenderView& view, ID3D11ShaderResourceView* sceneSRV,
                                  float feedback);

private:
    // viewKey (0=AssetPreview 1=runtime 2=SceneView 3=GameView) 毎に履歴を分ける
    static constexpr int kHistorySlots = 4;

    // ping-pong する履歴 (フル解像度 HDR)。write = 今フレームの書き込み先 (読みは 1-write)。
    // 「前フレームも描かれたか」は描画通番で見るので毎フレームのクリアは要らない
    struct History {
        RenderTexture color[2];
        int write = 0;
        int w = 0;
        int h = 0;
        uint32_t lastSerial = 0;
        bool hasLast = false;
    };

    History hist_[kHistorySlots];
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    bool ready_ = false;
};

} // namespace mye
