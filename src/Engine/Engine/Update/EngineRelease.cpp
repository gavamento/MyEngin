//====================================================================================
//                          EngineRelease.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          配布版の版番号・GitHub Release の解析と取得の実装
//====================================================================================
#include "Engine/Engine/Update/EngineRelease.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <Windows.h>

#include "nlohmann/json.hpp"

#include "Engine/Engine/App/Project.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

constexpr size_t kApiResponseLimit = 4 * 1024 * 1024;
constexpr size_t kSmallAssetLimit = 64 * 1024;
constexpr size_t kCopyChunkBytes = 1024 * 1024;
// プレリリースを含めるときに一覧から見る件数 (GitHub の 1 ページ)
constexpr int kReleaseListPageSize = 30;
constexpr const char* kLocalReleaseFile = "release.json";

// 数字の並びを数として比べる自然順 ("rc2" < "rc10")
int CompareNatural(std::string_view a, std::string_view b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            uint64_t na = 0;
            uint64_t nb = 0;
            while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) {
                na = na * 10 + static_cast<uint64_t>(a[i++] - '0');
            }
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) {
                nb = nb * 10 + static_cast<uint64_t>(b[j++] - '0');
            }
            if (na != nb) {
                return na < nb ? -1 : 1;
            }
            continue;
        }
        if (a[i] != b[j]) {
            return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]) ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) {
        return 1;
    }
    if (j < b.size()) {
        return -1;
    }
    return 0;
}

std::string JsonString(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool JsonBool(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() && it->get<bool>();
}

bool ParseOneRelease(const nlohmann::json& obj, ReleaseInfo& out)
{
    if (!obj.is_object()) {
        return false;
    }
    out = ReleaseInfo{};
    out.tag = JsonString(obj, "tag_name");
    if (!ParseEngineVersion(out.tag, out.version)) {
        return false;
    }
    out.title = JsonString(obj, "name");
    out.notes = JsonString(obj, "body");
    out.publishedAt = JsonString(obj, "published_at");
    out.prerelease = JsonBool(obj, "prerelease");
    out.draft = JsonBool(obj, "draft");
    const auto assets = obj.find("assets");
    if (assets != obj.end() && assets->is_array()) {
        for (const nlohmann::json& a : *assets) {
            if (!a.is_object()) {
                continue;
            }
            ReleaseAsset asset;
            asset.name = JsonString(a, "name");
            asset.url = JsonString(a, "browser_download_url");
            const auto size = a.find("size");
            if (size != a.end() && size->is_number_unsigned()) {
                asset.size = size->get<uint64_t>();
            }
            if (!asset.name.empty() && !asset.url.empty()) {
                out.assets.push_back(std::move(asset));
            }
        }
    }
    return true;
}

HttpOptions GitHubOptions()
{
    HttpOptions o;
    o.userAgent = std::string("MyEngine/") + kEngineVersion;
    o.headers.push_back("Accept: application/vnd.github+json");
    o.headers.push_back("X-GitHub-Api-Version: 2022-11-28");
    return o;
}

HttpOptions DownloadOptions()
{
    HttpOptions o;
    o.userAgent = std::string("MyEngine/") + kEngineVersion;
    o.headers.push_back("Accept: application/octet-stream");
    return o;
}

bool IsHttpsUrl(std::string_view url)
{
    return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

// ローカル取得元での asset の実パス。絶対パスはそのまま、それ以外は取得元フォルダからの相対
std::filesystem::path LocalAssetPath(const ReleaseSource& source, const ReleaseAsset& asset)
{
    const std::filesystem::path p(Utf8ToWide(asset.url));
    return p.is_absolute() ? p : std::filesystem::path(source.localDir) / p;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& out, size_t limit)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return out.size() <= limit;
}

} // namespace

bool ParseEngineVersion(std::string_view text, EngineVersion& out)
{
    out = EngineVersion{};
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) {
        text.remove_prefix(1);
    }
    const size_t dash = text.find('-');
    const std::string_view numbers = text.substr(0, dash);
    if (dash != std::string_view::npos) {
        out.suffix = std::string(text.substr(dash + 1));
        if (out.suffix.empty()) {
            return false;
        }
        for (const char c : out.suffix) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.') {
                return false; // asset 名とフォルダ名に入るので英数字とドットだけ
            }
        }
    }
    size_t part = 0;
    size_t pos = 0;
    while (pos <= numbers.size()) {
        size_t dot = numbers.find('.', pos);
        if (dot == std::string_view::npos) {
            dot = numbers.size();
        }
        const std::string_view digits = numbers.substr(pos, dot - pos);
        if (digits.empty() || digits.size() > 9 || part >= out.parts.size()) {
            return false;
        }
        uint32_t value = 0;
        for (const char c : digits) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        out.parts[part++] = value;
        pos = dot + 1;
    }
    out.text = std::string(text);
    out.valid = true;
    return true;
}

int CompareEngineVersion(const EngineVersion& a, const EngineVersion& b)
{
    for (size_t i = 0; i < a.parts.size(); ++i) {
        if (a.parts[i] != b.parts[i]) {
            return a.parts[i] < b.parts[i] ? -1 : 1;
        }
    }
    if (a.suffix.empty() != b.suffix.empty()) {
        return a.suffix.empty() ? 1 : -1;
    }
    return CompareNatural(a.suffix, b.suffix);
}

EngineVersion CurrentEngineVersion()
{
    EngineVersion v;
    ParseEngineVersion(kEngineVersion, v);
    return v;
}

std::string EnginePackageAssetName(const EngineVersion& version)
{
    return "MyEngine-" + version.text + "-win64.zip";
}

std::string EnginePackageHashAssetName(const EngineVersion& version)
{
    return EnginePackageAssetName(version) + ".sha256";
}

bool ParseGitHubReleases(std::string_view json, std::vector<ReleaseInfo>& out, std::string& error)
{
    out.clear();
    const nlohmann::json root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded()) {
        error = "release JSON is malformed";
        return false;
    }
    if (root.is_object()) {
        ReleaseInfo r;
        if (ParseOneRelease(root, r)) {
            out.push_back(std::move(r));
        }
        return true;
    }
    if (root.is_array()) {
        for (const nlohmann::json& item : root) {
            ReleaseInfo r;
            if (ParseOneRelease(item, r)) {
                out.push_back(std::move(r));
            }
        }
        return true;
    }
    error = "release JSON is neither an object nor an array";
    return false;
}

const ReleaseInfo* PickLatestRelease(const std::vector<ReleaseInfo>& releases, bool includePrerelease)
{
    const ReleaseInfo* best = nullptr;
    for (const ReleaseInfo& r : releases) {
        if (r.draft || (r.prerelease && !includePrerelease)) {
            continue;
        }
        if (!best || CompareEngineVersion(r.version, best->version) > 0) {
            best = &r;
        }
    }
    return best;
}

const ReleaseAsset* FindReleaseAsset(const ReleaseInfo& release, std::string_view name)
{
    for (const ReleaseAsset& a : release.assets) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

bool ParseSha256Text(std::string_view text, std::string& hexOut)
{
    hexOut.clear();
    size_t i = 0;
    // UTF-8 の BOM と先頭の空白を飛ばす
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    constexpr size_t kHexLen = 64;
    if (text.size() - i < kHexLen) {
        return false;
    }
    for (size_t k = 0; k < kHexLen; ++k) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + k])));
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        hexOut += c;
    }
    // 65 桁目以降も 16 進なら SHA-256 ではない
    if (i + kHexLen < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + kHexLen]))) {
        hexOut.clear();
        return false;
    }
    return true;
}

ReleaseSource ResolveReleaseSource(const std::wstring& cliOverride)
{
    ReleaseSource s;
    if (!cliOverride.empty()) {
        s.localDir = cliOverride;
        return s;
    }
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(kReleaseSourceEnv, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        s.localDir = buf;
    }
    return s;
}

FetchReleaseResult FetchLatestRelease(const ReleaseSource& source, bool includePrerelease,
                                      const std::atomic<bool>* cancel)
{
    FetchReleaseResult result;
    std::string body;
    if (!source.localDir.empty()) {
        const std::filesystem::path path = std::filesystem::path(source.localDir) / kLocalReleaseFile;
        if (!ReadWholeFile(path, body, kApiResponseLimit)) {
            result.error = "cannot read " + WideToUtf8(path.wstring());
            return result;
        }
    } else {
        // 正式版だけなら /latest (GitHub 側で下書きとプレリリースが除かれる)。含めるなら一覧から選ぶ
        std::string url = std::string("https://api.github.com/repos/") + kReleaseRepoOwner + "/" + kReleaseRepoName
            + "/releases";
        url += includePrerelease ? "?per_page=" + std::to_string(kReleaseListPageSize) : "/latest";
        const HttpResult r = HttpGetText(url, GitHubOptions(), body, kApiResponseLimit, cancel);
        if (!r.ok) {
            // Release が 1 件も無いと /latest は 404 になる
            if (r.status == 404) {
                result.ok = true;
                return result;
            }
            result.error = r.error;
            return result;
        }
    }
    std::vector<ReleaseInfo> releases;
    if (!ParseGitHubReleases(body, releases, result.error)) {
        return result;
    }
    result.ok = true;
    if (const ReleaseInfo* latest = PickLatestRelease(releases, includePrerelease)) {
        result.found = true;
        result.release = *latest;
    }
    return result;
}

HttpResult FetchReleaseAsset(const ReleaseSource& source, const ReleaseAsset& asset, const std::wstring& destPath,
                             const HttpProgressFn& progress, const std::atomic<bool>* cancel)
{
    if (source.localDir.empty() || IsHttpsUrl(asset.url)) {
        return HttpDownloadFile(asset.url, destPath, DownloadOptions(), progress, cancel);
    }
    HttpResult r;
    const std::filesystem::path srcPath = LocalAssetPath(source, asset);
    std::ifstream in(srcPath, std::ios::binary);
    std::ofstream out(std::filesystem::path(destPath), std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        r.error = "cannot copy " + WideToUtf8(srcPath.wstring());
        return r;
    }
    std::error_code ec;
    const uint64_t total = std::filesystem::file_size(srcPath, ec);
    std::vector<char> buf(kCopyChunkBytes);
    uint64_t done = 0;
    while (in) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            r.cancelled = true;
            r.error = "cancelled";
            break;
        }
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) {
            break;
        }
        out.write(buf.data(), n);
        done += static_cast<uint64_t>(n);
        if (progress) {
            progress(done, total);
        }
    }
    out.close();
    if (!r.cancelled && (out.fail() || in.bad())) {
        r.error = "copy failed: " + WideToUtf8(srcPath.wstring());
    }
    r.ok = r.error.empty();
    r.status = r.ok ? 200 : 0;
    if (!r.ok) {
        std::filesystem::remove(destPath, ec);
    }
    return r;
}

HttpResult FetchReleaseAssetText(const ReleaseSource& source, const ReleaseAsset& asset, std::string& text,
                                 const std::atomic<bool>* cancel)
{
    if (source.localDir.empty() || IsHttpsUrl(asset.url)) {
        return HttpGetText(asset.url, DownloadOptions(), text, kSmallAssetLimit, cancel);
    }
    HttpResult r;
    if (!ReadWholeFile(LocalAssetPath(source, asset), text, kSmallAssetLimit)) {
        r.error = "cannot read " + asset.name;
        return r;
    }
    r.ok = true;
    r.status = 200;
    return r;
}

} // namespace mye
