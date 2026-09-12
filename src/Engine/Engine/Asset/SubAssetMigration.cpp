#include "Engine/Engine/Asset/SubAssetMigration.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Engine/Renderer/Skeleton.h"

namespace fs = std::filesystem;

namespace mye::subasset {
namespace {

constexpr size_t kPrefixLen = assetkey::kSubAssetKeyScheme.size() + 16; // "guid://" + 16hex

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

} // namespace

std::string LegacyKeyOf(std::string_view key, std::string_view legacyPrefix)
{
    uint64_t guid = 0;
    if (!assetkey::ParseSubAssetKey(key, guid)) {
        return {};
    }
    const std::string_view prefix = key.substr(0, kPrefixLen);
    std::string out;
    out.reserve(key.size() + legacyPrefix.size());
    size_t pos = 0;
    for (;;) {
        const size_t hit = key.find(prefix, pos);
        if (hit == std::string_view::npos) {
            out.append(key.substr(pos));
            break;
        }
        out.append(key.substr(pos, hit - pos));
        out.append(legacyPrefix);
        pos = hit + prefix.size();
    }
    return out;
}

size_t AddLegacyMappings(const RenderResources& resources, const std::wstring& assetsRoot,
                         const std::vector<std::wstring>& legacyAssetsRoots, IdMap& out)
{
    const std::wstring rootKey = NormalizePathKey(assetsRoot);
    // GUID ごとの「assets 相対の後半 ("\model\x.fbx")」。同じモデルのキーは何百本もあるので引き直さない
    std::unordered_map<uint64_t, std::wstring> relByGuid;
    size_t added = 0;

    auto add = [&](const std::string& name, uint64_t newId) {
        uint64_t guid = 0;
        if (!assetkey::ParseSubAssetKey(name, guid)) {
            return; // builtin:// / 名前キーの材質 / 手続き生成
        }
        auto it = relByGuid.find(guid);
        if (it == relByGuid.end()) {
            std::wstring rel;
            const std::wstring cur = assetguid::ResolvePath(guid);
            if (!cur.empty()) {
                const std::wstring curKey = NormalizePathKey(cur);
                if (curKey.size() > rootKey.size() && curKey.compare(0, rootKey.size(), rootKey) == 0
                    && curKey[rootKey.size()] == L'\\') {
                    rel = curKey.substr(rootKey.size());
                }
            }
            it = relByGuid.emplace(guid, std::move(rel)).first;
        }
        if (it->second.empty()) {
            return; // assets の外 / GUID 未解決 = 旧 clone 先での位置が決まらない
        }
        for (const std::wstring& legacyRoot : legacyAssetsRoots) {
            // 旧キーの接頭辞は当時の NormalizePathKey(実パス) — 小文字化・区切り統一まで同じ手順で作る
            const std::string legacyPrefix = WideToUtf8(NormalizePathKey(legacyRoot + it->second));
            const uint64_t oldId = HashStr(LegacyKeyOf(name, legacyPrefix));
            if (oldId != newId && out.emplace(oldId, newId).second) {
                ++added;
            }
        }
    };

    for (const AssetEntry& e : resources.meshes.Enumerate()) {
        add(e.name, e.id.value);
    }
    for (const AssetEntry& e : resources.materials.Enumerate()) {
        add(e.name, e.id.value);
    }
    for (const AssetEntry& e : resources.textures.Enumerate()) {
        add(e.name, e.id.value);
    }
    for (const SkinnedModelEntry& e : resources.skinnedModels.Enumerate()) {
        add(e.name, e.hash);
    }
    return added;
}

size_t RewriteIds(std::string& jsonText, const IdMap& map)
{
    std::string out;
    out.reserve(jsonText.size());
    size_t replaced = 0;
    bool inString = false;
    const size_t n = jsonText.size();
    for (size_t i = 0; i < n;) {
        const char c = jsonText[i];
        if (inString) {
            out.push_back(c);
            if (c == '\\' && i + 1 < n) {
                out.push_back(jsonText[i + 1]); // エスケープの次の 1 文字は終端の '"' ではない
                i += 2;
                continue;
            }
            inString = (c != '"');
            ++i;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            // 数値トークンを丸ごと切り出す (符号・小数点・指数も含めて 1 トークン)
            size_t j = i;
            while (j < n) {
                const char d = jsonText[j];
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e'
                    || d == 'E') {
                    ++j;
                } else {
                    break;
                }
            }
            const std::string_view tok(jsonText.data() + i, j - i);
            const bool plainUnsigned =
                tok.size() <= 20
                && std::all_of(tok.begin(), tok.end(), [](char d) { return d >= '0' && d <= '9'; });
            if (plainUnsigned) {
                uint64_t v = 0;
                const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
                if (ec == std::errc() && ptr == tok.data() + tok.size()) {
                    const auto hit = map.find(v);
                    if (hit != map.end()) {
                        out.append(std::to_string(hit->second));
                        ++replaced;
                        i = j;
                        continue;
                    }
                }
            }
            out.append(tok);
            i = j;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    if (replaced != 0) {
        jsonText.swap(out);
    }
    return replaced;
}

int RunMigration(const std::wstring& assetsRootIn, const std::vector<std::wstring>& legacyProjectRoots,
                 bool dryRun)
{
    std::error_code ec;
    const std::wstring assetsRoot = fs::absolute(assetsRootIn, ec).lexically_normal().wstring();
    if (!fs::is_directory(assetsRoot, ec)) {
        std::fprintf(stderr, "[migrate] ERROR: assets root not found: %s\n",
                     WideToUtf8(assetsRoot).c_str());
        return 2;
    }

    // 旧 ID の出どころ = 「当時の assets ルート」。この clone 先自身も必ず含める —
    // M74a 以前にこのマシンで保存したシーンの ID は、ここのパス由来だから
    std::vector<std::wstring> legacyAssetsRoots{ assetsRoot };
    for (const std::wstring& r : legacyProjectRoots) {
        const std::wstring candidate = (fs::path(r) / L"assets").wstring();
        const bool dup = std::any_of(legacyAssetsRoots.begin(), legacyAssetsRoots.end(),
                                     [&](const std::wstring& x) {
                                         return NormalizePathKey(x) == NormalizePathKey(candidate);
                                     });
        if (!dup) {
            legacyAssetsRoots.push_back(candidate);
        }
    }

    // エンジン起動と同じ順序: 走査 (.meta を揃える) → resolver。これが無いとキーの GUID が
    // path-hash に落ちて、実行時とは別の新 ID へ書き換えてしまう
    AssetDatabase db;
    db.ScanAndSync(assetsRoot);
    db.InstallAsKeyResolver();

    RenderResources resources; // ヘッドレス: GPU 生成は黙って失敗するが登録名と ID は揃う
    ShaderManager shaders;
    size_t models = 0;
    std::vector<std::wstring> documents;
    for (const auto& e : fs::recursive_directory_iterator(assetsRoot, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        const std::wstring lower = Lower(p);
        if (EndsWith(lower, L".fbx")) {
            models += FbxLoader::RegisterAssets(resources, shaders, p, /*logErrors=*/true) ? 1 : 0;
        } else if (EndsWith(lower, L".glb") || EndsWith(lower, L".gltf")) {
            models += ModelLoader::RegisterAssets(resources, shaders, p, /*logErrors=*/true) ? 1 : 0;
        } else if (EndsWith(lower, L".scene.json") || EndsWith(lower, L".prefab.json")
                   || EndsWith(lower, L".actor.json")) {
            documents.push_back(p);
        }
    }
    std::sort(documents.begin(), documents.end()); // 出力の並びを走査順に依らせない

    IdMap map;
    const size_t mappings = AddLegacyMappings(resources, assetsRoot, legacyAssetsRoots, map);
    std::printf("[migrate] assets root: %s\n", WideToUtf8(assetsRoot).c_str());
    for (const std::wstring& r : legacyAssetsRoots) {
        std::printf("[migrate]   legacy assets root: %s\n", WideToUtf8(r).c_str());
    }
    std::printf("[migrate] %zu model(s) registered, %zu legacy id mapping(s)%s\n", models, mappings,
                dryRun ? " (dry run: nothing is written)" : "");

    size_t filesChanged = 0;
    size_t idsReplaced = 0;
    int rc = 0;
    for (const std::wstring& p : documents) {
        std::string text;
        {
            std::ifstream f(p, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        const size_t count = RewriteIds(text, map);
        if (count == 0) {
            continue;
        }
        ++filesChanged;
        idsReplaced += count;
        std::printf("[migrate]   %6zu id(s)  %s\n", count, WideToUtf8(p).c_str());
        if (!dryRun) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!f) {
                std::fprintf(stderr, "[migrate] ERROR: write failed: %s\n", WideToUtf8(p).c_str());
                rc = 1;
            }
        }
    }
    std::printf("[migrate] %s: %zu id(s) in %zu of %zu document(s)\n",
                dryRun ? "would rewrite" : "rewrote", idsReplaced, filesChanged, documents.size());

    AssetDatabase::UninstallKeyResolver();
    return rc;
}

} // namespace mye::subasset
