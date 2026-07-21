#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class GraphicsDevice;
class ShaderManager;
struct RenderResources;

// ゲーム内 UI 描画 (M21)。UIElementComponent を screen-space クアッド (色/画像/テキスト) として
// backbuffer / GameView RT に重ね描画する。フォントは ImGui 内蔵アトラス (ProggyClean) を
// 一時コンテキストで焼いて再利用する (新規 external / フォントファイル不要)。
// レイヤ規約: 生 D3D11 はこのクラスに閉じる。決定論規約: sim には触れない (描画専用 = 非ハッシュ)。
// ボタン操作は描画に非関与 — スクリプトが InputSnapshot のマウス (決定論) でヒットテストする。
class UIRenderer {
public:
    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();
    bool IsReady() const { return ready_; }

    // world の UIElementComponent を rtv に重ね描画する (クリアしない)。
    // mouse* は button の hover/press 表示にのみ使う (display only、非決定論可)。
    void Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                RenderResources& resources, ID3D11RenderTargetView* rtv, int width, int height,
                int mouseX, int mouseY, bool mouseDown);

    // ---- レイアウト補助 (ヘッドレスでも使える純関数、selftest 対象) ----
    // 単一行テキストのピクセル幅
    float TextWidth(const char* s, float scale) const;
    float LineHeight(float scale) const { return fontLineH_ * scale; }
    // anchor(0..8) + オフセット (x,y) + サイズ(w,h) から最終ピクセル矩形の左上を求める
    static void ResolveAnchor(int anchor, float x, float y, float w, float h, int screenW,
                              int screenH, float& outX, float& outY);

private:
    struct UIVertex {
        DirectX::XMFLOAT2 pos; // ピクセル座標
        DirectX::XMFLOAT2 uv;
        DirectX::XMFLOAT4 color;
    };
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // アトラス UV
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // ペンからの px オフセット (fontSize 基準)
        float advance = 0;
        bool valid = false;
    };
    struct Batch {
        ID3D11ShaderResourceView* srv = nullptr;
        uint32_t start = 0;
        uint32_t count = 0;
    };

    void BakeFont(GraphicsDevice& device);
    void PushQuad(ID3D11ShaderResourceView* srv, float x, float y, float w, float h, float u0,
                  float v0, float u1, float v1, const DirectX::XMFLOAT4& col);
    // テキストをグリフクアッド列として積む (左上 px 起点、fontSrv_ を使う)
    void PushText(const char* s, float x, float y, float scale, const DirectX::XMFLOAT4& col);

    bool ready_ = false;
    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    uint32_t vbCapacity_ = 0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fontTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontSrv_;
    Glyph glyphs_[128];
    float fontSize_ = 13.0f;  // アトラスの基準ピクセルサイズ
    float fontLineH_ = 15.0f; // 行送り

    // フレームごとの作業領域 (Render 内で構築)
    std::vector<UIVertex> verts_;
    std::vector<Batch> batches_;
    ID3D11ShaderResourceView* whiteSrv_ = nullptr;
};

} // namespace mye
