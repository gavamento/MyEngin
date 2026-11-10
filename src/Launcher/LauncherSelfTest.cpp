//====================================================================================
//                          LauncherSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher のインストール経路の自己テストの実装
//====================================================================================
#include "Launcher/LauncherSelfTest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"
#include "Launcher/InstallService.h"
#include "Launcher/LauncherSelf.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

constexpr mz_uint kDeflateLevel = static_cast<mz_uint>(MZ_DEFAULT_LEVEL);

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

bool WriteZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files)
{
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, WideToUtf8(path.wstring()).c_str(), 0)) {
        return false;
    }
    bool ok = true;
    for (const auto& [name, data] : files) {
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), kDeflateLevel) != 0;
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    return ok;
}

// 取得元フォルダに 1 版分 (zip + .sha256 + release.json) を置き、その ReleaseInfo を返す。
// hashOverride が空でなければ .sha256 にその値を書く (不一致の検査用)
ReleaseInfo PublishLocal(const fs::path& srcDir, const std::string& version,
                         const std::vector<std::pair<std::string, std::string>>& files,
                         const std::string& hashOverride = std::string())
{
    fs::create_directories(srcDir);
    EngineVersion v;
    ParseEngineVersion(version, v);
    const std::string zipName = EnginePackageAssetName(v);
    WriteZip(srcDir / Utf8ToWide(zipName), files);
    std::string hex;
    Sha256FileHex((srcDir / Utf8ToWide(zipName)).wstring(), hex);
    WriteText(srcDir / Utf8ToWide(zipName + ".sha256"), (hashOverride.empty() ? hex : hashOverride) + "  " + zipName);
    const std::string json = "{ \"tag_name\": \"v" + version + "\", \"assets\": [ { \"name\": \"" + zipName
        + "\", \"browser_download_url\": \"" + zipName + "\" }, { \"name\": \"" + zipName
        + ".sha256\", \"browser_download_url\": \"" + zipName + ".sha256\" } ] }";
    WriteText(srcDir / L"release.json", json);
    std::vector<ReleaseInfo> list;
    std::string error;
    ParseGitHubReleases(json, list, error);
    return list.empty() ? ReleaseInfo{} : list.front();
}

std::vector<std::pair<std::string, std::string>> GoodPackage(const std::string& marker)
{
    return { { "MyEngine/bin/x64/Release/Editor.exe", "fake-editor-" + marker },
             { "MyEngine/assets/readme.txt", "assets" },
             { "MyEngine/src/Shared/ScriptAPI.h", "// shared" } };
}

bool HasLeftovers(const std::wstring& root)
{
    std::error_code ec;
    for (fs::directory_iterator it(EnginesDir(root), ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.find(kStagingInfix) != std::wstring::npos || name.find(kDeletingInfix) != std::wstring::npos) {
            return true;
        }
    }
    for (fs::directory_iterator it(DownloadsDir(root), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".part") {
            return true;
        }
    }
    return false;
}

} // namespace

bool RunLauncherSelfTest()
{
    MYE_LOG_INFO("==== Launcher self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    const fs::path tmp = fs::temp_directory_path() / (L"mye_launcher_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const std::wstring root = (tmp / L"root").wstring();

    // 1. 正常なインストール
    {
        const fs::path src = tmp / L"src_ok";
        const ReleaseInfo rel = PublishLocal(src, "9.1", GoodPackage("9.1"));
        const ReleaseSource source = ResolveReleaseSource(src.wstring());
        uint64_t lastDone = 0;
        bool sawDownload = false;
        bool sawExtract = false;
        const InstallOutcome r = InstallRelease(
            source, rel, root,
            [&](InstallStage stage, uint64_t done, uint64_t) {
                sawDownload = sawDownload || stage == InstallStage::Downloading;
                sawExtract = sawExtract || stage == InstallStage::Extracting;
                lastDone = done;
            },
            nullptr);
        check(r.ok && !r.alreadyInstalled && fs::exists(InstalledEditorPath(r.installDir)), "install: success");
        check(sawDownload && sawExtract, "install: progress reports download and extract stages");
        InstallMarker marker;
        check(ReadInstallMarker(r.installDir, marker) && marker.version == "9.1" && marker.tag == "v9.1"
                  && marker.sha256.size() == 64,
              "install: install.json is written");
        check(fs::exists(fs::path(r.installDir) / L"src" / L"Shared" / L"ScriptAPI.h")
                  && !fs::exists(fs::path(r.installDir) / L"MyEngine"),
              "install: the top folder of the zip is stripped (repository layout)");
        check(!HasLeftovers(root), "install: no staging or .part left");

        const std::wstring exeDir = InstalledBinDir(r.installDir);
        check(FindManagedInstallDir(exeDir) == r.installDir, "layout: the editor finds its install folder");
        check(FindManagedInstallDir((tmp / L"a" / L"b" / L"c").wstring()).empty(),
              "layout: a folder without install.json is not managed");

        const InstallOutcome again = InstallRelease(source, rel, root, InstallProgressFn{}, nullptr);
        check(again.ok && again.alreadyInstalled, "install: the same version again is a no-op");
    }

    // 2. 新しい版を並べて入れる (古い版は残る)
    {
        const fs::path src = tmp / L"src_new";
        const ReleaseInfo rel = PublishLocal(src, "9.2-rc1", GoodPackage("9.2"));
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, nullptr);
        const std::vector<InstalledEngine> list = ScanInstalledEngines(root);
        check(r.ok && list.size() == 2 && list[0].version.text == "9.2-rc1" && list[1].version.text == "9.1",
              "install: versions sit side by side, newest first");
    }

    // 3. SHA-256 の不一致: 何も入らない
    {
        const fs::path src = tmp / L"src_badhash";
        const ReleaseInfo rel = PublishLocal(src, "9.3", GoodPackage("9.3"), std::string(64, '0'));
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, nullptr);
        check(!r.ok && r.error.find("SHA-256") != std::string::npos && !fs::exists(fs::path(EnginesDir(root)) / L"9.3")
                  && !HasLeftovers(root),
              "install: SHA-256 mismatch installs nothing");
    }

    // 4. Editor.exe の無いパッケージ
    {
        const fs::path src = tmp / L"src_noeditor";
        const ReleaseInfo rel = PublishLocal(src, "9.4", { { "MyEngine/assets/a.txt", "a" } });
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, nullptr);
        check(!r.ok && !fs::exists(fs::path(EnginesDir(root)) / L"9.4") && !HasLeftovers(root),
              "install: a package without Editor.exe is rejected");
    }

    // 5. zip slip を含むパッケージ
    {
        const fs::path src = tmp / L"src_slip";
        auto files = GoodPackage("9.5");
        files.push_back({ "MyEngine/../../../evil.txt", "evil" });
        const ReleaseInfo rel = PublishLocal(src, "9.5", files);
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, nullptr);
        check(!r.ok && !fs::exists(tmp / L"evil.txt") && !fs::exists(fs::path(EnginesDir(root)) / L"9.5")
                  && !HasLeftovers(root),
              "install: path traversal in the package is rejected");
    }

    // 6. 取り消し
    {
        const fs::path src = tmp / L"src_cancel";
        const ReleaseInfo rel = PublishLocal(src, "9.6", GoodPackage("9.6"));
        std::atomic<bool> cancel{ true };
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, &cancel);
        check(!r.ok && r.cancelled && !fs::exists(fs::path(EnginesDir(root)) / L"9.6") && !HasLeftovers(root),
              "install: cancel leaves nothing behind");
    }

    // 7. 印の無い同名フォルダは上書きしない
    {
        fs::create_directories(fs::path(EnginesDir(root)) / L"9.7");
        WriteText(fs::path(EnginesDir(root)) / L"9.7" / L"mine.txt", "user data");
        const fs::path src = tmp / L"src_foreign";
        const ReleaseInfo rel = PublishLocal(src, "9.7", GoodPackage("9.7"));
        const InstallOutcome r = InstallRelease(ResolveReleaseSource(src.wstring()), rel, root, InstallProgressFn{}, nullptr);
        check(!r.ok && fs::exists(fs::path(EnginesDir(root)) / L"9.7" / L"mine.txt"),
              "install: a foreign folder with the same name is left alone");
        check(ScanInstalledEngines(root).size() == 2, "scan: folders without install.json are ignored");
    }

    // 8. 削除: 使用中 (ファイルが開いている) なら何も消さない、閉じれば消える
    {
        const std::vector<InstalledEngine> list = ScanInstalledEngines(root);
        const std::wstring victim = list.back().dir; // 9.1
        const std::wstring editor = InstalledEditorPath(victim);
        const HANDLE h = CreateFileW(editor.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        const InstallOutcome busy = UninstallEngine(victim, InstallProgressFn{});
        check(h != INVALID_HANDLE_VALUE && !busy.ok && fs::exists(editor), "uninstall: a version in use is kept intact");
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
        const InstallOutcome r = UninstallEngine(victim, InstallProgressFn{});
        check(r.ok && !fs::exists(victim) && ScanInstalledEngines(root).size() == 1 && !HasLeftovers(root),
              "uninstall: removes the version");
        check(!UninstallEngine((fs::path(EnginesDir(root)) / L"9.7").wstring(), InstallProgressFn{}).ok
                  && fs::exists(fs::path(EnginesDir(root)) / L"9.7" / L"mine.txt"),
              "uninstall: refuses a folder the Launcher did not install");
    }

    // 9. 残骸の片付け
    {
        fs::create_directories(fs::path(EnginesDir(root)) / L"9.8.staging-1");
        fs::create_directories(DownloadsDir(root));
        WriteText(fs::path(DownloadsDir(root)) / L"x.zip.1.part", "partial");
        CleanupStaleInstallWork(root);
        check(!HasLeftovers(root) && ScanInstalledEngines(root).size() == 1, "cleanup: removes leftovers only");
    }

    // 10. Launcher 自身: 置き場へ写す・新しい版の取得と入れ替え
    {
        const std::wstring selfRoot = (tmp / L"selfroot").wstring();
        PlaceLauncherCopy(selfRoot, false);
        const fs::path placed = fs::path(LauncherDir(selfRoot)) / kLauncherExeName;
        check(fs::exists(placed) && PlacedLauncherVersion(selfRoot).valid
                  && CompareEngineVersion(PlacedLauncherVersion(selfRoot), CurrentEngineVersion()) == 0,
              "self: the running exe is copied to the Launcher folder with its version");

        // 新しい版の Launcher (中身は何でもよい) を取得元に置く
        const fs::path src = tmp / L"src_self";
        fs::create_directories(src);
        WriteText(src / kLauncherExeName, "new-launcher-bytes");
        const std::string hex = Sha256Hex("new-launcher-bytes");
        WriteText(src / L"MyEngineLauncher.exe.sha256", hex + "  MyEngineLauncher.exe");
        const std::string json = R"({ "tag_name": "v99.0", "assets": [
            { "name": "MyEngineLauncher.exe", "browser_download_url": "MyEngineLauncher.exe" },
            { "name": "MyEngineLauncher.exe.sha256", "browser_download_url": "MyEngineLauncher.exe.sha256" } ] })";
        std::vector<ReleaseInfo> list;
        std::string error;
        ParseGitHubReleases(json, list, error);
        check(!list.empty() && LauncherUpdateAvailable(list.front()), "self: a newer Launcher in the release is detected");
        const InstallOutcome d = DownloadLauncherUpdate(ResolveReleaseSource(src.wstring()), list.front(), selfRoot,
                                                        InstallProgressFn{}, nullptr);
        check(d.ok, "self: the new Launcher is downloaded and verified");
        check(ApplyLauncherUpdate(selfRoot, "99.0", false, error), "self: the new Launcher is swapped in");
        std::ifstream f(placed, std::ios::binary);
        std::string now((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        check(now == "new-launcher-bytes" && PlacedLauncherVersion(selfRoot).text == "99.0",
              "self: the placed Launcher and its version are the new ones");
        // 置いてある方が新しいなら古い exe で上書きしない
        PlaceLauncherCopy(selfRoot, false);
        std::ifstream f2(placed, std::ios::binary);
        std::string after((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        f2.close();
        check(after == "new-launcher-bytes", "self: an older Launcher does not overwrite a newer placed one");
        CleanupOldLauncherFiles(selfRoot);
        check(!fs::exists(fs::path(LauncherDir(selfRoot)) / L"MyEngineLauncher.old.exe"), "self: the old exe is cleaned up");

        WriteText(src / L"MyEngineLauncher.exe.sha256", std::string(64, '0') + "  MyEngineLauncher.exe");
        const InstallOutcome bad = DownloadLauncherUpdate(ResolveReleaseSource(src.wstring()), list.front(), selfRoot,
                                                          InstallProgressFn{}, nullptr);
        check(!bad.ok && !fs::exists(fs::path(LauncherDir(selfRoot)) / L"MyEngineLauncher.new.exe"),
              "self: a Launcher with a wrong SHA-256 is discarded");
    }

    fs::remove_all(tmp, ec);
    MYE_LOG_INFO("Launcher self test: %s (%d failures)", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
