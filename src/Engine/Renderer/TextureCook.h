#pragma once
#include <string>

namespace mye::TextureCook {

// PNG/JPG/TGA を読み込み、mip チェーン付きの BCn 圧縮 DDS に変換して出力する (オフラインクック)。
// アルファを含むなら BC3 (DXT5)、不透明なら BC1 (DXT1) を選ぶ。GPU 不要 (CPU の stb_dxt のみ)。
// 決定論には無関係 (アセット生成)。成功で true。
bool CookImageToDds(const std::wstring& srcImagePath, const std::wstring& dstDdsPath);

} // namespace mye::TextureCook
