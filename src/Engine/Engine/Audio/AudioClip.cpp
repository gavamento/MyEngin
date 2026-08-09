#include "Engine/Engine/Audio/AudioClip.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Audio/StbVorbis.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// WAVE の formatTag (mmreg.h と同値。<Windows.h> を引かずに済ませるため自前定義)
constexpr uint16_t kFmtPcm = 0x0001;
constexpr uint16_t kFmtIeeeFloat = 0x0003;
constexpr uint16_t kFmtExtensible = 0xFFFE;

constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

uint32_t Rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Rd16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

// [-1, 1] の実数を 16bit へ (最近傍丸め + 飽和)
int16_t FloatToI16(double v)
{
    const double s = v * 32767.0;
    const double r = s >= 0.0 ? s + 0.5 : s - 0.5;
    if (r >= 32767.0) {
        return 32767;
    }
    if (r <= -32768.0) {
        return -32768;
    }
    return static_cast<int16_t>(r);
}

// 1 サンプル (1 チャンネルぶん) を 16bit へ正規化する
int16_t ConvertSample(const uint8_t* p, uint16_t formatTag, uint16_t bits)
{
    if (formatTag == kFmtIeeeFloat) {
        if (bits == 32) {
            float f = 0.0f;
            std::memcpy(&f, p, sizeof(f));
            return FloatToI16(static_cast<double>(f));
        }
        double d = 0.0;
        std::memcpy(&d, p, sizeof(d));
        return FloatToI16(d);
    }
    switch (bits) {
    case 8:
        // 8bit WAVE は **符号なし** (128 が無音)
        return static_cast<int16_t>((static_cast<int32_t>(p[0]) - 128) * 256);
    case 16:
        return static_cast<int16_t>(Rd16(p));
    case 24: {
        // 24bit LE を符号拡張してから上位 16bit を採る
        int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                         (static_cast<uint32_t>(p[1]) << 8) |
                                         (static_cast<uint32_t>(p[2]) << 16));
        if ((v & 0x00800000) != 0) {
            v |= static_cast<int32_t>(0xFF000000u);
        }
        return static_cast<int16_t>(v >> 8);
    }
    case 32: {
        const int32_t v = static_cast<int32_t>(Rd32(p));
        return static_cast<int16_t>(v >> 16);
    }
    default:
        return 0;
    }
}

} // namespace

bool ParseWavHeader(const uint8_t* bytes, size_t len, WavInfo& out)
{
    out = WavInfo{};
    if (bytes == nullptr || len < 44) {
        return false;
    }
    if (Rd32(bytes) != FourCC('R', 'I', 'F', 'F') ||
        Rd32(bytes + 8) != FourCC('W', 'A', 'V', 'E')) {
        return false;
    }

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bits = 0;
    size_t dataOffset = 0;
    size_t dataBytes = 0;
    uint64_t dataDeclared = 0;
    bool haveFmt = false;
    bool haveData = false;

    // チャンク走査。サイズは信用せず必ず len でクランプする (壊れたファイルで溢れないこと)
    size_t off = 12;
    while (off + 8 <= len) {
        const uint32_t id = Rd32(bytes + off);
        const uint32_t csz = Rd32(bytes + off + 4);
        const size_t body = off + 8;
        const size_t avail = len - body;
        const size_t bodySize = (csz <= avail) ? csz : avail; // 0xFFFFFFFF なストリーム wav 対策

        if (id == FourCC('f', 'm', 't', ' ') && bodySize >= 16) {
            formatTag = Rd16(bytes + body);
            channels = Rd16(bytes + body + 2);
            sampleRate = Rd32(bytes + body + 4);
            bits = Rd16(bytes + body + 14);
            if (formatTag == kFmtExtensible && bodySize >= 40) {
                // cbSize(+16) validBits(+18) channelMask(+20) SubFormat GUID(+24)。
                // GUID 先頭 2 バイト (Data1 の下位ワード) が実フォーマット
                formatTag = Rd16(bytes + body + 24);
            }
            haveFmt = true;
        } else if (id == FourCC('d', 'a', 't', 'a')) {
            dataOffset = body;
            dataBytes = bodySize;
            dataDeclared = csz;
            haveData = true;
        }

        // チャンクはワード境界。加算前にオーバーフロー/後退を弾く
        const size_t advance = bodySize + (bodySize & 1u);
        if (advance == 0 && csz != 0) {
            break;
        }
        const size_t next = body + advance;
        if (next <= off) {
            break;
        }
        off = next;
    }

    if (!haveFmt || !haveData || channels == 0 || sampleRate == 0) {
        return false;
    }
    if (formatTag != kFmtPcm && formatTag != kFmtIeeeFloat) {
        return false; // ADPCM / xWMA 等は非対応
    }
    if (formatTag == kFmtPcm && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        return false;
    }
    if (formatTag == kFmtIeeeFloat && bits != 32 && bits != 64) {
        return false;
    }

    out.formatTag = formatTag;
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.bits = bits;
    out.dataOffset = dataOffset;
    out.dataBytes = dataBytes;
    out.dataBytesDeclared = dataDeclared;
    return true;
}

void ConvertWavSamples(const uint8_t* src, size_t count, uint16_t formatTag, uint16_t bits,
                       int16_t* dst)
{
    if (src == nullptr || dst == nullptr || count == 0) {
        return;
    }
    if (formatTag == kFmtPcm && bits == 16) {
        // 最頻ケースは変換不要 (バイト列がそのまま int16 LE)。x64 は LE なので memcpy でよい
        std::memcpy(dst, src, count * sizeof(int16_t));
        return;
    }
    const size_t bytesPerSample = bits / 8u;
    for (size_t i = 0; i < count; ++i) {
        dst[i] = ConvertSample(src + i * bytesPerSample, formatTag, bits);
    }
}

bool DecodeWav(const uint8_t* bytes, size_t len, AudioClip& out)
{
    out = AudioClip{};
    WavInfo info;
    if (!ParseWavHeader(bytes, len, info)) {
        return false;
    }
    size_t totalSamples = info.dataBytes / info.BytesPerSample();
    totalSamples -= totalSamples % info.channels; // 半端なフレームは捨てる
    if (totalSamples == 0) {
        return false;
    }

    out.samples.resize(totalSamples);
    out.channels = info.channels;
    out.sampleRate = info.sampleRate;
    ConvertWavSamples(bytes + info.dataOffset, totalSamples, info.formatTag, info.bits,
                      out.samples.data());
    return true;
}

bool DecodeOgg(const uint8_t* bytes, size_t len, AudioClip& out)
{
    out = AudioClip{};
    if (bytes == nullptr || len < 4 || len > static_cast<size_t>(INT32_MAX)) {
        return false;
    }
    int channels = 0;
    int sampleRate = 0;
    short* pcm = nullptr;
    // 全展開 (SE 用)。ストリーミングは M45f で open_memory + アリーナ経路を別に作る
    const int frames = stb_vorbis_decode_memory(bytes, static_cast<int>(len), &channels,
                                                &sampleRate, &pcm);
    if (frames <= 0 || pcm == nullptr || channels <= 0 || sampleRate <= 0) {
        if (pcm != nullptr) {
            std::free(pcm);
        }
        return false;
    }
    const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    out.samples.resize(total);
    std::memcpy(out.samples.data(), pcm, total * sizeof(int16_t));
    out.channels = static_cast<uint16_t>(channels);
    out.sampleRate = static_cast<uint32_t>(sampleRate);
    std::free(pcm); // stb_vorbis は malloc で返す (同一 CRT なので free でよい)
    return true;
}

bool DecodeAudio(const uint8_t* bytes, size_t len, AudioClip& out)
{
    out = AudioClip{};
    if (bytes == nullptr || len < 4) {
        return false;
    }
    if (Rd32(bytes) == FourCC('R', 'I', 'F', 'F')) {
        return DecodeWav(bytes, len, out);
    }
    if (Rd32(bytes) == FourCC('O', 'g', 'g', 'S')) {
        return DecodeOgg(bytes, len, out);
    }
    return false;
}

namespace {

// .mpcm の payload: sampleRate(u32) + channels(u16) + 予約(u16) + サンプル数(u64) + i16 列
bool HasOggExt(const std::wstring& path)
{
    if (path.size() < 4) {
        return false;
    }
    const std::wstring ext = path.substr(path.size() - 4);
    return ext == L".ogg" || ext == L".OGG" || ext == L".Ogg";
}

} // namespace

bool LoadCookedClip(const std::wstring& srcPath, AudioClip& out)
{
    std::vector<uint8_t> payload;
    if (!CookedCache::ReadValidated(srcPath, L".mpcm", payload)) {
        return false;
    }
    if (payload.size() < 16) {
        return false;
    }
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint64_t count = 0;
    memcpy(&rate, payload.data(), 4);
    memcpy(&channels, payload.data() + 4, 2);
    memcpy(&count, payload.data() + 8, 8);
    // count は残量から逆算した値と厳密一致でなければ破損 (巨大 count の乗算オーバーフロー対策)
    const uint64_t expected = (payload.size() - 16) / sizeof(int16_t);
    if (count != expected || (payload.size() - 16) % sizeof(int16_t) != 0 || channels == 0
        || rate == 0) {
        MYE_LOG_WARN("[cook] corrupt pcm blob, recooking: %s", WideToUtf8(srcPath).c_str());
        return false;
    }
    out = AudioClip{};
    out.sampleRate = rate;
    out.channels = channels;
    out.samples.resize(static_cast<size_t>(count));
    memcpy(out.samples.data(), payload.data() + 16, out.samples.size() * sizeof(int16_t));
    MYE_LOG_INFO("[cook] pcm cache hit: %s", WideToUtf8(srcPath).c_str());
    return true;
}

void SaveCookedClip(const std::wstring& srcPath, const AudioClip& clip)
{
    if (!CookedCache::Enabled() || clip.Empty()) {
        return;
    }
    std::vector<uint8_t> payload(16 + clip.samples.size() * sizeof(int16_t));
    const uint16_t reserved = 0;
    const uint64_t count = clip.samples.size();
    memcpy(payload.data(), &clip.sampleRate, 4);
    memcpy(payload.data() + 4, &clip.channels, 2);
    memcpy(payload.data() + 6, &reserved, 2);
    memcpy(payload.data() + 8, &count, 8);
    memcpy(payload.data() + 16, clip.samples.data(), clip.samples.size() * sizeof(int16_t));
    if (CookedCache::Write(srcPath, L".mpcm", payload.data(), payload.size())) {
        MYE_LOG_INFO("[cook] pcm cooked: %s (%zu KB)", WideToUtf8(srcPath).c_str(),
                     payload.size() / 1024);
    }
}

bool LoadAudioFile(const std::wstring& path, AudioClip& out)
{
    out = AudioClip{};
    // .ogg はデコード結果をクックしてある — ヒットすればファイル読みも Vorbis デコードも省略。
    // .wav は変換がほぼ memcpy なので対象外 (spec §10)
    const bool ogg = HasOggExt(path);
    if (ogg && LoadCookedClip(path, out)) {
        return true;
    }
    std::error_code ec;
    const std::filesystem::path fsPath{ path };
    const auto size = std::filesystem::file_size(fsPath, ec);
    if (ec || size == 0) {
        MYE_LOG_WARN("[audio] open failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    std::ifstream f(fsPath, std::ios::binary);
    if (!f) {
        MYE_LOG_WARN("[audio] open failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (f.gcount() != static_cast<std::streamsize>(bytes.size())) {
        MYE_LOG_WARN("[audio] read failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    if (!DecodeAudio(bytes.data(), bytes.size(), out)) {
        MYE_LOG_WARN("[audio] unsupported audio file: %s", WideToUtf8(path).c_str());
        return false;
    }
    if (ogg) {
        SaveCookedClip(path, out);
    }
    return true;
}

} // namespace mye
