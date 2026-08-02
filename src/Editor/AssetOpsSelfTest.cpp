#include "Editor/AssetOpsSelfTest.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "Editor/AssetOps.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EngineLoop.h"

namespace fs = std::filesystem;

namespace mye {

namespace {

void WriteDummy(const fs::path& path)
{
    std::ofstream f(path, std::ios::binary);
    f << "dummy";
}

} // namespace

bool RunAssetOpsSelfTest()
{
    MYE_LOG_INFO("==== AssetOps self test ====");
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
    const fs::path root = fs::temp_directory_path(ec) / L"mye_assetops_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    EngineContext ctx; // assetDb は null (テーブル更新は AssetDatabaseSelfTest が担当)

    // ---- (1) 複合サフィックス維持 + .meta 同伴 ----
    const fs::path walk = root / L"walk.anim.json";
    WriteDummy(walk);
    WriteDummy(walk.wstring() + L".meta");
    const std::wstring run = RenameAsset(ctx, walk.wstring(), "run");
    check(run == (root / L"run.anim.json").wstring(),
          "rename keeps compound suffix (.anim.json)");
    check(fs::exists(run, ec) && !fs::exists(walk, ec), "renamed file exists, old one is gone");
    check(fs::exists(run + L".meta", ec) && !fs::exists(walk.wstring() + L".meta", ec),
          "meta sidecar renamed alongside (GUID preserved)");

    // ---- (1b) M45c の複合サフィックス (.sound.json / .mixer.json) ----
    // kCompound に載っていないと "x.sound (1).json" になり ClassifyPath が壊れる
    const fs::path hit = root / L"hit.sound.json";
    WriteDummy(hit);
    const std::wstring hurt = RenameAsset(ctx, hit.wstring(), "hurt");
    check(hurt == (root / L"hurt.sound.json").wstring(),
          "rename keeps compound suffix (.sound.json)");
    check(AssetDatabase::ClassifyPath(hurt) == AssetType::Sound, "classify .sound.json as Sound");
    const fs::path mixer = root / L"main.mixer.json";
    WriteDummy(mixer);
    check(RenameAsset(ctx, mixer.wstring(), "game") == (root / L"game.mixer.json").wstring(),
          "rename keeps compound suffix (.mixer.json)");
    check(AssetDatabase::ClassifyPath(L"a.ogg") == AssetType::Audio, "classify .ogg as Audio");

    // ---- (2) 通常拡張子 ----
    const fs::path tex = root / L"tex.png";
    WriteDummy(tex);
    const std::wstring icon = RenameAsset(ctx, tex.wstring(), "icon");
    check(icon == (root / L"icon.png").wstring(), "rename keeps simple extension (.png)");

    // ---- (3) 同名衝突 → " (1)" 連番 ----
    const fs::path b = root / L"b.png";
    WriteDummy(b);
    const std::wstring collided = RenameAsset(ctx, b.wstring(), "icon");
    check(collided == (root / L"icon (1).png").wstring(),
          "name collision resolves with \" (1)\" suffix");

    // ---- (4) 不正文字は拒否 (ファイルは無傷) ----
    check(RenameAsset(ctx, icon, "bad/name").empty() && fs::exists(icon, ec),
          "invalid characters are rejected (file untouched)");

    // ---- (5) 同名 no-op ----
    check(RenameAsset(ctx, icon, "icon").empty(), "same name is a no-op");

    // ---- (6) フォルダリネーム (中身と .meta ごと移動) ----
    const fs::path sub = root / L"sub";
    fs::create_directories(sub, ec);
    WriteDummy(sub / L"child.png");
    WriteDummy((sub / L"child.png").wstring() + L".meta");
    const std::wstring moved = RenameAsset(ctx, sub.wstring(), "renamed");
    check(moved == (root / L"renamed").wstring() && fs::is_directory(moved, ec),
          "folder rename works");
    check(fs::exists(fs::path(moved) / L"child.png", ec)
              && fs::exists((fs::path(moved) / L"child.png").wstring() + L".meta", ec),
          "folder contents (incl. meta) move with the folder");

    fs::remove_all(root, ec);

    // ---- (7) スクリプトアタッチ (M31): 種別分類とガード ----
    // .cs は Script 種別に分類され、AttachScriptToEntity のディスパッチが成立する
    check(AssetDatabase::ClassifyPath(L"Foo.cs") == AssetType::Script,
          "classify .cs as Script (attach dispatch key)");
    check(AssetDatabase::ClassifyPath(L"Foo.png") != AssetType::Script,
          "classify .png as non-Script");
    // 非 .cs パスは種別ガードで即 false (scene/entity に触れないので null ctx でも安全)。
    // 実アタッチ (コンポーネント付与 + Undo) はエディタでの D&D 手動確認で担保する。
    {
        Selection sel;
        UndoStack undo;
        check(!AttachScriptToEntity(ctx, sel, undo, L"model.glb", kNullEntity),
              "AttachScriptToEntity rejects non-.cs payload (guard, no scene deref)");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== AssetOps self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== AssetOps self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
