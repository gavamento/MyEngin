#include "Engine/Engine/Asset/CookedCache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::CookedCache {
namespace {

constexpr uint32_t kMagic = 0x4B43594Du; // "MYCK" (LE)
// ヘッダ固定部のオフセット (自己修復の in-place パッチが使う)
constexpr size_t kOffMtime = 24;
constexpr size_t kFixedHeaderSize = 40; // magic..srcContentHash

std::wstring g_dir;
bool g_enabled = false;
bool g_sealed = false; // M51j: 配布ビルドの封印キャッシュ (kSealedMarker の有無で決まる)

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
    if (!f) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.gcount() == static_cast<std::streamsize>(out.size());
}

// mtime は file_time_type の生カウントで比較する。時計の意味論には踏み込まない —
// 「前回見た値と同じか」だけが問題で、違っていてもコンテンツハッシュが最終判定する
bool StatSource(const std::wstring& path, uint64_t& sizeOut, int64_t& mtimeOut)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    sizeOut = static_cast<uint64_t>(size);
    mtimeOut = static_cast<int64_t>(t.time_since_epoch().count());
    return true;
}

// 境界検査つきリーダ (破損ファイルで落ちない)
struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool Bytes(void* dst, size_t n)
    {
        if (pos + n > size || pos + n < pos) {
            return false;
        }
        memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool Pod(T& v) { return Bytes(&v, sizeof(T)); }
    bool Utf8(std::string& s)
    {
        uint32_t len = 0;
        if (!Pod(len) || pos + len > size) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return true;
    }
};

void Append(std::vector<uint8_t>& buf, const void* src, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), b, b + n);
}
template <typename T> void AppendPod(std::vector<uint8_t>& buf, const T& v)
{
    Append(buf, &v, sizeof(T));
}
void AppendUtf8(std::vector<uint8_t>& buf, const std::string& s)
{
    AppendPod(buf, static_cast<uint32_t>(s.size()));
    Append(buf, s.data(), s.size());
}

} // namespace

void Configure(const std::wstring& cookedDir, bool enabled)
{
    g_dir = cookedDir;
    g_enabled = enabled && !cookedDir.empty();
    std::error_code ec;
    g_sealed = g_enabled
        && std::filesystem::exists(cookedDir + L"\\" + kSealedMarker, ec);
    if (g_sealed) {
        // 配布ビルド (BuildSettings のパッケージ段がマーカーを書く)。ソース検証を跳ばして
        // クック時の登録列をそのまま再生する — 詳細はヘッダの kSealedMarker コメント
        MYE_LOG_INFO("[cook] sealed bundle detected - replaying cooked registrations as-is");
    }
}

bool Enabled()
{
    return g_enabled;
}

bool Sealed()
{
    return g_sealed;
}

const std::wstring& Dir()
{
    return g_dir;
}

std::wstring PathFor(const std::wstring& srcPath, const wchar_t* ext)
{
    if (!g_enabled) {
        return {};
    }
    const uint64_t guid = assetkey::Resolve(NormalizePathKey(srcPath));
    if (guid == 0) {
        return {};
    }
    wchar_t name[64];
    swprintf_s(name, L"\\%016llx%s", static_cast<unsigned long long>(guid), ext);
    return g_dir + name;
}

bool ReadValidated(const std::wstring& srcPath, const wchar_t* ext,
                   std::vector<uint8_t>& payloadOut)
{
    if (!g_enabled) {
        return false;
    }
    const std::wstring cookPath = PathFor(srcPath, ext);
    if (cookPath.empty()) {
        return false;
    }
    std::vector<uint8_t> file;
    if (!ReadWholeFile(cookPath, file)) {
        return false; // キャッシュ無し = 普通のコールドスタート (ログ不要)
    }

    Reader r{ file.data(), file.size(), 0 };
    uint32_t magic = 0, version = 0;
    uint64_t guid = 0, srcSize = 0, srcHash = 0;
    int64_t srcMtime = 0;
    std::string pathKey;
    if (!r.Pod(magic) || !r.Pod(version) || !r.Pod(guid) || !r.Pod(srcSize) || !r.Pod(srcMtime)
        || !r.Pod(srcHash) || !r.Utf8(pathKey)) {
        MYE_LOG_WARN("[cook] corrupt header, recooking: %s", WideToUtf8(cookPath).c_str());
        return false;
    }
    if (magic != kMagic || version != kCookVersion
        || guid != assetkey::Resolve(NormalizePathKey(srcPath))) {
        return false; // 旧版/別アセット — 再クックで上書きされる
    }
    // M51j: 封印キャッシュ (配布ビルド) はソース検証を跳ばす。移設先では絶対パスが変わり
    // pathKey は必ず不一致になるが、配布シーンはクック元パス由来のサブアセット ID を
    // 参照しているので、クック時の登録列をそのまま再生するのが正しい (kSealedMarker 参照)
    if (!g_sealed) {
        if (pathKey != WideToUtf8(NormalizePathKey(srcPath))) {
            return false; // 移動/リネーム — フレッシュパースは別キーを登録するので追随する
        }

        uint64_t curSize = 0;
        int64_t curMtime = 0;
        if (!StatSource(srcPath, curSize, curMtime) || curSize != srcSize) {
            return false;
        }
        if (curMtime != srcMtime) {
            std::vector<uint8_t> src;
            if (!ReadWholeFile(srcPath, src) || HashBytes(src.data(), src.size()) != srcHash) {
                return false; // 内容が変わった — 再クック
            }
            // touch 等で mtime だけ動いた: ヘッダの mtime を自己修復して次回はハッシュ計算を省く
            std::fstream patch(std::filesystem::path{ cookPath },
                               std::ios::binary | std::ios::in | std::ios::out);
            if (patch) {
                patch.seekp(static_cast<std::streamoff>(kOffMtime));
                patch.write(reinterpret_cast<const char*>(&curMtime), sizeof(curMtime));
            }
        }
    }

    uint32_t depCount = 0;
    if (!r.Pod(depCount)) {
        return false;
    }
    for (uint32_t i = 0; i < depCount; ++i) {
        std::string depUtf8;
        if (!r.Utf8(depUtf8)) {
            return false;
        }
        std::error_code ec;
        if (!g_sealed && !std::filesystem::exists(Utf8ToWide(depUtf8), ec)) {
            MYE_LOG_INFO("[cook] dependency missing, recooking: %s (dep %s)",
                         WideToUtf8(srcPath).c_str(), depUtf8.c_str());
            return false; // 外部テクスチャが消えた/動いた — フレッシュ探索に任せる
        }
    }

    uint64_t payloadSize = 0;
    if (!r.Pod(payloadSize) || r.pos + payloadSize != file.size()) {
        MYE_LOG_WARN("[cook] truncated payload, recooking: %s", WideToUtf8(cookPath).c_str());
        return false;
    }
    payloadOut.assign(file.begin() + static_cast<ptrdiff_t>(r.pos), file.end());
    return true;
}

bool Write(const std::wstring& srcPath, const wchar_t* ext, const void* payload,
           size_t payloadSize, const std::vector<std::wstring>& deps)
{
    if (!g_enabled) {
        return false;
    }
    const std::wstring cookPath = PathFor(srcPath, ext);
    if (cookPath.empty()) {
        return false;
    }
    uint64_t srcSize = 0;
    int64_t srcMtime = 0;
    std::vector<uint8_t> src;
    if (!StatSource(srcPath, srcSize, srcMtime) || !ReadWholeFile(srcPath, src)) {
        return false;
    }

    std::vector<uint8_t> buf;
    buf.reserve(kFixedHeaderSize + payloadSize + 256);
    AppendPod(buf, kMagic);
    AppendPod(buf, kCookVersion);
    AppendPod(buf, assetkey::Resolve(NormalizePathKey(srcPath)));
    AppendPod(buf, srcSize);
    AppendPod(buf, srcMtime);
    AppendPod(buf, HashBytes(src.data(), src.size()));
    AppendUtf8(buf, WideToUtf8(NormalizePathKey(srcPath)));
    AppendPod(buf, static_cast<uint32_t>(deps.size()));
    for (const std::wstring& d : deps) {
        AppendUtf8(buf, WideToUtf8(d));
    }
    AppendPod(buf, static_cast<uint64_t>(payloadSize));
    Append(buf, payload, payloadSize);

    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    // 書きかけのファイルを次回起動が有効ヘッダと誤認しないよう、テンポラリ → rename にする
    const std::wstring tmpPath = cookPath + L".tmp";
    {
        std::ofstream f(std::filesystem::path{ tmpPath }, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (!f.good()) {
            return false;
        }
    }
    std::filesystem::rename(tmpPath, cookPath, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

} // namespace mye::CookedCache
