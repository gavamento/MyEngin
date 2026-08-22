#include "Editor/AssetOpsSelfTest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "Editor/AssetOps.h"
#include "Editor/AssetPreviewCache.h"
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

    // ---- (8) M51i: Duplicate = 連番 + 新 GUID / Delete = ごみ箱 / ファイル操作 Undo ----
    {
        const fs::path r2 = fs::temp_directory_path(ec) / L"mye_assetops_selftest_m51i";
        fs::remove_all(r2, ec);
        fs::create_directories(r2, ec);
        Scene scene; // ファイル操作エントリは Scene に触れない (Undo 呼び出しのダミー)
        Selection sel;
        UndoStack undo;
        undo.SetFileOpContext(&ctx);

        // Duplicate: 連番命名 + 新 GUID 発行 (本体コピー・旧 .meta 非コピー)
        const fs::path hero = r2 / L"hero.png";
        WriteDummy(hero);
        const uint64_t srcGuid = AssetDatabase::EnsureMeta(hero.wstring());
        const std::wstring dup = DuplicateAsset(ctx, hero.wstring(), &undo);
        check(dup == (r2 / L"hero (1).png").wstring(), "duplicate numbers the copy");
        check(fs::exists(dup, ec) && fs::exists(dup + L".meta", ec),
              "duplicate writes the body and issues a sidecar .meta");
        AssetMeta dm;
        check(AssetDatabase::ReadMeta(dup + L".meta", dm) && dm.guid != 0 && dm.guid != srcGuid,
              "duplicate gets a fresh GUID (old .meta is not copied)");

        // 複合サフィックスも連番で維持される
        const fs::path unit = r2 / L"unit.actor.json";
        WriteDummy(unit);
        check(DuplicateAsset(ctx, unit.wstring(), nullptr)
                  == (r2 / L"unit (1).actor.json").wstring(),
              "duplicate keeps the compound suffix while numbering");

        // 宛先に孤児 .meta が残っていても旧 GUID を継がない (先に除去して新規発行)
        AssetMeta stale;
        stale.guid = 0xDEADBEEFDEADBEEFull;
        stale.type = AssetType::Texture;
        AssetDatabase::WriteMeta((r2 / L"hero (2).png.meta").wstring(), stale);
        const std::wstring dup2 = DuplicateAsset(ctx, hero.wstring(), nullptr);
        AssetMeta dm2;
        check(dup2 == (r2 / L"hero (2).png").wstring()
                  && AssetDatabase::ReadMeta(dup2 + L".meta", dm2) && dm2.guid != stale.guid,
              "duplicate does not inherit a stale orphan .meta GUID");

        // Duplicate の Undo/Redo (undo = ごみ箱へ / redo = 再複製で同じパスハッシュ GUID)
        const uint64_t serial0 = undo.StateSerial();
        undo.Undo(scene, sel);
        check(!fs::exists(dup, ec) && !fs::exists(dup + L".meta", ec),
              "undo duplicate recycles the copy together with its .meta");
        undo.Redo(scene, sel);
        AssetMeta dm3;
        check(fs::exists(dup, ec) && AssetDatabase::ReadMeta(dup + L".meta", dm3)
                  && dm3.guid == dm.guid,
              "redo duplicate re-copies and re-issues the same path-hash GUID");
        check(undo.StateSerial() == serial0,
              "file op entries do not disturb the scene dirty serial");

        // Rename の Undo/Redo (Relocate の往復 + .meta 同伴)
        const std::wstring renamed = RenameAsset(ctx, dup, "clone", &undo);
        check(renamed == (r2 / L"clone.png").wstring(), "rename with undo recording works");
        undo.Undo(scene, sel);
        check(fs::exists(dup, ec) && fs::exists(dup + L".meta", ec) && !fs::exists(renamed, ec),
              "undo rename moves the file (and .meta) back");
        undo.Redo(scene, sel);
        check(fs::exists(renamed, ec) && !fs::exists(dup, ec),
              "redo rename applies the forward relocate again");

        // 逆操作先の消滅は WARN + no-op (何も上書きせず、落ちない)
        fs::remove(renamed, ec);
        fs::remove(renamed + L".meta", ec);
        undo.Undo(scene, sel); // clone.png はもう居ない
        check(!fs::exists(dup, ec) && !fs::exists(renamed, ec),
              "undo with a missing target is a safe no-op");

        // Create の Undo/Redo (undo = ごみ箱へ / redo = 内容の書き戻し)
        const std::wstring made = CreateActorAsset(ctx, r2.wstring(), "UndoActor");
        std::string bytes0;
        {
            std::ifstream f{ fs::path(made), std::ios::binary };
            bytes0.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        RecordAssetCreated(undo, made);
        undo.Undo(scene, sel);
        check(!fs::exists(made, ec), "undo create recycles the created asset");
        undo.Redo(scene, sel);
        std::string bytes1;
        {
            std::ifstream f{ fs::path(made), std::ios::binary };
            bytes1.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        check(fs::exists(made, ec) && bytes0 == bytes1 && !bytes0.empty(),
              "redo create restores the exact original content");

        // Delete: 本体 + .meta がごみ箱へ / フォルダは配下ごと / 不在は静かに失敗
        check(DeleteAssetToRecycleBin(ctx, hero.wstring()) && !fs::exists(hero, ec)
                  && !fs::exists(hero.wstring() + L".meta", ec),
              "delete moves the asset and its .meta to the recycle bin");
        const fs::path delDir = r2 / L"deldir";
        fs::create_directories(delDir, ec);
        WriteDummy(delDir / L"a.png");
        AssetDatabase::EnsureMeta((delDir / L"a.png").wstring());
        check(DeleteAssetToRecycleBin(ctx, delDir.wstring()) && !fs::exists(delDir, ec),
              "delete recycles a folder recursively");
        check(!DeleteAssetToRecycleBin(ctx, (r2 / L"nope.png").wstring()),
              "delete of a missing path fails gracefully");

        fs::remove_all(r2, ec);
    }

    // ---- マテリアルプレビュー (M53) ----
    // 描画そのものは D3D 依存でヘッドレスに回せないので、絵を決める **入力** 側だけ見る:
    // どのパスがプレビュー対象になるか、と「保存する JSON → Material」の変換
    {
        check(AssetPreviewCache::IsMaterialPath(L"C:\\a\\hero.mat.json")
                  && AssetPreviewCache::IsMaterialPath(L"C:\\a\\HERO.MAT.JSON")
                  && !AssetPreviewCache::IsMaterialPath(L"C:\\a\\hero.prefab.json")
                  && !AssetPreviewCache::IsMaterialPath(L"C:\\a\\mat.json"),
              "preview: .mat.json is detected case-insensitively (and only as a suffix)");
        check(AssetPreviewCache::IsPreviewable(L"C:\\a\\hero.mat.json")
                  && AssetPreviewCache::IsPreviewable(L"C:\\a\\hero.glb")
                  && AssetPreviewCache::IsPreviewable(L"C:\\a\\hero.actor.json")
                  && !AssetPreviewCache::IsPreviewable(L"C:\\a\\hero.png")
                  && !AssetPreviewCache::IsPreviewable(L"C:\\a\\hero.scene.json"),
              "preview: materials joined the previewable set without dragging others in");

        // ヘッドレスなので TextureLibrary::Init は呼ばない = GPU 生成は全て空振りする。
        // 見たいのは数値フィールドの写りと、テクスチャ無し (=White) 経路が落ちないこと
        RenderResources res;
        Material m;
        const char* json = R"({"engine":"MyEngine","material":1,"name":"t",
            "shader":"forward_lit","baseColor":[0.25,0.5,0.75,1.0],"metallic":0.5,
            "roughness":0.125,"emissive":2.5,"texture":"","normalMap":"","transparent":true})";
        check(MaterialLibrary::MaterialFromJsonText(json, res.textures, L"", m)
                  && m.baseColor.x == 0.25f && m.baseColor.y == 0.5f && m.baseColor.z == 0.75f
                  && m.metallic == 0.5f && m.roughness == 0.125f && m.emissiveIntensity == 2.5f
                  && m.transparent == 1,
              "preview: MaterialFromJsonText maps every editable field");
        Material def;
        check(MaterialLibrary::MaterialFromJsonText("{}", res.textures, L"", def)
                  && def.metallic == 0.0f && def.roughness == 0.5f
                  && def.emissiveIntensity == 0.0f && def.transparent == 0,
              "preview: missing keys fall back to the same defaults as LoadFromFile");
        Material broken = m;
        check(!MaterialLibrary::MaterialFromJsonText("{ not json", res.textures, L"", broken)
                  && !MaterialLibrary::MaterialFromJsonText("[1,2]", res.textures, L"", broken),
              "preview: malformed / non-object json is rejected (caller keeps the last good one)");

        // 匿名登録はプレビュー専用 — 参照ピッカー (Enumerate) に漏れてはいけない
        const size_t before = res.materials.Enumerate().size();
        const AssetID anon = res.materials.RegisterAnonymous(0x9E3779B97F4A7C15ull, m);
        check(!anon.IsNull() && res.materials.Get(anon) != nullptr
                  && res.materials.Enumerate().size() == before,
              "preview: RegisterAnonymous is resolvable by Get but stays out of Enumerate");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== AssetOps self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== AssetOps self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
