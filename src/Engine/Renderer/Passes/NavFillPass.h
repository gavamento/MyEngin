//====================================================================================
//                          NavFillPass.h
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          ナビメッシュの半透明の塗りの描画パス
//====================================================================================
#pragma once
#include <cstdint>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// 半透明の三角形リストを SceneView / GameView / 撮影の RT に重ね描きする (M82e)。
// 深度テストあり・深度書き込みなし・アルファブレンド。床との Z ファイトは、持ち上げ (呼び出し側が頂点に含める)
// と傾斜つきの深度バイアスで避ける。シェーダは EditorLinePass と同じ editor_line (位置 + 色をそのまま出す)。
// 頂点は 1 つ 28 バイト (位置 float3 + 色 float4)。内容が変わったことは serial で知らせる
// (同じ serial の間は GPU の頂点バッファを作り直さない)。
// レイヤ規約: 生 D3D11 はこの Renderer 層に閉じる。決定論規約: sim には触れない
class NavFillPass {
public:
    static constexpr uint32_t kVertexStride = 28;

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // vertices: kVertexStride バイトの頂点を 3 つで三角形 1 枚。vertexCount が 0 なら何もしない
    void Render(GraphicsDevice& device, ShaderManager& shaders, const void* vertices, uint32_t vertexCount,
                uint64_t serial, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int width, int height,
                const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj);

private:
    bool Upload(GraphicsDevice& device, const void* vertices, uint32_t vertexCount, uint64_t serial);

    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;      // 頂点数
    uint32_t uploadedCount_ = 0;   // vb_ に入っている頂点数
    uint64_t uploadedSerial_ = ~0ull;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
};

} // namespace mye
