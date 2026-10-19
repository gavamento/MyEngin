//====================================================================================
//                          Sprite2DAssetSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライト資産とソート層の回帰テスト
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Asset/ImportMetaResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Scene/SortingLayers.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace mye {
namespace {

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary);
    f << bytes;
}

importmeta::SpriteSlice MakeSlice(uint32_t id, const char* name, int x, int y, int w, int h)
{
    importmeta::SpriteSlice s;
    s.id = id;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    return s;
}

// 実リポジトリの既存 .meta を探す (selftest の cwd はリポジトリ直下とは限らない)
fs::path FindRepoMeta()
{
    const fs::path rel = fs::path("assets") / "models" / "player_fp_v01" / "previews" / "first_person.png.meta";
    std::error_code ec;
    fs::path base = fs::current_path(ec);
    for (int up = 0; up < 6 && !base.empty(); ++up) {
        if (fs::exists(base / rel, ec)) {
            return base / rel;
        }
        base = base.parent_path();
    }
    return {};
}

} // namespace

bool RunSprite2DAssetSelfTest()
{
    MYE_LOG_INFO("==== Sprite2DAsset self test ====");
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
    const fs::path root = fs::temp_directory_path(ec) / L"mye_sprite2d_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- (a) .meta の往復 ----
    {
        // スプライト設定の無いテクスチャの .meta は従来とバイト同一 (キーは辞書順、末尾改行なし)
        const std::string legacy =
            "{\n  \"guid\": \"6dfc018d03cea5b3\",\n  \"tex\": {\n    \"compress\": 0,\n    \"generateMips\": 1,\n"
            "    \"srgb\": 0\n  },\n  \"type\": \"texture\",\n  \"version\": 2\n}";
        const fs::path legacyIn = root / L"legacy.png.meta";
        const fs::path legacyOut = root / L"legacy_out.png.meta";
        WriteAll(legacyIn, legacy);
        AssetMeta m;
        check(AssetDatabase::ReadMeta(legacyIn.wstring(), m), "legacy meta reads");
        check(m.tex.filter == 0 && m.tex.pixelsPerUnit == 100 && m.tex.sprites.empty(),
              "legacy meta: sprite settings are defaults");
        check(AssetDatabase::WriteMeta(legacyOut.wstring(), m) && ReadAll(legacyOut) == legacy,
              "legacy meta: Read -> Write is byte-identical (no sprites / filter / ppu keys)");

        // リポジトリ内の実 .meta (受け入れ条件)
        const fs::path repoMeta = FindRepoMeta();
        if (repoMeta.empty()) {
            MYE_LOG_INFO("  SKIP: repo .meta not found from cwd (first_person.png.meta)");
        } else {
            AssetMeta rm;
            const fs::path repoOut = root / L"repo_out.png.meta";
            const bool same = AssetDatabase::ReadMeta(repoMeta.wstring(), rm)
                && AssetDatabase::WriteMeta(repoOut.wstring(), rm) && ReadAll(repoOut) == ReadAll(repoMeta);
            check(same, "repo first_person.png.meta: Read -> Write is byte-identical");
        }

        // sprites / filter / ppu 付きの往復で同一 JSON
        AssetMeta s;
        s.guid = 0xAB12;
        s.type = AssetType::Texture;
        s.version = 2;
        s.tex.filter = 2;
        s.tex.pixelsPerUnit = 16;
        s.tex.sprites.push_back(MakeSlice(1, "idle_0", 0, 0, 16, 16));
        s.tex.sprites.push_back(MakeSlice(2, "idle_1", 16, 0, 16, 16));
        importmeta::SpriteSlice nine = MakeSlice(5, "panel", 32, 0, 24, 24);
        nine.pivotX = 0.25f;
        nine.pivotY = 0.75f;
        nine.borderL = 4;
        nine.borderB = 5;
        nine.borderR = 6;
        nine.borderT = 7;
        s.tex.sprites.push_back(nine);
        const fs::path sp1 = root / L"s1.png.meta";
        const fs::path sp2 = root / L"s2.png.meta";
        check(AssetDatabase::WriteMeta(sp1.wstring(), s), "sprite meta writes");
        AssetMeta back;
        check(AssetDatabase::ReadMeta(sp1.wstring(), back) && back.tex.sprites.size() == 3
                  && back.tex.filter == 2 && back.tex.pixelsPerUnit == 16,
              "sprite meta reads back (3 slices, filter, ppu)");
        const importmeta::SpriteSlice& b2 = back.tex.sprites[2];
        check(b2.id == 5 && std::strcmp(b2.name, "panel") == 0 && b2.x == 32 && b2.w == 24 && b2.pivotX == 0.25f
                  && b2.pivotY == 0.75f && b2.borderL == 4 && b2.borderB == 5 && b2.borderR == 6
                  && b2.borderT == 7,
              "slice fields round-trip (rect / pivot / border / id)");
        check(AssetDatabase::WriteMeta(sp2.wstring(), back) && ReadAll(sp1) == ReadAll(sp2),
              "sprite meta: Write -> Read -> Write is the same JSON");
        check(back.tex.NextSpriteId() == 6, "NextSpriteId = max id + 1");
    }

    // ---- (b) 改名しても同じ AssetID、新規 id は最大 + 1 ----
    {
        const fs::path tex = root / L"hero.png";
        WriteAll(tex, "dummy");
        AssetMeta m;
        m.guid = 0xAB34CD56EF789012ull;
        m.type = AssetType::Texture;
        m.version = 2;
        m.tex.pixelsPerUnit = 32;
        m.tex.filter = 2;
        m.tex.sprites.push_back(MakeSlice(1, "idle", 0, 0, 32, 32));
        m.tex.sprites.push_back(MakeSlice(2, "run", 32, 0, 32, 32));
        AssetDatabase::WriteMeta((tex.wstring() + L".meta"), m);

        AssetDatabase db;
        db.ScanAndSync(root.wstring());
        db.InstallAsKeyResolver();

        SpriteLibrary lib;
        check(lib.Register(tex.wstring()) == 2, "Register: 2 slices");
        const AssetID runId = SpriteLibrary::SliceId(tex.wstring(), 2);
        char expectKey[64];
        std::snprintf(expectKey, sizeof(expectKey), "guid://%016llx#sprite2",
                      static_cast<unsigned long long>(m.guid));
        check(runId.value == HashStr(expectKey), "slice AssetID = HashStr(guid://<16hex>#sprite<id>)");
        const SpriteDef* run = lib.Get(runId);
        check(run != nullptr && std::strcmp(run->name, "run") == 0 && run->rect[0] == 32 && run->ppu == 32
                  && run->filter == 2 && run->texture.value == m.guid,
              "Get(slice): def carries rect / ppu / filter / texture");

        // 改名 + 新規追加。id 2 の AssetID は変わらず、新規は最大 + 1 = 3
        AssetMeta edit;
        AssetDatabase::ReadMeta(tex.wstring() + L".meta", edit);
        std::snprintf(edit.tex.sprites[1].name, sizeof(edit.tex.sprites[1].name), "%s", "sprint");
        const uint32_t added = edit.tex.NextSpriteId();
        edit.tex.sprites.push_back(MakeSlice(added, "jump", 64, 0, 32, 32));
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", edit);
        check(added == 3, "new slice id = max + 1");
        check(lib.Register(tex.wstring()) == 3, "re-Register: 3 slices");
        const SpriteDef* sprint = lib.Get(runId);
        check(SpriteLibrary::SliceId(tex.wstring(), 2).value == runId.value && sprint != nullptr
                  && std::strcmp(sprint->name, "sprint") == 0,
              "rename keeps the AssetID (name changes only)");
        check(lib.FindByName(tex.wstring(), "jump") != nullptr && lib.FindByName(tex.wstring(), "run") == nullptr,
              "FindByName follows the rename");

        // 削除した id は引けなくなる (マゼンタへ落とすのは呼び手)
        edit.tex.sprites.erase(edit.tex.sprites.begin());
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", edit);
        lib.Register(tex.wstring());
        check(lib.Get(SpriteLibrary::SliceId(tex.wstring(), 1)) == nullptr, "removed slice is unresolved (nullptr)");
        check(lib.Get(runId) != nullptr, "other slices keep their ids after a delete");

        // テクスチャ自身の AssetID は全面。新しいライブラリでも GUID から遅延登録できる
        SpriteLibrary fresh;
        const SpriteDef* full = fresh.Get(AssetID{ m.guid });
        check(full != nullptr && full->IsFullTexture() && full->ppu == 32 && full->pivot[0] == 0.5f,
              "texture AssetID resolves lazily as the full sprite");
        check(fresh.Get(runId) != nullptr, "slices registered by the lazy lookup");
        check(fresh.Get(AssetID{ 0x1234 }) == nullptr && fresh.Get(AssetID{}) == nullptr,
              "unknown / null AssetID is nullptr");

        std::vector<SpriteListing> list;
        fresh.Enumerate(list);
        bool sorted = !list.empty();
        for (size_t i = 1; i < list.size(); ++i) {
            sorted = sorted && list[i - 1].displayName <= list[i].displayName;
        }
        check(list.size() == 3 && sorted && list[0].displayName == "hero" && list[1].displayName == "hero#jump",
              "Enumerate: 'tex' and 'tex#name', sorted by display name");

        // hash 対象外・Install の入れ替え
        spritelib::Install(&lib);
        check(spritelib::Library() == &lib, "spritelib::Install connects");
        spritelib::Install(nullptr);
        check(spritelib::Library() == nullptr, "spritelib::Install(nullptr) disconnects");

        AssetDatabase::UninstallKeyResolver();
    }

    // ---- (c)(d) ソート層 ----
    {
        SortingLayers sl;
        check(sl.Count() == 1 && sl.At(0).id == 0 && std::strcmp(sl.At(0).name, "Default") == 0,
              "SortingLayers: starts with Default only");
        check(sl.Add("Background") == 1 && sl.Add("Props") == 2 && sl.Add("Player") == 3, "Add assigns 1, 2, 3");
        check(!sl.Remove(0) && !sl.Remove(99), "Remove: Default and unknown ids are refused");
        check(sl.Remove(2) && sl.Count() == 3, "Remove(2)");
        check(sl.Add("Effects") == 2, "Add reuses the smallest free id (2)");

        while (sl.Add("x") >= 0) {
        }
        check(sl.Count() == SortingLayers::kMaxLayers, "capacity is 32");
        check(sl.Add("overflow") == -1, "Add at capacity returns -1");
        uint32_t mask = 0;
        for (int i = 0; i < sl.Count(); ++i) {
            mask |= 1u << sl.At(i).id;
        }
        check(mask == 0xFFFFFFFFu, "32 layers use ids 0..31 exactly once");

        // 描画順 = 配列順。表に無い id は Default
        SortingLayers order;
        order.Add("Bg");
        order.Add("Fg");
        check(order.OrderOf(0) == 0 && order.OrderOf(1) == 1 && order.OrderOf(2) == 2, "OrderOf follows the array");
        check(order.Move(2, 0), "Move(2 -> 0)");
        check(order.OrderOf(2) == 0 && order.OrderOf(0) == 1 && order.OrderOf(1) == 2,
              "OrderOf follows the array after Move (id unchanged, order changed)");
        check(order.OrderOf(31) == order.OrderOf(0), "OrderOf: unknown id is treated as Default");
        check(!order.Move(0, 3) && !order.Move(-1, 0), "Move: out of range is refused");
        check(order.Rename(1, "Back") && order.IdOf("Back") == 1 && order.IdOf("Bg") == -1, "Rename / IdOf");

        // 保存と読み直し。他キーを壊さない
        const fs::path settings = root / L"project_settings.json";
        WriteAll(settings, "{\n  \"tags\": [\"a\", \"b\"]\n}");
        SortingLayers disk;
        disk.Load(root.wstring(), true);
        check(disk.Count() == 1 && !disk.DiffersFromDisk(), "Load: no key -> Default only, clean");
        disk.Add("Sky");
        check(disk.DiffersFromDisk(), "DiffersFromDisk after an edit");
        check(disk.Save(root.wstring()), "Save");
        disk.Load(root.wstring(), true);
        check(disk.Count() == 2 && disk.At(1).id == 1 && std::strcmp(disk.At(1).name, "Sky") == 0
                  && !disk.DiffersFromDisk(),
              "Save -> Load round trip, clean again");
        nlohmann::json j = nlohmann::json::parse(ReadAll(settings));
        check(j.contains("tags") && j["tags"].size() == 2 && j.contains("sortingLayers"),
              "Save keeps the other keys of project_settings.json");

        // 壊れた表: Default 欠落 / 重複 id / 範囲外 id
        WriteAll(settings,
                 "{\"sortingLayers\":[{\"id\":5,\"name\":\"A\"},{\"id\":5,\"name\":\"dup\"},"
                 "{\"id\":40,\"name\":\"big\"},{\"id\":-1,\"name\":\"neg\"},{\"id\":7}]}");
        SortingLayers broken;
        broken.Load(root.wstring(), true);
        check(broken.Count() == 3 && broken.At(0).id == 0 && broken.At(1).id == 5 && broken.At(2).id == 7,
              "Load repairs: Default prepended, duplicate / out-of-range ids dropped");
    }

    // ---- (e) Sprite2D の登録とローカル矩形 ----
    {
        RegisterBuiltinComponents(); // 多重呼び出しは無害
        // TypeId は登録順そのもの。80 (TwoBoneIK) の次
        check(Sprite2DComponent::sTypeId == 81, "Sprite2D is TypeId 81");
        const ComponentDesc& d = ComponentRegistry::Get().Desc(Sprite2DComponent::sTypeId);
        check((d.flags & kComponentNoHash) != 0, "Sprite2D is kComponentNoHash (render-only)");

        // 全面スプライト: 画素寸法 / ppu、pivot 中央
        SpriteDef full;
        full.ppu = 32;
        float r[4] = {};
        Sprite2DRenderer::LocalRect(full, 64, 32, r);
        check(r[0] == -1.0f && r[1] == -0.5f && r[2] == 1.0f && r[3] == 0.5f, "LocalRect: full texture 64x32 @ ppu 32, centered");

        // スライス: rect の寸法を使い、pivot (左下原点) を原点に取る
        SpriteDef slice;
        slice.rect[0] = 16;
        slice.rect[1] = 8;
        slice.rect[2] = 32;
        slice.rect[3] = 16;
        slice.ppu = 16;
        slice.pivot[0] = 0.0f;
        slice.pivot[1] = 1.0f;
        Sprite2DRenderer::LocalRect(slice, 1024, 1024, r);
        check(r[0] == 0.0f && r[1] == -1.0f && r[2] == 2.0f && r[3] == 0.0f, "LocalRect: slice 32x16 @ ppu 16, pivot top-left");
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== Sprite2DAsset self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Sprite2DAsset self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
