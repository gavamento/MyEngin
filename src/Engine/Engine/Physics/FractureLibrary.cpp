//====================================================================================
//                          FractureLibrary.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片資産(.mfrac)の登録実装
//====================================================================================
#include "Engine/Engine/Physics/FractureLibrary.h"

#include <algorithm>
#include <utility>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

namespace mye {
namespace {

std::string FragName(const std::string& prefix, size_t i) { return prefix + "#frag" + std::to_string(i); }
std::string CapName(const std::string& prefix, size_t i) { return FragName(prefix, i) + "#cap"; }
std::string HullName(const std::string& prefix, size_t i) { return FragName(prefix, i) + "#hull"; }

MeshVertex ToMeshVertex(const FractureVertex& v)
{
    MeshVertex mv;
    mv.position = v.position;
    mv.normal = v.normal;
    mv.uv = v.uv;
    return mv;
}

// FractureVertex (Renderer 非依存) → MeshVertex への詰め替え。ボーン欄は既定 (非スキン、sub-10 で埋める)
void ToMeshData(const FractureMesh& mesh, std::vector<MeshVertex>& verts, std::vector<uint32_t>& indices)
{
    verts.resize(mesh.verts.size());
    for (size_t i = 0; i < mesh.verts.size(); ++i) {
        verts[i] = ToMeshVertex(mesh.verts[i]);
    }
    indices.assign(mesh.indices.begin(), mesh.indices.end());
}

} // namespace

FractureAsset::FractureData BuildFractureAssetData(const FractureBakeResult& bake, uint64_t sourceMeshHash,
                                                    uint32_t seed, int32_t pieceCount, int32_t openMeshMode,
                                                    int32_t voxelResolution)
{
    FractureAsset::FractureData data;
    data.sourceMeshHash = sourceMeshHash;
    data.seed = seed;
    data.pieceCount = pieceCount;
    data.openMeshMode = openMeshMode;
    data.voxelResolution = voxelResolution;
    data.mergedCount = bake.mergedCount;
    data.pieces.reserve(bake.pieces.size());
    for (const FracturePieceBake& piece : bake.pieces) {
        FractureAsset::PieceRecord pr;
        pr.origin = piece.origin;
        pr.volume = piece.volume;
        ToMeshData(piece.outer, pr.outerVerts, pr.outerIndices);
        ToMeshData(piece.cap, pr.capVerts, pr.capIndices);
        pr.hull = piece.hull;
        pr.neighbors.reserve(piece.neighbors.size());
        for (const FractureNeighbor& n : piece.neighbors) {
            pr.neighbors.push_back({ n.pieceIndex, n.area });
        }
        data.droppedNeighborTotal += piece.droppedNeighbors;
        data.pieces.push_back(std::move(pr));
    }
    return data;
}

const FractureAssetHandle* FractureLibrary::LoadFromFile(const std::wstring& path)
{
    const std::string prefix = assetkey::SubAssetKeyPrefix(path);
    const auto it = handles_.find(prefix);
    if (it != handles_.end()) {
        return &it->second;
    }
    if (failedPaths_.count(path) != 0) {
        return nullptr; // 既に ERROR 済み (同じ壊れた資産へ毎回スパムしない)
    }
    FractureAsset::FractureData data;
    if (!FractureAsset::Load(path, data)) {
        MYE_LOG_ERROR("[fracture] failed to load .mfrac: %s", WideToUtf8(path).c_str());
        failedPaths_.insert(path);
        return nullptr;
    }
    return RegisterInternal(prefix, std::move(data));
}

const FractureAssetHandle* FractureLibrary::RegisterBaked(const std::string& namePrefix,
                                                           const FractureBakeResult& bake,
                                                           uint64_t sourceMeshHash, uint32_t seed,
                                                           int32_t pieceCount, int32_t openMeshMode,
                                                           int32_t voxelResolution)
{
    FractureAsset::FractureData data
        = BuildFractureAssetData(bake, sourceMeshHash, seed, pieceCount, openMeshMode, voxelResolution);
    return RegisterInternal(namePrefix, std::move(data));
}

const FractureAssetHandle* FractureLibrary::Find(const std::string& namePrefix) const
{
    const auto it = handles_.find(namePrefix);
    return it != handles_.end() ? &it->second : nullptr;
}

const FractureAssetHandle* FractureLibrary::FindByAssetId(AssetID id) const
{
    const auto it = byHash_.find(id.value);
    return it != byHash_.end() ? Find(it->second) : nullptr;
}

const FractureAssetHandle* FractureLibrary::RegisterInternal(const std::string& prefix,
                                                              FractureAsset::FractureData data)
{
    FractureAssetHandle handle;
    handle.namePrefix = prefix;
    handle.pieces.reserve(data.pieces.size());
    for (size_t i = 0; i < data.pieces.size(); ++i) {
        const FractureAsset::PieceRecord& pr = data.pieces[i];
        FracturePieceRef ref;
        ref.origin = pr.origin;
        ref.volume = pr.volume;
        // 完全に内部の破片は outer/cap が 0 頂点になり得る (幾何的には正当)。0 頂点のまま
        // Register すると頂点/インデックスバッファの作成が失敗するため、空なら未登録のままにする
        if (resources_ != nullptr) {
            if (!pr.outerVerts.empty() && !pr.outerIndices.empty()) {
                ref.outerMesh = resources_->meshes.Register(FragName(prefix, i), pr.outerVerts, pr.outerIndices);
            }
            if (!pr.capVerts.empty() && !pr.capIndices.empty()) {
                ref.capMesh = resources_->meshes.Register(CapName(prefix, i), pr.capVerts, pr.capIndices);
            }
        }
        const AssetID hullId{ HashStr(HullName(prefix, i)) };
        if (colliders_ != nullptr) {
            colliders_->Register(hullId, pr.hull);
        }
        ref.hull = hullId;
        ref.neighbors = pr.neighbors;
        handle.pieces.push_back(std::move(ref));
    }
    handle.data = std::move(data);
    byHash_[HashStr(prefix)] = prefix;
    return &handles_.insert_or_assign(prefix, std::move(handle)).first->second;
}

void FractureLibrary::ReregisterAll()
{
    if (colliders_ == nullptr) {
        return;
    }
    for (auto& [prefix, handle] : handles_) {
        const size_t n = std::min(handle.pieces.size(), handle.data.pieces.size());
        for (size_t i = 0; i < n; ++i) {
            colliders_->Register(handle.pieces[i].hull, handle.data.pieces[i].hull);
        }
    }
}

void FractureLibrary::Clear()
{
    handles_.clear();
    byHash_.clear();
    failedPaths_.clear();
}

namespace fracturelib {
namespace {
FractureLibrary* g_lib = nullptr;
} // namespace

void Install(FractureLibrary* lib)
{
    g_lib = lib;
}

FractureLibrary* Library()
{
    return g_lib;
}

} // namespace fracturelib
} // namespace mye
