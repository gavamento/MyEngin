#include "Engine/Engine/AssetDatabaseSelfTest.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "Engine/Core/AssetGuidResolver.h"
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

    // ---- 複合サフィックスの分類 (M48d で Actor を追加) ----
    // .actor.json は .prefab.json とは別種別だが、どちらも PrefabLibrary が扱う。
    // 種別名の往復も見る (.meta には TypeName の文字列で保存されるため)
    check(AssetDatabase::ClassifyPath(L"x\\Goblin.actor.json") == AssetType::Actor
              && AssetDatabase::ClassifyPath(L"x\\Goblin.prefab.json") == AssetType::Prefab
              && AssetDatabase::ClassifyPath(L"x\\Goblin.scene.json") == AssetType::Scene,
          "actor/prefab/scene are distinguished by compound suffix");
    check(AssetDatabase::ClassifyPath(L"x\\Goblin.ACTOR.JSON") == AssetType::Actor,
          "compound suffix classification ignores case");
    // M48j: .component.schema.json は .scene.json 等と紛れない (最長サフィックス優先)
    check(AssetDatabase::ClassifyPath(L"x\\Health.component.schema.json") == AssetType::Schema
              && AssetDatabase::ClassifyPath(L"x\\Health.json") == AssetType::Unknown,
          "component schema is a distinct asset type");
    for (AssetType t : { AssetType::Actor, AssetType::Prefab, AssetType::Sound, AssetType::Mixer,
                         AssetType::Schema, AssetType::PhysMat }) {
        check(AssetDatabase::ParseTypeName(AssetDatabase::TypeName(t)) == t,
              "asset type name round-trips through .meta");
    }
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
        // ---- GuidResolver (M39a): GUID → 現在パス (.mat/.controller のサブ参照解決) ----
        check(assetguid::ResolvePath(guidFoo) == moved,
              "guid resolver: guid resolves to current (moved) path");
        check(assetguid::ResolvePath(0xDEADBEEFDEADBEEFull).empty(),
              "guid resolver: unknown guid resolves to empty");
        check(assetguid::ResolvePath(0).empty(), "guid resolver: guid 0 resolves to empty");
        AssetDatabase::UninstallKeyResolver();
        check(assetkey::Resolve(NormalizePathKey(moved)) == ExpectedPathHash(moved),
              "resolver: uninstall restores default path-hash");
        check(assetguid::ResolvePath(guidFoo).empty(),
              "guid resolver: uninstall restores default (empty)");

        // ---- end-to-end: 移動後の再起動相当 (新 DB で再走査 + resolver) でもキー不変 ----
        AssetDatabase db4;
        db4.ScanAndSync(root.wstring());
        db4.InstallAsKeyResolver();
        check(AnimationLibrary::HashForPath(moved) == guidFoo,
              "resolver: key stable across restart (rescan) after move");
        AssetDatabase::UninstallKeyResolver();
    }

    // ---- .meta v2 (M39b): テクスチャインポート設定の round-trip + v1 後方互換 ----
    {
        const std::wstring texPng = (root / L"tex.png").wstring();
        const std::wstring texMeta = texPng + L".meta";
        WriteDummy(texPng);

        // v1 相当 (tex キー無し) を手書き → 既定値 (= 従来挙動) で読める
        {
            std::ofstream f(texMeta);
            f << "{\n  \"guid\": \"00000000000000aa\",\n  \"type\": \"texture\",\n"
                 "  \"version\": 1\n}";
        }
        AssetMeta v1;
        check(AssetDatabase::ReadMeta(texMeta, v1) && v1.guid == 0xAA && v1.version == 1,
              "v1 meta (no tex block) still reads");
        check(v1.tex.srgb == 0 && v1.tex.generateMips == 1 && v1.tex.compress == 0,
              "v1 meta yields default import settings");

        // v2 書き → 読み round-trip (テクスチャは WriteMeta が version 2 + tex を書く)
        v1.tex.srgb = 2;
        v1.tex.generateMips = 0;
        v1.tex.compress = 1;
        check(AssetDatabase::WriteMeta(texMeta, v1), "v2 meta write");
        AssetMeta v2;
        check(AssetDatabase::ReadMeta(texMeta, v2) && v2.version == 2 && v2.guid == 0xAA,
              "v2 meta reads back (version bumped, guid preserved)");
        check(v2.tex.srgb == 2 && v2.tex.generateMips == 0 && v2.tex.compress == 1,
              "v2 import settings round-trip");

        // importmeta フック: install 中は .meta の設定が返り、uninstall で未解決に戻る
        AssetDatabase db5;
        db5.InstallAsKeyResolver();
        importmeta::TextureImportSettings s;
        check(importmeta::Resolve(texPng, s) && s.srgb == 2 && s.generateMips == 0
                  && s.compress == 1,
              "importmeta resolver reads settings from .meta");
        importmeta::TextureImportSettings none;
        check(!importmeta::Resolve((root / L"missing.png").wstring(), none),
              "importmeta resolver: no .meta -> unresolved (defaults)");
        AssetDatabase::UninstallKeyResolver();
        check(!importmeta::Resolve(texPng, s), "importmeta uninstall restores default");
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
