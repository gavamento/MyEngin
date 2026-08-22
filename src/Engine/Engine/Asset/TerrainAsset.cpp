#include "Engine/Engine/Asset/TerrainAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Asset/TerrainEdit.h" // M58f: ブラシ編集サイドカーの取り込み
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"
#include "stb/stb_image.h"

namespace fs = std::filesystem;

namespace mye {
namespace TerrainAsset {
namespace {

using nlohmann::json;

constexpr uint32_t kMagic = 0x52525450u; // "PTRR" (LE) — .mterr payload の先頭

// ---- blob ライタ (ModelCook.cpp と同じ流儀: 生バイト保存で float のビットを保つ) ----
void Append(std::vector<uint8_t>& buf, const void* src, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), b, b + n);
}
template <typename T> void AppendPod(std::vector<uint8_t>& buf, const T& v)
{
    Append(buf, &v, sizeof(T));
}
void AppendStr(std::vector<uint8_t>& buf, const std::string& s)
{
    AppendPod(buf, static_cast<uint32_t>(s.size()));
    Append(buf, s.data(), s.size());
}

// 境界検査つきリーダ。長さは必ず「残量」で検算してから resize する —
// 破損 blob の巨大な要素数をそのまま信じると bad_alloc で即死する
// (ModelCook が実際にそれで落ちた。selftest が同じ穴を突く)
struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool Bytes(void* dst, size_t n)
    {
        if (n > size - pos) {
            return false;
        }
        std::memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool Pod(T& v) { return Bytes(&v, sizeof(T)); }
    bool Str(std::string& s)
    {
        uint32_t len = 0;
        if (!Pod(len) || len > size - pos) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return true;
    }
    // 要素数を読み、「1 要素の最小サイズ x 個数 <= 残量」で検算する
    bool Count(uint64_t& n, size_t minElemBytes)
    {
        if (!Pod(n) || (minElemBytes != 0 && n > (size - pos) / minElemBytes)) {
            return false;
        }
        return true;
    }
};

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

std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// `.terrain.json` からの相対パスを絶対パスへ。空文字列は空のまま
std::wstring ResolveRel(const std::wstring& srcPath, const std::string& rel)
{
    if (rel.empty()) {
        return {};
    }
    std::error_code ec;
    const fs::path p = fs::path(srcPath).parent_path() / fs::path(Utf8ToWide(rel));
    const fs::path abs = fs::weakly_canonical(p, ec);
    return ec ? p.wstring() : abs.wstring();
}

float ReadFloat(const json& j, const char* key, float def)
{
    if (!j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

uint32_t ReadUint(const json& j, const char* key, uint32_t def)
{
    if (!j.contains(key) || !j[key].is_number_unsigned()) {
        return def;
    }
    return j[key].get<uint32_t>();
}

std::string ReadStr(const json& j, const char* key)
{
    if (!j.contains(key) || !j[key].is_string()) {
        return {};
    }
    return j[key].get<std::string>();
}

// [a, b] 形式の 2 要素配列。無ければ既定値のまま
void ReadPair2(const json& j, const char* key, float& a, float& b)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 2 || !j[key][0].is_number()
        || !j[key][1].is_number()) {
        return;
    }
    a = j[key][0].get<float>();
    b = j[key][1].get<float>();
}

// [r, g, b] 形式の 3 要素配列 (レイヤの tint)。無ければ既定値のまま
void ReadTriple(const json& j, const char* key, float& a, float& b, float& c)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3 || !j[key][0].is_number()
        || !j[key][1].is_number() || !j[key][2].is_number()) {
        return;
    }
    a = j[key][0].get<float>();
    b = j[key][1].get<float>();
    c = j[key][2].get<float>();
}

void ReadPair2u(const json& j, const char* key, uint32_t& a, uint32_t& b)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 2
        || !j[key][0].is_number_unsigned() || !j[key][1].is_number_unsigned()) {
        return;
    }
    a = j[key][0].get<uint32_t>();
    b = j[key][1].get<uint32_t>();
}

// ---- 手続き生成 (値ノイズ + fBm) ----
// 整数ハッシュ (FNV-1a) だけで格子値を作るので `rand()` も実時間も混ざらない。
// 補間は smoothstep。/fp:precise 固定なので Debug / Release / WARP でビット一致する
float LatticeValue(int32_t x, int32_t y, uint32_t seed)
{
    uint64_t h = HashCombine(static_cast<uint64_t>(seed), static_cast<uint64_t>(static_cast<uint32_t>(x)));
    h = HashCombine(h, static_cast<uint64_t>(static_cast<uint32_t>(y)));
    // 上位 24bit を [0,1) へ。2^24 までは float が整数を厳密に表現できる = 丸めが入らない
    return static_cast<float>((h >> 40) & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

float SmoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

float ValueNoise(float x, float y, uint32_t seed)
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int32_t ix = static_cast<int32_t>(fx);
    const int32_t iy = static_cast<int32_t>(fy);
    const float tx = SmoothStep(x - fx);
    const float ty = SmoothStep(y - fy);
    const float v00 = LatticeValue(ix, iy, seed);
    const float v10 = LatticeValue(ix + 1, iy, seed);
    const float v01 = LatticeValue(ix, iy + 1, seed);
    const float v11 = LatticeValue(ix + 1, iy + 1, seed);
    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

float Fbm(float x, float y, const TerrainProcedural& p)
{
    float amp = 1.0f;
    float freq = p.frequency;
    float sum = 0.0f;
    float norm = 0.0f;
    const uint32_t octaves = std::clamp<uint32_t>(p.octaves, 1u, 8u);
    for (uint32_t o = 0; o < octaves; ++o) {
        sum += ValueNoise(x * freq, y * freq, p.seed + o * 1013u) * amp;
        norm += amp;
        amp *= p.gain;
        freq *= p.lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

uint16_t ToU16(float normalized)
{
    const float v = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<uint16_t>(v * 65535.0f + 0.5f);
}

void GenerateHeights(TerrainData& d)
{
    d.heights.assign(static_cast<size_t>(d.heightW) * d.heightH, 0);
    for (uint32_t z = 0; z < d.heightH; ++z) {
        for (uint32_t x = 0; x < d.heightW; ++x) {
            const float u = d.heightW > 1 ? static_cast<float>(x) / static_cast<float>(d.heightW - 1) : 0.0f;
            const float v = d.heightH > 1 ? static_cast<float>(z) / static_cast<float>(d.heightH - 1) : 0.0f;
            d.heights[static_cast<size_t>(z) * d.heightW + x] = ToU16(Fbm(u, v, d.proc));
        }
    }
}

// 高さ帯からレイヤ重みを作る (手続きスプラットの既定)。M58d の本番ブレンドはシェーダ側だが、
// アセット側の不変量「1 テクセルの重み合計 = kSplatWeightSum」はここで必ず成立させる
void GenerateSplat(TerrainData& d)
{
    d.splat.assign(static_cast<size_t>(d.splatW) * d.splatH * 4, 0);
    const uint32_t layerCount = std::max<uint32_t>(1, static_cast<uint32_t>(d.layers.size()));
    for (uint32_t z = 0; z < d.splatH; ++z) {
        for (uint32_t x = 0; x < d.splatW; ++x) {
            const float u = d.splatW > 1 ? static_cast<float>(x) / static_cast<float>(d.splatW - 1) : 0.0f;
            const float v = d.splatH > 1 ? static_cast<float>(z) / static_cast<float>(d.splatH - 1) : 0.0f;
            const float h = Fbm(u, v, d.proc); // ハイトマップと同じ場 = 高い所ほど岩/雪
            float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (uint32_t i = 0; i < layerCount && i < kMaxLayers; ++i) {
                // レイヤ i の中心を帯の中央に置き、三角形の窓で重ねる (隣接帯だけが混ざる)
                const float center = (static_cast<float>(i) + 0.5f) / static_cast<float>(layerCount);
                const float band = 1.0f / static_cast<float>(layerCount);
                const float t = std::fabs(h - center) / band;
                w[i] = std::max(0.0f, 1.0f - t);
            }
            QuantizeSplatWeights(w, &d.splat[(static_cast<size_t>(z) * d.splatW + x) * 4]);
        }
    }
}

// ---- ソース画像の取り込み ----
// R16 は BCn 非対応なので DDS クック (TextureCook::CookImageToDds) は経由しない。
// stbi_load_16 は 8bit 画像も v*257 で 16bit へ持ち上げてくれるので、素材が 8bit PNG でも通る
bool LoadHeightImage(const std::wstring& path, uint32_t& w, uint32_t& h, std::vector<uint16_t>& out)
{
    const std::string utf8 = WideToUtf8(path);
    const std::string lower = LowerAscii(utf8);
    if (EndsWith(lower, ".r16") || EndsWith(lower, ".raw")) {
        // 生の 16bit ハイトマップ (World Machine 等の標準的な吐き出し)。
        // 解像度はファイルに書いていないので、JSON の heightRes を正とし、サイズで検算する
        std::vector<uint8_t> bytes;
        if (!ReadWholeFile(path, bytes)) {
            return false;
        }
        const size_t need = static_cast<size_t>(w) * h * 2;
        if (w == 0 || h == 0 || bytes.size() != need) {
            MYE_LOG_ERROR("[terrain] raw heightmap size mismatch: %s (%zu bytes, expected %zu)",
                          utf8.c_str(), bytes.size(), need);
            return false;
        }
        out.resize(static_cast<size_t>(w) * h);
        std::memcpy(out.data(), bytes.data(), need); // x64 固定 = LE のまま
        return true;
    }
    int iw = 0, ih = 0, comp = 0;
    stbi_us* pixels = stbi_load_16(utf8.c_str(), &iw, &ih, &comp, 1);
    if (pixels == nullptr) {
        MYE_LOG_ERROR("[terrain] heightmap decode failed: %s", utf8.c_str());
        return false;
    }
    if (iw <= 0 || ih <= 0 || static_cast<uint32_t>(iw) > kMaxResolution
        || static_cast<uint32_t>(ih) > kMaxResolution) {
        stbi_image_free(pixels);
        MYE_LOG_ERROR("[terrain] heightmap resolution out of range: %s (%dx%d)", utf8.c_str(), iw, ih);
        return false;
    }
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
    out.assign(pixels, pixels + static_cast<size_t>(w) * h);
    stbi_image_free(pixels);
    return true;
}

bool LoadSplatImage(const std::wstring& path, uint32_t& w, uint32_t& h, std::vector<uint8_t>& out)
{
    const std::string utf8 = WideToUtf8(path);
    int iw = 0, ih = 0, comp = 0;
    stbi_uc* pixels = stbi_load(utf8.c_str(), &iw, &ih, &comp, 4);
    if (pixels == nullptr) {
        MYE_LOG_ERROR("[terrain] splatmap decode failed: %s", utf8.c_str());
        return false;
    }
    if (iw <= 0 || ih <= 0 || static_cast<uint32_t>(iw) > kMaxResolution
        || static_cast<uint32_t>(ih) > kMaxResolution) {
        stbi_image_free(pixels);
        MYE_LOG_ERROR("[terrain] splatmap resolution out of range: %s (%dx%d)", utf8.c_str(), iw, ih);
        return false;
    }
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
    out.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    // 素材の RGBA がそのまま合計 255 とは限らないので、アセット側の不変量に合わせて正規化する
    for (size_t i = 0; i < out.size(); i += 4) {
        const float w4[4] = { static_cast<float>(out[i]), static_cast<float>(out[i + 1]),
                              static_cast<float>(out[i + 2]), static_cast<float>(out[i + 3]) };
        QuantizeSplatWeights(w4, &out[i]);
    }
    return true;
}

// 取り込んだ画像の同一性 (size + 内容ハッシュ) を記録する
bool StampSource(const std::wstring& absPath, const std::string& rel, TerrainSourceImage& out)
{
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(absPath, bytes)) {
        return false;
    }
    out.relPath = rel;
    out.byteSize = bytes.size();
    out.contentHash = HashBytes(bytes.data(), bytes.size());
    return true;
}

// 編集サイドカー (M58f) をクック結果へ被せる。サイドカーが在るかどうかに関わらず
// 「在れば刻印する」— 解像度違いで**中身を捨てた**ときも刻印は残す。残さないと
// 「サイドカーは在るのに blob は無いと言っている」= 下の EditSidecarStillMatches が
// 毎回 miss を返し、開くたびにフルクックし続ける状態になる
void ApplyEditSidecar(const std::wstring& srcPath, TerrainData& d)
{
    const std::wstring editPath = TerrainEdit::EditPathFor(srcPath);
    std::vector<uint8_t> blob;
    if (editPath.empty() || !ReadWholeFile(editPath, blob)) {
        return;
    }
    if (!TerrainEdit::ApplySidecarBlob(blob, d)) {
        MYE_LOG_WARN("[terrain] edit sidecar ignored (resolution mismatch): %s",
                     WideToUtf8(editPath).c_str());
    }
    d.editSrc.relPath = WideToUtf8(fs::path(editPath).filename().wstring());
    d.editSrc.byteSize = blob.size();
    d.editSrc.contentHash = HashBytes(blob.data(), blob.size());
}

// Load() のキャッシュ照合。焼き込んだ画像が差し替わっていたら miss にする
bool SourceImageStillMatches(const std::wstring& srcPath, const TerrainSourceImage& s)
{
    if (s.relPath.empty()) {
        return true; // 手続き生成 = 外部入力なし
    }
    if (CookedCache::Sealed()) {
        return true; // 配布物には元画像が無い (M51j の封印キャッシュと同じ理由)
    }
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(ResolveRel(srcPath, s.relPath), bytes)) {
        return false;
    }
    return bytes.size() == s.byteSize && HashBytes(bytes.data(), bytes.size()) == s.contentHash;
}

// 編集サイドカー用のキャッシュ照合 (M58f)。
// ★SourceImageStillMatches だけでは足りない: relPath が空だと「外部入力なし」で素通りするので、
//   **無かったサイドカーが増えた**ケース (= 初めてブラシを当てた地形をよそのプロセスが開く)
//   をキャッシュが隠してしまう。ディスク側の存在と blob の主張が食い違ったら miss にする
bool EditSidecarStillMatches(const std::wstring& srcPath, const TerrainData& cached)
{
    if (CookedCache::Sealed()) {
        return true; // 配布物にサイドカーは無い (焼き込み済み)
    }
    const std::wstring editPath = TerrainEdit::EditPathFor(srcPath);
    std::error_code ec;
    const bool present = !editPath.empty() && fs::exists(editPath, ec);
    if (present != !cached.editSrc.relPath.empty()) {
        return false;
    }
    return SourceImageStillMatches(srcPath, cached.editSrc);
}

} // namespace

// ==== TerrainData ====

bool TerrainData::Valid() const
{
    if (heightW < 2 || heightH < 2 || heightW > kMaxResolution || heightH > kMaxResolution) {
        return false;
    }
    if (splatW < 1 || splatH < 1 || splatW > kMaxResolution || splatH > kMaxResolution) {
        return false;
    }
    if (heights.size() != static_cast<size_t>(heightW) * heightH) {
        return false;
    }
    if (splat.size() != static_cast<size_t>(splatW) * splatH * 4) {
        return false;
    }
    if (layers.size() > kMaxLayers) {
        return false;
    }
    if (!(worldSizeX > 0.0f) || !(worldSizeZ > 0.0f)) {
        return false;
    }
    // NaN / Inf を弾く (JSON の手編集で入りうる。頂点生成が丸ごと壊れる)
    if (!std::isfinite(heightBase) || !std::isfinite(heightScale)) {
        return false;
    }
    return true;
}

float TerrainData::HeightAtTexel(uint32_t x, uint32_t z) const
{
    if (heights.empty()) {
        return heightBase;
    }
    const uint32_t cx = std::min(x, heightW - 1);
    const uint32_t cz = std::min(z, heightH - 1);
    const float n = static_cast<float>(heights[static_cast<size_t>(cz) * heightW + cx])
        * (1.0f / 65535.0f);
    return heightBase + n * heightScale;
}

// ==== 量子化正規化 ====

void QuantizeSplatWeights(const float weights[4], uint8_t out[4])
{
    float sum = 0.0f;
    float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 4; ++i) {
        w[i] = (std::isfinite(weights[i]) && weights[i] > 0.0f) ? weights[i] : 0.0f;
        sum += w[i];
    }
    if (!(sum > 0.0f)) {
        // 全部ゼロ (未塗り) は先頭レイヤ 100% に倒す — 合計 0 のテクセルを許すと
        // シェーダ側が 0 除算するか黒く抜けるかのどちらかになる
        out[0] = static_cast<uint8_t>(kSplatWeightSum);
        out[1] = out[2] = out[3] = 0;
        return;
    }
    // 最大剰余法: 切り捨て → 余りを剰余の大きい順に 1 ずつ配る。
    // 同点はレイヤ番号の若い順 (安定 = 決定論。浮動小数の比較順に依存させない)
    uint32_t base[4] = { 0, 0, 0, 0 };
    float frac[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t used = 0;
    for (int i = 0; i < 4; ++i) {
        const float scaled = w[i] / sum * static_cast<float>(kSplatWeightSum);
        const float f = std::floor(scaled);
        base[i] = static_cast<uint32_t>(f);
        frac[i] = scaled - f;
        used += base[i];
    }
    uint32_t remain = (used < kSplatWeightSum) ? kSplatWeightSum - used : 0;
    while (remain > 0) {
        int best = -1;
        for (int i = 0; i < 4; ++i) {
            if (base[i] >= kSplatWeightSum) {
                continue;
            }
            if (best < 0 || frac[i] > frac[best]) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        base[best] += 1;
        frac[best] = -1.0f; // 同じレイヤへ 2 度配らない
        --remain;
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(std::min<uint32_t>(base[i], 255));
    }
}

// ==== 直列化 ====

void Serialize(const TerrainData& d, std::vector<uint8_t>& out)
{
    out.clear();
    out.reserve(64 + d.heights.size() * 2 + d.splat.size());
    AppendPod(out, kMagic);
    AppendPod(out, kBlobVersion);
    AppendPod(out, d.heightW);
    AppendPod(out, d.heightH);
    AppendPod(out, d.splatW);
    AppendPod(out, d.splatH);
    AppendPod(out, d.worldSizeX);
    AppendPod(out, d.worldSizeZ);
    AppendPod(out, d.heightBase);
    AppendPod(out, d.heightScale);
    for (const TerrainSourceImage* s : { &d.heightSrc, &d.splatSrc, &d.editSrc }) {
        AppendStr(out, s->relPath);
        AppendPod(out, s->byteSize);
        AppendPod(out, s->contentHash);
    }
    AppendPod(out, d.proc.seed);
    AppendPod(out, d.proc.octaves);
    AppendPod(out, d.proc.frequency);
    AppendPod(out, d.proc.lacunarity);
    AppendPod(out, d.proc.gain);
    AppendPod(out, static_cast<uint32_t>(d.layers.size()));
    for (const TerrainLayer& l : d.layers) {
        AppendStr(out, l.name);
        AppendStr(out, l.albedo);
        AppendStr(out, l.normal);
        AppendPod(out, l.tilingU);
        AppendPod(out, l.tilingV);
        AppendPod(out, l.tintR); // v2 で末尾 append (M58d)
        AppendPod(out, l.tintG);
        AppendPod(out, l.tintB);
    }
    AppendPod(out, static_cast<uint64_t>(d.heights.size()));
    Append(out, d.heights.data(), d.heights.size() * sizeof(uint16_t));
    AppendPod(out, static_cast<uint64_t>(d.splat.size()));
    Append(out, d.splat.data(), d.splat.size());
}

bool Deserialize(const std::vector<uint8_t>& in, TerrainData& out)
{
    out = TerrainData{};
    Reader r{ in.data(), in.size(), 0 };
    uint32_t magic = 0, version = 0;
    if (!r.Pod(magic) || !r.Pod(version) || magic != kMagic || version != kBlobVersion) {
        return false;
    }
    if (!r.Pod(out.heightW) || !r.Pod(out.heightH) || !r.Pod(out.splatW) || !r.Pod(out.splatH)
        || !r.Pod(out.worldSizeX) || !r.Pod(out.worldSizeZ) || !r.Pod(out.heightBase)
        || !r.Pod(out.heightScale)) {
        return false;
    }
    for (TerrainSourceImage* s : { &out.heightSrc, &out.splatSrc, &out.editSrc }) {
        if (!r.Str(s->relPath) || !r.Pod(s->byteSize) || !r.Pod(s->contentHash)) {
            return false;
        }
    }
    if (!r.Pod(out.proc.seed) || !r.Pod(out.proc.octaves) || !r.Pod(out.proc.frequency)
        || !r.Pod(out.proc.lacunarity) || !r.Pod(out.proc.gain)) {
        return false;
    }
    uint32_t layerCount = 0;
    if (!r.Pod(layerCount) || layerCount > kMaxLayers) {
        return false;
    }
    out.layers.resize(layerCount);
    for (TerrainLayer& l : out.layers) {
        if (!r.Str(l.name) || !r.Str(l.albedo) || !r.Str(l.normal) || !r.Pod(l.tilingU)
            || !r.Pod(l.tilingV) || !r.Pod(l.tintR) || !r.Pod(l.tintG) || !r.Pod(l.tintB)) {
            return false;
        }
    }
    uint64_t heightCount = 0;
    if (!r.Count(heightCount, sizeof(uint16_t))) {
        return false;
    }
    out.heights.resize(static_cast<size_t>(heightCount));
    if (!r.Bytes(out.heights.data(), static_cast<size_t>(heightCount) * sizeof(uint16_t))) {
        return false;
    }
    uint64_t splatCount = 0;
    if (!r.Count(splatCount, 1)) {
        return false;
    }
    out.splat.resize(static_cast<size_t>(splatCount));
    if (!r.Bytes(out.splat.data(), static_cast<size_t>(splatCount))) {
        return false;
    }
    if (r.pos != r.size) {
        return false; // 余りがある = 別形式/継ぎ足し。丸ごと捨てる
    }
    if (!out.Valid()) {
        out = TerrainData{};
        return false;
    }
    return true;
}

// ==== クック ====

bool IsSourcePath(const std::wstring& path)
{
    const std::string s = LowerAscii(WideToUtf8(path));
    return EndsWith(s, ".terrain.json");
}

std::wstring ResolveLayerPath(const std::wstring& srcPath, const std::string& rel)
{
    return ResolveRel(srcPath, rel);
}

bool CookFromSource(const std::wstring& srcPath, TerrainData& out)
{
    out = TerrainData{};
    std::ifstream f(fs::path{ srcPath });
    if (!f) {
        MYE_LOG_ERROR("[terrain] cannot open source: %s", WideToUtf8(srcPath).c_str());
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        MYE_LOG_ERROR("[terrain] json parse failed: %s (%s)", WideToUtf8(srcPath).c_str(), e.what());
        return false;
    }
    if (!j.is_object()) {
        return false;
    }

    TerrainData d;
    d.heightW = 129;
    d.heightH = 129;
    d.splatW = 128;
    d.splatH = 128;
    d.worldSizeX = 256.0f;
    d.worldSizeZ = 256.0f;
    d.heightBase = 0.0f;
    d.heightScale = 32.0f;
    ReadPair2u(j, "heightRes", d.heightW, d.heightH);
    ReadPair2u(j, "splatRes", d.splatW, d.splatH);
    ReadPair2(j, "worldSize", d.worldSizeX, d.worldSizeZ);
    d.heightBase = ReadFloat(j, "heightBase", d.heightBase);
    d.heightScale = ReadFloat(j, "heightScale", d.heightScale);
    if (j.contains("procedural") && j["procedural"].is_object()) {
        const json& p = j["procedural"];
        d.proc.seed = ReadUint(p, "seed", d.proc.seed);
        d.proc.octaves = ReadUint(p, "octaves", d.proc.octaves);
        d.proc.frequency = ReadFloat(p, "frequency", d.proc.frequency);
        d.proc.lacunarity = ReadFloat(p, "lacunarity", d.proc.lacunarity);
        d.proc.gain = ReadFloat(p, "gain", d.proc.gain);
    }
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const json& lj : j["layers"]) {
            if (d.layers.size() >= kMaxLayers) {
                MYE_LOG_WARN("[terrain] more than %u layers, extra ones ignored: %s", kMaxLayers,
                             WideToUtf8(srcPath).c_str());
                break;
            }
            if (!lj.is_object()) {
                continue;
            }
            TerrainLayer l;
            l.name = ReadStr(lj, "name");
            l.albedo = ReadStr(lj, "albedo");
            l.normal = ReadStr(lj, "normal");
            ReadPair2(lj, "tiling", l.tilingU, l.tilingV);
            ReadTriple(lj, "tint", l.tintR, l.tintG, l.tintB); // M58d (既定 = 白 = 恒等)
            d.layers.push_back(std::move(l));
        }
    }

    const std::string heightRel = ReadStr(j, "heightmap");
    if (!heightRel.empty()) {
        const std::wstring abs = ResolveRel(srcPath, heightRel);
        if (!LoadHeightImage(abs, d.heightW, d.heightH, d.heights)
            || !StampSource(abs, heightRel, d.heightSrc)) {
            return false;
        }
    } else {
        GenerateHeights(d);
    }

    const std::string splatRel = ReadStr(j, "splatmap");
    if (!splatRel.empty()) {
        const std::wstring abs = ResolveRel(srcPath, splatRel);
        if (!LoadSplatImage(abs, d.splatW, d.splatH, d.splat)
            || !StampSource(abs, splatRel, d.splatSrc)) {
            return false;
        }
    } else {
        GenerateSplat(d);
    }

    // ★サイドカーはレシピを解いた**後**に被せる。JSON は「地形の作り方」、サイドカーは
    //   「ブラシが直した画素」なので、解像度が一致する限り後者が勝つ (M58f)
    ApplyEditSidecar(srcPath, d);

    if (!d.Valid()) {
        MYE_LOG_ERROR("[terrain] cooked data failed validation: %s", WideToUtf8(srcPath).c_str());
        return false;
    }
    out = std::move(d);
    return true;
}

bool WriteCache(const std::wstring& srcPath, const TerrainData& d)
{
    if (!CookedCache::Enabled()) {
        return false;
    }
    std::vector<uint8_t> blob;
    Serialize(d, blob);
    // deps は「存在検証」だけなので、焼き込んだ画像/サイドカーの**内容**は
    // TerrainSourceImage 側で見る。それでも deps に載せるのは、消えた/動いたを
    // 1 回のファイル読みで即 miss にできるため
    std::vector<std::wstring> deps;
    for (const TerrainSourceImage* s : { &d.heightSrc, &d.splatSrc, &d.editSrc }) {
        if (!s->relPath.empty()) {
            deps.push_back(ResolveRel(srcPath, s->relPath));
        }
    }
    return CookedCache::Write(srcPath, kTerrainExt, blob.data(), blob.size(), deps);
}

bool Load(const std::wstring& srcPath, TerrainData& out)
{
    out = TerrainData{};
    std::vector<uint8_t> payload;
    if (CookedCache::ReadValidated(srcPath, kTerrainExt, payload)) {
        TerrainData cached;
        if (Deserialize(payload, cached) && SourceImageStillMatches(srcPath, cached.heightSrc)
            && SourceImageStillMatches(srcPath, cached.splatSrc)
            && EditSidecarStillMatches(srcPath, cached)) {
            MYE_LOG_INFO("[cook] terrain cache hit: %s", WideToUtf8(srcPath).c_str());
            out = std::move(cached);
            return true;
        }
        MYE_LOG_INFO("[cook] terrain cache stale, recooking: %s", WideToUtf8(srcPath).c_str());
    }
    if (!CookFromSource(srcPath, out)) {
        return false;
    }
    if (WriteCache(srcPath, out)) {
        MYE_LOG_INFO("[cook] terrain cooked: %s", WideToUtf8(srcPath).c_str());
    }
    return true;
}

} // namespace TerrainAsset
} // namespace mye
