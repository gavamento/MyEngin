//====================================================================================
//                          PixelPerfect2D.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          ピクセルパーフェクトカメラの整数倍率・低解像度 RT・orthoSize の計算
//====================================================================================
#pragma once
#include <cstdint>

namespace mye::pixelperfect2d {

// PixelPerfectCamera2D の設定のうち計算に使う値 (コンポーネントを知らない Renderer 層の入力)
struct Settings {
    int32_t assetsPPU = 100;
    int32_t refResolutionX = 320;
    int32_t refResolutionY = 180;
    bool upscaleRT = true;
};

struct Result {
    int32_t zoom = 1;          // 画面 1 ピクセルあたり何倍か (整数、1 以上)
    bool upscale = false;      // 低解像度 RT に描いて拡大するか (upscaleRT && zoom > 1)
    int32_t rtW = 0;           // 描く解像度。upscale でなければ画面そのもの
    int32_t rtH = 0;
    float orthoSize = 0.0f;    // 描く解像度での正射影の半高 (CameraComponent.orthoSize の代わり)
    float viewOrthoSize = 0.0f; // 画面に見える範囲の半高 (拡大で RT の端を切った後。UI のワールド追従用)
};

// 画面 W x H で ref 解像度に整数倍で収まる最大の zoom を求める (zoom = max(1, min(H / refY, W / refX)))。
// upscale のとき RT は画面を覆う最小の偶数サイズ (端数は RT 側を大きく取り、拡大後に画面からはみ出た分を切る = 黒帯を作らない)
// で、orthoSize = rtH / 2 / PPU (RT の 1 ピクセル = スプライトの 1 ピクセル)。
// それ以外は画面解像度で、orthoSize = H / 2 / (zoom * PPU)。W / H / 設定が 1 未満なら 1 として扱う
Result Compute(int32_t screenW, int32_t screenH, const Settings& settings);

} // namespace mye::pixelperfect2d
