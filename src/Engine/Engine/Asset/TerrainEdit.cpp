#include "Engine/Engine/Asset/TerrainEdit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

using DirectX::XMFLOAT3;

namespace mye {
namespace TerrainEdit {
namespace {

constexpr uint32_t kEditMagic = 0x54444554u;  // "TEDT" (LE) — サイドカーの先頭
constexpr uint32_t kPatchMagic = 0x54415054u; // "TPAT" (LE) — Undo パッチの先頭

// ---- バイト列の読み書き (TerrainAsset.cpp と同じ流儀: 生バイト保存 + 残量で検算) ----

template <typename Buf> void Append(Buf& buf, const void* src, size_t n)
{
    if (n == 0) {
        return; // 空ベクタの data() は nullptr でありうる (nullptr 同士の range は組めない)
    }
    const auto* b = static_cast<const typename Buf::value_type*>(src);
    buf.insert(buf.end(), b, b + n);
}
template <typename Buf, typename T> void AppendPod(Buf& buf, const T& v)
{
    Append(buf, &v, sizeof(T));
}

struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool Bytes(void* dst, size_t n)
    {
        if (n > size - pos) {
            return false;
        }
        if (n != 0) {
            std::memcpy(dst, p + pos, n);
            pos += n;
        }
        return true;
    }
    template <typename T> bool Pod(T& v) { return Bytes(&v, sizeof(T)); }
};

// 正規化高さ [0,1] → R16。TerrainAsset.cpp の ToU16 と**同じ丸め**であること
// (違うと「ブラシで 0 を足しただけ」で全 texel が 1 ずれる)
uint16_t ToU16(float normalized)
{
    const float v = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<uint16_t>(v * 65535.0f + 0.5f);
}

// 中心 1 / 縁 0 の減衰 (smoothstep)。半径 0 以下は常に 0
float Falloff(float dist, float radius)
{
    if (!(radius > 0.0f)) {
        return 0.0f;
    }
    const float t = std::clamp(dist / radius, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// ハイトマップの texel x → 地形ローカル X (頂点格子なので端が地形の縁)
float HeightTexelX(const TerrainAsset::TerrainData& d, uint32_t x)
{
    return (static_cast<float>(x) / static_cast<float>(d.heightW - 1) - 0.5f) * d.worldSizeX;
}
float HeightTexelZ(const TerrainAsset::TerrainData& d, uint32_t z)
{
    return (static_cast<float>(z) / static_cast<float>(d.heightH - 1) - 0.5f) * d.worldSizeZ;
}
// スプラットの texel i → 地形ローカル X (テクスチャなので **texel 中心** = (i+0.5)/n)
float SplatTexelX(const TerrainAsset::TerrainData& d, uint32_t x)
{
    return ((static_cast<float>(x) + 0.5f) / static_cast<float>(d.splatW) - 0.5f) * d.worldSizeX;
}
float SplatTexelZ(const TerrainAsset::TerrainData& d, uint32_t z)
{
    return ((static_cast<float>(z) + 0.5f) / static_cast<float>(d.splatH) - 0.5f) * d.worldSizeZ;
}

// 実数区間 [lo, hi] を [0, n-1] の texel 範囲へ。u = (local / worldSize + 0.5) * scale - bias
bool TexelSpan(float lo, float hi, float worldSize, float scale, float bias, uint32_t n,
               uint32_t& i0, uint32_t& i1)
{
    const float a = (lo / worldSize + 0.5f) * scale - bias;
    const float b = (hi / worldSize + 0.5f) * scale - bias;
    const float fMax = static_cast<float>(n - 1);
    if (b < 0.0f || a > fMax) {
        return false; // ブラシが地形の外
    }
    i0 = static_cast<uint32_t>(std::clamp(std::floor(a), 0.0f, fMax));
    i1 = static_cast<uint32_t>(std::clamp(std::ceil(b), 0.0f, fMax));
    return true;
}

bool HeightRect(const TerrainAsset::TerrainData& d, const Brush& b, uint32_t& x0, uint32_t& x1,
                uint32_t& z0, uint32_t& z1)
{
    const float sx = static_cast<float>(d.heightW - 1);
    const float sz = static_cast<float>(d.heightH - 1);
    return TexelSpan(b.centerX - b.radius, b.centerX + b.radius, d.worldSizeX, sx, 0.0f, d.heightW,
                     x0, x1)
        && TexelSpan(b.centerZ - b.radius, b.centerZ + b.radius, d.worldSizeZ, sz, 0.0f, d.heightH,
                     z0, z1);
}

bool SplatRect(const TerrainAsset::TerrainData& d, const Brush& b, uint32_t& x0, uint32_t& x1,
               uint32_t& z0, uint32_t& z1)
{
    const float sx = static_cast<float>(d.splatW);
    const float sz = static_cast<float>(d.splatH);
    return TexelSpan(b.centerX - b.radius, b.centerX + b.radius, d.worldSizeX, sx, 0.5f, d.splatW,
                     x0, x1)
        && TexelSpan(b.centerZ - b.radius, b.centerZ + b.radius, d.worldSizeZ, sz, 0.5f, d.splatH,
                     z0, z1);
}

bool ApplyRaise(TerrainAsset::TerrainData& d, const Brush& b)
{
    if (!(d.heightScale > 0.0f)) {
        return false; // 正規化高さへ換算できない (高さ 0 の板地形)
    }
    uint32_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if (!HeightRect(d, b, x0, x1, z0, z1)) {
        return false;
    }
    const float delta = b.strength / d.heightScale; // 正規化高さの増分
    bool changed = false;
    for (uint32_t z = z0; z <= z1; ++z) {
        const float lz = HeightTexelZ(d, z);
        for (uint32_t x = x0; x <= x1; ++x) {
            const float lx = HeightTexelX(d, x);
            const float dx = lx - b.centerX;
            const float dz = lz - b.centerZ;
            const float w = Falloff(std::sqrt(dx * dx + dz * dz), b.radius);
            if (!(w > 0.0f)) {
                continue;
            }
            uint16_t& h = d.heights[static_cast<size_t>(z) * d.heightW + x];
            const uint16_t nv = ToU16(static_cast<float>(h) * (1.0f / 65535.0f) + delta * w);
            if (nv != h) {
                h = nv;
                changed = true;
            }
        }
    }
    return changed;
}

bool ApplySmooth(TerrainAsset::TerrainData& d, const Brush& b)
{
    uint32_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if (!HeightRect(d, b, x0, x1, z0, z1)) {
        return false;
    }
    const float amount = std::clamp(b.strength, 0.0f, 1.0f);
    if (!(amount > 0.0f)) {
        return false;
    }
    // ★近傍平均は**書き込む前の高さ**から取る。書きながら読むと走査順が結果に効いてしまい、
    //   「同じブラシを同じ地形に当てたら同じバイト列」という決定論が崩れる。
    //   矩形を 1 texel ぶん外へ広げたコピーを作り、参照は必ずそちらから
    const uint32_t px0 = (x0 > 0) ? x0 - 1 : 0;
    const uint32_t pz0 = (z0 > 0) ? z0 - 1 : 0;
    const uint32_t px1 = std::min(x1 + 1, d.heightW - 1);
    const uint32_t pz1 = std::min(z1 + 1, d.heightH - 1);
    const uint32_t pw = px1 - px0 + 1;
    std::vector<uint16_t> src(static_cast<size_t>(pw) * (pz1 - pz0 + 1));
    for (uint32_t z = pz0; z <= pz1; ++z) {
        std::memcpy(&src[static_cast<size_t>(z - pz0) * pw],
                    &d.heights[static_cast<size_t>(z) * d.heightW + px0], pw * sizeof(uint16_t));
    }
    auto at = [&](int32_t x, int32_t z) {
        const uint32_t cx = static_cast<uint32_t>(
            std::clamp(x, static_cast<int32_t>(px0), static_cast<int32_t>(px1)));
        const uint32_t cz = static_cast<uint32_t>(
            std::clamp(z, static_cast<int32_t>(pz0), static_cast<int32_t>(pz1)));
        return static_cast<float>(src[static_cast<size_t>(cz - pz0) * pw + (cx - px0)]);
    };

    bool changed = false;
    for (uint32_t z = z0; z <= z1; ++z) {
        const float lz = HeightTexelZ(d, z);
        for (uint32_t x = x0; x <= x1; ++x) {
            const float lx = HeightTexelX(d, x);
            const float dx = lx - b.centerX;
            const float dz = lz - b.centerZ;
            const float w = Falloff(std::sqrt(dx * dx + dz * dz), b.radius);
            if (!(w > 0.0f)) {
                continue;
            }
            const int32_t ix = static_cast<int32_t>(x);
            const int32_t iz = static_cast<int32_t>(z);
            float sum = 0.0f;
            for (int32_t oz = -1; oz <= 1; ++oz) {
                for (int32_t ox = -1; ox <= 1; ++ox) {
                    sum += at(ix + ox, iz + oz);
                }
            }
            const float avg = sum * (1.0f / 9.0f);
            uint16_t& h = d.heights[static_cast<size_t>(z) * d.heightW + x];
            const float cur = at(ix, iz);
            const uint16_t nv = ToU16((cur + (avg - cur) * w * amount) * (1.0f / 65535.0f));
            if (nv != h) {
                h = nv;
                changed = true;
            }
        }
    }
    return changed;
}

bool ApplyPaint(TerrainAsset::TerrainData& d, const Brush& b)
{
    if (b.layer >= TerrainAsset::kMaxLayers) {
        return false;
    }
    uint32_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if (!SplatRect(d, b, x0, x1, z0, z1)) {
        return false;
    }
    const float amount = std::clamp(b.strength, 0.0f, 1.0f);
    if (!(amount > 0.0f)) {
        return false;
    }
    bool changed = false;
    for (uint32_t z = z0; z <= z1; ++z) {
        const float lz = SplatTexelZ(d, z);
        for (uint32_t x = x0; x <= x1; ++x) {
            const float lx = SplatTexelX(d, x);
            const float dx = lx - b.centerX;
            const float dz = lz - b.centerZ;
            const float w = Falloff(std::sqrt(dx * dx + dz * dz), b.radius);
            if (!(w > 0.0f)) {
                continue;
            }
            uint8_t* px = &d.splat[(static_cast<size_t>(z) * d.splatW + x) * 4];
            const float a = w * amount;
            float f[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t i = 0; i < 4; ++i) {
                const float target =
                    (i == b.layer) ? static_cast<float>(TerrainAsset::kSplatWeightSum) : 0.0f;
                f[i] = static_cast<float>(px[i]) * (1.0f - a) + target * a;
            }
            uint8_t out[4] = { 0, 0, 0, 0 };
            // ★量子化正規化を通す = 「1 texel の重み合計 = kSplatWeightSum」を崩さない。
            //   ここを自前で丸めるとシェーダ側の再正規化 (M58d) が効く前に色が痩せる
            TerrainAsset::QuantizeSplatWeights(f, out);
            if (std::memcmp(px, out, 4) != 0) {
                std::memcpy(px, out, 4);
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace

// ==== ブラシ ====

bool ApplyBrush(TerrainAsset::TerrainData& d, const Brush& b)
{
    if (!d.Valid() || !(b.radius > 0.0f) || !std::isfinite(b.centerX) || !std::isfinite(b.centerZ)
        || !std::isfinite(b.strength)) {
        return false;
    }
    switch (b.mode) {
    case BrushMode::Raise:
        return ApplyRaise(d, b);
    case BrushMode::Smooth:
        return ApplySmooth(d, b);
    case BrushMode::Paint:
        return ApplyPaint(d, b);
    default:
        return false;
    }
}

// ==== パッチ ====

bool MakeDiffPatch(const TerrainAsset::TerrainData& before, const TerrainAsset::TerrainData& after,
                   TerrainPatch& out)
{
    out = TerrainPatch{};
    if (!before.Valid() || !after.Valid()) {
        return false;
    }
    if (before.heightW != after.heightW || before.heightH != after.heightH
        || before.splatW != after.splatW || before.splatH != after.splatH) {
        return false; // 解像度が変わった = パッチでは表せない (ブラシの外で起きた変更)
    }

    uint32_t x0 = UINT32_MAX, x1 = 0, z0 = UINT32_MAX, z1 = 0;
    for (uint32_t z = 0; z < before.heightH; ++z) {
        for (uint32_t x = 0; x < before.heightW; ++x) {
            const size_t i = static_cast<size_t>(z) * before.heightW + x;
            if (before.heights[i] == after.heights[i]) {
                continue;
            }
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            z0 = std::min(z0, z);
            z1 = std::max(z1, z);
        }
    }
    if (x0 != UINT32_MAX) {
        out.hx0 = x0;
        out.hz0 = z0;
        out.hw = x1 - x0 + 1;
        out.hh = z1 - z0 + 1;
        out.heightBefore.resize(static_cast<size_t>(out.hw) * out.hh);
        out.heightAfter.resize(out.heightBefore.size());
        for (uint32_t z = 0; z < out.hh; ++z) {
            const size_t srcRow = static_cast<size_t>(z0 + z) * before.heightW + x0;
            const size_t dstRow = static_cast<size_t>(z) * out.hw;
            std::memcpy(&out.heightBefore[dstRow], &before.heights[srcRow],
                        out.hw * sizeof(uint16_t));
            std::memcpy(&out.heightAfter[dstRow], &after.heights[srcRow],
                        out.hw * sizeof(uint16_t));
        }
    }

    x0 = UINT32_MAX;
    x1 = 0;
    z0 = UINT32_MAX;
    z1 = 0;
    for (uint32_t z = 0; z < before.splatH; ++z) {
        for (uint32_t x = 0; x < before.splatW; ++x) {
            const size_t i = (static_cast<size_t>(z) * before.splatW + x) * 4;
            if (std::memcmp(&before.splat[i], &after.splat[i], 4) == 0) {
                continue;
            }
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            z0 = std::min(z0, z);
            z1 = std::max(z1, z);
        }
    }
    if (x0 != UINT32_MAX) {
        out.sx0 = x0;
        out.sz0 = z0;
        out.sw = x1 - x0 + 1;
        out.sh = z1 - z0 + 1;
        out.splatBefore.resize(static_cast<size_t>(out.sw) * out.sh * 4);
        out.splatAfter.resize(out.splatBefore.size());
        for (uint32_t z = 0; z < out.sh; ++z) {
            const size_t srcRow = (static_cast<size_t>(z0 + z) * before.splatW + x0) * 4;
            const size_t dstRow = static_cast<size_t>(z) * out.sw * 4;
            std::memcpy(&out.splatBefore[dstRow], &before.splat[srcRow], out.sw * 4);
            std::memcpy(&out.splatAfter[dstRow], &after.splat[srcRow], out.sw * 4);
        }
    }
    return true;
}

bool ApplyPatch(TerrainAsset::TerrainData& d, const TerrainPatch& p, bool redo)
{
    if (!d.Valid()) {
        return false;
    }
    // ★**先に全部検算してから書く。** 途中で弾くと「半分だけ巻き戻った地形」が残り、
    //   その状態から次の Undo を当てると 2 度と元に戻せない
    const size_t hCount = static_cast<size_t>(p.hw) * p.hh;
    if (p.hw != 0 || p.hh != 0) {
        if (p.hw == 0 || p.hh == 0 || p.hx0 + p.hw > d.heightW || p.hz0 + p.hh > d.heightH
            || p.heightBefore.size() != hCount || p.heightAfter.size() != hCount) {
            return false;
        }
    }
    const size_t sCount = static_cast<size_t>(p.sw) * p.sh * 4;
    if (p.sw != 0 || p.sh != 0) {
        if (p.sw == 0 || p.sh == 0 || p.sx0 + p.sw > d.splatW || p.sz0 + p.sh > d.splatH
            || p.splatBefore.size() != sCount || p.splatAfter.size() != sCount) {
            return false;
        }
    }

    const std::vector<uint16_t>& hs = redo ? p.heightAfter : p.heightBefore;
    for (uint32_t z = 0; z < p.hh; ++z) {
        std::memcpy(&d.heights[static_cast<size_t>(p.hz0 + z) * d.heightW + p.hx0],
                    &hs[static_cast<size_t>(z) * p.hw], p.hw * sizeof(uint16_t));
    }
    const std::vector<uint8_t>& ss = redo ? p.splatAfter : p.splatBefore;
    for (uint32_t z = 0; z < p.sh; ++z) {
        std::memcpy(&d.splat[(static_cast<size_t>(p.sz0 + z) * d.splatW + p.sx0) * 4],
                    &ss[static_cast<size_t>(z) * p.sw * 4], p.sw * 4);
    }
    return true;
}

void SerializePatch(const TerrainPatch& p, std::string& out)
{
    out.clear();
    AppendPod(out, kPatchMagic);
    AppendPod(out, kPatchVersion);
    AppendPod(out, p.hx0);
    AppendPod(out, p.hz0);
    AppendPod(out, p.hw);
    AppendPod(out, p.hh);
    Append(out, p.heightBefore.data(), p.heightBefore.size() * sizeof(uint16_t));
    Append(out, p.heightAfter.data(), p.heightAfter.size() * sizeof(uint16_t));
    AppendPod(out, p.sx0);
    AppendPod(out, p.sz0);
    AppendPod(out, p.sw);
    AppendPod(out, p.sh);
    Append(out, p.splatBefore.data(), p.splatBefore.size());
    Append(out, p.splatAfter.data(), p.splatAfter.size());
}

bool DeserializePatch(const std::string& in, TerrainPatch& out)
{
    out = TerrainPatch{};
    Reader r{ reinterpret_cast<const uint8_t*>(in.data()), in.size(), 0 };
    uint32_t magic = 0, version = 0;
    if (!r.Pod(magic) || !r.Pod(version) || magic != kPatchMagic || version != kPatchVersion) {
        return false;
    }
    if (!r.Pod(out.hx0) || !r.Pod(out.hz0) || !r.Pod(out.hw) || !r.Pod(out.hh)) {
        return false;
    }
    // 要素数は必ず「残量」で検算してから resize する (破損入力の巨大な値で bad_alloc に
    // ならないように — TerrainAsset の Reader::Count と同じ理由)
    const uint64_t hCount = static_cast<uint64_t>(out.hw) * out.hh;
    if (hCount * sizeof(uint16_t) * 2 > r.size - r.pos) {
        return false;
    }
    out.heightBefore.resize(static_cast<size_t>(hCount));
    out.heightAfter.resize(static_cast<size_t>(hCount));
    if (!r.Bytes(out.heightBefore.data(), static_cast<size_t>(hCount) * sizeof(uint16_t))
        || !r.Bytes(out.heightAfter.data(), static_cast<size_t>(hCount) * sizeof(uint16_t))) {
        return false;
    }
    if (!r.Pod(out.sx0) || !r.Pod(out.sz0) || !r.Pod(out.sw) || !r.Pod(out.sh)) {
        return false;
    }
    const uint64_t sCount = static_cast<uint64_t>(out.sw) * out.sh * 4;
    if (sCount * 2 > r.size - r.pos) {
        return false;
    }
    out.splatBefore.resize(static_cast<size_t>(sCount));
    out.splatAfter.resize(static_cast<size_t>(sCount));
    if (!r.Bytes(out.splatBefore.data(), static_cast<size_t>(sCount))
        || !r.Bytes(out.splatAfter.data(), static_cast<size_t>(sCount))) {
        return false;
    }
    if (r.pos != r.size) {
        return false; // 余りがある = 別形式/継ぎ足し
    }
    return true;
}

// ==== サイドカー ====

std::wstring EditPathFor(const std::wstring& srcPath)
{
    if (!TerrainAsset::IsSourcePath(srcPath)) {
        return {};
    }
    // IsSourcePath が `.terrain.json` (大文字小文字無視) を保証しているので、
    // 末尾 5 文字 (`.json`) だけを差し替える
    return srcPath.substr(0, srcPath.size() - 5) + L".edit";
}

void SerializeSidecar(const TerrainAsset::TerrainData& d, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(32 + d.heights.size() * 2 + d.splat.size());
    AppendPod(out, kEditMagic);
    AppendPod(out, kEditVersion);
    AppendPod(out, d.heightW);
    AppendPod(out, d.heightH);
    AppendPod(out, d.splatW);
    AppendPod(out, d.splatH);
    Append(out, d.heights.data(), d.heights.size() * sizeof(uint16_t));
    Append(out, d.splat.data(), d.splat.size());
}

bool ApplySidecarBlob(const std::vector<uint8_t>& blob, TerrainAsset::TerrainData& d)
{
    Reader r{ blob.data(), blob.size(), 0 };
    uint32_t magic = 0, version = 0, hw = 0, hh = 0, sw = 0, sh = 0;
    if (!r.Pod(magic) || !r.Pod(version) || magic != kEditMagic || version != kEditVersion) {
        return false;
    }
    if (!r.Pod(hw) || !r.Pod(hh) || !r.Pod(sw) || !r.Pod(sh)) {
        return false;
    }
    // **解像度が一致するときだけ受理する** — JSON の heightRes を変えたら古いブラシ結果は
    // 自動的に無効になる (寸法違いを引き伸ばして拾うと、地形が静かに別物になる)
    if (hw != d.heightW || hh != d.heightH || sw != d.splatW || sh != d.splatH) {
        return false;
    }
    std::vector<uint16_t> heights(static_cast<size_t>(hw) * hh);
    std::vector<uint8_t> splat(static_cast<size_t>(sw) * sh * 4);
    if (!r.Bytes(heights.data(), heights.size() * sizeof(uint16_t))
        || !r.Bytes(splat.data(), splat.size()) || r.pos != r.size) {
        return false;
    }
    d.heights = std::move(heights);
    d.splat = std::move(splat);
    return true;
}

bool SaveEdits(const std::wstring& srcPath, TerrainAsset::TerrainData& d)
{
    const std::wstring editPath = EditPathFor(srcPath);
    if (editPath.empty() || !d.Valid()) {
        return false;
    }
    std::vector<uint8_t> blob;
    SerializeSidecar(d, blob);
    {
        std::ofstream f(fs::path{ editPath }, std::ios::binary | std::ios::trunc);
        if (!f) {
            MYE_LOG_ERROR("[terrain] cannot write edit sidecar: %s",
                          WideToUtf8(editPath).c_str());
            return false;
        }
        f.write(reinterpret_cast<const char*>(blob.data()),
                static_cast<std::streamsize>(blob.size()));
        if (!f) {
            return false;
        }
    }
    // 刻印は**いま書いたバイト列**から作る (読み直すと、書いた直後に誰かが触った場合に
    // 「キャッシュだけが未来を指す」状態になる)
    d.editSrc.relPath = WideToUtf8(fs::path(editPath).filename().wstring());
    d.editSrc.byteSize = blob.size();
    d.editSrc.contentHash = HashBytes(blob.data(), blob.size());
    TerrainAsset::WriteCache(srcPath, d);
    return true;
}

// ==== 高さ場の問い合わせ (エディタ専用) ====

float SampleHeightLocal(const TerrainAsset::TerrainData& d, float lx, float lz)
{
    if (!d.Valid()) {
        return d.heightBase;
    }
    const float fx = std::clamp(lx / d.worldSizeX + 0.5f, 0.0f, 1.0f)
        * static_cast<float>(d.heightW - 1);
    const float fz = std::clamp(lz / d.worldSizeZ + 0.5f, 0.0f, 1.0f)
        * static_cast<float>(d.heightH - 1);
    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t z0 = static_cast<uint32_t>(fz);
    const uint32_t x1 = std::min(x0 + 1, d.heightW - 1);
    const uint32_t z1 = std::min(z0 + 1, d.heightH - 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    const float h00 = d.HeightAtTexel(x0, z0);
    const float h10 = d.HeightAtTexel(x1, z0);
    const float h01 = d.HeightAtTexel(x0, z1);
    const float h11 = d.HeightAtTexel(x1, z1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

bool RaycastLocal(const TerrainAsset::TerrainData& d, const XMFLOAT3& origin, const XMFLOAT3& dir,
                  float maxDist, XMFLOAT3& outHit)
{
    if (!d.Valid() || !(maxDist > 0.0f)) {
        return false;
    }
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 1e-6f)) {
        return false;
    }
    const XMFLOAT3 nd{ dir.x / len, dir.y / len, dir.z / len };
    const float halfX = d.worldSizeX * 0.5f;
    const float halfZ = d.worldSizeZ * 0.5f;
    auto inside = [&](float x, float z) {
        return x >= -halfX && x <= halfX && z >= -halfZ && z <= halfZ;
    };
    auto diffAt = [&](float t) {
        const float px = origin.x + nd.x * t;
        const float pz = origin.z + nd.z * t;
        return (origin.y + nd.y * t) - SampleHeightLocal(d, px, pz);
    };

    // 1 タイルぶんずつ進めて符号の反転を探す。地形の XZ 範囲の外は「判定しない」—
    // SampleHeightLocal は範囲外を端の値へクランプするので、そのまま見ると地形の
    // 延長平面に当たってしまう
    const float step = std::max(0.05f, std::min(d.worldSizeX / static_cast<float>(d.heightW - 1),
                                                d.worldSizeZ / static_cast<float>(d.heightH - 1)));
    bool havePrev = false;
    float prevT = 0.0f;
    float prevDiff = 0.0f;
    for (float t = 0.0f; t <= maxDist; t += step) {
        const float px = origin.x + nd.x * t;
        const float pz = origin.z + nd.z * t;
        if (!inside(px, pz)) {
            havePrev = false;
            continue;
        }
        const float diff = diffAt(t);
        if (havePrev && prevDiff > 0.0f && diff <= 0.0f) {
            float lo = prevT;
            float hi = t;
            for (int i = 0; i < 24; ++i) { // 1 タイルを 2^-24 まで詰める
                const float mid = (lo + hi) * 0.5f;
                if (diffAt(mid) > 0.0f) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            outHit = XMFLOAT3{ origin.x + nd.x * hi, origin.y + nd.y * hi, origin.z + nd.z * hi };
            return true;
        }
        havePrev = true;
        prevT = t;
        prevDiff = diff;
    }
    return false;
}

} // namespace TerrainEdit
} // namespace mye
