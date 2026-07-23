#pragma once
#include <string>

namespace mye::TextureCook {

// クックオプション (M39b、.meta の Import Settings から渡す)。既定 = 従来挙動
struct CookOptions {
    bool generateMips = true; // false = mip0 のみ
    bool compress = true;     // true = BCn 自動 (アルファ有=BC3/無=BC1) / false = RGBA8 非圧縮
};

// PNG/JPG/TGA を読み込み、mip チェーン付きの DDS に変換して出力する (オフラインクック)。
// compress=true はアルファを含むなら BC3 (DXT5)、不透明なら BC1 (DXT1) を選ぶ。
// compress=false は RGBA8 非圧縮 (レガシーマスク形式 — GpuResources の読込対応済、M38b)。
// GPU 不要 (CPU の stb_dxt のみ)。決定論には無関係 (アセット生成)。成功で true。
bool CookImageToDds(const std::wstring& srcImagePath, const std::wstring& dstDdsPath,
                    const CookOptions& opts = {});

} // namespace mye::TextureCook
