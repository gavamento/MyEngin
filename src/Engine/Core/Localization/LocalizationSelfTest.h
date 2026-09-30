#pragma once

namespace mye {

// ローカライズ基盤 (M47) のヘッドレス回帰テスト:
//   - StrId の全項目が en/ja とも非空で解決する
//   - "###" を含む文字列は ID 部 (右辺) が en/ja で一致する = 言語切替で ImGui の
//     ウィンドウ ID / ドッキング配置が変わらない
//   - 変換指定子の並びが en/ja で一致する (MSVC printf は位置指定引数に非対応)
//   - UTF-8 の切り詰め (utf8::CopyTruncated) がマルチバイト列を分断しない
bool RunLocalizationSelfTest();

} // namespace mye
