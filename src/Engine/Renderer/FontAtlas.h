#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Renderer/FontGeometry.h"

struct stbtt_fontinfo;

namespace mye {

class GraphicsDevice;

// 動的グリフキャッシュ (M34)。stb_truetype で TTF/TTC をオンデマンド焼成し、
// シェルフパッキングの成長型アトラス (512²→1024²→2048², RGBA8) に載せる。
// TTF が見つからない環境では従来の埋め込み 8x8 ビットマップフォント (ASCII のみ) に
// フォールバックする — ヘッドレス/最小環境でも従来どおり動く。
// 使い方 (パス内 2 フェーズ): 描画パスの先頭で全文字列を EnsureText (焼成・成長はここだけ)
// → その後 Find/Glyphs で計測してクアッド構築。クアッドは毎フレーム再構築なので
// 成長時の UV 無効化は自然に解消される (Version はデバッグ用)。
// フォント優先順: <assetsRoot>\fonts\*.ttf/.ttc (名前順) → システム日本語フォント
// (YuGothM→meiryo→msgothic) → 埋め込み 8x8。※stb_truetype は CFF ベースの .otf 非対応。
// 決定論規約: 描画専用 (kComponentNoHash のテキストのみ描く) — sim には触れない。
class FontAtlas {
public:
    // fontScale=1 の行高 px (8x8 時代からの互換値。UIElement の見た目を保つ)
    static constexpr float kUILineH = 10.0f;

    FontAtlas();
    ~FontAtlas();

    // assetsRoot は空可 (フォント探索をシステム→8x8 のみにする)
    // forceEmbedded=true で TTF 探索を丸ごと飛ばして内蔵 8x8 (ASCII) に固定する。
    // M52c: スクショ回帰の golden を「そのマシンに入っているフォント」から切り離すため —
    // 英語版 Windows Server の CI ランナーには日本語 TTF が無く、探索させると別の絵になる
    bool Init(GraphicsDevice& device, const std::wstring& assetsRoot,
              bool forceEmbedded = false);
    void Shutdown();
    bool IsReady() const { return ready_; }
    bool IsTtf() const { return ttfMode_; }

    // 文字列の全コードポイントを焼成する (未焼成のみ)。満杯ならアトラスを倍化して全再焼成。
    void EnsureText(const char* utf8);

    // 焼成済みグリフの計測 (未焼成/焼成失敗は nullptr → 呼び出し側が '?' を使う)
    const FontGlyphInfo* Find(uint32_t codepoint) const;
    const FontGlyphMap& Glyphs() const { return glyphs_; }

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }

    // ---- 計測 (ベイク px 基準。GlyphScale を掛けて最終 px にする) ----
    float AscentPx() const { return ascentPx_; }
    float BaseLineHPx() const { return baseLineHPx_; }
    // fontScale → 「ベイク px 単位の計測値」に掛ける係数 (fontScale=1 で行高 kUILineH px)
    float GlyphScale(float fontScale) const { return kUILineH * fontScale / baseLineHPx_; }

    // アトラス再構築 (成長) の世代。UV を跨フレームでキャッシュする場合の無効化キー
    uint32_t Version() const { return version_; }

private:
    bool InitTtf(std::vector<uint8_t> file);
    void InitEmbedded8x8();
    bool CreateAtlas(int size);              // CPU バッファ + GPU テクスチャを size² で作り直す
    bool BakeInto(uint32_t cp);              // 1 グリフ焼成 (満杯で false)
    bool Grow();                             // 倍化 + 全再焼成 (上限で false)
    void EnsureCodepoint(uint32_t cp);
    void UploadRegion(int x, int y, int w, int h);

    bool ready_ = false;
    bool ttfMode_ = false;
    GraphicsDevice* device_ = nullptr;

    // TTF データ (stbtt はポインタを保持するので寿命はこの vector が担う)
    std::vector<uint8_t> fontData_;
    std::unique_ptr<stbtt_fontinfo> info_;
    float scale_ = 0.0f;      // stbtt_ScaleForPixelHeight(basePx)
    float ascentPx_ = 8.0f;   // ベイク px
    float baseLineHPx_ = kUILineH;

    // アトラス (CPU ミラー + GPU)
    std::vector<uint8_t> pixels_; // RGBA8
    int atlasSize_ = 0;
    fontgeom::FontShelfPacker packer_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    FontGlyphMap glyphs_;
    uint32_t version_ = 0;
};

} // namespace mye
