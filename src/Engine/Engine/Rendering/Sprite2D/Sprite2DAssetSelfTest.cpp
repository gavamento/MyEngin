//====================================================================================
//                          Sprite2DAssetSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライト資産とソート層の回帰テスト
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.h"

#include <algorithm>
#include <cmath>
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
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Scene/SortingLayers.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"

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
        check(!run->texSrgbOn, "Get(slice): srgb auto does not raise the Inspector's sRGB warning flag");

        // srgb = on の .meta は texSrgbOn が立つ (全面スプライトにも)。2D は UNORM 前提なので Inspector が警告する
        {
            AssetMeta srgb;
            AssetDatabase::ReadMeta(tex.wstring() + L".meta", srgb);
            srgb.tex.srgb = 1;
            AssetDatabase::WriteMeta(tex.wstring() + L".meta", srgb);
            SpriteLibrary srgbLib;
            srgbLib.Register(tex.wstring());
            const SpriteDef* srgbRun = srgbLib.Get(runId);
            const SpriteDef* srgbFull = srgbLib.Get(AssetID{ m.guid });
            check(srgbRun != nullptr && srgbRun->texSrgbOn && srgbFull != nullptr && srgbFull->texSrgbOn,
                  "srgb = on in the .meta sets texSrgbOn on slices and the full-texture sprite");
        }

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

    // ---- (f) SortingGroup2D / SpriteMask2D の登録、Sliced / Tiled の分割 (M94f) ----
    {
        check(SortingGroup2DComponent::sTypeId == 82 && SpriteMask2DComponent::sTypeId == 83,
              "SortingGroup2D / SpriteMask2D are TypeId 82 / 83");
        const ComponentDesc& g = ComponentRegistry::Get().Desc(SortingGroup2DComponent::sTypeId);
        const ComponentDesc& mk = ComponentRegistry::Get().Desc(SpriteMask2DComponent::sTypeId);
        check((g.flags & kComponentNoHash) != 0 && (mk.flags & kComponentNoHash) != 0,
              "group / mask are kComponentNoHash (render-only)");
        const SpriteMask2DComponent defMask;
        check(defMask.alphaCutoff == 0.5f && defMask.rangeBack < -1000000 && defMask.rangeFront > 1000000,
              "mask defaults: cutoff 0.5, range covers everything");

        using Q = Sprite2DRenderer::SliceQuad;
        Sprite2DRenderer::SliceInput in;
        in.rect[2] = 16.0f; // 16x16 の絵 (テクスチャも 16x16)
        in.rect[3] = 16.0f;
        in.texW = 16.0f;
        in.texH = 16.0f;
        in.ppu = 16.0f;
        in.sizeX = 3.0f;
        in.sizeY = 1.0f;
        std::vector<Q> quads;

        // border 4 px の Sliced 3x1: 角は 4/16 = 0.25 単位の原寸、辺と中央が伸びる
        for (int i = 0; i < 4; ++i) {
            in.border[i] = 4.0f;
        }
        in.tiled = false;
        check(!Sprite2DRenderer::BuildSliceQuads(in, quads) && quads.size() == 9, "Sliced: 9 quads");
        float cornerW = 0.0f;
        float centerW = 0.0f;
        float centerH = 0.0f;
        for (const Q& q : quads) {
            if (q.x0 == -1.5f && q.y0 == -0.5f) {
                cornerW = q.x1 - q.x0; // 左下の角
            }
            if (q.x0 == -1.25f && q.y0 == -0.25f) {
                centerW = q.x1 - q.x0; // 中央
                centerH = q.y1 - q.y0;
            }
        }
        check(cornerW == 0.25f && centerW == 2.5f && centerH == 0.5f,
              "Sliced: corners keep 4px / ppu = 0.25, the center stretches to 2.5 x 0.5");

        // border 全 0 の Sliced は 1 枚 (size いっぱい、UV は全面)
        for (int i = 0; i < 4; ++i) {
            in.border[i] = 0.0f;
        }
        check(!Sprite2DRenderer::BuildSliceQuads(in, quads) && quads.size() == 1 && quads[0].x0 == -1.5f
                  && quads[0].x1 == 1.5f && quads[0].u0 == 0.0f && quads[0].u1 == 1.0f,
              "Sliced with border 0 = one quad over the full size");

        // size が角の合計より小さいときは角を縮める
        for (int i = 0; i < 4; ++i) {
            in.border[i] = 8.0f; // 角 0.5 + 0.5 = 1.0
        }
        in.sizeX = 0.5f;
        in.sizeY = 0.5f;
        Sprite2DRenderer::BuildSliceQuads(in, quads);
        float minX = 1.0e9f;
        float maxX = -1.0e9f;
        for (const Q& q : quads) {
            minX = (std::min)(minX, q.x0);
            maxX = (std::max)(maxX, q.x1);
        }
        check(minX == -0.25f && maxX == 0.25f, "Sliced: corners shrink to fit a size smaller than the borders");

        // Tiled 3x2 (border 0): 原寸 1x1 の絵が 3x2 枚、UV は全面
        for (int i = 0; i < 4; ++i) {
            in.border[i] = 0.0f;
        }
        in.sizeX = 3.0f;
        in.sizeY = 2.0f;
        in.tiled = true;
        check(!Sprite2DRenderer::BuildSliceQuads(in, quads) && quads.size() == 6, "Tiled 3x2: 6 quads");
        bool uvFull = true;
        for (const Q& q : quads) {
            uvFull = uvFull && q.u0 == 0.0f && q.u1 == 1.0f && q.v0 == 0.0f && q.v1 == 1.0f
                     && q.x1 - q.x0 == 1.0f && q.y1 - q.y0 == 1.0f;
        }
        check(uvFull, "Tiled 3x2: every tile is the full image at original size");

        // 端のタイルは UV を切る (右端 0.5 幅 -> u1 = 0.5)。タイルは左から並ぶ
        in.sizeX = 2.5f;
        in.sizeY = 1.0f;
        Sprite2DRenderer::BuildSliceQuads(in, quads);
        const Q* last = nullptr;
        for (const Q& q : quads) {
            if (last == nullptr || q.x0 > last->x0) {
                last = &q;
            }
        }
        check(quads.size() == 3 && last != nullptr && last->x1 - last->x0 == 0.5f && last->u0 == 0.0f
                  && last->u1 == 0.5f,
              "Tiled: the last tile is clipped in UV (0.5 wide -> u 0..0.5)");

        // 上限 (4096 頂点 = 682 枚) で切る
        in.sizeX = 1000.0f;
        in.sizeY = 1000.0f;
        const bool capped = Sprite2DRenderer::BuildSliceQuads(in, quads);
        check(capped && quads.size() * 6 <= Sprite2DRenderer::kMaxSpriteVertices && quads.size() > 600,
              "Tiled: capped at 4096 vertices");

        // 不正な size は何も出さない
        in.sizeX = 0.0f;
        check(!Sprite2DRenderer::BuildSliceQuads(in, quads) && quads.empty(), "size <= 0 emits nothing");
    }

    // ---- (g) Light2D の登録と、収集 -> ライト群 (M94o)。GPU を使わない部分だけ ----
    {
        check(Light2DComponent::sTypeId == 85, "Light2D is TypeId 85");
        const ComponentDesc& ld = ComponentRegistry::Get().Desc(Light2DComponent::sTypeId);
        check((ld.flags & kComponentNoHash) != 0, "Light2D is kComponentNoHash (render-only)");

        // 層 id 1 を一時的に足す (終わったら消す)。Default (0) と合わせて 2 層
        SortingLayers& layers = SortingLayers::Get();
        const int32_t fgId = layers.Add("LightTestFg");
        check(fgId > 0, "Light2D test: an extra sorting layer is available");

        RenderResources resources;
        Sprite2DRenderer renderer;
        const auto addSprite = [](World& w, int32_t layerId, bool lit) {
            const EntityID e = w.CreateEntity("sprite");
            auto* sp = w.AddComponent<Sprite2DComponent>(e);
            sp->sortingLayerId = layerId;
            sp->lit = lit;
            return e;
        };
        const auto addLight = [](World& w, int32_t type, uint32_t mask) {
            const EntityID e = w.CreateEntity("light");
            auto* lc = w.AddComponent<Light2DComponent>(e);
            lc->type = type;
            lc->targetLayerMask = mask;
            return e;
        };

        // Light2D が無いシーン = presence gate (ライト群もライト頂点も作らない)
        {
            World w;
            addSprite(w, 0, true);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(!l.HasLights() && l.lights.empty() && l.lightVertices.empty() && l.groupLights.empty(),
                  "Light2D: no light in the scene -> no light groups (presence gate)");
        }

        // Global (0.3) + Spot (Default 層だけ) と、Default / 別層の lit スプライト
        {
            World w;
            addSprite(w, 0, true);
            addSprite(w, fgId, true);
            addSprite(w, fgId, false);
            const EntityID global = addLight(w, light2dtype::kGlobal, 0xFFFFFFFFu);
            auto* g = w.GetComponent<Light2DComponent>(global);
            g->color = { 0.3f, 0.3f, 0.3f, 1.0f };
            const EntityID spot = addLight(w, light2dtype::kSpot, 1u << 0);
            w.GetComponent<Light2DComponent>(spot)->outerRadius = 3.0f;
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lights.size() == 1 && l.lights[0].shape == light2dshape::kSpot && l.lightVertices.size() == 6,
                  "Light2D: Spot becomes one instance with a 6-vertex quad; Global is not an instance");
            check(l.lightGroups.size() == 2 && l.lightGroups[0].anyLit && l.lightGroups[1].anyLit,
                  "Light2D: two layers with different light sets -> two groups");
            check(l.lightGroups.size() == 2 && l.lightGroups[0].lightCount == 1 && l.lightGroups[1].lightCount == 0,
                  "Light2D: Default layer gets the Spot, the other layer (mask excludes it) gets only Global");
            check(l.lightGroups.size() == 2 && l.lightGroups[0].globalColor[0] == 0.3f
                      && l.lightGroups[1].globalColor[0] == 0.3f,
                  "Light2D: Global color is the clear color of every group");
        }

        // 同じ集合を照らす 2 層は 1 群にまとまる。lit=false だけのシーンは群が anyLit にならない
        {
            World w;
            addSprite(w, 0, true);
            addSprite(w, fgId, true);
            addLight(w, light2dtype::kGlobal, 0xFFFFFFFFu);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lightGroups.size() == 1 && l.lightGroups[0].anyLit, "Light2D: same light set across layers -> one group");
        }
        {
            World w;
            addSprite(w, 0, false);
            addLight(w, light2dtype::kGlobal, 0xFFFFFFFFu);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lightGroups.size() == 1 && !l.lightGroups[0].anyLit,
                  "Light2D: a group with only lit=false sprites builds no light buffer");
        }

        // Spot の角度: 90 度の扇形は useAngle、360 度は全周
        {
            World w;
            addSprite(w, 0, true);
            const EntityID spot = addLight(w, light2dtype::kSpot, 0xFFFFFFFFu);
            auto* lc = w.GetComponent<Light2DComponent>(spot);
            lc->innerAngleDeg = 0.0f;
            lc->outerAngleDeg = 90.0f;
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lights.size() == 1 && l.lights[0].useAngle == 1.0f
                      && std::fabs(l.lights[0].cosOuter - 0.70710678f) < 1.0e-5f
                      && std::fabs(l.lights[0].invCosRange - 1.0f / (1.0f - 0.70710678f)) < 1.0e-3f,
                  "Light2D: Spot 0..90 deg cone -> cosOuter = cos 45, range 1 - cos 45");
            lc->outerAngleDeg = 360.0f;
            const Sprite2DDrawList& full = renderer.Collect(w, resources);
            check(full.lights.size() == 1 && full.lights[0].useAngle == 0.0f, "Light2D: Spot outer 360 deg = point light");
            lc->outerRadius = 0.0f;
            check(renderer.Collect(w, resources).lights.empty(), "Light2D: Spot with radius 0 is dropped");
        }

        // Freeform: 正方形 (耳刈り 2 枚 = 6 頂点) + 外縁 4 辺 x 6 頂点。外縁幅 0 は内側だけ
        {
            World w;
            addSprite(w, 0, true);
            const EntityID ff = addLight(w, light2dtype::kFreeform, 0xFFFFFFFFu);
            auto* lc = w.GetComponent<Light2DComponent>(ff);
            lc->pointCount = 4;
            lc->points[0] = { -1.0f, -1.0f };
            lc->points[1] = { 1.0f, -1.0f };
            lc->points[2] = { 1.0f, 1.0f };
            lc->points[3] = { -1.0f, 1.0f };
            lc->falloffWidth = 0.5f;
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lights.size() == 1 && l.lights[0].vertexCount == 6 + 4 * 6 && l.lightVertices.size() == 30,
                  "Light2D: Freeform square = 2 triangles + 4 edge strips");
            bool outerOutside = true; // edge 0 の頂点は正方形の外 (|x| または |y| が 1 を超える)
            for (const Light2DVertex& v : l.lightVertices) {
                if (v.edge == 0.0f) {
                    outerOutside = outerOutside && (std::fabs(v.pos[0]) > 1.0f || std::fabs(v.pos[1]) > 1.0f);
                }
            }
            check(outerOutside, "Light2D: Freeform edge vertices (edge 0) are pushed outside the polygon");
            lc->falloffWidth = 0.0f;
            check(renderer.Collect(w, resources).lights[0].vertexCount == 6, "Light2D: Freeform width 0 = fill only");
            lc->pointCount = 2;
            check(renderer.Collect(w, resources).lights.empty(), "Light2D: Freeform with < 3 points is dropped");
        }

        // 未知の層 id (表に無い) は Default として照らされる。mask = 0 のライトはどの層も照らさない
        {
            World w;
            addSprite(w, 31, true);
            addLight(w, light2dtype::kGlobal, 1u << 0);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lightGroups.size() == 1 && l.lightGroups[0].globalColor[0] == 1.0f,
                  "Light2D: a layer id missing from the table is lit as Default");
            World w2;
            addSprite(w2, 0, true);
            addLight(w2, light2dtype::kGlobal, 0u);
            const Sprite2DDrawList& none = renderer.Collect(w2, resources);
            check(none.lightGroups.size() == 1 && none.lightGroups[0].globalColor[0] == 0.0f,
                  "Light2D: targetLayerMask 0 lights nothing (the group's clear color is black)");
        }
        layers.Remove(fgId);
    }

    // ---- (h) ShadowCaster2D の登録と収集、法線の変換 (M94p)。GPU を使わない部分だけ ----
    {
        check(ShadowCaster2DComponent::sTypeId == 86, "ShadowCaster2D is TypeId 86");
        const ComponentDesc& sd = ComponentRegistry::Get().Desc(ShadowCaster2DComponent::sTypeId);
        check((sd.flags & kComponentNoHash) != 0, "ShadowCaster2D is kComponentNoHash (render-only)");
        const ShadowCaster2DComponent defaults;
        check(defaults.source == shadowcaster2dsource::kCollider && !defaults.selfShadow && defaults.castsShadows
                  && defaults.pointCount == 0,
              "ShadowCaster2D defaults: source 0, selfShadow false, castsShadows true");

        RenderResources resources;
        Sprite2DRenderer renderer;
        World w;
        const EntityID sprite = w.CreateEntity("sprite");
        w.AddComponent<Sprite2DComponent>(sprite);
        const EntityID lightEntity = w.CreateEntity("light");
        auto* light = w.AddComponent<Light2DComponent>(lightEntity);
        light->type = light2dtype::kSpot;
        light->outerRadius = 3.0f;
        // キャスター 1: Box (中心 (2, 0)、半径 (0.5, 1))
        const EntityID box = w.CreateEntity("box");
        w.AddComponent<ColliderComponent>(box);
        auto* boxCaster = w.AddComponent<ShadowCaster2DComponent>(box);
        // AddComponent は他のコンポーネントのポインタを無効にする (アーキタイプ移動)。足し終えてから取る
        auto* boxCol = w.GetComponent<ColliderComponent>(box);
        boxCol->shape = collidershape::kBox;
        boxCol->halfExtents = { 0.5f, 1.0f, 0.5f };
        w.GetComponent<WorldMatrixComponent>(box)->value._41 = 2.0f;

        // 影を落とすライトが無ければキャスターを集めない (presence gate)
        check(renderer.Collect(w, resources).shadowCasters.empty(), "Shadow: no light with shadows -> no casters collected");
        light->shadows = true;
        light->shadowStrength = 2.0f;
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.lights.size() == 1 && l.lights[0].shadows && l.lights[0].shadowStrength == 1.0f,
                  "Shadow: Light2D.shadows reaches the instance, strength clamps to 0..1");
            check(l.shadowCasters.size() == 1 && l.shadowCasters[0].pointCount == 4 && l.shadowPoints.size() == 4,
                  "Shadow: Box collider -> 4 points");
            bool inBox = l.shadowPoints.size() == 4;
            for (const Shadow2DPoint& p : l.shadowPoints) {
                inBox = inBox && std::fabs(std::fabs(p.x - 2.0f) - 0.5f) < 1.0e-5f && std::fabs(std::fabs(p.y) - 1.0f) < 1.0e-5f;
            }
            check(inBox, "Shadow: Box corners are at the world position +/- halfExtents");
            check(l.shadowCasters[0].fillCount == 6 && l.shadowFill.size() == 6,
                  "Shadow: selfShadow false -> the polygon is triangulated for the fill pass (2 triangles)");
        }
        boxCaster->selfShadow = true;
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.shadowCasters.size() == 1 && l.shadowCasters[0].fillCount == 0 && l.shadowFill.empty(),
                  "Shadow: selfShadow true -> no fill pass");
        }
        boxCaster->castsShadows = false;
        check(renderer.Collect(w, resources).shadowCasters.empty(), "Shadow: castsShadows false -> excluded");
        boxCaster->castsShadows = true;

        // Global ライトの shadows は無視 (影の対象は非 Global だけ)
        light->type = light2dtype::kGlobal;
        check(renderer.Collect(w, resources).shadowCasters.empty(), "Shadow: Global light has no shadow");
        light->type = light2dtype::kSpot;

        // Sphere = 16 角、対応外の形 (カプセル) は無し
        boxCol->shape = collidershape::kSphere;
        boxCol->radius = 0.5f;
        check(renderer.Collect(w, resources).shadowCasters.size() == 1
                  && renderer.Collect(w, resources).shadowCasters[0].pointCount == 16,
              "Shadow: Sphere collider -> 16-gon");
        boxCol->shape = collidershape::kCapsule;
        check(renderer.Collect(w, resources).shadowCasters.empty(), "Shadow: unsupported collider shape -> no caster");

        // source 1: points。3 点未満は無し、上限 16 点
        boxCaster->source = shadowcaster2dsource::kPoints;
        boxCaster->pointCount = 2;
        check(renderer.Collect(w, resources).shadowCasters.empty(), "Shadow: points source with < 3 points -> no caster");
        boxCaster->pointCount = 3;
        boxCaster->points[0] = { -1.0f, 0.0f };
        boxCaster->points[1] = { 1.0f, 0.0f };
        boxCaster->points[2] = { 0.0f, 1.0f };
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.shadowCasters.size() == 1 && l.shadowCasters[0].pointCount == 3 && l.shadowPoints[0].x == 1.0f,
                  "Shadow: points source -> the points, carried by the world matrix (x + 2)");
        }
        boxCaster->pointCount = 99;
        check(renderer.Collect(w, resources).shadowCasters[0].pointCount == kShadowCaster2DMaxPoints,
              "Shadow: pointCount is capped at 16");

        // Polygon2D コライダーは .shape2d の点列
        ShapeLibrary2D shapeLib;
        shape2d::Install(&shapeLib);
        Shape2D poly;
        poly.points = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
        const uint64_t polyHash = shapeLib.Register(L"m94p_shadow_poly.shape2d.json", poly);
        boxCaster->source = shadowcaster2dsource::kCollider;
        boxCol->shape = collidershape::kPolygon2D;
        boxCol->meshAsset = AssetID{ polyHash };
        check(renderer.Collect(w, resources).shadowCasters.size() == 1
                  && renderer.Collect(w, resources).shadowCasters[0].pointCount == 4,
              "Shadow: Polygon2D collider -> the .shape2d points");
        shape2d::Install(nullptr);

        // 法線の変換: 回転と flip を畳んだ 2x2 (スケールは除く)
        const auto xform = [](const DirectX::XMFLOAT4X4& m, bool fx, bool fy, float out[4]) {
            Sprite2DRenderer::NormalTransform(m, fx, fy, out);
        };
        float t[4];
        DirectX::XMFLOAT4X4 id = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        xform(id, false, false, t);
        check(t[0] == 1.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 1.0f, "Normal: identity world matrix -> identity");
        xform(id, true, false, t);
        check(t[0] == -1.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 1.0f, "Normal: flipX negates the x axis");
        xform(id, false, true, t);
        check(t[0] == 1.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == -1.0f, "Normal: flipY negates the y axis");
        DirectX::XMFLOAT4X4 rot90 = { 0, 3, 0, 0, -3, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; // 反時計回り 90 度、スケール 3
        xform(rot90, false, false, t);
        check(t[0] == 0.0f && t[1] == 1.0f && t[2] == -1.0f && t[3] == 0.0f,
              "Normal: 90 deg rotation (scale 3) -> unit axes (0,1) and (-1,0)");
        DirectX::XMFLOAT4X4 degenerate = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        xform(degenerate, false, false, t);
        check(t[0] == 1.0f && t[1] == 0.0f && t[2] == 0.0f && t[3] == 1.0f, "Normal: a zero-length axis falls back to identity");

        // 法線マップを持つスプライトが無ければ、ライトがあっても法線の一覧は空 (presence gate)
        check(!renderer.Collect(w, resources).HasNormals(), "Normal: no normalMap in the scene -> no normal batches");
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
