//====================================================================================
//                          UpdateWindowSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          エディタの更新確認の判断の自己テストの実装
//====================================================================================
#include "Editor/Update/UpdateWindowSelfTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <Windows.h>

#include "Editor/Update/UpdateWindow.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Update/InstallLayout.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

void Touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << "x";
}

} // namespace

bool RunUpdateWindowSelfTest()
{
    MYE_LOG_INFO("==== UpdateWindow self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    constexpr int64_t kDay = 24 * 60 * 60;
    constexpr int64_t kNow = 1'800'000'000;
    check(ShouldAutoCheckUpdate(true, 0, kNow), "auto check: never checked");
    check(!ShouldAutoCheckUpdate(true, kNow - kDay + 1, kNow), "auto check: not again within a day");
    check(ShouldAutoCheckUpdate(true, kNow - kDay, kNow), "auto check: again after a day");
    check(ShouldAutoCheckUpdate(true, kNow + 3600, kNow), "auto check: the clock went back");
    check(!ShouldAutoCheckUpdate(false, 0, kNow), "auto check: off in the settings");

    const fs::path tmp = fs::temp_directory_path() / (L"mye_updwin_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    {
        const std::wstring root = (tmp / L"root").wstring();
        const fs::path exeDir = tmp / L"engine" / L"bin" / L"x64" / L"Release";
        fs::create_directories(exeDir);
        check(FindLauncherForUpdate(root, exeDir.wstring()).empty(), "launcher: none found");
        Touch(exeDir / kLauncherExeName);
        check(FindLauncherForUpdate(root, exeDir.wstring()) == (exeDir / kLauncherExeName).wstring(),
              "launcher: the one beside the editor");
        Touch(fs::path(LauncherDir(root)) / kLauncherExeName);
        check(FindLauncherForUpdate(root, exeDir.wstring()) == LauncherDir(root) + L"\\" + kLauncherExeName,
              "launcher: the placed one wins");
        const fs::path chosen = tmp / L"downloads" / kLauncherExeName;
        Touch(chosen);
        RecordLauncherLocation(root, chosen.wstring());
        check(FindLauncherForUpdate(root, exeDir.wstring()) == chosen.wstring(),
              "launcher: the location the Launcher recorded wins");
        // 印の無い置き場は開発ビルド扱い (更新ボタンを出さない)
        check(FindManagedInstallDir(exeDir.wstring()).empty(), "managed: no install.json is a source build");
        InstallMarker marker;
        marker.version = "0.86";
        WriteInstallMarker((tmp / L"engine").wstring(), marker);
        check(FindManagedInstallDir(exeDir.wstring()) == (tmp / L"engine").wstring(), "managed: install.json found");
    }
    fs::remove_all(tmp, ec);

    // 書式の引数 (規則 10 は %s の並びの一致を見る。ここでは実際に埋めて落ちないことを見る)
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        char buf[512] = {};
        snprintf(buf, sizeof(buf), TrIn(lang, StrId::Upd_Available), "0.86", "2026-11-10");
        const std::string s = buf;
        check(s.find("0.86") != std::string::npos && s.find("2026-11-10") != std::string::npos,
              lang == Lang::En ? "strings: Upd_Available (en)" : "strings: Upd_Available (ja)");
    }

    MYE_LOG_INFO("UpdateWindow self test: %s (%d failures)", failCount == 0 ? "OK" : "FAILED", failCount);
    return failCount == 0;
}

} // namespace mye
