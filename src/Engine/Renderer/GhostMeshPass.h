#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
struct Mesh;

// 分岐のゴーストのメッシュ描画 (M72i)。エディタの補助描画で、EditorLinePass の隣に立つ。
// 「(Mesh, ワールド行列, 色) の列を、描き終わった SceneView の RT へ、深度テストあり・
// 深度書き込みなし・アルファブレンドで重ねる」だけ。ライティングもマテリアルも読まない
// (ghost_mesh.hlsl が法線で軽く陰影を付けるだけ)。
//
// ★sim にも本編の描画にも触れない: RT は SceneView のもの、状態は自前、描くのは呼び出し側が
//   Add した物だけ。golden (Runtime / GameView) には 1 画素も出ない。
// 使い方: Begin() → Add() で貯める → Render() で描画 (EditorLinePass と同じ 3 段)
class GhostMeshPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    void Begin();
    // rgba = 0xRRGGBBAA。alpha がそのまま不透明度になる
    void Add(const Mesh* mesh, const DirectX::XMFLOAT4X4& world, uint32_t rgba);

    // 貯めた物を rtv/dsv に描く (クリアしない = シーンの上に重ねる)
    void Render(GraphicsDevice& device, ShaderManager& shaders, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv, int width, int height,
                const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj);

    size_t Count() const { return items_.size(); }

private:
    struct Item {
        const Mesh* mesh = nullptr;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4 color;
    };

    bool ready_ = false;
    AssetID shader_ = {};
    std::vector<Item> items_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
};

} // namespace mye
