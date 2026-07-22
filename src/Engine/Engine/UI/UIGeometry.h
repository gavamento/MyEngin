#pragma once
// UI クアッド構築の純関数群 (M35)。D3D 非依存 — UISelfTest がヘッドレスで検証する。
// UIElement は描画専用 (kComponentNoHash) なので決定論契約とは無関係。
#include <algorithm>

namespace mye {
namespace uigeom {

// スクリーン px 矩形 + アトラス UV (UIRenderer::PushQuad の引数一式)
struct UIQuad {
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// fillAmount によるクリップ (HP バー)。mode 1=水平 (左→右)、2=垂直 (下→上)。
// UV も同率でクリップするので画像が「切れて」いく (スケールされない)
inline UIQuad BuildFillQuad(float x, float y, float w, float h, int mode, float amount)
{
    const float a = std::clamp(amount, 0.0f, 1.0f);
    UIQuad q;
    q.x = x;
    q.y = y;
    q.w = w;
    q.h = h;
    if (mode == 1) { // 水平: 左から a 割
        q.w = w * a;
        q.u1 = a;
    } else if (mode == 2) { // 垂直: 下から a 割
        q.h = h * a;
        q.y = y + h * (1.0f - a);
        q.v0 = 1.0f - a;
    }
    return q;
}

// 9-slice: 四隅は原寸 (border px)、辺と中央を伸縮。border は l,t,r,b (テクスチャ px =
// スクリーン px、Unity の Sprite Border と同じ解釈)。texW/texH はテクスチャ寸法。
// 戻り値 = 書き込んだクアッド数 (退化した行/列はスキップ、最大 9)。
// border が大きすぎて矩形に収まらない場合は比率を保って縮める。
inline int Build9Slice(float x, float y, float w, float h, float l, float t, float r, float b,
                       float texW, float texH, UIQuad out[9])
{
    if (w <= 0.0f || h <= 0.0f || texW <= 0.0f || texH <= 0.0f) {
        return 0;
    }
    l = std::max(0.0f, l);
    t = std::max(0.0f, t);
    r = std::max(0.0f, r);
    b = std::max(0.0f, b);
    // スクリーン矩形に収まらない border は縮める (Unity 同様のガード)
    if (l + r > w && l + r > 0.0f) {
        const float k = w / (l + r);
        l *= k;
        r *= k;
    }
    if (t + b > h && t + b > 0.0f) {
        const float k = h / (t + b);
        t *= k;
        b *= k;
    }
    // 列 (x 位置と幅) × 行 (y 位置と高さ)。UV は texW/texH 基準の border 位置
    const float xs[3] = { x, x + l, x + w - r };
    const float ws[3] = { l, w - l - r, r };
    const float ys[3] = { y, y + t, y + h - b };
    const float hs[3] = { t, h - t - b, b };
    const float us[4] = { 0.0f, l / texW, 1.0f - r / texW, 1.0f };
    const float vs[4] = { 0.0f, t / texH, 1.0f - b / texH, 1.0f };
    int n = 0;
    for (int row = 0; row < 3; ++row) {
        if (hs[row] <= 0.0f) {
            continue;
        }
        for (int col = 0; col < 3; ++col) {
            if (ws[col] <= 0.0f) {
                continue;
            }
            UIQuad& q = out[n++];
            q.x = xs[col];
            q.y = ys[row];
            q.w = ws[col];
            q.h = hs[row];
            q.u0 = us[col];
            q.u1 = us[col + 1];
            q.v0 = vs[row];
            q.v1 = vs[row + 1];
        }
    }
    return n;
}

} // namespace uigeom
} // namespace mye
