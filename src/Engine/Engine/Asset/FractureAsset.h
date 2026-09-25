//====================================================================================
//                          FractureAsset.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片資産(.mfrac)の保存形式(spec §4.2)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Physics/ConvexHull.h"
#include "Engine/Renderer/GpuResources.h"

namespace mye {
namespace FractureAsset {

// エディタで焼いた Voronoi 分割の結果 (破片ごとのメッシュ・凸包・隣接) を保存する形式。
// 分割コア (FractureBake.h) は Renderer 非依存の FractureMesh を使うが、ここでは
// MeshLibrary へそのまま渡せる MeshVertex で保持する (詰め替えは FractureLibrary が行う)

inline constexpr const wchar_t* kFractureExt = L".mfrac";
inline constexpr uint32_t kVersion = 1;

// 破片 i の隣接 1 本 (相手 index、面積)。FractureBake.h の FractureNeighbor と同じ意味
struct NeighborRecord {
    int32_t pieceIndex = 0;
    float area = 0.0f;
};

// 破片 1 個分。outer/cap/hull は破片原点 (体積重心) を基準にしたローカル空間
struct PieceRecord {
    DirectX::XMFLOAT3 origin{ 0, 0, 0 }; // ソース空間の体積重心
    double volume = 0.0;
    std::vector<MeshVertex> outerVerts;
    std::vector<uint32_t> outerIndices;
    std::vector<MeshVertex> capVerts;
    std::vector<uint32_t> capIndices;
    ConvexHullData hull;
    std::vector<NeighborRecord> neighbors; // 相手 index 昇順、32 本まで
    std::string boneName; // スキン破壊 (骨に割り当てた破片) 用。空 = 骨なし
};

struct FractureData {
    uint64_t sourceMeshHash = 0; // ソースメッシュ登録名のハッシュ (焼き直しの照合用)
    uint32_t seed = 0;
    int32_t pieceCount = 0; // 焼き入力の目安値。実際の破片数は pieces.size()
    int32_t openMeshMode = 0;
    int32_t voxelResolution = 0;
    int32_t mergedCount = 0;          // 極小片統合が起きた回数
    int32_t droppedNeighborTotal = 0; // 32 本を超えて切り捨てた隣接の総数
    std::vector<PieceRecord> pieces;
};

// blob <-> 構造体。Deserialize は境界検査つきで、壊れた blob でも false を返すだけで落ちない。
// 書き出しは同じ入力で同じバイト列 (Debug/Release 一致の前提)
void Serialize(const FractureData& d, std::vector<uint8_t>& out);
bool Deserialize(const std::vector<uint8_t>& in, FractureData& out);

// `.mfrac` の読み書き。Save は書き切れたときだけ既存ファイルを置き換える (WriteFileReplacing)
bool Save(const std::wstring& path, const FractureData& d);
bool Load(const std::wstring& path, FractureData& out);

} // namespace FractureAsset
} // namespace mye
