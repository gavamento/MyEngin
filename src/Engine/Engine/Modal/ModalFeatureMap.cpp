//====================================================================================
//                          ModalFeatureMap.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          特徴マップの構築と .msfm 表の直列化
//====================================================================================
#include "Engine/Engine/Modal/ModalFeatureMap.h"

#include <cstring>

#include <DirectXPackedVector.h>

namespace mye {
namespace {

// .mcvx (ConvexColliderLibrary.cpp の AppendPod32) と同型のヘルパ。
// フィールド単位で書く (struct をそのまま memcpy しない、Material の暗黙パディングの罠と同じ理由)
void AppendU16(std::vector<uint8_t>& buf, uint16_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), b, b + sizeof(v));
}
void AppendU32(std::vector<uint8_t>& buf, uint32_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), b, b + sizeof(v));
}
void AppendU64(std::vector<uint8_t>& buf, uint64_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), b, b + sizeof(v));
}
void AppendF32(std::vector<uint8_t>& buf, float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    AppendU32(buf, bits);
}

constexpr uint32_t kModalTableVersion = 1;
constexpr uint32_t kModalTableSanityCap = 4096; // 1 モデルの feature map 数の上限 (壊れた blob 対策)

void SerializeOne(const ModalFeatureMap& map, std::vector<uint8_t>& out)
{
    AppendU32(out, map.version);
    AppendU64(out, map.modelHash);
    for (float v : map.frame.origin) {
        AppendF32(out, v);
    }
    AppendF32(out, map.frame.voxelSize);
    for (float v : map.frame.aabbMin) {
        AppendF32(out, v);
    }
    for (float v : map.frame.aabbMax) {
        AppendF32(out, v);
    }
    AppendF32(out, map.frame.longestEdge);
    AppendU32(out, map.validCount);
    for (uint16_t s : map.cellSlot) {
        AppendU16(out, s);
    }
    AppendU32(out, static_cast<uint32_t>(map.feat.size()));
    for (uint16_t v : map.feat) {
        AppendU16(out, v);
    }
}

bool DeserializeOne(const uint8_t* data, size_t size, size_t& pos, ModalFeatureMap& out)
{
    auto readU16 = [&](uint16_t& v) {
        if (pos + sizeof(uint16_t) > size) {
            return false;
        }
        std::memcpy(&v, data + pos, sizeof(uint16_t));
        pos += sizeof(uint16_t);
        return true;
    };
    auto readU32 = [&](uint32_t& v) {
        if (pos + sizeof(uint32_t) > size) {
            return false;
        }
        std::memcpy(&v, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return true;
    };
    auto readU64 = [&](uint64_t& v) {
        if (pos + sizeof(uint64_t) > size) {
            return false;
        }
        std::memcpy(&v, data + pos, sizeof(uint64_t));
        pos += sizeof(uint64_t);
        return true;
    };
    auto readF32 = [&](float& v) {
        uint32_t bits = 0;
        if (!readU32(bits)) {
            return false;
        }
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    };

    ModalFeatureMap m;
    if (!readU32(m.version) || m.version != kMsfmVersion) {
        return false;
    }
    if (!readU64(m.modelHash)) {
        return false;
    }
    for (float& v : m.frame.origin) {
        if (!readF32(v)) {
            return false;
        }
    }
    if (!readF32(m.frame.voxelSize)) {
        return false;
    }
    for (float& v : m.frame.aabbMin) {
        if (!readF32(v)) {
            return false;
        }
    }
    for (float& v : m.frame.aabbMax) {
        if (!readF32(v)) {
            return false;
        }
    }
    if (!readF32(m.frame.longestEdge) || !readU32(m.validCount)) {
        return false;
    }
    if (m.validCount > 4096) {
        return false;
    }
    for (uint16_t& s : m.cellSlot) {
        if (!readU16(s)) {
            return false;
        }
    }
    uint32_t featCount = 0;
    if (!readU32(featCount) || featCount != m.validCount * static_cast<uint32_t>(kModalChannels)) {
        return false; // feat 長は必ず validCount*192 (書式の自己検査)
    }
    m.feat.resize(featCount);
    for (uint16_t& v : m.feat) {
        if (!readU16(v)) {
            return false;
        }
    }
    out = std::move(m);
    return true;
}

} // namespace

int ModalFeatureMap::RowOf(int rawCell) const
{
    if (rawCell < 0 || rawCell >= 4096 || validCount == 0) {
        return -1;
    }
    const int validRawCell = cellSlot[rawCell];
    int row = -1;
    for (int c = 0; c <= validRawCell; ++c) {
        if (cellSlot[c] == c) {
            ++row;
        }
    }
    return row;
}

bool ModalFeatureMap::CellFeature(int rawCell, ModalCellFeature& out) const
{
    const int row = RowOf(rawCell);
    if (row < 0 || static_cast<size_t>(row + 1) * kModalChannels > feat.size()) {
        return false;
    }
    const auto* half = reinterpret_cast<const DirectX::PackedVector::HALF*>(
        feat.data() + static_cast<size_t>(row) * kModalChannels);
    for (int i = 0; i < kModalChannels; ++i) {
        out.v[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    }
    return true;
}

bool BuildFeatureMap(ModalInferenceBackend& backend, const DmNet& net, const modal::VoxelGrid& grid,
                    ModalFeatureMap& out, std::string* err)
{
    std::vector<float> raw; // [channel][cell] channel-major (kModalChannels x 4096)
    if (!backend.Infer(grid, raw, err)) {
        return false;
    }
    if (raw.size() != static_cast<size_t>(kModalChannels) * 4096) {
        if (err) {
            *err = "backend Infer() returned unexpected size";
        }
        return false;
    }

    uint16_t cellSlot[4096];
    modal::BuildCellSlotTable(grid, cellSlot);

    ModalFeatureMap map;
    map.version = kMsfmVersion;
    map.modelHash = net.header.weightsHash;
    map.frame = grid.frame;
    std::memcpy(map.cellSlot, cellSlot, sizeof(cellSlot));

    uint32_t validCount = 0;
    for (int cell = 0; cell < 4096; ++cell) {
        if (cellSlot[cell] == cell) {
            ++validCount;
        }
    }
    map.validCount = validCount;
    map.feat.resize(static_cast<size_t>(validCount) * kModalChannels);

    size_t row = 0;
    for (int cell = 0; cell < 4096; ++cell) {
        if (cellSlot[cell] != cell) {
            continue;
        }
        for (int c = 0; c < kModalChannels; ++c) {
            const float v = raw[static_cast<size_t>(c) * 4096 + static_cast<size_t>(cell)];
            map.feat[row * kModalChannels + c] =
                DirectX::PackedVector::XMConvertFloatToHalf(v);
        }
        ++row;
    }

    out = std::move(map);
    return true;
}

void SerializeModalTable(const std::vector<std::pair<std::string, ModalFeatureMap>>& table,
                         std::vector<uint8_t>& out)
{
    out.clear();
    AppendU32(out, kModalTableVersion);
    AppendU32(out, static_cast<uint32_t>(table.size()));
    for (const auto& [key, map] : table) {
        AppendU32(out, static_cast<uint32_t>(key.size()));
        out.insert(out.end(), key.begin(), key.end());
        SerializeOne(map, out);
    }
}

bool DeserializeModalTable(const std::vector<uint8_t>& in,
                           std::vector<std::pair<std::string, ModalFeatureMap>>& out)
{
    out.clear();
    size_t pos = 0;
    auto readU32 = [&](uint32_t& v) {
        if (pos + sizeof(uint32_t) > in.size()) {
            return false;
        }
        std::memcpy(&v, in.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return true;
    };
    uint32_t version = 0, count = 0;
    if (!readU32(version) || version != kModalTableVersion || !readU32(count)
        || count > kModalTableSanityCap) {
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t keyLen = 0;
        if (!readU32(keyLen) || pos + keyLen > in.size()) {
            out.clear();
            return false;
        }
        std::string key(reinterpret_cast<const char*>(in.data() + pos), keyLen);
        pos += keyLen;
        ModalFeatureMap map;
        if (!DeserializeOne(in.data(), in.size(), pos, map)) {
            out.clear();
            return false;
        }
        out.emplace_back(std::move(key), std::move(map));
    }
    return true;
}

} // namespace mye
