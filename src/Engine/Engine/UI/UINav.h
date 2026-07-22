#pragma once
// パッド/キーボードの UI フォーカスナビゲーション純関数 (M35)。D3D 非依存・scalar float のみ。
// エンジンはフォーカスの「状態」を持たない — 状態はスクリプト側が保持し、UIElement.focused
// (表示専用) に書き戻す設計 (M21 の「ボタン操作はスクリプトがヒットテスト」哲学の踏襲)。
// M37 で ABI (UIFocusNav) として公開予定。InputSnapshot 由来の決定論入力で呼べば決定論。
#include <cstdint>

namespace mye {
namespace uinav {

// フォーカス候補 1 個のスクリーン矩形。index はタイブレーク兼識別子 (entity.index)
struct NavRect {
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t index = 0;
};

enum NavDir : int { kNavUp = 0, kNavDown = 1, kNavLeft = 2, kNavRight = 3 };

// current から dir 方向で最も近い候補の index を返す (無ければ current.index のまま)。
// スコア = 軸方向距離 + 直交ずれ×2 (Unity の自動ナビと同系の重み)。同点は index 昇順。
inline uint32_t FindNext(const NavRect* rects, int count, const NavRect& current, int dir)
{
    const float cx = current.x + current.w * 0.5f;
    const float cy = current.y + current.h * 0.5f;
    float bestScore = 0.0f;
    uint32_t bestIndex = current.index;
    bool found = false;
    for (int i = 0; i < count; ++i) {
        const NavRect& r = rects[i];
        if (r.index == current.index) {
            continue;
        }
        const float rx = r.x + r.w * 0.5f;
        const float ry = r.y + r.h * 0.5f;
        const float dx = rx - cx;
        const float dy = ry - cy;
        float axial = 0.0f;
        float ortho = 0.0f;
        switch (dir) {
        case kNavUp:    axial = -dy; ortho = (dx < 0.0f) ? -dx : dx; break;
        case kNavDown:  axial = dy;  ortho = (dx < 0.0f) ? -dx : dx; break;
        case kNavLeft:  axial = -dx; ortho = (dy < 0.0f) ? -dy : dy; break;
        case kNavRight: axial = dx;  ortho = (dy < 0.0f) ? -dy : dy; break;
        default: return current.index;
        }
        if (axial <= 0.0f) {
            continue; // dir の半平面内のみ
        }
        const float score = axial + ortho * 2.0f;
        if (!found || score < bestScore
            || (score == bestScore && r.index < bestIndex)) {
            bestScore = score;
            bestIndex = r.index;
            found = true;
        }
    }
    return bestIndex;
}

} // namespace uinav
} // namespace mye
