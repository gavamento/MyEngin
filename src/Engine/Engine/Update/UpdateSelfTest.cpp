//====================================================================================
//                          UpdateSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          配布と更新の共通部品の自己テストの実装
//====================================================================================
#include "Engine/Engine/Update/UpdateSelfTest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Update/EngineRelease.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"
#include "Engine/Platform/ZipExtract.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

constexpr mz_uint kDeflateLevel = static_cast<mz_uint>(MZ_DEFAULT_LEVEL);

struct ZipItem
{
    std::string name;
    std::string data;
    bool directory = false;
};

// level 0 = 無圧縮 (中身のバイトが zip にそのまま載るので、壊す検査に使える)
bool WriteTestZip(const fs::path& path, const std::vector<ZipItem>& items, mz_uint level)
{
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, WideToUtf8(path.wstring()).c_str(), 0)) {
        return false;
    }
    bool ok = true;
    for (const ZipItem& item : items) {
        if (item.directory) {
            ok = ok && mz_zip_writer_add_mem(&zip, item.name.c_str(), nullptr, 0, 0) != 0;
        } else {
            ok = ok && mz_zip_writer_add_mem(&zip, item.name.c_str(), item.data.data(), item.data.size(), level) != 0;
        }
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    return ok;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

bool DirIsEmpty(const fs::path& dir)
{
    std::error_code ec;
    return fs::is_empty(dir, ec) && !ec;
}

} // namespace

bool RunUpdateSelfTest()
{
    MYE_LOG_INFO("==== Update self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 版の解析と比較 ----
    {
        EngineVersion v;
        check(ParseEngineVersion("0.6.8.22", v) && v.parts[0] == 0 && v.parts[1] == 6 && v.parts[2] == 8
                  && v.parts[3] == 22 && v.suffix.empty() && v.text == "0.6.8.22",
              "version: 0.6.8.22");
        check(ParseEngineVersion("v0.6.8.23-rc1", v) && v.parts[3] == 23 && v.suffix == "rc1"
                  && v.text == "0.6.8.23-rc1",
              "version: leading v and suffix");
        check(ParseEngineVersion("1.2", v) && v.parts[0] == 1 && v.parts[1] == 2 && v.parts[2] == 0,
              "version: missing parts are 0");
        const char* const bad[] = { "", "v", "1..2", "1.2.3.4.5", "1.2-", "1.2-a/b", "abc", "1.2.", "-rc1",
                                    "1.2.3.4-rc 1", "M7-done" };
        bool allRejected = true;
        for (const char* b : bad) {
            if (ParseEngineVersion(b, v)) {
                MYE_LOG_ERROR("    accepted: '%s'", b);
                allRejected = false;
            }
        }
        check(allRejected, "version: malformed text is rejected (incl. old milestone tags)");

        auto cmp = [](const char* a, const char* b) {
            EngineVersion va;
            EngineVersion vb;
            ParseEngineVersion(a, va);
            ParseEngineVersion(b, vb);
            return CompareEngineVersion(va, vb);
        };
        check(cmp("0.6.8.22", "0.6.8.23") < 0 && cmp("0.6.8.23", "0.6.8.22") > 0, "compare: last part");
        check(cmp("0.6.9", "0.6.8.99") > 0, "compare: earlier part wins");
        check(cmp("1.0", "1.0.0.0") == 0, "compare: missing parts equal 0");
        check(cmp("0.6.8.23-rc1", "0.6.8.23") < 0, "compare: prerelease is older than the release");
        check(cmp("0.6.8.23-rc1", "0.6.8.22") > 0, "compare: prerelease is newer than the previous release");
        check(cmp("1.0-rc2", "1.0-rc10") < 0, "compare: suffix numbers compare naturally");
        check(CurrentEngineVersion().valid, "kEngineVersion parses");

        ParseEngineVersion("v0.6.8.23", v);
        check(EnginePackageAssetName(v) == "MyEngine-0.6.8.23-win64.zip"
                  && EnginePackageHashAssetName(v) == "MyEngine-0.6.8.23-win64.zip.sha256",
              "asset names follow the release.yml contract");
    }

    // ---- Release JSON ----
    {
        const char* json = R"([
          { "tag_name": "v0.7.0.0", "draft": true, "prerelease": false, "assets": [] },
          { "tag_name": "v0.6.9.0-rc1", "draft": false, "prerelease": true, "assets": [] },
          { "tag_name": "v0.6.8.23", "name": "MyEngine 0.6.8.23", "body": "notes",
            "published_at": "2026-11-10T00:00:00Z", "draft": false, "prerelease": false,
            "assets": [ { "name": "MyEngine-0.6.8.23-win64.zip", "size": 1234,
                          "browser_download_url": "https://example.invalid/a.zip" },
                        { "name": "broken" } ] },
          { "tag_name": "M7-done", "draft": false, "prerelease": false, "assets": [] },
          { "tag_name": "v0.6.8.22", "draft": false, "prerelease": false, "assets": [] }
        ])";
        std::vector<ReleaseInfo> list;
        std::string error;
        check(ParseGitHubReleases(json, list, error) && list.size() == 4, "release list: invalid tags are dropped");
        const ReleaseInfo* stable = PickLatestRelease(list, false);
        check(stable && stable->tag == "v0.6.8.23" && stable->notes == "notes" && stable->assets.size() == 1
                  && stable->assets[0].size == 1234,
              "release list: latest stable skips drafts and prereleases");
        const ReleaseInfo* pre = PickLatestRelease(list, true);
        check(pre && pre->tag == "v0.6.9.0-rc1", "release list: prerelease is picked when allowed");
        check(stable && FindReleaseAsset(*stable, "MyEngine-0.6.8.23-win64.zip") != nullptr
                  && FindReleaseAsset(*stable, "missing") == nullptr,
              "release: asset lookup by name");

        list.clear();
        check(ParseGitHubReleases(R"({ "tag_name": "v1.0", "assets": [] })", list, error) && list.size() == 1,
              "release: single object (/releases/latest)");
        check(!ParseGitHubReleases("{ not json", list, error), "release: malformed JSON fails");
        check(PickLatestRelease({}, true) == nullptr, "release: empty list has no latest");
    }

    // ---- SHA-256 ----
    {
        check(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256: empty");
        check(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: abc");
        std::string hex;
        check(ParseSha256Text("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  x.zip\n", hex)
                  && hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "sha256 text: sha256sum format, upper case");
        check(ParseSha256Text("\xEF\xBB\xBF ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex),
              "sha256 text: BOM and leading space");
        check(!ParseSha256Text("ba7816bf", hex), "sha256 text: too short");
        check(!ParseSha256Text("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0", hex),
              "sha256 text: 65 hex digits");
    }

    // ---- zip のエントリ名 ----
    {
        std::string out;
        check(SanitizeZipEntryPath("MyEngine/bin/x64/Release/Editor.exe", out)
                  && out == "MyEngine/bin/x64/Release/Editor.exe",
              "zip name: plain");
        check(SanitizeZipEntryPath("MyEngine\\assets\\a.png", out) && out == "MyEngine/assets/a.png",
              "zip name: backslashes");
        check(SanitizeZipEntryPath("MyEngine/", out) && out == "MyEngine", "zip name: folder entry");
        check(SanitizeZipEntryPath("./a//b", out) && out == "a/b", "zip name: . and empty parts");
        const char* const unsafe[] = { "../evil.txt", "a/../../evil.txt", "/abs.txt", "C:/x.txt", "\\\\srv\\x",
                                       "a/file.txt:ads", "a/NUL", "a/con.txt", "a/LPT1", "a/trail.", "a/trail ",
                                       "", "a/b\x01", ".." };
        bool allRejected = true;
        for (const char* u : unsafe) {
            if (SanitizeZipEntryPath(u, out)) {
                MYE_LOG_ERROR("    accepted: '%s'", u);
                allRejected = false;
            }
        }
        check(allRejected, "zip name: unsafe names are rejected");
        check(SanitizeZipEntryPath("a/console.txt", out) && SanitizeZipEntryPath("a/COM0", out),
              "zip name: names that only look reserved are allowed");
    }

    // ---- zip の展開 ----
    const fs::path tmp = fs::temp_directory_path() / (L"mye_update_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        const std::string big(300 * 1024, 'x'); // 書き出しが複数回に分かれる大きさ
        const fs::path zipPath = tmp / L"pkg.zip";
        const bool written = WriteTestZip(zipPath,
                                          { { "MyEngine/", "", true },
                                            { "MyEngine/a.txt", "hello", false },
                                            { "MyEngine/sub/b.bin", big, false },
                                            { "MyEngine/empty/", "", true } },
                                          kDeflateLevel);
        check(written, "zip: test archive written");

        const fs::path dest = tmp / L"strip";
        fs::create_directories(dest, ec);
        uint64_t lastDone = 0;
        uint64_t lastTotal = 0;
        const ZipExtractResult r = ExtractZip(zipPath.wstring(), dest.wstring(), true,
                                              [&](uint64_t d, uint64_t t) {
                                                  lastDone = d;
                                                  lastTotal = t;
                                              },
                                              nullptr);
        check(r.ok && r.files == 2 && ReadText(dest / L"a.txt") == "hello" && ReadText(dest / L"sub" / L"b.bin") == big
                  && fs::is_directory(dest / L"empty") && !fs::exists(dest / L"MyEngine"),
              "zip: extract with the top folder stripped");
        check(lastTotal == 5 + big.size() && lastDone == lastTotal, "zip: progress reaches the total");

        const fs::path keep = tmp / L"keep";
        fs::create_directories(keep, ec);
        const ZipExtractResult r2 = ExtractZip(zipPath.wstring(), keep.wstring(), false, ZipProgressFn{}, nullptr);
        check(r2.ok && ReadText(keep / L"MyEngine" / L"a.txt") == "hello", "zip: extract keeping the top folder");

        // 最上位が 2 つあるなら外さない
        const fs::path twoTopZip = tmp / L"twotop.zip";
        WriteTestZip(twoTopZip, { { "A/x.txt", "1", false }, { "B/y.txt", "2", false } }, kDeflateLevel);
        const fs::path twoTop = tmp / L"twotop";
        fs::create_directories(twoTop, ec);
        const ZipExtractResult r3 = ExtractZip(twoTopZip.wstring(), twoTop.wstring(), true, ZipProgressFn{}, nullptr);
        check(r3.ok && fs::exists(twoTop / L"A" / L"x.txt") && fs::exists(twoTop / L"B" / L"y.txt"),
              "zip: two top folders are not stripped");

        // zip slip: 1 件でも外へ出る名前があれば何も書かない
        const fs::path slipZip = tmp / L"slip.zip";
        WriteTestZip(slipZip, { { "MyEngine/ok.txt", "ok", false }, { "MyEngine/../../evil.txt", "evil", false } },
                     kDeflateLevel);
        const fs::path slipDest = tmp / L"slip" / L"dest";
        fs::create_directories(slipDest, ec);
        const ZipExtractResult r4 = ExtractZip(slipZip.wstring(), slipDest.wstring(), true, ZipProgressFn{}, nullptr);
        check(!r4.ok && r4.error.find("unsafe path") != std::string::npos && DirIsEmpty(slipDest)
                  && !fs::exists(tmp / L"evil.txt") && !fs::exists(tmp / L"slip" / L"evil.txt"),
              "zip: path traversal is rejected before writing anything");

        // 壊れた中身 (CRC 不一致)
        const fs::path corruptZip = tmp / L"corrupt.zip";
        const std::string payload = "PAYLOAD-0123456789-PAYLOAD";
        WriteTestZip(corruptZip, { { "c.txt", payload, false } }, 0);
        std::string bytes = ReadText(corruptZip);
        const size_t at = bytes.find(payload);
        if (at != std::string::npos) {
            bytes[at + 3] ^= 0x20;
            WriteText(corruptZip, bytes);
        }
        const fs::path corruptDest = tmp / L"corrupt";
        fs::create_directories(corruptDest, ec);
        const ZipExtractResult r5 = ExtractZip(corruptZip.wstring(), corruptDest.wstring(), false, ZipProgressFn{},
                                               nullptr);
        check(at != std::string::npos && !r5.ok, "zip: CRC mismatch fails");

        // 途中で切れた zip (ダウンロードの中断)
        const fs::path truncZip = tmp / L"trunc.zip";
        std::string full = ReadText(zipPath);
        WriteText(truncZip, full.substr(0, full.size() / 2));
        const fs::path truncDest = tmp / L"trunc";
        fs::create_directories(truncDest, ec);
        check(!ExtractZip(truncZip.wstring(), truncDest.wstring(), true, ZipProgressFn{}, nullptr).ok,
              "zip: truncated archive fails");

        // 取り消し
        std::atomic<bool> cancel{ true };
        const fs::path cancelDest = tmp / L"cancel";
        fs::create_directories(cancelDest, ec);
        const ZipExtractResult r6 = ExtractZip(zipPath.wstring(), cancelDest.wstring(), true, ZipProgressFn{}, &cancel);
        check(!r6.ok && r6.cancelled, "zip: cancel stops the extraction");

        std::string fileHex;
        check(Sha256FileHex(zipPath.wstring(), fileHex) && fileHex == Sha256Hex(ReadText(zipPath)),
              "sha256: file and memory agree");
        check(!Sha256FileHex((tmp / L"missing.bin").wstring(), fileHex), "sha256: missing file fails");
    }

    // ---- ローカルの取得元 (Launcher / エディタの検証経路) ----
    {
        const fs::path src = tmp / L"source";
        fs::create_directories(src, ec);
        WriteTestZip(src / L"MyEngine-9.0.0.1-win64.zip", { { "MyEngine/a.txt", "v9", false } }, kDeflateLevel);
        std::string zipHex;
        Sha256FileHex((src / L"MyEngine-9.0.0.1-win64.zip").wstring(), zipHex);
        WriteText(src / L"MyEngine-9.0.0.1-win64.zip.sha256", zipHex + "  MyEngine-9.0.0.1-win64.zip\n");
        WriteText(src / L"release.json", R"({ "tag_name": "v9.0.0.1", "prerelease": false, "body": "local",
            "assets": [ { "name": "MyEngine-9.0.0.1-win64.zip", "browser_download_url": "MyEngine-9.0.0.1-win64.zip" },
                        { "name": "MyEngine-9.0.0.1-win64.zip.sha256",
                          "browser_download_url": "MyEngine-9.0.0.1-win64.zip.sha256" } ] })");

        const ReleaseSource source = ResolveReleaseSource(src.wstring());
        check(source.localDir == src.wstring(), "source: the command line wins");
        const FetchReleaseResult latest = FetchLatestRelease(source, false, nullptr);
        check(latest.ok && latest.found && latest.release.tag == "v9.0.0.1"
                  && CompareEngineVersion(latest.release.version, CurrentEngineVersion()) > 0,
              "source: local release.json is read");

        const ReleaseAsset* zipAsset = FindReleaseAsset(latest.release, EnginePackageAssetName(latest.release.version));
        const ReleaseAsset* hashAsset =
            FindReleaseAsset(latest.release, EnginePackageHashAssetName(latest.release.version));
        std::string hashText;
        std::string expected;
        const bool hashOk = hashAsset && FetchReleaseAssetText(source, *hashAsset, hashText, nullptr).ok
            && ParseSha256Text(hashText, expected);
        const fs::path part = tmp / L"download.part";
        std::string gotHex;
        const bool copied = zipAsset && FetchReleaseAsset(source, *zipAsset, part.wstring(), HttpProgressFn{}, nullptr).ok
            && Sha256FileHex(part.wstring(), gotHex);
        check(hashOk && copied && gotHex == expected, "source: asset copy matches the published SHA-256");

        const FetchReleaseResult missing = FetchLatestRelease(ResolveReleaseSource((tmp / L"nowhere").wstring()), false,
                                                              nullptr);
        check(!missing.ok && !missing.error.empty(), "source: a missing release.json is an error, not a crash");
    }

    // ---- HTTP は https だけ (ネットワークには出ない) ----
    {
        std::string body;
        const HttpResult r = HttpGetText("http://example.invalid/", HttpOptions{}, body, 1024, nullptr);
        check(!r.ok && r.error.find("https") != std::string::npos, "http: plain http is refused before connecting");
        const HttpResult r2 = HttpGetText("not a url", HttpOptions{}, body, 1024, nullptr);
        check(!r2.ok, "http: malformed URL fails");
    }

    fs::remove_all(tmp, ec);
    MYE_LOG_INFO("Update self test: %s (%d failures)", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
