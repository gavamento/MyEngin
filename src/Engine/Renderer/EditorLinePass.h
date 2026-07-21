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

// エディタ補助線描画 (M9)。グリッド・コライダーワイヤ・選択アウトライン・アイコンを
// 線分リストとして貯めて SceneView の RT に上書き描画する。
// レイヤ規約: 生 D3D11 はこの Renderer 層に閉じる。決定論規約: sim には触れない。
//
// 使い方: Begin() → Add*() で線を貯める → Render() で描画。
// 深度テスト有り (occluded) / 無し (常時最前面 = 選択アウトライン) を分けて保持する。
class EditorLinePass {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    void Begin();
    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t rgba,
                 bool onTop = false);
    // ワールド行列で変換した単位ボックス (中心原点・半径 half) のワイヤ
    void AddWireBox(const DirectX::XMFLOAT4X4& world, const DirectX::XMFLOAT3& half, uint32_t rgba,
                    bool onTop = false);
    // 軸平行ワイヤボックス (min/max)
    void AddAABB(const DirectX::XMFLOAT3& lo, const DirectX::XMFLOAT3& hi, uint32_t rgba,
                 bool onTop = false);
    void AddWireSphere(const DirectX::XMFLOAT3& center, float radius, uint32_t rgba,
                       bool onTop = false);
    // カプセルワイヤ (M28a)。axisX/Y/Z は正規直交基底 (Y = カプセル軸)、halfSeg は線分半長。
    // 物理 (ShapePose) と同じパラメータ表現 — ギズモと判定のズレを構造的に防ぐ
    void AddWireCapsule(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& axisX,
                        const DirectX::XMFLOAT3& axisY, const DirectX::XMFLOAT3& axisZ,
                        float radius, float halfSeg, uint32_t rgba, bool onTop = false);
    void AddGrid(int halfCount, float spacing, uint32_t lineRgba, uint32_t axisXRgba,
                 uint32_t axisZRgba);

    // 貯めた線を rtv/dsv に描画する (クリアしない = シーンの上に重ねる)
    void Render(GraphicsDevice& device, ShaderManager& shaders, ID3D11RenderTargetView* rtv,
                ID3D11DepthStencilView* dsv, int width, int height,
                const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj);

private:
    struct LineVertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 color;
    };
    void PushVerts(std::vector<LineVertex>& dst, ID3D11DeviceContext* dc, GraphicsDevice& device);
    static DirectX::XMFLOAT4 Unpack(uint32_t rgba);

    bool ready_ = false;
    AssetID shader_ = {};
    std::vector<LineVertex> depthTested_;
    std::vector<LineVertex> onTop_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOn_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
};

} // namespace mye
