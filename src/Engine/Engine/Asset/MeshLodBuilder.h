//====================================================================================
//                          MeshLodBuilder.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          メッシュ LOD の自動生成 (meshoptimizer による単純化)
//====================================================================================
#pragma once
#include <cmath>
#include <string>
#include <vector>

#include "Engine/Core/Asset/ImportMetaResolver.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye::ModelCook {

// 段の目標三角形比から決める、その段へ落とす画面高さ比の既定。.meta の screenSize が 0 のときに使う
inline float AutoLodScreenSize(float ratio)
{
    return 0.5f * std::sqrt(ratio);
}

// 自動生成した LOD1 以降。MeshLibrary::Register の lodIndices / lodLevels へそのまま渡せる
struct MeshLodData {
    std::vector<uint32_t> indices;    // LOD1 以降の連結。指す頂点は LOD0 と同じ頂点バッファ
    std::vector<MeshLodLevel> levels; // indexStart は LOD0 の後ろからの絶対位置 (indices.size() 起点)
    bool Empty() const { return levels.empty(); }
};

// settings.levels の分だけ段を作る。生成は決定的 (同じ入力から同じバイト列)。
// 目標の三角形数に届かない段は作らず、最初の段から作れなければ空を返す (key ごとに WARN 1 回)。
// 頂点は変更しない (単純化は元の頂点を指す index 列を返す)
MeshLodData BuildMeshLods(const std::string& key, const std::vector<MeshVertex>& vertices,
                          const std::vector<uint32_t>& indices, const importmeta::ModelLodSettings& settings);

} // namespace mye::ModelCook
