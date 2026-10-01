//====================================================================================
//                          Provenance.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          出自情報 (SimProvenance) の算出と照合の実装
//====================================================================================
#include "Engine/Engine/Session/Provenance.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Replay/Replay.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/PathUtil.h"
#include "Shared/EngineAPI.h"

// ビルド時に埋め込まれる git 記述子 (build\Common.props の MyeBuildInfo。CrashHandler.cpp と同じ生成物)
#if __has_include("MyeBuildInfo.h")
#include "MyeBuildInfo.h"
#endif
#ifndef MYE_GIT_HASH
#define MYE_GIT_HASH "unknown"
#endif

namespace fs = std::filesystem;

namespace mye {
namespace {

constexpr size_t kHashChunkBytes = 1u << 20;
constexpr std::string_view kUnknownGit = "unknown";
constexpr std::string_view kDirtySuffix = "-dirty";
// schema::WriteCSharpBindings が起動のたびに書き直す出力先 (正規化パス)。Server は書かないので、
// 対象に入れると「Runtime を一度起動したかどうか」で contentHash が割れる
constexpr std::string_view kGeneratedScriptsDir = "scripts/generated/";

// 描画・音声専用の資産。sim はこれらを読まない (画像は材質経由の描画、音声は出力レーン、
// シェーダとフォント実体は GPU / UI の見た目)。.meta は GUID を持つので対象に残す
constexpr const wchar_t* kExcludedExtensions[] = {
    L".png", L".jpg", L".jpeg", L".tga", L".dds", L".hdr", L".bmp", // 画像
    L".hlsl", L".hlsli", L".surface", L".cso",                       // シェーダ
    L".wav", L".ogg", L".mp3", L".flac",                             // 音声
    L".ttf", L".otf",                                                // フォント実体
};

// 8 ビット ASCII だけを小文字にする。ロケール依存の towlower は使わない (全環境で同じ並びにする)
std::string AsciiLower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::wstring AsciiLowerW(std::wstring_view s)
{
    std::wstring out(s);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return out;
}

// ファイルを chunk ずつ読んで FNV-1a を畳む。size は読めたバイト数
bool HashFile(const fs::path& path, uint64_t& outHash, uint64_t& outSize)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<char> buf(kHashChunkBytes);
    uint64_t h = kFnvOffset;
    uint64_t total = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            h = HashBytes(buf.data(), static_cast<size_t>(got), h);
            total += static_cast<uint64_t>(got);
        }
    }
    if (in.bad()) {
        return false;
    }
    outHash = h;
    outSize = total;
    return true;
}

std::string Hex64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool ParseHex64(const nlohmann::json& j, uint64_t& out)
{
    if (!j.is_string()) {
        return false;
    }
    const std::string s = j.get<std::string>();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    if (end == s.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

} // namespace

std::string_view EngineBuildGit()
{
    return MYE_GIT_HASH;
}

uint64_t EngineVersionFromGit(std::string_view git)
{
    if (git.empty() || git == kUnknownGit) {
        return 0;
    }
    const uint64_t h = HashStr(git);
    return h == 0 ? 1ull : h; // 0 は「不明」の予約値
}

bool IsDirtyGit(std::string_view git)
{
    return git.size() >= kDirtySuffix.size()
        && git.substr(git.size() - kDirtySuffix.size()) == kDirtySuffix;
}

uint64_t HashFileBytes(const std::wstring& path)
{
    uint64_t h = 0;
    uint64_t size = 0;
    if (path.empty() || !HashFile(fs::path(path), h, size)) {
        return 0;
    }
    return h == 0 ? 1ull : h;
}

uint32_t ComputeSchemaVersion()
{
    uint64_t h = HashStr("mye.schema.v1");
    h = HashCombine(h, kSimSnapshotVersion);
    h = HashCombine(h, sizeof(InputSnapshot));
    h = HashCombine(h, kReplayFileVersion);
    return static_cast<uint32_t>(h ^ (h >> 32));
}

bool IsContentExcludedExtension(std::wstring_view ext)
{
    const std::wstring lower = AsciiLowerW(ext);
    for (const wchar_t* e : kExcludedExtensions) {
        if (lower == e) {
            return true;
        }
    }
    return false;
}

bool CollectContentEntries(const std::wstring& assetsRoot, std::vector<ContentEntry>& out)
{
    out.clear();
    const fs::path root(assetsRoot);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        MYE_LOG_ERROR("[content] assets root not found: %s", WideToUtf8(assetsRoot).c_str());
        return false;
    }
    bool ok = true;
    const std::string manifestKey = AsciiLower(WideToUtf8(kContentManifestName));
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const fs::path& p = it->path();
        if (IsContentExcludedExtension(p.extension().wstring())) {
            continue;
        }
        // 除外する種類の .meta (x.png.meta など) も同じ扱い。DDS 一括クックや初回起動で
        // 新しい .dds.meta が増えても contentHash が動かないようにする (GUID を持つ他の .meta は対象)
        if (AsciiLowerW(p.extension().wstring()) == L".meta"
            && IsContentExcludedExtension(fs::path(p).replace_extension().extension().wstring())) {
            continue;
        }
        ContentEntry e;
        e.path = AsciiLower(WideToUtf8(p.lexically_relative(root).generic_wstring()));
        if (e.path == manifestKey || e.path == manifestKey + ".meta") {
            continue; // manifest 自身は対象に入れない (自己参照になる)。AssetDatabase が付ける .meta も同様
        }
        if (e.path.compare(0, kGeneratedScriptsDir.size(), kGeneratedScriptsDir) == 0) {
            continue; // 起動時にエンジンが書き出す派生物 (スキーマ由来の C# 糖衣)。元の schema は対象に入っている
        }
        if (!HashFile(p, e.hash, e.size)) {
            MYE_LOG_ERROR("[content] cannot read %s", WideToUtf8(p.wstring()).c_str());
            ok = false;
            continue;
        }
        out.push_back(std::move(e));
    }
    if (ec) {
        MYE_LOG_ERROR("[content] directory walk failed under %s: %s", WideToUtf8(assetsRoot).c_str(),
                      ec.message().c_str());
        ok = false;
    }
    // 走査順はファイルシステム依存なので、正規化パスのバイト順に整列し直す (決定的な並び)
    std::sort(out.begin(), out.end(), [](const ContentEntry& a, const ContentEntry& b) {
        if (a.path != b.path) {
            return a.path < b.path;
        }
        if (a.size != b.size) {
            return a.size < b.size;
        }
        return a.hash < b.hash;
    });
    return ok;
}

uint64_t FoldContentEntries(const std::vector<ContentEntry>& entries)
{
    uint64_t h = HashStr("mye.content.v1");
    for (const ContentEntry& e : entries) {
        h = HashCombine(h, e.path.size());
        h = HashBytes(e.path.data(), e.path.size(), h);
        h = HashCombine(h, e.size);
        h = HashCombine(h, e.hash);
    }
    return h == 0 ? 1ull : h;
}

bool WriteContentManifest(const std::wstring& manifestPath, const std::wstring& assetsRoot,
                          uint64_t* outHash)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ContentEntry> entries;
    if (!CollectContentEntries(assetsRoot, entries)) {
        return false;
    }
    const uint64_t total = FoldContentEntries(entries);
    nlohmann::json files = nlohmann::json::array();
    for (const ContentEntry& e : entries) {
        files.push_back({ { "path", e.path }, { "size", e.size }, { "hash", Hex64(e.hash) } });
    }
    const nlohmann::json doc = {
        { "version", 1 },
        { "contentHash", Hex64(total) },
        { "fileCount", entries.size() },
        { "files", files },
    };
    std::error_code ec;
    const fs::path out(manifestPath);
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
    }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        MYE_LOG_ERROR("[content] cannot write %s", WideToUtf8(manifestPath).c_str());
        return false;
    }
    f << doc.dump(1, '\t') << '\n';
    f.flush();
    if (!f) {
        MYE_LOG_ERROR("[content] write failed: %s", WideToUtf8(manifestPath).c_str());
        return false;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MYE_LOG_INFO("[content] manifest written: %s (%zu files, hash %s, %.1f ms)",
                 WideToUtf8(manifestPath).c_str(), entries.size(), Hex64(total).c_str(), ms);
    if (outHash != nullptr) {
        *outHash = total;
    }
    return true;
}

bool ReadContentManifest(const std::wstring& manifestPath, uint64_t& outHash, size_t* outFileCount)
{
    std::ifstream f{ fs::path(manifestPath), std::ios::binary };
    if (!f) {
        return false;
    }
    const nlohmann::json doc = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
    uint64_t recorded = 0;
    if (!doc.is_object() || !doc.contains("files") || !doc["files"].is_array()
        || !doc.contains("contentHash") || !ParseHex64(doc["contentHash"], recorded)) {
        MYE_LOG_ERROR("[content] malformed manifest: %s", WideToUtf8(manifestPath).c_str());
        return false;
    }
    std::vector<ContentEntry> entries;
    for (const nlohmann::json& j : doc["files"]) {
        ContentEntry e;
        if (!j.is_object() || !j.contains("path") || !j["path"].is_string() || !j.contains("size")
            || !j["size"].is_number_unsigned() || !j.contains("hash") || !ParseHex64(j["hash"], e.hash)) {
            MYE_LOG_ERROR("[content] malformed manifest entry: %s", WideToUtf8(manifestPath).c_str());
            return false;
        }
        e.path = j["path"].get<std::string>();
        e.size = j["size"].get<uint64_t>();
        entries.push_back(std::move(e));
    }
    // 手で編集された manifest を黙って信用しない: エントリから畳み直した値と記録値を突き合わせる
    // (配布物の中身との照合まではしない。改ざん検知は対象外)
    if (FoldContentEntries(entries) != recorded) {
        MYE_LOG_ERROR("[content] manifest is inconsistent (entries do not fold to contentHash): %s",
                      WideToUtf8(manifestPath).c_str());
        return false;
    }
    outHash = recorded;
    if (outFileCount != nullptr) {
        *outFileCount = entries.size();
    }
    return true;
}

uint64_t ResolveContentHash(const std::wstring& assetsRoot)
{
    const std::wstring manifest = assetsRoot + L"\\" + kContentManifestName;
    std::error_code ec;
    if (fs::exists(manifest, ec)) {
        uint64_t h = 0;
        size_t count = 0;
        if (ReadContentManifest(manifest, h, &count)) {
            MYE_LOG_INFO("[content] manifest: %zu files, hash %s", count, Hex64(h).c_str());
            return h;
        }
        MYE_LOG_WARN("[content] manifest unusable - computing from assets instead");
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ContentEntry> entries;
    if (!CollectContentEntries(assetsRoot, entries)) {
        return 0;
    }
    const uint64_t h = FoldContentEntries(entries);
    uint64_t bytes = 0;
    for (const ContentEntry& e : entries) {
        bytes += e.size;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MYE_LOG_INFO("[content] no manifest: hashed %zu files (%.1f MB) in %.1f ms, hash %s",
                 entries.size(), static_cast<double>(bytes) / (1024.0 * 1024.0), ms, Hex64(h).c_str());
    return h;
}

SimProvenance MakeSimProvenance(uint64_t dllHash, uint64_t contentHash, uint32_t protocolVersion)
{
    SimProvenance p = {};
    p.engineVersion = EngineVersionFromGit(EngineBuildGit());
    p.protocolVersion = protocolVersion;
    p.apiVersion = MYE_API_VERSION;
    p.schemaVersion = ComputeSchemaVersion();
    p.replayVersion = kReplayFileVersion;
    p.gameVersion = dllHash;
    p.contentHash = contentHash;
    p.initialSnapshotHash = 0;
    return p;
}

std::string FormatProvenance(const SimProvenance& p)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[provenance] engine=%s (%s) game=%s content=%s api=%u schema=%08x proto=%u rep=%u",
                  Hex64(p.engineVersion).c_str(), std::string(EngineBuildGit()).c_str(),
                  Hex64(p.gameVersion).c_str(), Hex64(p.contentHash).c_str(), p.apiVersion,
                  p.schemaVersion, p.protocolVersion, p.replayVersion);
    return buf;
}

const char* ProvenanceMismatchName(ProvenanceMismatch m)
{
    switch (m) {
    case ProvenanceMismatch::None: return "none";
    case ProvenanceMismatch::ProtocolVersion: return "protocol version";
    case ProvenanceMismatch::ApiVersion: return "MYE_API_VERSION";
    case ProvenanceMismatch::ReplayVersion: return "replay file version";
    case ProvenanceMismatch::SchemaVersion: return "sim schema (snapshot / input layout)";
    case ProvenanceMismatch::EngineVersion: return "engine build (git)";
    case ProvenanceMismatch::GameVersion: return "GameLogic.dll";
    case ProvenanceMismatch::ContentHash: return "assets content";
    case ProvenanceMismatch::InitialSnapshot: return "initial snapshot";
    }
    return "?";
}

ProvenanceMismatch CompareProvenance(const SimProvenance& a, const SimProvenance& b,
                                     bool allowGameMismatch)
{
    if (a.protocolVersion != b.protocolVersion) return ProvenanceMismatch::ProtocolVersion;
    if (a.apiVersion != b.apiVersion) return ProvenanceMismatch::ApiVersion;
    if (a.replayVersion != b.replayVersion) return ProvenanceMismatch::ReplayVersion;
    if (a.schemaVersion != b.schemaVersion) return ProvenanceMismatch::SchemaVersion;
    // 0 = 不明。双方が不明なら比べようが無いので WARN 付きで通し、片方だけ不明なら食い違い扱い
    if (a.engineVersion != b.engineVersion) return ProvenanceMismatch::EngineVersion;
    if (a.engineVersion == 0) {
        MYE_LOG_WARN("[provenance] engine build is unknown on both sides (git unavailable) - not compared");
    }
    if (a.gameVersion != b.gameVersion) {
        if (!allowGameMismatch) {
            return ProvenanceMismatch::GameVersion;
        }
        MYE_LOG_WARN("[provenance] GameLogic.dll differs (%s vs %s) - allowed by --allow-game-mismatch",
                     Hex64(a.gameVersion).c_str(), Hex64(b.gameVersion).c_str());
    }
    if (a.contentHash != b.contentHash) return ProvenanceMismatch::ContentHash;
    if (a.contentHash == 0) {
        MYE_LOG_WARN("[provenance] assets content hash is unknown on both sides - not compared");
    }
    if (a.initialSnapshotHash != b.initialSnapshotHash) return ProvenanceMismatch::InitialSnapshot;
    return ProvenanceMismatch::None;
}

} // namespace mye
