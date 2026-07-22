#include "Engine/Engine/UI/UIRenderer.h"

#include <algorithm>
#include <cstring>
#include <tuple>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace mye {
namespace {

// 埋め込み 8x8 ビットマップフォント (font8x8_basic、パブリックドメイン。ASCII 0x20..0x7F)。
// 各行の bit0(LSB) が左端ピクセル。外部依存もフォントファイルも不要 = 自己完結・決定論。
const unsigned char kFont8x8[96][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x20 space
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, // !
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // "
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, // #
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 }, // $
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 }, // %
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 }, // &
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00 }, // (
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00 }, // )
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, // *
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00 }, // +
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // ,
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00 }, // -
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // .
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00 }, // /
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00 }, // 0
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00 }, // 1
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00 }, // 2
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00 }, // 3
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00 }, // 4
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00 }, // 5
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00 }, // 6
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00 }, // 7
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00 }, // 8
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00 }, // 9
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // :
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // ;
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00 }, // <
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00 }, // =
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, // >
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00 }, // ?
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00 }, // @
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, // A
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00 }, // B
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00 }, // C
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00 }, // D
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00 }, // E
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00 }, // F
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00 }, // G
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00 }, // H
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // I
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00 }, // J
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00 }, // K
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00 }, // L
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00 }, // M
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00 }, // N
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, // O
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00 }, // P
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00 }, // Q
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00 }, // R
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00 }, // S
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // T
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00 }, // U
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // V
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, // W
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, // X
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00 }, // Y
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00 }, // Z
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00 }, // [
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, // backslash
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 }, // ]
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00 }, // ^
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF }, // _
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 }, // `
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00 }, // a
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00 }, // b
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00 }, // c
    { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00 }, // d
    { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00 }, // e
    { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00 }, // f
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // g
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00 }, // h
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // i
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E }, // j
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00 }, // k
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // l
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00 }, // m
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00 }, // n
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00 }, // o
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F }, // p
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78 }, // q
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00 }, // r
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00 }, // s
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00 }, // t
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00 }, // u
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // v
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00 }, // w
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00 }, // x
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // y
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00 }, // z
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00 }, // {
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, // |
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00 }, // }
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // ~
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x7F
};

} // namespace

void UIRenderer::ResolveAnchor(int anchor, float x, float y, float w, float h, int screenW,
                               int screenH, float& outX, float& outY)
{
    (void)w;
    (void)h;
    const int col = anchor % 3;  // 0=左 1=中 2=右
    const int rowa = anchor / 3; // 0=上 1=中 2=下
    const float ox = (col == 0) ? 0.0f : (col == 1) ? screenW * 0.5f : static_cast<float>(screenW);
    const float oy = (rowa == 0) ? 0.0f : (rowa == 1) ? screenH * 0.5f : static_cast<float>(screenH);
    outX = ox + x;
    outY = oy + y;
}

bool UIRenderer::Init(GraphicsDevice& device, ShaderManager& shaders)
{
    ID3D11Device* dev = device.Device();
    shader_ = shaders.Load("ui");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4); // (invW, invH, pad, pad)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.GetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    // ビットマップフォントは POINT でにじまず crisp に (隣接セルのブリードも防ぐ)
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, sampler_.GetAddressOf()))) {
        return false;
    }

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bld, blend_.GetAddressOf()))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE; // UI は最前面 (深度テスト無し)
    if (FAILED(dev->CreateDepthStencilState(&dd, depthOff_.GetAddressOf()))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        return false;
    }

    BakeFont(device);

    ready_ = true;
    return true;
}

void UIRenderer::BakeFont(GraphicsDevice& device)
{
    // 埋め込み 8x8 フォントを 16x6 グリッドのアトラス (128x48 RGBA) に焼く。
    // 各グリフは rgb=255・a=(ビット立ちで 255)。テキストは color*atlas でカバレッジ乗算。
    constexpr int kCols = 16;
    constexpr int kRows = 6;
    constexpr int kCell = 8;
    constexpr int aw = kCols * kCell; // 128
    constexpr int ah = kRows * kCell; // 48
    std::vector<uint8_t> rgba(static_cast<size_t>(aw) * ah * 4, 0);

    for (int gi = 0; gi < 96; ++gi) {
        const int cx = (gi % kCols) * kCell;
        const int cy = (gi / kCols) * kCell;
        for (int row = 0; row < kCell; ++row) {
            const unsigned char bits = kFont8x8[gi][row];
            for (int coln = 0; coln < kCell; ++coln) {
                if (bits & (1u << coln)) {
                    uint8_t* p = &rgba[(static_cast<size_t>(cy + row) * aw + (cx + coln)) * 4];
                    p[0] = 255;
                    p[1] = 255;
                    p[2] = 255;
                    p[3] = 255;
                }
            }
        }
        const int ch = 32 + gi;
        if (ch < 128) {
            Glyph& g = glyphs_[ch];
            g.u0 = static_cast<float>(cx) / aw;
            g.v0 = static_cast<float>(cy) / ah;
            g.u1 = static_cast<float>(cx + kCell) / aw;
            g.v1 = static_cast<float>(cy + kCell) / ah;
            g.x0 = 0.0f;
            g.y0 = 0.0f;
            g.x1 = static_cast<float>(kCell);
            g.y1 = static_cast<float>(kCell);
            g.advance = static_cast<float>(kCell);
            g.valid = true;
        }
    }
    fontSize_ = static_cast<float>(kCell); // 8
    fontLineH_ = kCell + 2.0f;             // 10 (行送り)

    ID3D11Device* dev = device.Device();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = aw;
    td.Height = ah;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = rgba.data();
    sd.SysMemPitch = static_cast<UINT>(aw) * 4;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, fontTex_.GetAddressOf()))) {
        dev->CreateShaderResourceView(fontTex_.Get(), nullptr, fontSrv_.GetAddressOf());
    }
}

void UIRenderer::Shutdown()
{
    cb_.Reset();
    vb_.Reset();
    sampler_.Reset();
    blend_.Reset();
    depthOff_.Reset();
    raster_.Reset();
    fontTex_.Reset();
    fontSrv_.Reset();
    verts_.clear();
    batches_.clear();
    ready_ = false;
}

bool UIRenderer::CopyGlyphTable(VfxGlyph* out128) const
{
    if (!out128 || !fontSrv_) {
        return false;
    }
    for (int i = 0; i < 128; ++i) {
        const Glyph& g = glyphs_[i];
        VfxGlyph& o = out128[i];
        o.u0 = g.u0;
        o.v0 = g.v0;
        o.u1 = g.u1;
        o.v1 = g.v1;
        o.w = g.x1 - g.x0;
        o.h = g.y1 - g.y0;
        o.advance = g.advance;
        o.valid = g.valid;
    }
    return true;
}

float UIRenderer::TextWidth(const char* s, float scale) const
{
    if (!s) {
        return 0.0f;
    }
    float wsum = 0.0f;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\n') {
            break; // 単一行幅
        }
        const Glyph& g = glyphs_[c & 127];
        wsum += (g.valid ? g.advance : fontSize_ * 0.5f) * scale;
    }
    return wsum;
}

void UIRenderer::PushQuad(ID3D11ShaderResourceView* srv, float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1, const XMFLOAT4& col)
{
    if (batches_.empty() || batches_.back().srv != srv) {
        Batch b;
        b.srv = srv;
        b.start = static_cast<uint32_t>(verts_.size());
        b.count = 0;
        batches_.push_back(b);
    }
    const UIVertex q[6] = {
        { { x, y }, { u0, v0 }, col },     { { x + w, y }, { u1, v0 }, col },
        { { x + w, y + h }, { u1, v1 }, col }, { { x, y }, { u0, v0 }, col },
        { { x + w, y + h }, { u1, v1 }, col }, { { x, y + h }, { u0, v1 }, col },
    };
    for (const UIVertex& v : q) {
        verts_.push_back(v);
    }
    batches_.back().count += 6;
}

void UIRenderer::PushText(const char* s, float x, float y, float scale, const XMFLOAT4& col)
{
    if (!s || !fontSrv_) {
        return;
    }
    float penX = x;
    const float topY = y;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\n') {
            penX = x;
            y = topY; // 単一行前提だが改行も一応処理
            continue;
        }
        const Glyph& g = glyphs_[c & 127];
        if (!g.valid) {
            penX += fontSize_ * 0.5f * scale;
            continue;
        }
        // 可視グリフのみクアッド化 (空白は advance だけ)
        if (g.u1 > g.u0 && g.v1 > g.v0) {
            const float gx0 = penX + g.x0 * scale;
            const float gy0 = topY + g.y0 * scale;
            const float gw = (g.x1 - g.x0) * scale;
            const float gh = (g.y1 - g.y0) * scale;
            PushQuad(fontSrv_.Get(), gx0, gy0, gw, gh, g.u0, g.v0, g.u1, g.v1, col);
        }
        penX += g.advance * scale;
    }
}

void UIRenderer::Render(World& world, GraphicsDevice& device, ShaderManager& shaders,
                        RenderResources& resources, ID3D11RenderTargetView* rtv, int width,
                        int height, int mouseX, int mouseY, bool mouseDown)
{
    if (!ready_ || width <= 0 || height <= 0) {
        return;
    }
    ShaderProgram* prog = shaders.Get(shader_);
    if (!prog || !prog->valid) {
        return;
    }

    // ---- 収集 (order → entity.index の明示キーで安定ソート) ----
    std::vector<std::tuple<int32_t, uint32_t, const UIElementComponent*>> items;
    const ComponentTypeId req[] = { UIElementComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* el = static_cast<const UIElementComponent*>(arch.GetPtr(ci, row));
            items.emplace_back(el->order, e.index, el);
        }
    });
    if (items.empty()) {
        return;
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) {
            return std::get<0>(a) < std::get<0>(b);
        }
        return std::get<1>(a) < std::get<1>(b);
    });

    verts_.clear();
    batches_.clear();
    Texture* whiteTex = resources.textures.Get(resources.textures.White());
    whiteSrv_ = whiteTex ? whiteTex->srv.Get() : nullptr;
    if (!whiteSrv_) {
        return;
    }

    for (const auto& it : items) {
        const UIElementComponent& el = *std::get<2>(it);
        float rx, ry;
        ResolveAnchor(el.anchor, el.x, el.y, el.w, el.h, width, height, rx, ry);

        if (el.kind == 1) {
            // テキスト (背景無し)
            PushText(el.text, rx, ry, el.fontScale, el.color);
        } else if (el.kind == 2) {
            // ボタン: 背景 + hover/press ハイライト (display only) + 中央ラベル
            XMFLOAT4 bg = el.color;
            const bool hover = mouseX >= static_cast<int>(rx) && mouseX < static_cast<int>(rx + el.w)
                && mouseY >= static_cast<int>(ry) && mouseY < static_cast<int>(ry + el.h);
            if (hover) {
                const float k = mouseDown ? 0.8f : 1.25f; // press で暗く、hover で明るく
                bg.x = std::min(1.0f, bg.x * k);
                bg.y = std::min(1.0f, bg.y * k);
                bg.z = std::min(1.0f, bg.z * k);
            }
            PushQuad(whiteSrv_, rx, ry, el.w, el.h, 0, 0, 1, 1, bg);
            const float tw = TextWidth(el.text, el.fontScale);
            const float th = LineHeight(el.fontScale);
            const XMFLOAT4 label = { 1, 1, 1, 1 };
            PushText(el.text, rx + (el.w - tw) * 0.5f, ry + (el.h - th) * 0.5f, el.fontScale, label);
        } else {
            // パネル / 画像
            ID3D11ShaderResourceView* srv = whiteSrv_;
            if (el.texture.value != 0) {
                Texture* t = resources.textures.Get(el.texture);
                if (t && t->srv) {
                    srv = t->srv.Get();
                }
            }
            PushQuad(srv, rx, ry, el.w, el.h, 0, 0, 1, 1, el.color);
        }
    }

    if (verts_.empty()) {
        return;
    }

    // ---- VB 確保 + アップロード ----
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();
    const uint32_t needed = static_cast<uint32_t>(verts_.size());
    if (needed > vbCapacity_) {
        vbCapacity_ = needed + needed / 2 + 512;
        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = vbCapacity_ * sizeof(UIVertex);
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vb_.Reset();
        if (FAILED(dev->CreateBuffer(&vbd, nullptr, vb_.GetAddressOf()))) {
            return;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(dc->Map(vb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, verts_.data(), verts_.size() * sizeof(UIVertex));
        dc->Unmap(vb_.Get(), 0);
    }

    // 定数バッファ (1/screen)
    const XMFLOAT4 inv(1.0f / width, 1.0f / height, 0.0f, 0.0f);
    if (SUCCEEDED(dc->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &inv, sizeof(inv));
        dc->Unmap(cb_.Get(), 0);
    }

    // ---- 描画 ----
    dc->OMSetRenderTargets(1, &rtv, nullptr); // 深度なし
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    dc->RSSetViewports(1, &vp);

    const UINT stride = sizeof(UIVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    dc->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->IASetInputLayout(prog->inputLayout.Get());
    dc->VSSetShader(prog->vs.Get(), nullptr, 0);
    dc->PSSetShader(prog->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb_.Get() };
    dc->VSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[1] = { sampler_.Get() };
    dc->PSSetSamplers(0, 1, samps);
    dc->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(depthOff_.Get(), 0);
    dc->RSSetState(raster_.Get());

    for (const Batch& b : batches_) {
        ID3D11ShaderResourceView* srvs[1] = { b.srv };
        dc->PSSetShaderResources(0, 1, srvs);
        dc->Draw(b.count, b.start);
    }
}

} // namespace mye
