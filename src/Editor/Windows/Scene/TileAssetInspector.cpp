//====================================================================================
//                          TileAssetInspector.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産の Inspector 簡易表示
//====================================================================================
#include "Editor/Windows/Scene/TileAssetInspector.h"

#include <cstdio>

#include "imgui.h"

#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {

void DrawTileAssetInspector(AssetType type, const std::wstring& path)
{
    if (type == AssetType::TilePalette) {
        const TilePalette* palette = tilepalette::Resolve(TilePaletteLibrary::HashForPath(path));
        if (palette == nullptr) {
            ImGui::TextDisabled("%s", Tr(StrId::Tilemap_Failed));
            return;
        }
        char line[96];
        std::snprintf(line, sizeof(line), Tr(StrId::TilePalette_Summary), static_cast<int>(palette->tiles.size()));
        ImGui::TextUnformatted(line);
        return;
    }

    const TilemapAsset* map = tilemap::Resolve(AssetID{ TilemapLibrary::HashForPath(path) });
    if (map == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Tilemap_Failed));
        return;
    }
    int32_t minCx = 0;
    int32_t minCy = 0;
    int32_t maxCx = -1;
    int32_t maxCy = -1;
    int32_t width = 0;
    int32_t height = 0;
    if (map->ChunkBounds(minCx, minCy, maxCx, maxCy)) {
        width = (maxCx - minCx + 1) * kTilemapChunkSize;
        height = (maxCy - minCy + 1) * kTilemapChunkSize;
    }
    char line[160];
    std::snprintf(line, sizeof(line), Tr(StrId::Tilemap_Summary), static_cast<int>(map->layers.size()),
                  static_cast<int>(map->ChunkCount()), width, height, static_cast<double>(map->cellSize[0]),
                  static_cast<double>(map->cellSize[1]));
    ImGui::TextWrapped("%s", line);
    if (map->palette != 0) {
        const TilePalette* palette = tilepalette::Resolve(map->palette);
        char pal[160];
        std::snprintf(pal, sizeof(pal), Tr(StrId::Tilemap_Palette),
                      palette != nullptr ? palette->name.c_str() : Tr(StrId::Tilemap_PaletteMissing));
        ImGui::TextUnformatted(pal);
    }
}

} // namespace mye
