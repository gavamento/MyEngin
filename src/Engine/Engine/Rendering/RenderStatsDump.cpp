//====================================================================================
//                          RenderStatsDump.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          描画統計と GPU 時間の JSON ダンプの実装
//====================================================================================
#include "Engine/Engine/Rendering/RenderStatsDump.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Engine/Engine/Rendering/RenderSystem.h"

namespace mye {
namespace {

void AppendF(std::string& out, const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) {
        out.append(buf, static_cast<size_t>((n < static_cast<int>(sizeof(buf))) ? n : sizeof(buf) - 1));
    }
}

void AppendIntArray(std::string& out, const int* values, int count)
{
    out += "[";
    for (int i = 0; i < count; ++i) {
        AppendF(out, "%s%d", (i == 0) ? "" : ", ", values[i]);
    }
    out += "]";
}

// 統計 1 組を "key": value の並びで出す (前後の波括弧は呼び出し側)
void AppendStatsBody(std::string& out, const prof::RenderStats& s, const char* indent)
{
    AppendF(out, "%s\"drawCalls\": %d,\n", indent, s.drawCalls);
    AppendF(out, "%s\"triangles\": %d,\n", indent, s.triangles);
    AppendF(out, "%s\"culled\": %d,\n", indent, s.culled);
    AppendF(out, "%s\"shadowDrawCalls\": %d,\n", indent, s.shadowDrawCalls);
    AppendF(out, "%s\"shadowTriangles\": %d,\n", indent, s.shadowTriangles);
    AppendF(out, "%s\"shadowCascadeDraws\": ", indent);
    AppendIntArray(out, s.shadowCascadeDraws, prof::kRenderStatsCascadeSlots);
    out += ",\n";
    AppendF(out, "%s\"lodDraws\": ", indent);
    AppendIntArray(out, s.lodDraws, prof::kRenderStatsLodSlots);
    out += ",\n";
    AppendF(out, "%s\"paletteEvaluated\": %d,\n", indent, s.paletteEvaluated);
    AppendF(out, "%s\"paletteReused\": %d,\n", indent, s.paletteReused);
    AppendF(out, "%s\"occlusionPhase1Draws\": %d,\n", indent, s.occlusionPhase1Draws);
    AppendF(out, "%s\"occlusionPhase2Draws\": %d,\n", indent, s.occlusionPhase2Draws);
    AppendF(out, "%s\"occluded\": %d", indent, s.occluded);
}

bool HasActivity(const prof::RenderStats& s)
{
    return s.drawCalls != 0 || s.triangles != 0 || s.culled != 0 || s.shadowDrawCalls != 0;
}

} // namespace

RenderStatsDump CollectRenderStatsDump(const RenderSystem& renderSystem, uint64_t frame, uint32_t width,
                                       uint32_t height)
{
    RenderStatsDump d;
    d.frame = frame;
    d.width = width;
    d.height = height;
    d.total = prof::GetRenderStats();
    for (int i = 0; i < prof::kRenderStatsViewSlots; ++i) {
        d.views[i] = prof::GetRenderStatsForView(static_cast<uint32_t>(i));
    }
    d.atlasTiles = renderSystem.ShadowAtlasTiles();
    d.atlasDraws = renderSystem.ShadowAtlasDraws();
    d.atlasCulledDraws = renderSystem.ShadowAtlasCulledDraws();
    d.atlasCulledFaces = renderSystem.ShadowAtlasCulledFaces();
    d.frameMs = renderSystem.FrameGpuMs();
    d.gbufferMs = renderSystem.GbufferGpuMs();
    d.forwardOpaqueMs = renderSystem.ForwardOpaqueGpuMs();
    d.occlusionMs = renderSystem.OcclusionGpuMs();
    d.csmMs = renderSystem.ShadowCsmGpuMs();
    d.atlasMs = renderSystem.ShadowAtlasGpuMs();
    d.hzbMs = renderSystem.HzbGpuMs();
    d.ssrMs = renderSystem.SsrGpuMs();
    d.postFxMs = renderSystem.PostFxGpuMs();
    return d;
}

std::string FormatRenderStatsJson(const RenderStatsDump& dump)
{
    std::string out;
    out += "{\n";
    out += "  \"schema\": 1,\n";
    AppendF(out, "  \"frame\": %llu,\n", static_cast<unsigned long long>(dump.frame));
    AppendF(out, "  \"width\": %u,\n", dump.width);
    AppendF(out, "  \"height\": %u,\n", dump.height);

    out += "  \"counts\": {\n";
    out += "    \"total\": {\n";
    AppendStatsBody(out, dump.total, "      ");
    out += "\n    },\n";
    out += "    \"views\": [";
    bool firstView = true;
    for (int i = 0; i < prof::kRenderStatsViewSlots; ++i) {
        if (!HasActivity(dump.views[i])) {
            continue;
        }
        out += firstView ? "\n" : ",\n";
        firstView = false;
        AppendF(out, "      {\n        \"viewKey\": %d,\n", i);
        AppendStatsBody(out, dump.views[i], "        ");
        out += "\n      }";
    }
    out += firstView ? "],\n" : "\n    ],\n";
    out += "    \"shadowAtlas\": {\n";
    AppendF(out, "      \"tiles\": %d,\n", dump.atlasTiles);
    AppendF(out, "      \"draws\": %d,\n", dump.atlasDraws);
    AppendF(out, "      \"culledDraws\": %d,\n", dump.atlasCulledDraws);
    AppendF(out, "      \"culledFaces\": %d\n", dump.atlasCulledFaces);
    out += "    }\n";
    out += "  },\n";

    out += "  \"gpuMs\": {\n";
    AppendF(out, "    \"frame\": %.4f,\n", static_cast<double>(dump.frameMs));
    AppendF(out, "    \"gbuffer\": %.4f,\n", static_cast<double>(dump.gbufferMs));
    AppendF(out, "    \"forwardOpaque\": %.4f,\n", static_cast<double>(dump.forwardOpaqueMs));
    AppendF(out, "    \"occlusion\": %.4f,\n", static_cast<double>(dump.occlusionMs));
    AppendF(out, "    \"csm\": %.4f,\n", static_cast<double>(dump.csmMs));
    AppendF(out, "    \"shadowAtlas\": %.4f,\n", static_cast<double>(dump.atlasMs));
    AppendF(out, "    \"hzb\": %.4f,\n", static_cast<double>(dump.hzbMs));
    AppendF(out, "    \"ssr\": %.4f,\n", static_cast<double>(dump.ssrMs));
    AppendF(out, "    \"postFx\": %.4f\n", static_cast<double>(dump.postFxMs));
    out += "  }\n";
    out += "}\n";
    return out;
}

bool WriteRenderStatsDump(const std::wstring& path, const RenderStatsDump& dump)
{
    std::ofstream file{ std::filesystem::path(path), std::ios::binary | std::ios::trunc };
    if (!file) {
        return false;
    }
    const std::string json = FormatRenderStatsJson(dump);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    return static_cast<bool>(file);
}

} // namespace mye
