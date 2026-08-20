#pragma once
#include <cstdint>
#include <string>

namespace mye {

// PNG 2 枚のピクセル比較 (M52c: スクリーンショット回帰テスト)。
// GPU も D3D も要らない — stb_image で CPU デコードして数えるだけなので
// selftest からも CLI (--img-diff) からも同じ本体を通せる。
struct ImageDiffResult {
    bool valid = false;         // 両方読めて寸法が一致した
    int width = 0;
    int height = 0;
    int maxChannelDiff = 0;     // 全画素・全チャンネルの絶対差の最大値 (0-255)。tolerance と無関係
    int64_t diffPixels = 0;     // tolerance を**超える**チャンネルを 1 つでも持つ画素の数
    int64_t diffPixelsAny = 0;  // 差が 1 でもある画素の数 (tolerance 無視。ノイズ量の把握用)
    int64_t totalPixels = 0;
    int worstX = -1;            // maxChannelDiff を出した画素 (走査順で最初の 1 つ)
    int worstY = -1;
    std::string error;          // valid=false の理由 (ログ用。英字)
};

// a と b を突き合わせる。tolerance 以下のチャンネル差は差分として数えない。
// diffOutPath が空でなければ差分ヒート PNG を書く
// (一致部は元画像を暗いグレーで敷き、差のある画素を差の大きさに応じた黄→赤で塗る)。
ImageDiffResult CompareImageFiles(const std::wstring& aPath, const std::wstring& bPath,
                                  int tolerance = 0, const std::wstring& diffOutPath = L"");

} // namespace mye
