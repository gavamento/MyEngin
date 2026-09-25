//====================================================================================
//                          FractureAsset.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片資産(.mfrac)の直列化とファイル入出力
//====================================================================================
#include "Engine/Engine/Asset/FractureAsset.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/ByteIo.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/FractureBake.h" // kMaxFracturePieces / kMaxFractureNeighbors (上限の検算)
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace FractureAsset {
namespace {

// 実体はディスク上で 'M','F','R','C' の順に並ぶ (x64 は LE 固定)
constexpr uint32_t kMagic = static_cast<uint32_t>('M') | (static_cast<uint32_t>('F') << 8)
    | (static_cast<uint32_t>('R') << 16) | (static_cast<uint32_t>('C') << 24);

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    std::ifstream f(fs::path{ path }, std::ios::binary);
    if (!f) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (!out.empty()) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (f.gcount() != static_cast<std::streamsize>(out.size())) {
            return false;
        }
    }
    return true;
}

// 凸包は SerializeConvexHull の生バイトを長さ前置きでそのまま埋め込む
void WriteHull(ByteWriter& w, const ConvexHullData& hull)
{
    std::vector<uint8_t> blob;
    SerializeConvexHull(hull, blob);
    w.PodVector(blob);
}

bool ReadHull(ByteReader& r, ConvexHullData& out)
{
    const std::vector<uint8_t> blob = r.PodVector<uint8_t>();
    if (!r.Ok()) {
        return false;
    }
    size_t pos = 0;
    return DeserializeConvexHull(blob.data(), blob.size(), pos, out) && pos == blob.size();
}

} // namespace

void Serialize(const FractureData& d, std::vector<uint8_t>& out)
{
    std::vector<std::byte> buf;
    ByteWriter w(buf);
    w.U32(kMagic);
    w.U32(kVersion);
    w.U64(d.sourceMeshHash);
    w.U32(d.seed);
    w.I32(d.pieceCount);
    w.I32(d.openMeshMode);
    w.I32(d.voxelResolution);
    w.I32(d.mergedCount);
    w.I32(d.droppedNeighborTotal);
    w.U64(static_cast<uint64_t>(d.pieces.size()));
    for (const PieceRecord& p : d.pieces) {
        w.Pod(p.origin);
        w.Pod(p.volume);
        w.PodVector(p.outerVerts);
        w.PodVector(p.outerIndices);
        w.PodVector(p.capVerts);
        w.PodVector(p.capIndices);
        WriteHull(w, p.hull);
        w.PodVector(p.neighbors);
        w.Str(p.boneName);
    }
    out.assign(reinterpret_cast<const uint8_t*>(buf.data()),
               reinterpret_cast<const uint8_t*>(buf.data()) + buf.size());
}

bool Deserialize(const std::vector<uint8_t>& in, FractureData& out)
{
    out = FractureData{};
    ByteReader r(reinterpret_cast<const std::byte*>(in.data()), in.size());
    const uint32_t magic = r.U32();
    const uint32_t version = r.U32();
    if (!r.Ok() || magic != kMagic || version != kVersion) {
        return false;
    }
    out.sourceMeshHash = r.U64();
    out.seed = r.U32();
    out.pieceCount = r.I32();
    out.openMeshMode = r.I32();
    out.voxelResolution = r.I32();
    out.mergedCount = r.I32();
    out.droppedNeighborTotal = r.I32();

    // 件数は resize 前に上限で検算する (壊れた blob の巨大な値をそのまま resize しない)
    const uint64_t pieceCount = r.U64();
    if (!r.Ok() || pieceCount > static_cast<uint64_t>(kMaxFracturePieces)) {
        return false;
    }
    out.pieces.resize(static_cast<size_t>(pieceCount));
    for (PieceRecord& p : out.pieces) {
        p.origin = r.Pod<DirectX::XMFLOAT3>();
        p.volume = r.Pod<double>();
        p.outerVerts = r.PodVector<MeshVertex>();
        p.outerIndices = r.PodVector<uint32_t>();
        p.capVerts = r.PodVector<MeshVertex>();
        p.capIndices = r.PodVector<uint32_t>();
        if (!r.Ok() || !ReadHull(r, p.hull)) {
            return false;
        }
        p.neighbors = r.PodVector<NeighborRecord>();
        if (!r.Ok() || p.neighbors.size() > static_cast<size_t>(kMaxFractureNeighbors)) {
            return false;
        }
        p.boneName = r.Str();
    }
    if (!r.Ok() || r.Remaining() != 0) {
        return false; // 余りがある = 別形式/継ぎ足し。丸ごと捨てる
    }
    return true;
}

bool Save(const std::wstring& path, const FractureData& d)
{
    std::vector<uint8_t> blob;
    Serialize(d, blob);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
    if (!WriteFileReplacing(path, bytes)) {
        MYE_LOG_ERROR("[fracture] failed to write .mfrac: %s", WideToUtf8(path).c_str());
        return false;
    }
    return true;
}

bool Load(const std::wstring& path, FractureData& out)
{
    std::vector<uint8_t> blob;
    if (!ReadWholeFile(path, blob)) {
        return false;
    }
    return Deserialize(blob, out);
}

} // namespace FractureAsset
} // namespace mye
