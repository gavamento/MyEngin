#include "Engine/Engine/AssetDatabaseSelfTest.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

namespace {

void WriteDummy(const std::wstring& path)
{
    std::ofstream f(path, std::ios::binary);
    f << "dummy";
}

uint64_t ExpectedPathHash(const std::wstring& path)
{
    return HashStr(WideToUtf8(NormalizePathKey(path)));
}

} // namespace

bool RunAssetDatabaseSelfTest()
{
    MYE_LOG_INFO("==== AssetDatabase self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_assetdb_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::wstring fooPng = (root / L"foo.png").wstring();
    const std::wstring bazWav = (root / L"baz.wav").wstring();
    WriteDummy(fooPng);
    WriteDummy(bazWav);

    // ---- 走査 1: .meta 生成 + GUID 継承 ----
    AssetDatabase db1;
    db1.ScanAndSync(root.wstring());

    const std::wstring fooMeta = fooPng + L".meta";
    check(fs::exists(fooMeta, ec), "meta sidecar created for foo.png");

    const uint64_t guidFoo = db1.GuidForPath(fooPng);
    // 継承: GUID は現行 AssetID (パスハッシュ) と一致 → 既存シーン/リプレイ無傷 (bump 不要)
    check(guidFoo == ExpectedPathHash(fooPng), "foo.png GUID inherits current path-hash (AssetID)");
    check(guidFoo != 0, "foo.png GUID is non-zero");
    check(db1.PathForGuid(guidFoo) == fooPng, "GUID -> path round-trips");
    check(db1.TypeForPath(fooPng) == AssetType::Texture, "foo.png classified as texture");
    check(db1.TypeForPath(bazWav) == AssetType::Audio, "baz.wav classified as audio");
    check(db1.GuidForPath(bazWav) == ExpectedPathHash(bazWav), "baz.wav GUID inherits path-hash");

    // ---- リネーム: 本体と .meta を一緒に移動 ----
    const std::wstring barPng = (root / L"bar.png").wstring();
    const std::wstring barMeta = barPng + L".meta";
    fs::rename(fooPng, barPng, ec);
    fs::rename(fooMeta, barMeta, ec);
    check(!ec, "renamed foo.png + sidecar to bar.png");

    // ---- 走査 2: リネーム後も GUID が永続するか ----
    AssetDatabase db2;
    db2.ScanAndSync(root.wstring());
    const uint64_t guidBar = db2.GuidForPath(barPng);
    check(guidBar == guidFoo, "GUID persists across rename (rename resilience)");
    // 決定的な証拠: リネーム後のパスハッシュとは異なる = .meta の GUID が優先された
    check(guidBar != ExpectedPathHash(barPng), "renamed GUID != new path-hash (sidecar overrides)");
    check(db2.PathForGuid(guidFoo) == barPng, "old GUID now resolves to new path");

    // ---- 走査 3: 冪等性 (再走査で GUID 不変) ----
    AssetDatabase db3;
    db3.ScanAndSync(root.wstring());
    check(db3.GuidForPath(barPng) == guidFoo, "re-scan is idempotent (GUID unchanged)");

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== AssetDatabase self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== AssetDatabase self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
