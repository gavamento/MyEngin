//====================================================================================
//                          DmNet.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          .dmnet ローダの実装
//====================================================================================
#include "Engine/Engine/Modal/DmNet.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include <DirectXPackedVector.h>

namespace mye {
namespace {

// ヘッダ/op 表を先頭から読み進めるだけのカーソル。フィールド単位で読む
// (Material の暗黙パディングの罠と同じ理由で、struct をそのまま cast しない)
struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool ReadU32(uint32_t& v)
    {
        if (pos + sizeof(uint32_t) > size) {
            return false;
        }
        std::memcpy(&v, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return true;
    }
    bool ReadI32(int32_t& v)
    {
        uint32_t bits = 0;
        if (!ReadU32(bits)) {
            return false;
        }
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }
    bool ReadF32(float& v)
    {
        uint32_t bits = 0;
        if (!ReadU32(bits)) {
            return false;
        }
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }
    bool ReadU64(uint64_t& v)
    {
        if (pos + sizeof(uint64_t) > size) {
            return false;
        }
        std::memcpy(&v, data + pos, sizeof(uint64_t));
        pos += sizeof(uint64_t);
        return true;
    }
};

bool ReadHeader(Reader& r, DmNetHeader& out, std::string* err)
{
    uint32_t magic = 0, version = 0, opCount = 0, bufferCount = 0;
    if (!r.ReadU32(magic) || !r.ReadU32(version) || !r.ReadU32(opCount) || !r.ReadU32(bufferCount)) {
        if (err) {
            *err = "truncated header";
        }
        return false;
    }
    out.magic = magic;
    out.version = version;
    out.opCount = opCount;
    out.bufferCount = bufferCount;
    if (!r.ReadI32(out.inN) || !r.ReadI32(out.outN) || !r.ReadI32(out.bands)
        || !r.ReadI32(out.channels)) {
        if (err) {
            *err = "truncated header (shape fields)";
        }
        return false;
    }
    if (!r.ReadF32(out.fMinHz) || !r.ReadF32(out.fMaxHz) || !r.ReadF32(out.logAmpMin)
        || !r.ReadF32(out.logAmpMax) || !r.ReadF32(out.ampScale) || !r.ReadF32(out.maskThreshold)) {
        if (err) {
            *err = "truncated header (amp fields)";
        }
        return false;
    }
    if (!r.ReadF32(out.refYoung) || !r.ReadF32(out.refDensity) || !r.ReadF32(out.refPoisson)
        || !r.ReadF32(out.refSizeL) || !r.ReadF32(out.refAlpha) || !r.ReadF32(out.refBeta)) {
        if (err) {
            *err = "truncated header (reference material fields)";
        }
        return false;
    }
    for (int i = 0; i < kModalBands; ++i) {
        if (!r.ReadF32(out.bandCenterHz[i])) {
            if (err) {
                *err = "truncated header (bandCenterHz)";
            }
            return false;
        }
    }
    if (!r.ReadU64(out.weightsHash) || !r.ReadU32(out.paramCount)) {
        if (err) {
            *err = "truncated header (weightsHash/paramCount)";
        }
        return false;
    }
    // reserved (9 x u32、layout.py の伸び代)。中身は読み捨ててよい — 値の入れ物 (DmNetHeader)
    // は先頭 1 個しか持たないので、残りは検証に使わない (将来ヘッダを増やす余白)
    for (int i = 0; i < 9; ++i) {
        uint32_t reserved = 0;
        if (!r.ReadU32(reserved)) {
            if (err) {
                *err = "truncated header (reserved)";
            }
            return false;
        }
        if (i == 0) {
            out.reserved = reserved;
        }
    }
    if (r.pos != kDmNetHeaderBytes) {
        if (err) {
            *err = "header length mismatch (field count drifted from kDmNetHeaderBytes)";
        }
        return false;
    }
    return true;
}

bool ReadOp(Reader& r, DmNetOp& op, std::string* err)
{
    uint32_t weightOffset = 0, biasOffset = 0;
    if (!r.ReadI32(op.type) || !r.ReadI32(op.in0) || !r.ReadI32(op.in1) || !r.ReadI32(op.out)
        || !r.ReadI32(op.cin) || !r.ReadI32(op.cout) || !r.ReadI32(op.k) || !r.ReadI32(op.stride)
        || !r.ReadI32(op.pad) || !r.ReadI32(op.outPad) || !r.ReadU32(weightOffset)
        || !r.ReadU32(biasOffset)) {
        if (err) {
            *err = "truncated op table";
        }
        return false;
    }
    op.weightOffset = weightOffset;
    op.biasOffset = biasOffset;
    return true;
}

} // namespace

uint64_t DmNetFnv1a64(const uint8_t* data, size_t size)
{
    // FNV-1a 64bit。export.py の fnv1a64 と同一定数 (http://www.isthe.com/chongo/tech/comp/fnv/)
    uint64_t h = 0xCBF29CE484222325ULL;
    constexpr uint64_t kPrime = 0x100000001B3ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= kPrime;
    }
    return h;
}

bool LoadDmNet(const std::wstring& path, DmNet& out, std::string* err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) {
            *err = "cannot open file";
        }
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < kDmNetHeaderBytes) {
        if (err) {
            *err = "file too small for header";
        }
        return false;
    }

    Reader r{ bytes.data(), bytes.size(), 0 };
    DmNetHeader header;
    if (!ReadHeader(r, header, err)) {
        return false;
    }
    if (header.magic != DmNetHeader{}.magic) {
        if (err) {
            *err = "bad .dmnet magic";
        }
        return false;
    }
    if (header.version != DmNetHeader{}.version) {
        if (err) {
            *err = "unsupported .dmnet version";
        }
        return false;
    }
    if (header.paramCount > kDmNetMaxParamCount) {
        if (err) {
            *err = "paramCount exceeds budget";
        }
        return false;
    }

    const size_t opsBytes = static_cast<size_t>(header.opCount) * kDmNetOpBytes;
    if (kDmNetHeaderBytes + opsBytes > bytes.size()) {
        if (err) {
            *err = "file too small for op table";
        }
        return false;
    }
    std::vector<DmNetOp> ops;
    ops.reserve(header.opCount);
    for (uint32_t i = 0; i < header.opCount; ++i) {
        DmNetOp op;
        if (!ReadOp(r, op, err)) {
            return false;
        }
        ops.push_back(op);
    }

    // 参照される重み/バイアスが全てファイル内に収まっているかを検査する
    for (const DmNetOp& op : ops) {
        if (op.weightOffset == kDmNetOffsetNone && op.biasOffset == kDmNetOffsetNone) {
            continue; // ReLU/Add
        }
        if (op.weightOffset == kDmNetOffsetNone || op.biasOffset == kDmNetOffsetNone) {
            if (err) {
                *err = "op has weight without bias (or vice versa)";
            }
            return false;
        }
        const size_t numel = static_cast<size_t>(op.cin) * static_cast<size_t>(op.cout)
            * static_cast<size_t>(op.k) * static_cast<size_t>(op.k) * static_cast<size_t>(op.k);
        const size_t wEnd = static_cast<size_t>(op.weightOffset) + numel * sizeof(uint16_t);
        const size_t bEnd = static_cast<size_t>(op.biasOffset) + static_cast<size_t>(op.cout) * sizeof(float);
        if (wEnd > bytes.size() || bEnd > bytes.size()) {
            if (err) {
                *err = "op weight/bias offset out of range";
            }
            return false;
        }
    }

    // 重み+バイアス blob の FNV-1a を照合する (export.py と同じ範囲: ヘッダ+op 表の直後から末尾まで)
    const size_t blobStart = kDmNetHeaderBytes + opsBytes;
    if (blobStart > bytes.size()) {
        if (err) {
            *err = "file too small for weight/bias blob";
        }
        return false;
    }
    const uint64_t computedHash = DmNetFnv1a64(bytes.data() + blobStart, bytes.size() - blobStart);
    if (computedHash != header.weightsHash) {
        if (err) {
            *err = "weightsHash mismatch (corrupt or truncated .dmnet)";
        }
        return false;
    }

    out.header = header;
    out.ops = std::move(ops);
    out.bytes = std::move(bytes);
    return true;
}

bool DmNetOpWeightFloat(const DmNet& net, const DmNetOp& op, std::vector<float>& weightOut,
                        std::vector<float>& biasOut, std::string* err)
{
    if (op.weightOffset == kDmNetOffsetNone || op.biasOffset == kDmNetOffsetNone) {
        if (err) {
            *err = "op has no weights (ReLU/Add)";
        }
        return false;
    }
    const size_t numel = static_cast<size_t>(op.cin) * static_cast<size_t>(op.cout)
        * static_cast<size_t>(op.k) * static_cast<size_t>(op.k) * static_cast<size_t>(op.k);
    const size_t wBytes = numel * sizeof(uint16_t);
    if (static_cast<size_t>(op.weightOffset) + wBytes > net.bytes.size()) {
        if (err) {
            *err = "weight offset out of range";
        }
        return false;
    }
    weightOut.resize(numel);
    const auto* half =
        reinterpret_cast<const DirectX::PackedVector::HALF*>(net.bytes.data() + op.weightOffset);
    for (size_t i = 0; i < numel; ++i) {
        weightOut[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    }
    const size_t bBytes = static_cast<size_t>(op.cout) * sizeof(float);
    if (static_cast<size_t>(op.biasOffset) + bBytes > net.bytes.size()) {
        if (err) {
            *err = "bias offset out of range";
        }
        return false;
    }
    biasOut.resize(static_cast<size_t>(op.cout));
    std::memcpy(biasOut.data(), net.bytes.data() + op.biasOffset, bBytes);
    return true;
}

} // namespace mye
