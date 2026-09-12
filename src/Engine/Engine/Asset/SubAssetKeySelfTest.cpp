#include "Engine/Engine/Asset/SubAssetKeySelfTest.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/SubAssetMigration.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"

namespace mye {
namespace {

namespace fs = std::filesystem;

using KeyList = std::vector<std::pair<std::string, uint64_t>>; // (登録名, AssetID) 名前順

// ライブラリ 4 種の登録内容を名前順で 1 本に並べる (比較とサンプル抽出用)
KeyList CollectKeys(const RenderResources& res)
{
    KeyList out;
    for (const AssetEntry& e : res.meshes.Enumerate()) {
        out.emplace_back("mesh " + e.name, e.id.value);
    }
    for (const AssetEntry& e : res.materials.Enumerate()) {
        out.emplace_back("mat " + e.name, e.id.value);
    }
    for (const AssetEntry& e : res.textures.Enumerate()) {
        out.emplace_back("tex " + e.name, e.id.value);
    }
    for (const SkinnedModelEntry& e : res.skinnedModels.Enumerate()) {
        out.emplace_back("skin " + e.name, e.hash);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// モデル由来 (guid://) の登録名だけを抜く。ShaderManager / White() などの共通登録は除外する
KeyList ModelKeys(const KeyList& all)
{
    KeyList out;
    for (const auto& kv : all) {
        const size_t sp = kv.first.find(' ');
        uint64_t guid = 0;
        if (sp != std::string::npos
            && assetkey::ParseSubAssetKey(std::string_view(kv.first).substr(sp + 1), guid)) {
            out.push_back(kv);
        }
    }
    return out;
}

bool Register(const std::wstring& path, RenderResources& res, ShaderManager& shaders)
{
    const bool fbx = path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".fbx") == 0;
    return fbx ? FbxLoader::RegisterAssets(res, shaders, path, /*logErrors=*/true)
               : ModelLoader::RegisterAssets(res, shaders, path, /*logErrors=*/true);
}

void WriteText(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void WriteMetaGuid(const fs::path& asset, uint64_t guid)
{
    AssetMeta m;
    m.guid = guid;
    m.type = AssetType::Model;
    AssetDatabase::WriteMeta(asset.wstring() + L".meta", m);
}

std::string Hex16(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

bool RunSubAssetKeySelfTest()
{
    MYE_LOG_INFO("==== SubAssetKey self test ====");
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
    const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_subasset_selftest";
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);
    AssetDatabase::UninstallKeyResolver(); // 前のスイートが外し忘れても既定 (path-hash) から始める

    // ---- (1) 接頭辞の綴りと解析 ----
    {
        const std::wstring p = L"C:\\Some\\Where\\Model.FBX";
        const std::string expected =
            "guid://" + Hex16(HashStr(WideToUtf8(NormalizePathKey(p))));
        check(assetkey::SubAssetKeyPrefix(p) == expected,
              "prefix: without a resolver the 16hex falls back to the legacy path-hash");
        uint64_t g = 0;
        check(assetkey::ParseSubAssetKey(expected + "#mesh3#part0", g)
                  && g == HashStr(WideToUtf8(NormalizePathKey(p))),
              "parse: guid:// key round-trips to the same 64-bit value");
        check(assetkey::ParseSubAssetKey(expected, g), "parse: a bare prefix (no suffix) is accepted");
        check(!assetkey::ParseSubAssetKey("builtin://cube", g)
                  && !assetkey::ParseSubAssetKey("c:\\x\\y.fbx#mesh0#part0", g)
                  && !assetkey::ParseSubAssetKey("guid://0123#mesh0", g)
                  && !assetkey::ParseSubAssetKey("guid://0123456789abcdefx#mesh0", g)
                  && !assetkey::ParseSubAssetKey("guid://0123456789ABCDEF#mesh0", g),
              "parse: builtin / legacy absolute path / short / trailing junk / upper-case are rejected");
    }

    // ---- (2) チェックアウト非依存: 同じ .meta を持つ同じモデルを別の絶対パスに置く ----
    // M74a の核。旧形式ではここで全 ID が食い違い、2 台のうち片方のモデルが黙って消えた
    const std::wstring srcRoot = FindAssetsRoot();
    struct ModelCase {
        const wchar_t* file;
        uint64_t guid;
        const char* label;
    };
    const ModelCase cases[] = {
        { L"skinned_beam.fbx", 0x1234567890abcdefull, "FBX skinned_beam" },
        { L"CesiumMan.glb", 0x0fedcba987654321ull, "glTF CesiumMan" },
    };
    const fs::path cloneA = tempRoot / L"a" / L"assets" / L"models";
    const fs::path cloneB = tempRoot / L"b" / L"somewhere" / L"much" / L"deeper" / L"assets" / L"models";
    fs::create_directories(cloneA, ec);
    fs::create_directories(cloneB, ec);
    KeyList fbxKeysA; // (5) の移行検査で使う
    {
        AssetDatabase db; // テーブルは空。resolver は各コピーの隣の .meta をディスクから読む
        db.InstallAsKeyResolver();
        for (const ModelCase& mc : cases) {
            const fs::path a = cloneA / mc.file;
            const fs::path b = cloneB / mc.file;
            fs::copy_file(srcRoot + L"\\models\\" + mc.file, a, fs::copy_options::overwrite_existing, ec);
            fs::copy_file(srcRoot + L"\\models\\" + mc.file, b, fs::copy_options::overwrite_existing, ec);
            WriteMetaGuid(a, mc.guid);
            WriteMetaGuid(b, mc.guid);

            RenderResources resA, resB;
            ShaderManager shA, shB;
            check(Register(a.wstring(), resA, shA) && Register(b.wstring(), resB, shB), mc.label);
            const KeyList keysA = ModelKeys(CollectKeys(resA));
            const KeyList keysB = ModelKeys(CollectKeys(resB));
            check(!keysA.empty(), "clone: model registers guid:// keys");
            check(keysA == keysB,
                  "clone: two checkout paths register identical names and AssetIDs (M74a)");
            const std::string prefix = "guid://" + Hex16(mc.guid) + "#";
            check(std::all_of(keysA.begin(), keysA.end(),
                              [&](const auto& kv) {
                                  return kv.first.find(prefix) != std::string::npos;
                              }),
                  "clone: every model key carries the .meta GUID, not the path");
            if (mc.guid == cases[0].guid) {
                fbxKeysA = keysA;
            }
        }
        AssetDatabase::UninstallKeyResolver();
    }

    // ---- (3) 対照実験: .meta が無ければ path-hash に落ちて食い違う (= (2) の一致は .meta 由来) ----
    {
        const fs::path c = tempRoot / L"c" / L"skinned_beam.fbx";
        const fs::path d = tempRoot / L"d" / L"skinned_beam.fbx";
        fs::create_directories(c.parent_path(), ec);
        fs::create_directories(d.parent_path(), ec);
        fs::copy_file(srcRoot + L"\\models\\skinned_beam.fbx", c, fs::copy_options::overwrite_existing, ec);
        fs::copy_file(srcRoot + L"\\models\\skinned_beam.fbx", d, fs::copy_options::overwrite_existing, ec);
        RenderResources resC, resD;
        ShaderManager shC, shD;
        Register(c.wstring(), resC, shC);
        Register(d.wstring(), resD, shD);
        const KeyList keysC = ModelKeys(CollectKeys(resC));
        const KeyList keysD = ModelKeys(CollectKeys(resD));
        check(!keysC.empty() && keysC.size() == keysD.size() && keysC != keysD,
              "control: without a .meta the two paths still diverge (the match above is the GUID's)");
    }

    // ---- (4) 凸包クックのソースパス: GUID → 現在パス ----
    {
        AssetDatabase db;
        db.ScanAndSync((tempRoot / L"a" / L"assets").wstring());
        db.InstallAsKeyResolver();
        const std::wstring expected = (cloneA / L"skinned_beam.fbx").wstring();
        const std::string meshName = "guid://" + Hex16(cases[0].guid) + "#mesh1#part0";
        check(NormalizePathKey(ConvexCookSourcePath(meshName)) == NormalizePathKey(expected),
              "convex: guid:// mesh name resolves to the model's current path");
        check(ConvexCookSourcePath("builtin://cube").empty()
                  && ConvexCookSourcePath("c:\\legacy\\model.fbx#mesh0#part0").empty()
                  && ConvexCookSourcePath("guid://00000000deadbeef#mesh0#part0").empty(),
              "convex: builtin / legacy / unknown GUID yield no cook source");
        AssetDatabase::UninstallKeyResolver();
        check(ConvexCookSourcePath(meshName).empty(),
              "convex: without a resolver there is no cook source (build in place)");
    }

    // ---- (5) 旧 ID の再計算 (M74b) ----
    {
        check(subasset::LegacyKeyOf("guid://0000000000000001#img:guid://0000000000000001#mat0",
                                    "c:\\p\\m.glb")
                  == "c:\\p\\m.glb#img:c:\\p\\m.glb#mat0",
              "legacy key: every occurrence of the prefix is replaced (glTF #img: embeds the material key)");
        check(subasset::LegacyKeyOf("builtin://cube", "c:\\x").empty()
                  && subasset::LegacyKeyOf("c:\\x\\y.fbx#mesh0", "c:\\x").empty(),
              "legacy key: non guid:// names are not converted");

        AssetDatabase db;
        const std::wstring assetsA = (tempRoot / L"a" / L"assets").wstring();
        db.ScanAndSync(assetsA);
        db.InstallAsKeyResolver();
        RenderResources res;
        ShaderManager sh;
        Register((cloneA / L"skinned_beam.fbx").wstring(), res, sh);
        const std::wstring otherMachine = L"D:\\Elsewhere\\Game\\assets";
        subasset::IdMap map;
        const size_t added = subasset::AddLegacyMappings(res, assetsA, { assetsA, otherMachine }, map);
        check(added > 0 && added == map.size(), "legacy map: mappings were produced");

        // 手計算 = M74a 以前のローダが作っていた ID そのもの (NormalizePathKey(実パス) + サフィックス)
        bool allOk = !fbxKeysA.empty();
        for (const auto& kv : fbxKeysA) {
            const std::string name = kv.first.substr(kv.first.find(' ') + 1);
            const std::string suffix = name.substr(name.find('#'));
            const uint64_t here = HashStr(
                WideToUtf8(NormalizePathKey((cloneA / L"skinned_beam.fbx").wstring())) + suffix);
            const uint64_t there =
                HashStr(WideToUtf8(NormalizePathKey(otherMachine + L"\\models\\skinned_beam.fbx"))
                        + suffix);
            const auto h = map.find(here);
            const auto t = map.find(there);
            allOk = allOk && h != map.end() && h->second == kv.second && t != map.end()
                && t->second == kv.second;
        }
        check(allOk,
              "legacy map: pre-M74 ids for this clone and for another machine both map to the new id");
        AssetDatabase::UninstallKeyResolver();
    }

    // ---- (6) テキスト置換: 数値トークンだけを、書式を変えずに ----
    {
        subasset::IdMap map{ { 18446744073709551615ull, 42 }, { 5, 7 }, { 123, 9 } };
        std::string text = "{\"mesh\": 18446744073709551615, \"name\": \"18446744073709551615\", "
                           "\"f\": -0.0, \"g\": 1.5e5, \"arr\": [5, 6], \"s\": \"a\\\"123\\\"\", "
                           "\"n\": -5, \"big\": 1844674407370955161500}";
        const size_t n = subasset::RewriteIds(text, map);
        check(n == 2, "rewrite: exactly the two plain integers in the map are replaced");
        check(text == "{\"mesh\": 42, \"name\": \"18446744073709551615\", \"f\": -0.0, \"g\": 1.5e5, "
                      "\"arr\": [7, 6], \"s\": \"a\\\"123\\\"\", \"n\": -5, "
                      "\"big\": 1844674407370955161500}",
              "rewrite: strings (even with escaped quotes), floats, negatives and overflow stay byte-identical");
        std::string untouched = "{\"a\": 1}";
        check(subasset::RewriteIds(untouched, map) == 0 && untouched == "{\"a\": 1}",
              "rewrite: no match leaves the text as is");
    }

    // ---- (7) 端から端まで: 旧 ID を持つシーンを RunMigration が新 ID へ書き換える ----
    {
        const fs::path proj = tempRoot / L"proj";
        const fs::path models = proj / L"assets" / L"models";
        const fs::path scenes = proj / L"assets" / L"scenes";
        fs::create_directories(models, ec);
        fs::create_directories(scenes, ec);
        const fs::path model = models / L"skinned_beam.fbx";
        fs::copy_file(srcRoot + L"\\models\\skinned_beam.fbx", model, fs::copy_options::overwrite_existing, ec);
        WriteMetaGuid(model, cases[0].guid);

        const std::string suffix = fbxKeysA.empty()
            ? std::string("#none")
            : fbxKeysA.front().first.substr(fbxKeysA.front().first.find('#'));
        const uint64_t newId = fbxKeysA.empty() ? 0 : fbxKeysA.front().second;
        const uint64_t legacyHere = HashStr(WideToUtf8(NormalizePathKey(model.wstring())) + suffix);
        const uint64_t legacyOld = HashStr(
            WideToUtf8(NormalizePathKey(L"E:\\OldClone\\proj\\assets\\models\\skinned_beam.fbx")) + suffix);
        const std::string scene = "{\n  \"a\": " + std::to_string(legacyHere) + ",\n  \"b\": "
            + std::to_string(legacyOld) + ",\n  \"c\": 77\n}";
        const fs::path scenePath = scenes / L"t.scene.json";
        WriteText(scenePath, scene);

        check(subasset::RunMigration((proj / L"assets").wstring(), { L"E:\\OldClone\\proj" },
                                     /*dryRun=*/true) == 0
                  && ReadText(scenePath) == scene,
              "migrate: dry run reports without writing");
        check(subasset::RunMigration((proj / L"assets").wstring(), { L"E:\\OldClone\\proj" },
                                     /*dryRun=*/false) == 0,
              "migrate: run succeeds");
        const std::string expected = "{\n  \"a\": " + std::to_string(newId) + ",\n  \"b\": "
            + std::to_string(newId) + ",\n  \"c\": 77\n}";
        check(newId != 0 && ReadText(scenePath) == expected,
              "migrate: both this clone's and the old clone's ids become the guid:// id, nothing else changes");
        check(assetguid::ResolvePath(cases[0].guid).empty(),
              "migrate: the resolver is uninstalled afterwards");
    }

    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== SubAssetKey self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== SubAssetKey self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
