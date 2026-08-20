#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/FontAtlas.h"

namespace mye {

class World;
class GraphicsDevice;
class ShaderManager;
struct RenderResources;

// ゲーム内 UI 描画 (M21、M34 で日本語対応、M51e で親子/クリップ/整列/折返し)。
// UIElementComponent を screen-space クアッド (色/画像/テキスト) として backbuffer /
// GameView RT に重ね描画する。矩形解決は uilayout::ResolveRect (UIFocusNav と共有)。
// clipChildren はシザー矩形 (バッチはシザー変化でも分割)。フォントは FontAtlas
// (stb_truetype 動的グリフキャッシュ、TTF 無し環境は埋め込み 8x8 フォールバック)。
// レイヤ規約: 生 D3D11 はこのクラスに閉じる。決定論規約: sim には触れない (描画専用 = 非ハッシュ)。
// ボタン操作は描画に非関与 — スクリプトが InputSnapshot のマウス (決定論) でヒットテストする。
class UIRenderer {
public:
    // fontEmbedded=true でフォントアトラスを内蔵 8x8 に固定する (M52c: 決定的スクショ)
    bool Init(GraphicsDevice& device, ShaderManager& shaders, const std::wstring& assetsRoot,
              bool fontEmbedded = false);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // world の UIElementComponent を rtv に重ね描画する (クリアしない)。
    // mouse* は button の hover/press 表示にのみ使う (display only、非決定論可)。
    void Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                RenderResources& resources, ID3D11RenderTargetView* rtv, int width, int height,
                int mouseX, int mouseY, bool mouseDown);

    // ---- レイアウト補助 ----
    float LineHeight(float scale) const { return FontAtlas::kUILineH * scale; }

    // ---- フォント共有 (M34: VfxRenderer/TextMesh が EnsureText/Glyphs/SRV を使う) ----
    FontAtlas& Font() { return font_; }
    const FontAtlas& Font() const { return font_; }

private:
    struct UIVertex {
        DirectX::XMFLOAT2 pos; // ピクセル座標
        DirectX::XMFLOAT2 uv;
        DirectX::XMFLOAT4 color;
    };
    struct Batch {
        ID3D11ShaderResourceView* srv = nullptr;
        bool linear = false; // フォント (TTF) は LINEAR、画像/8x8 は POINT
        D3D11_RECT scissor = {}; // クリップ (M51e)。全要素で設定 (クリップ無しは RT 全域)
        uint32_t start = 0;
        uint32_t count = 0;
    };

    void PushQuad(ID3D11ShaderResourceView* srv, bool linear, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, const DirectX::XMFLOAT4& col);
    // [begin,end) の 1 行をグリフクアッド列として積む (左上 px 起点。UTF-8、'?' フォールバック)
    void PushTextLine(const char* begin, const char* end, float x, float y, float scale,
                      const DirectX::XMFLOAT4& col);
    // 矩形内に整列 (9-grid) + 折返しでテキストを積む (M51e。テキスト/ボタンラベル共通経路)
    void PushTextInRect(const char* s, float rx, float ry, float rw, float rh, float scale,
                        const DirectX::XMFLOAT4& col, int align, bool wrap);

    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;       // POINT (画像/8x8 フォント)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_; // LINEAR (TTF フォント縮小)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_; // ScissorEnable=TRUE (M51e)
    FontAtlas font_;

    // フレームごとの作業領域 (Render 内で構築)
    std::vector<UIVertex> verts_;
    std::vector<Batch> batches_;
    ID3D11ShaderResourceView* whiteSrv_ = nullptr;
    D3D11_RECT curScissor_ = {}; // PushQuad が参照する現在のシザー (要素毎に設定)
};

} // namespace mye
