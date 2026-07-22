#include "Engine/Engine/AssetDatabaseSelfTest.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
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

    // ---- MoveAsset API (M30b): フォルダ移動 + 実行時テーブル更新 ----
    {
        const fs::path sub = root / L"sub";
        fs::create_directories(sub, ec);
        const std::wstring moved = (sub / L"bar.png").wstring();
        fs::rename(barPng, moved, ec);
        fs::rename(barMeta, moved + L".meta", ec);
        check(!ec, "moved bar.png + sidecar into sub/");
        db3.MoveAsset(barPng, moved);
        check(db3.GuidForPath(moved, /*createIfMissing=*/false) == guidFoo,
              "MoveAsset: new path resolves to original GUID");
        check(db3.PathForGuid(guidFoo) == moved, "MoveAsset: GUID -> new path");
        // 旧キーは除去済み: 旧パスの解決はディスクにも .meta が無く既定 path-hash に落ちる
        check(db3.GuidForPath(barPng, /*createIfMissing=*/false) == ExpectedPathHash(barPng),
              "MoveAsset: old path key removed from runtime tables");

        // ---- KeyResolver (M30c): ライブラリの HashForPath が GUID を返す ----
        db3.InstallAsKeyResolver();
        check(assetkey::Resolve(NormalizePathKey(moved)) == guidFoo,
              "resolver: moved file resolves to original GUID");
        check(assetkey::Resolve(NormalizePathKey(bazWav)) == ExpectedPathHash(bazWav),
              "resolver: unmoved file resolves to path-hash (bit-identical to legacy)");
        // 実ライブラリのキー計算も GUID に追従する (登録キーの安定性 = シーン参照維持の核)
        check(AnimationLibrary::HashForPath(moved) == guidFoo,
              "resolver: AnimationLibrary::HashForPath follows GUID");
        AssetDatabase::UninstallKeyResolver();
        check(assetkey::Resolve(NormalizePathKey(moved)) == ExpectedPathHash(moved),
              "resolver: uninstall restores default path-hash");

        // ---- end-to-end: 移動後の再起動相当 (新 DB で再走査 + resolver) でもキー不変 ----
        AssetDatabase db4;
        db4.ScanAndSync(root.wstring());
        db4.InstallAsKeyResolver();
        check(AnimationLibrary::HashForPath(moved) == guidFoo,
              "resolver: key stable across restart (rescan) after move");
        AssetDatabase::UninstallKeyResolver();
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== AssetDatabase self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== AssetDatabase self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
