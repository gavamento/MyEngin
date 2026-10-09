//====================================================================================
//                          RenderStatsDump.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          描画統計と GPU 時間の JSON ダンプ (--render-stats-dump)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "Engine/Core/Diagnostics/Profiler.h"

namespace mye {

class RenderSystem;

// --render-stats-dump の中身。決定的な数 (counts) と GPU 時間 (gpuMs) は JSON でも別の節に出す。
// Debug / Release / WARP で ms 以外の全欄が一致することが契約 (spec 4.1.1)
struct RenderStatsDump {
    uint64_t frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    prof::RenderStats total;                              // 全ビューの和 (従来の累積値)
    prof::RenderStats views[prof::kRenderStatsViewSlots]; // viewKey 別
    // 局所影アトラス (最後に描いたビューの値)
    int atlasTiles = 0;
    int atlasDraws = 0;
    int atlasCulledDraws = 0;
    int atlasCulledFaces = 0;
    // GPU 時間 [ms]。実機の参考値で、ゲートにしない
    float frameMs = 0.0f;
    float gbufferMs = 0.0f;
    float forwardOpaqueMs = 0.0f;
    float csmMs = 0.0f;
    float atlasMs = 0.0f;
    float hzbMs = 0.0f;
    float ssrMs = 0.0f;
    float postFxMs = 0.0f;
};

// 直近の Render が済んだ時点の統計を集める (prof の累積値と RenderSystem の計測口を読むだけ)
RenderStatsDump CollectRenderStatsDump(const RenderSystem& renderSystem, uint64_t frame, uint32_t width,
                                       uint32_t height);

// JSON 文字列にする。キーの並びは固定 (diff で比べられる)
std::string FormatRenderStatsJson(const RenderStatsDump& dump);

// path へ UTF-8 で書く。戻り値 false = 書けなかった
bool WriteRenderStatsDump(const std::wstring& path, const RenderStatsDump& dump);

} // namespace mye
