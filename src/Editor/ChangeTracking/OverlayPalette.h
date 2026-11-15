//====================================================================================
//                          OverlayPalette.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI の変更の強調に使う 4 色 (追加・更新・削除・競合)
//====================================================================================
#pragma once
#include <cstdint>

namespace mye {

struct OverlayColor {
    float r, g, b, a;
    bool operator==(const OverlayColor&) const = default;
};

enum class OverlayColorId : uint8_t { Added, Modified, Removed, Conflict };
constexpr int kOverlayColorCount = 4;

// 子孫にだけ変更があるノードに使う、変更の色の薄い不透明度
constexpr float kOverlayFaintAlpha = 0.35f;

// 強調の 4 色。EditorSettings に保存し、Hierarchy のバッジ・Inspector の印・Scene View のワイヤ箱・AI Changes 窓の帯が同じ値を読む。
// 競合を作る経路はまだ無い (色と描画の口だけ)
struct OverlayPalette {
    OverlayColor colors[kOverlayColorCount] = {
        { 0.30f, 0.78f, 0.40f, 1.0f }, // Added: 緑
        { 0.35f, 0.60f, 0.95f, 1.0f }, // Modified: 青
        { 0.82f, 0.44f, 0.40f, 1.0f }, // Removed: テーマのエラー色と同じ
        { 0.92f, 0.22f, 0.22f, 1.0f }, // Conflict: 赤
    };

    const OverlayColor& Get(OverlayColorId id) const { return colors[static_cast<int>(id)]; }
    void Set(OverlayColorId id, const OverlayColor& color) { colors[static_cast<int>(id)] = color; }
    void Reset() { *this = OverlayPalette(); }
    bool operator==(const OverlayPalette&) const = default;
};

// EditorLinePass が受ける 0xRRGGBBAA。各成分は 0〜1 に丸める
inline uint32_t PackOverlayRgba(const OverlayColor& color)
{
    auto byte = [](float v) -> uint32_t {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    return (byte(color.r) << 24) | (byte(color.g) << 16) | (byte(color.b) << 8) | byte(color.a);
}

} // namespace mye
