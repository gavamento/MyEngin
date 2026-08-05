#include "Engine/Engine/AssetDatabase.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string GuidToHex(uint64_t g)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(g));
    return std::string(buf);
}

uint64_t HexToGuid(const std::string& s)
{
    return std::strtoull(s.c_str(), nullptr, 16);
}

std::string LowerUtf8(const std::wstring& path)
{
    std::string s = WideToUtf8(path);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// パスハッシュ継承: 新規 GUID = 現行 AssetID (HashStr(normpath)) と一致させる。
uint64_t PathHash(const std::wstring& path)
{
    return HashStr(WideToUtf8(NormalizePathKey(path)));
}

} // namespace

AssetType AssetDatabase::ClassifyPath(const std::wstring& path)
{
    const std::string s = LowerUtf8(path);
    // 複合サフィックス (.xxx.json) を単一拡張子より先に判定する
    if (EndsWith(s, ".actor.json")) {
        return AssetType::Actor;
    }
    if (EndsWith(s, ".prefab.json")) {
        return AssetType::Prefab;
    }
    if (EndsWith(s, ".anim.json")) {
        return AssetType::Anim;
    }
    if (EndsWith(s, ".mat.json")) {
        return AssetType::Material;
    }
    if (EndsWith(s, ".controller.json")) {
        return AssetType::Controller;
    }
    if (EndsWith(s, ".scene.json")) {
        return AssetType::Scene;
    }
    if (EndsWith(s, ".sound.json")) {
        return AssetType::Sound;
    }
    if (EndsWith(s, ".mixer.json")) {
        return AssetType::Mixer;
    }
    if (EndsWith(s, ".png") || EndsWith(s, ".jpg") || EndsWith(s, ".jpeg") || EndsWith(s, ".tga")
        || EndsWith(s, ".bmp") || EndsWith(s, ".dds")) {
        return AssetType::Texture;
    }
    if (EndsWith(s, ".glb") || EndsWith(s, ".gltf") || EndsWith(s, ".fbx") || EndsWith(s, ".obj")) {
        return AssetType::Model;
    }
    if (EndsWith(s, ".wav") || EndsWith(s, ".ogg")) {
        return AssetType::Audio;
    }
    if (EndsWith(s, ".hlsl") || EndsWith(s, ".hlsli")) {
        return AssetType::Shader;
    }
    if (EndsWith(s, ".cs")) {
        return AssetType::Script;
    }
    return AssetType::Unknown;
}

const char* AssetDatabase::TypeName(AssetType t)
{
    switch (t) {
    case AssetType::Texture: return "texture";
    case AssetType::Model: return "model";
    case AssetType::Material: return "material";
    case AssetType::Prefab: return "prefab";
    case AssetType::Actor: return "actor";
    case AssetType::Anim: return "anim";
    case AssetType::Controller: return "controller";
    case AssetType::Scene: return "scene";
    case AssetType::Audio: return "audio";
    case AssetType::Shader: return "shader";
    case AssetType::Script: return "script";
    case AssetType::Sound: return "sound";
    case AssetType::Mixer: return "mixer";
    case AssetType::Unknown:
    default: return "unknown";
    }
}

AssetType AssetDatabase::ParseTypeName(const std::string& s)
{
    if (s == "texture") return AssetType::Texture;
    if (s == "model") return AssetType::Model;
    if (s == "material") return AssetType::Material;
    if (s == "prefab") return AssetType::Prefab;
    if (s == "actor") return AssetType::Actor;
    if (s == "anim") return AssetType::Anim;
    if (s == "controller") return AssetType::Controller;
    if (s == "scene") return AssetType::Scene;
    if (s == "audio") return AssetType::Audio;
    if (s == "shader") return AssetType::Shader;
    if (s == "script") return AssetType::Script;
    if (s == "sound") return AssetType::Sound;
    if (s == "mixer") return AssetType::Mixer;
    return AssetType::Unknown;
}

bool AssetDatabase::IsMetaPath(const std::wstring& path)
{
    return EndsWith(LowerUtf8(path), ".meta");
}

bool AssetDatabase::ReadMeta(const std::wstring& metaPath, AssetMeta& out)
{
    std::ifstream f(metaPath);
    if (!f) {
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object() || !j.contains("guid") || !j["guid"].is_string()) {
        return false;
    }
    out.guid = HexToGuid(j["guid"].get<std::string>());
    out.type = (j.contains("type") && j["type"].is_string())
                   ? ParseTypeName(j["type"].get<std::string>())
                   : AssetType::Unknown;
    out.version = (j.contains("version") && j["version"].is_number_integer())
                      ? j["version"].get<int32_t>()
                      : 1;
    // v2 (M39b): テクスチャインポート設定。v1 (キー無し) は既定値 = 従来挙動
    out.tex = importmeta::TextureImportSettings{};
    if (j.contains("tex") && j["tex"].is_object()) {
        const json& t = j["tex"];
        out.tex.srgb = t.value("srgb", 0);
        out.tex.generateMips = t.value("generateMips", 1);
        out.tex.compress = t.value("compress", 0);
    }
    return out.guid != 0;
}

bool AssetDatabase::WriteMeta(const std::wstring& metaPath, const AssetMeta& m)
{
    json j;
    j["guid"] = GuidToHex(m.guid);
    j["type"] = TypeName(m.type);
    // テクスチャは v2 (インポート設定付き) で書く。他種別は従来どおり
    if (m.type == AssetType::Texture) {
        j["version"] = (m.version < 2) ? 2 : m.version;
        j["tex"] = { { "srgb", m.tex.srgb },
                     { "generateMips", m.tex.generateMips },
                     { "compress", m.tex.compress } };
    } else {
        j["version"] = m.version;
    }
    std::ofstream f(metaPath);
    if (!f) {
        return false;
    }
    f << j.dump(2);
    return true;
}

uint64_t AssetDatabase::EnsureMeta(const std::wstring& assetPath)
{
    const std::wstring metaPath = assetPath + L".meta";
    AssetMeta m;
    if (ReadMeta(metaPath, m) && m.guid != 0) {
        return m.guid; // 既存 GUID を尊重 (リネーム耐性)
    }
    m.guid = PathHash(assetPath);
    m.type = ClassifyPath(assetPath);
    m.version = 1;
    WriteMeta(metaPath, m);
    return m.guid;
}

void AssetDatabase::SyncOne(const std::wstring& path)
{
    const uint64_t guid = EnsureMeta(path);
    const std::wstring key = NormalizePathKey(path);
    const AssetType type = ClassifyPath(path);
    byGuid_[guid] = path;
    byPath_[key] = guid;
    typeByPath_[key] = type;
}

void AssetDatabase::ScanAndSync(const std::wstring& assetsRoot)
{
    byGuid_.clear();
    byPath_.clear();
    typeByPath_.clear();

    std::error_code ec;
    if (!fs::is_directory(assetsRoot, ec)) {
        return;
    }
    size_t created = 0;
    for (const auto& e : fs::recursive_directory_iterator(assetsRoot, ec)) {
        if (!e.is_regular_file(ec)) {
            continue;
        }
        const std::wstring p = e.path().wstring();
        if (IsMetaPath(p)) {
            continue; // .meta サイドカー自体はアセットではない
        }
        // .meta 不在なら SyncOne 内で作成される。作成数の目安をログ用に数える
        const std::wstring metaPath = p + L".meta";
        if (!fs::exists(metaPath, ec)) {
            ++created;
        }
        SyncOne(p);
    }
    MYE_LOG_INFO("[assetdb] scanned %zu assets (%zu new .meta) under %s", byGuid_.size(), created,
                 WideToUtf8(assetsRoot).c_str());
}

uint64_t AssetDatabase::GuidForPath(const std::wstring& path, bool createIfMissing)
{
    const std::wstring key = NormalizePathKey(path);
    auto it = byPath_.find(key);
    if (it != byPath_.end()) {
        return it->second;
    }
    const std::wstring metaPath = path + L".meta";
    AssetMeta m;
    if (ReadMeta(metaPath, m) && m.guid != 0) {
        byGuid_[m.guid] = path;
        byPath_[key] = m.guid;
        typeByPath_[key] = ClassifyPath(path);
        return m.guid;
    }
    if (createIfMissing) {
        const uint64_t guid = EnsureMeta(path);
        byGuid_[guid] = path;
        byPath_[key] = guid;
        typeByPath_[key] = ClassifyPath(path);
        return guid;
    }
    return PathHash(path);
}

std::wstring AssetDatabase::PathForGuid(uint64_t guid) const
{
    auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? std::wstring() : it->second;
}

namespace {

// assetkey::Resolve → AssetDatabase の橋渡し (関数ポインタ + user データ)
uint64_t KeyResolverThunk(void* user, const std::wstring& normalizedPath)
{
    // createIfMissing=false: 解決だけで .meta は書かない (書き出しは Import/Create 側の責務)。
    // テーブル/ディスク .meta ヒット時は GUID、未知パスは従来の path-hash が返る
    return static_cast<AssetDatabase*>(user)->GuidForPath(normalizedPath,
                                                          /*createIfMissing=*/false);
}

// assetguid::ResolvePath → AssetDatabase の橋渡し (M39a、逆方向)
std::wstring GuidResolverThunk(void* user, uint64_t guid)
{
    return static_cast<AssetDatabase*>(user)->PathForGuid(guid);
}

// importmeta::Resolve → .meta ディスク読み (M39b)。テーブルを持たず毎回ディスクを読む —
// 呼び出しはテクスチャロード時のみで、Import Settings 適用が即反映される (無効化不要)
bool ImportMetaThunk(void* user, const std::wstring& path, importmeta::TextureImportSettings& out)
{
    (void)user;
    AssetMeta m;
    if (!AssetDatabase::ReadMeta(path + L".meta", m)) {
        return false;
    }
    out = m.tex;
    return true;
}

} // namespace

void AssetDatabase::InstallAsKeyResolver()
{
    assetkey::Install(&KeyResolverThunk, this);
    assetguid::Install(&GuidResolverThunk, this); // M39a: GUID→パスも同じ DB で解決
    importmeta::Install(&ImportMetaThunk, this);  // M39b: テクスチャインポート設定
}

void AssetDatabase::UninstallKeyResolver()
{
    assetkey::Install(nullptr, nullptr);
    assetguid::Install(nullptr, nullptr);
    importmeta::Install(nullptr, nullptr);
}

void AssetDatabase::MoveAsset(const std::wstring& oldPath, const std::wstring& newPath)
{
    // 1) 旧キーの除去: oldPath そのもの + (フォルダ移動なら) 配下すべて
    const std::wstring oldKey = NormalizePathKey(oldPath);
    const std::wstring oldPrefix = oldKey + L"\\";
    for (auto it = byPath_.begin(); it != byPath_.end();) {
        const std::wstring& key = it->first;
        const bool hit = (key == oldKey)
            || (key.size() > oldPrefix.size()
                && key.compare(0, oldPrefix.size(), oldPrefix) == 0);
        if (hit) {
            byGuid_.erase(it->second);
            typeByPath_.erase(key);
            it = byPath_.erase(it);
        } else {
            ++it;
        }
    }

    // 2) 新パスの再登録: 同伴移動した .meta の GUID を SyncOne (EnsureMeta 経由) が尊重する。
    //    .meta が無い場合は新 path-hash で採番される (フォールバック)
    std::error_code ec;
    if (fs::is_directory(newPath, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(newPath, ec)) {
            if (!e.is_regular_file(ec)) {
                continue;
            }
            const std::wstring p = e.path().wstring();
            if (!IsMetaPath(p)) {
                SyncOne(p);
            }
        }
    } else if (fs::exists(newPath, ec)) {
        SyncOne(newPath);
    }
}

AssetType AssetDatabase::TypeForPath(const std::wstring& path) const
{
    auto it = typeByPath_.find(NormalizePathKey(path));
    return it == typeByPath_.end() ? ClassifyPath(path) : it->second;
}

} // namespace mye
