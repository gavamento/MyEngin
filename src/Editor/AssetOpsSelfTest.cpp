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
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"

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

    // ---- (1c) M48d の複合サフィックス (.actor.json) ----
    const fs::path goblin = root / L"goblin.actor.json";
    WriteDummy(goblin);
    const std::wstring orc = RenameAsset(ctx, goblin.wstring(), "orc");
    check(orc == (root / L"orc.actor.json").wstring(),
          "rename keeps compound suffix (.actor.json)");
    check(AssetDatabase::ClassifyPath(orc) == AssetType::Actor, "classify .actor.json as Actor");
    // 連番も複合サフィックスを保つ (壊れると "orc.actor (1).json" になり種別判定が落ちる)
    WriteDummy(root / L"dup.actor.json");
    const fs::path dupSrc = root / L"sub" / L"dup.actor.json";
    fs::create_directories(root / L"sub", ec);
    WriteDummy(dupSrc);
    const std::wstring dupMoved = MoveAssetToFolder(ctx, dupSrc.wstring(), root.wstring());
    check(dupMoved == (root / L"dup (1).actor.json").wstring(),
          "numbering keeps the compound suffix (.actor.json)");

    // ---- (1d) Create > Actor が実際に読み込める最小アセットを吐くこと (M48d) ----
    // ここが崩れると「作った直後のアセットが配置できない」形で壊れる
    {
        const std::wstring created = CreateActorAsset(ctx, root.wstring(), "Goblin King");
        check(!created.empty() && fs::exists(created, ec), "Create > Actor writes a file");
        check(AssetDatabase::ClassifyPath(created) == AssetType::Actor,
              "the created file is classified as Actor");
        PrefabLibrary lib;
        const uint64_t h = lib.LoadFromFile(created);
        check(h != 0 && lib.Get(h) && lib.Get(h)->actorFormat && lib.Get(h)->entities.size() == 1,
              "the created actor loads back as a single-root actor-format asset");
        Scene s;
        const uint64_t rootFid = Prefab::Instantiate(s, lib, h, 0);
        s.GetWorld().ApplyStructuralChanges();
        GameObject g = s.FindByFileId(rootFid);
        check(g && s.GetWorld().AliveCount() == 1
                  && std::string(s.GetWorld().GetName(g.Id())) == "Goblin King",
              "the created actor instantiates to one entity named after the asset");
    }
    check(AssetDatabase::ClassifyPath(L"a.ogg") == AssetType::Audio, "classify .ogg as Audio");

    // ---- (1e) M50b: 緩いサニタイズ (日本語を通す) + Create の同名連番 ----
    // Create Prefab (Hierarchy) と Create > Actor が共有する経路。上書きは
    // パスハッシュ再登録 = 既存インスタンスの黙った張り替えなので、連番は安全装置
    {
        check(SanitizeFileName("敵/ボス:*?\"<>|", "Prefab") == "敵ボス",
              "sanitize strips only forbidden characters (Japanese passes)");
        check(SanitizeFileName("  ", "Prefab") == std::string("Prefab")
                  && SanitizeFileName("name... ", "Prefab") == "name",
              "sanitize falls back when empty and trims trailing dots/spaces");
        const std::wstring first = CreateActorAsset(ctx, root.wstring(), "武器");
        const std::wstring second = CreateActorAsset(ctx, root.wstring(), "武器");
        check(first == (root / L"武器.actor.json").wstring() && fs::exists(first, ec),
              "Create > Actor accepts a Japanese name");
        check(second == (root / L"武器 (1).actor.json").wstring() && fs::exists(second, ec)
                  && fs::exists(first, ec),
              "creating the same name again numbers instead of overwriting (path-hash safety)");
        check(MakeUniqueAssetPath(root.wstring(), L"武器.actor.json")
                  == (root / L"武器 (2).actor.json").wstring(),
              "MakeUniqueAssetPath keeps the compound suffix while numbering");
    }

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
