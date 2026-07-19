#pragma once
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;
class World;
struct RenderResources;

// エディタのマウスピッキング (M9)。メッシュを ID 付きで R32_UINT ターゲットへ描画し、
// 指定ピクセルの ID を staging テクスチャ経由で 1 ピクセルだけ読み戻す。
// レイヤ規約: 生 D3D11 はこの Renderer 層クラス内に閉じ、エディタへは Pick() だけ公開する。
// 決定論規約: sim 状態には一切触れない (WorldMatrix を読むだけ、RNG/構造変更なし)。
class PickingPass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // world の MeshRenderer を ID 付きで描画し、(px,py) の EntityID を返す (無ければ kNullEntity)。
    // view/proj は SceneView と同じ (未転置)。width/height は描画解像度、px/py は左上原点ピクセル
    EntityID Pick(GraphicsDevice& device, World& world, ShaderManager& shaders,
                  RenderResources& resources, const DirectX::XMFLOAT4X4& view,
                  const DirectX::XMFLOAT4X4& proj, int width, int height, int px, int py);

private:
    bool ready_ = false;
    AssetID shader_ = {};
    RenderTexture target_; // R32_UINT + depth
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perFrameCB_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> perObjectCB_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff_; // UINT RT はブレンド非対応 — 明示無効化
    std::vector<EntityID> idMap_; // 描画順。index+1 = ID
};

} // namespace mye
