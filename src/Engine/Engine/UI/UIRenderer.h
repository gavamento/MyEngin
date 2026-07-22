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

// ゲーム内 UI 描画 (M21、M34 で日本語対応)。UIElementComponent を screen-space クアッド
// (色/画像/テキスト) として backbuffer / GameView RT に重ね描画する。フォントは FontAtlas
// (stb_truetype 動的グリフキャッシュ、TTF 無し環境は埋め込み 8x8 フォールバック)。
// レイヤ規約: 生 D3D11 はこのクラスに閉じる。決定論規約: sim には触れない (描画専用 = 非ハッシュ)。
// ボタン操作は描画に非関与 — スクリプトが InputSnapshot のマウス (決定論) でヒットテストする。
class UIRenderer {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders, const std::wstring& assetsRoot);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // world の UIElementComponent を rtv に重ね描画する (クリアしない)。
    // mouse* は button の hover/press 表示にのみ使う (display only、非決定論可)。
    void Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                RenderResources& resources, ID3D11RenderTargetView* rtv, int width, int height,
                int mouseX, int mouseY, bool mouseDown);

    // ---- レイアウト補助 ----
    // 単一行テキストのピクセル幅 (UTF-8。グリフは EnsureText 済み前提 — Render 内では成立)
    float TextWidth(const char* s, float scale) const;
    float LineHeight(float scale) const { return FontAtlas::kUILineH * scale; }
    // anchor(0..8) + オフセット (x,y) + サイズ(w,h) から最終ピクセル矩形の左上を求める
    static void ResolveAnchor(int anchor, float x, float y, float w, float h, int screenW,
                              int screenH, float& outX, float& outY);

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
        uint32_t start = 0;
        uint32_t count = 0;
    };

    void PushQuad(ID3D11ShaderResourceView* srv, bool linear, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, const DirectX::XMFLOAT4& col);
    // テキストをグリフクアッド列として積む (左上 px 起点。UTF-8、'?' フォールバック)
    void PushText(const char* s, float x, float y, float scale, const DirectX::XMFLOAT4& col);

    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;       // POINT (画像/8x8 フォント)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_; // LINEAR (TTF フォント縮小)
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    FontAtlas font_;

    // フレームごとの作業領域 (Render 内で構築)
    std::vector<UIVertex> verts_;
    std::vector<Batch> batches_;
    ID3D11ShaderResourceView* whiteSrv_ = nullptr;
};

} // namespace mye
