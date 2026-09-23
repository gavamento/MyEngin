//====================================================================================
//                          ShaderCache.cpp
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          シェーダのバイトコードキャッシュの直列化
//====================================================================================
#include "Engine/Renderer/ShaderCache.h"

#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>

#include "Engine/Core/Hash.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr uint32_t kMagic = 0x4353594Du; // "MYSC"
// ★形式を変えたら上げる。古い版のファイルは Decode が弾き、次のコンパイルで上書きされる
// v2 (M79 sub-02): isSurface フィールドを追加 (5 blob のサーフェスプログラムに対応)
constexpr uint32_t kVersion = 2;
// 1 項目あたりの上限 (壊れた長さフィールドで巨大確保しないための保険)
constexpr uint32_t kMaxBlobBytes = 64u * 1024u * 1024u;
constexpr uint32_t kMaxDeps = 4096;
constexpr uint32_t kMaxStringBytes = 4096;

void AppendPod(std::vector<uint8_t>& buf, const auto& v)
{
    const size_t at = buf.size();
    buf.resize(at + sizeof(v));
    std::memcpy(buf.data() + at, &v, sizeof(v));
}

void AppendString(std::vector<uint8_t>& buf, const std::string& s)
{
    AppendPod(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// 先頭から順に読む。どこかで足りなくなったら ok_ が落ち、以降は全部失敗する
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool Pod(auto& out)
    {
        if (!ok_ || size_ - pos_ < sizeof(out)) {
            ok_ = false;
            return false;
        }
        std::memcpy(&out, data_ + pos_, sizeof(out));
        pos_ += sizeof(out);
        return true;
    }

    bool Bytes(size_t n, const uint8_t*& out)
    {
        if (!ok_ || size_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        out = data_ + pos_;
        pos_ += n;
        return true;
    }

    bool String(std::string& out)
    {
        uint32_t n = 0;
        const uint8_t* p = nullptr;
        if (!Pod(n) || n > kMaxStringBytes || !Bytes(n, p)) {
            ok_ = false;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(p), n);
        return true;
    }

    bool AtEnd() const { return ok_ && pos_ == size_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace

uint64_t ShaderCacheConfigKey(bool isCompute, uint32_t compileFlags)
{
    // D3D_COMPILER_VERSION はリンクしている d3dcompiler の版 (47)。
    // 版が変わるとバイトコードも変わりうるので、キーに混ぜて旧項目を自然に捨てる
    uint64_t h = HashStr(isCompute ? "CSMain|cs_5_0" : "VSMain|vs_5_0|PSMain|ps_5_0");
    h = HashCombine(h, compileFlags);
    h = HashCombine(h, static_cast<uint64_t>(D3D_COMPILER_VERSION));
    h = HashCombine(h, kVersion);
    return h;
}

uint64_t SurfaceShaderCacheConfigKey(uint32_t compileFlags)
{
    uint64_t h = HashStr(
        "MyeVSColor|vs_5_0|MyePSColor|ps_5_0|MyeVSShadow|vs_5_0|MyeVSVelocity|vs_5_0|MyePSVelocity|ps_5_0");
    h = HashCombine(h, compileFlags);
    h = HashCombine(h, static_cast<uint64_t>(D3D_COMPILER_VERSION));
    h = HashCombine(h, kVersion);
    return h;
}

std::wstring ShaderCacheFileName(const std::wstring& normalizedShaderPath, bool isCompute)
{
    // ★パスをそのままファイル名にしない — ルートが 2 つあるので同名シェーダが衝突し、
    //   しかも区切り文字がファイル名に使えない。正規化パスのハッシュで一意にする
    const uint64_t h =
        HashStr(isCompute ? "|cs" : "|vsps", HashStr(WideToUtf8(normalizedShaderPath)));
    wchar_t name[40];
    swprintf_s(name, L"%016llx.shc", static_cast<unsigned long long>(h));
    return name;
}

std::wstring SurfaceShaderCacheFileName(const std::wstring& normalizedShaderPath)
{
    const uint64_t h = HashStr("|surface", HashStr(WideToUtf8(normalizedShaderPath)));
    wchar_t name[40];
    swprintf_s(name, L"%016llx.shc", static_cast<unsigned long long>(h));
    return name;
}

std::vector<uint8_t> EncodeShaderCacheEntry(const ShaderCacheEntry& entry)
{
    std::vector<uint8_t> buf;
    AppendPod(buf, kMagic);
    AppendPod(buf, kVersion);
    AppendPod(buf, entry.configKey);
    AppendPod(buf, static_cast<uint32_t>(entry.isCompute ? 1 : 0));
    AppendPod(buf, static_cast<uint32_t>(entry.isSurface ? 1 : 0));
    AppendPod(buf, entry.sourceHash);
    AppendPod(buf, static_cast<uint32_t>(entry.deps.size()));
    for (const ShaderCacheEntry::Dependency& d : entry.deps) {
        AppendString(buf, d.requestedName);
        AppendString(buf, d.resolvedPath);
        AppendPod(buf, d.contentHash);
    }
    AppendPod(buf, static_cast<uint32_t>(entry.blobs.size()));
    for (const std::vector<uint8_t>& b : entry.blobs) {
        AppendPod(buf, static_cast<uint32_t>(b.size()));
        buf.insert(buf.end(), b.begin(), b.end());
    }
    // 末尾チェックサム。rename で書きかけは見えないが、ディスク破損や別プロセスの
    // 途中書きを「読めたが中身が違う」として黙って使わないための最後の砦
    AppendPod(buf, HashBytes(buf.data(), buf.size()));
    return buf;
}

bool DecodeShaderCacheEntry(const std::vector<uint8_t>& bytes, ShaderCacheEntry& out)
{
    out = ShaderCacheEntry{};
    if (bytes.size() < sizeof(uint64_t)) {
        return false;
    }
    const size_t bodySize = bytes.size() - sizeof(uint64_t);
    uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + bodySize, sizeof(stored));
    if (stored != HashBytes(bytes.data(), bodySize)) {
        return false;
    }

    Reader r(bytes.data(), bodySize);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t isCompute = 0;
    uint32_t isSurface = 0;
    uint32_t depCount = 0;
    if (!r.Pod(magic) || magic != kMagic || !r.Pod(version) || version != kVersion
        || !r.Pod(out.configKey) || !r.Pod(isCompute) || !r.Pod(isSurface) || !r.Pod(out.sourceHash)
        || !r.Pod(depCount) || depCount > kMaxDeps) {
        return false;
    }
    out.isCompute = (isCompute != 0);
    out.isSurface = (isSurface != 0);
    out.deps.resize(depCount);
    for (ShaderCacheEntry::Dependency& d : out.deps) {
        if (!r.String(d.requestedName) || !r.String(d.resolvedPath) || !r.Pod(d.contentHash)) {
            return false;
        }
    }
    uint32_t blobCount = 0;
    // CS = 1 本 / VS+PS = 2 本 / サーフェス = 5 本 以外は形式違反
    const uint32_t expectedBlobs = out.isSurface ? 5u : (out.isCompute ? 1u : 2u);
    if (!r.Pod(blobCount) || blobCount != expectedBlobs) {
        return false;
    }
    out.blobs.resize(blobCount);
    for (std::vector<uint8_t>& b : out.blobs) {
        uint32_t n = 0;
        const uint8_t* p = nullptr;
        if (!r.Pod(n) || n == 0 || n > kMaxBlobBytes || !r.Bytes(n, p)) {
            return false;
        }
        b.assign(p, p + n);
    }
    return r.AtEnd();
}

} // namespace mye
