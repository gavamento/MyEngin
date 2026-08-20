#include "Engine/Renderer/FontAtlas.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"

#include "stb/stb_truetype.h"

namespace mye {
namespace {

constexpr float kBasePx = 32.0f;  // TTF のベイク基準ピクセル高 (fontScale で拡縮)
constexpr int kAtlasStart = 512;  // 開始アトラスサイズ (満杯で倍化)
constexpr int kAtlasMax = 2048;   // 上限 (RGBA8 で 16MB)

// 埋め込み 8x8 ビットマップフォント (font8x8_basic、パブリックドメイン。ASCII 0x20..0x7F)。
// 各行の bit0(LSB) が左端ピクセル。TTF が 1 つも見つからない環境の最終フォールバック。
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

FontAtlas::FontAtlas() = default;
FontAtlas::~FontAtlas() = default;

bool FontAtlas::Init(GraphicsDevice& device, const std::wstring& assetsRoot,
                     bool forceEmbedded)
{
    device_ = &device;

    // フォント候補: assets\fonts\*.ttf/.ttc (名前順) → システム日本語フォント
    std::vector<std::wstring> candidates;
    if (!assetsRoot.empty() && !forceEmbedded) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(assetsRoot) / L"fonts";
        std::vector<std::wstring> found;
        if (std::filesystem::is_directory(dir, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file()) {
                    continue;
                }
                std::wstring ext = e.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                if (ext == L".ttf" || ext == L".ttc") {
                    found.push_back(e.path().wstring());
                }
            }
            std::sort(found.begin(), found.end());
        }
        candidates.insert(candidates.end(), found.begin(), found.end());
    }
    wchar_t windir[MAX_PATH] = {};
    if (!forceEmbedded && GetWindowsDirectoryW(windir, MAX_PATH) > 0) {
        const std::wstring fonts = std::wstring(windir) + L"\\Fonts\\";
        candidates.push_back(fonts + L"YuGothM.ttc"); // ImGuiTheme と同じ優先順
        candidates.push_back(fonts + L"meiryo.ttc");
        candidates.push_back(fonts + L"msgothic.ttc");
    }

    for (const std::wstring& path : candidates) {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        if (!f) {
            continue;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        if (InitTtf(std::move(data))) {
            ttfMode_ = true;
            MYE_LOG_INFO("[font] atlas font: %s (base %.0fpx)", WideToUtf8(path).c_str(), kBasePx);
            break;
        }
    }
    if (!ttfMode_ && forceEmbedded) {
        MYE_LOG_INFO("[font] atlas font: embedded 8x8 (forced, ascii only)");
        InitEmbedded8x8();
    } else if (!ttfMode_) {
        MYE_LOG_WARN("[font] no usable ttf found — falling back to embedded 8x8 (ASCII only)");
        InitEmbedded8x8();
    }

    ready_ = (srv_ != nullptr);
    if (ready_) {
        EnsureText("?"); // フォールバックグリフを常備
    }
    return ready_;
}

void FontAtlas::Shutdown()
{
    tex_.Reset();
    srv_.Reset();
    pixels_.clear();
    glyphs_.clear();
    fontData_.clear();
    info_.reset();
    device_ = nullptr;
    ready_ = false;
    ttfMode_ = false;
}

bool FontAtlas::InitTtf(std::vector<uint8_t> file)
{
    if (file.size() < 12) {
        return false;
    }
    fontData_ = std::move(file);
    info_ = std::make_unique<stbtt_fontinfo>();
    const int offset = stbtt_GetFontOffsetForIndex(fontData_.data(), 0); // ttc は先頭フォント
    if (offset < 0) {
        return false;
    }
    if (!stbtt_InitFont(info_.get(), fontData_.data(), offset)) {
        return false;
    }
    scale_ = stbtt_ScaleForPixelHeight(info_.get(), kBasePx);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(info_.get(), &asc, &desc, &gap);
    ascentPx_ = static_cast<float>(asc) * scale_;
    baseLineHPx_ = static_cast<float>(asc - desc + gap) * scale_;
    if (baseLineHPx_ <= 0.0f) {
        return false;
    }
    return CreateAtlas(kAtlasStart);
}

void FontAtlas::InitEmbedded8x8()
{
    // GlyphScale(s)==s になるメトリクス (baseLineH=kUILineH) → レガシー 8x8 描画とピクセル同一
    ascentPx_ = 8.0f;
    baseLineHPx_ = kUILineH;
    if (!CreateAtlas(128)) {
        return;
    }
    for (uint32_t c = 0x20; c < 0x80; ++c) {
        BakeInto(c);
    }
}

bool FontAtlas::CreateAtlas(int size)
{
    if (!device_) {
        return false;
    }
    pixels_.assign(static_cast<size_t>(size) * size * 4, 0);
    atlasSize_ = size;
    packer_.Reset(size, size);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(size);
    td.Height = static_cast<UINT>(size);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; // グリフ追加を UpdateSubresource で部分アップロード
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels_.data();
    sd.SysMemPitch = static_cast<UINT>(size) * 4;
    tex_.Reset();
    srv_.Reset();
    if (FAILED(device_->Device()->CreateTexture2D(&td, &sd, tex_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(device_->Device()->CreateShaderResourceView(tex_.Get(), nullptr,
                                                           srv_.GetAddressOf()))) {
        return false;
    }
    return true;
}

void FontAtlas::UploadRegion(int x, int y, int w, int h)
{
    if (!tex_ || !device_ || w <= 0 || h <= 0) {
        return;
    }
    D3D11_BOX box = {};
    box.left = static_cast<UINT>(x);
    box.top = static_cast<UINT>(y);
    box.front = 0;
    box.right = static_cast<UINT>(x + w);
    box.bottom = static_cast<UINT>(y + h);
    box.back = 1;
    const uint8_t* src = &pixels_[(static_cast<size_t>(y) * atlasSize_ + x) * 4];
    device_->Context()->UpdateSubresource(tex_.Get(), 0, &box, src,
                                          static_cast<UINT>(atlasSize_) * 4, 0);
}

bool FontAtlas::BakeInto(uint32_t cp)
{
    int w = 0, h = 0, xoff = 0, yoff = 0;
    float advance = 0.0f;
    std::vector<uint8_t> cov; // 8bpp カバレッジ

    if (ttfMode_) {
        const int gi = stbtt_FindGlyphIndex(info_.get(), static_cast<int>(cp));
        if (gi == 0) {
            glyphs_[cp] = FontGlyphInfo{}; // フォントに無い → 失敗を記録 ('?' で描かれる)
            return true;
        }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(info_.get(), gi, &adv, &lsb);
        advance = static_cast<float>(adv) * scale_;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(info_.get(), gi, scale_, scale_, &x0, &y0, &x1, &y1);
        w = x1 - x0;
        h = y1 - y0;
        xoff = x0;
        yoff = y0;
        if (w > 0 && h > 0) {
            cov.resize(static_cast<size_t>(w) * h);
            stbtt_MakeGlyphBitmap(info_.get(), cov.data(), w, h, w, scale_, scale_, gi);
        }
    } else {
        if (cp < 0x20 || cp > 0x7F) {
            glyphs_[cp] = FontGlyphInfo{}; // 8x8 は ASCII のみ
            return true;
        }
        w = 8;
        h = 8;
        xoff = 0;
        yoff = -8; // ベースライン = グリフ下端 (ascent=8 と対で「上端から描く」レガシー互換)
        advance = 8.0f;
        cov.resize(64);
        const unsigned char* rows = kFont8x8[cp - 0x20];
        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                cov[static_cast<size_t>(r) * 8 + c] = (rows[r] & (1u << c)) ? 255 : 0;
            }
        }
    }

    FontGlyphInfo g;
    g.advance = advance;
    g.xoff = static_cast<float>(xoff);
    g.yoff = static_cast<float>(yoff);
    g.w = static_cast<float>(w);
    g.h = static_cast<float>(h);
    g.valid = true;
    if (w > 0 && h > 0) {
        int px = 0, py = 0;
        if (!packer_.Alloc(w, h, px, py)) {
            return false; // 満杯 → 呼び出し側が Grow
        }
        for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
                uint8_t* d = &pixels_[(static_cast<size_t>(py + r) * atlasSize_ + (px + c)) * 4];
                d[0] = 255;
                d[1] = 255;
                d[2] = 255;
                d[3] = cov[static_cast<size_t>(r) * w + c];
            }
        }
        UploadRegion(px, py, w, h);
        const float inv = 1.0f / static_cast<float>(atlasSize_);
        g.u0 = static_cast<float>(px) * inv;
        g.v0 = static_cast<float>(py) * inv;
        g.u1 = static_cast<float>(px + w) * inv;
        g.v1 = static_cast<float>(py + h) * inv;
    }
    glyphs_[cp] = g;
    return true;
}

bool FontAtlas::Grow()
{
    if (!ttfMode_) {
        return false; // 8x8 は ASCII 固定で成長不要
    }
    const int next = atlasSize_ * 2;
    if (next > kAtlasMax) {
        MYE_LOG_WARN("[font] atlas full at %dx%d — further glyphs draw as '?'", atlasSize_,
                     atlasSize_);
        return false;
    }
    std::vector<uint32_t> keep;
    keep.reserve(glyphs_.size());
    for (const auto& kv : glyphs_) {
        if (kv.second.valid) {
            keep.push_back(kv.first);
        }
    }
    std::sort(keep.begin(), keep.end());
    if (!CreateAtlas(next)) {
        return false;
    }
    glyphs_.clear();
    for (uint32_t cp : keep) {
        BakeInto(cp); // 倍化後は必ず入る (旧総面積は新アトラスの 1/4 未満)
    }
    ++version_;
    MYE_LOG_INFO("[font] atlas grown to %dx%d (%zu glyphs)", next, next, glyphs_.size());
    return true;
}

void FontAtlas::EnsureCodepoint(uint32_t cp)
{
    if (!ready_ || cp < 0x20) {
        return; // 制御文字は描かない ('\n' は呼び出し側の行送り)
    }
    if (glyphs_.count(cp) != 0) {
        return;
    }
    if (BakeInto(cp)) {
        return;
    }
    if (Grow() && BakeInto(cp)) {
        return;
    }
    glyphs_[cp] = FontGlyphInfo{}; // 満杯 → 失敗を記録して再試行を止める
}

void FontAtlas::EnsureText(const char* utf8)
{
    if (!ready_ || !utf8) {
        return;
    }
    const char* p = utf8;
    for (;;) {
        const uint32_t cp = fontgeom::Utf8Next(p);
        if (cp == 0) {
            break;
        }
        EnsureCodepoint(cp);
    }
}

const FontGlyphInfo* FontAtlas::Find(uint32_t codepoint) const
{
    const auto it = glyphs_.find(codepoint);
    if (it == glyphs_.end() || !it->second.valid) {
        return nullptr;
    }
    return &it->second;
}

} // namespace mye
