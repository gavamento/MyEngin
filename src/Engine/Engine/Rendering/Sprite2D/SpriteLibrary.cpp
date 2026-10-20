//====================================================================================
//                          SpriteLibrary.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトの AssetID 解決表の実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Asset/ImportMetaResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

std::string StemUtf8(const std::wstring& path)
{
    return WideToUtf8(std::filesystem::path(path).stem().wstring());
}

} // namespace

AssetID SpriteLibrary::SliceId(const std::wstring& texPath, uint32_t sliceId)
{
    return AssetID{ HashStr(assetkey::SubAssetKeyPrefix(texPath) + "#sprite" + std::to_string(sliceId)) };
}

void SpriteLibrary::Unregister(uint64_t textureId)
{
    auto it = idsByTexture_.find(textureId);
    if (it == idsByTexture_.end()) {
        return;
    }
    for (uint64_t id : it->second) {
        entries_.erase(id);
    }
    idsByTexture_.erase(it);
}

int SpriteLibrary::Register(const std::wstring& texPath)
{
    const AssetID textureId{ assetkey::Resolve(NormalizePathKey(texPath)) };
    Unregister(textureId.value);
    unresolved_.clear();

    importmeta::TextureImportSettings tex; // .meta 無しは既定値
    importmeta::Resolve(texPath, tex);
    const int32_t ppu = tex.pixelsPerUnit > 0 ? tex.pixelsPerUnit
                                              : importmeta::TextureImportSettings::kDefaultPixelsPerUnit;
    const std::string stem = StemUtf8(texPath);
    std::vector<uint64_t>& ids = idsByTexture_[textureId.value];

    Entry full;
    full.def.texture = textureId;
    full.def.ppu = ppu;
    full.def.filter = tex.filter;
    full.def.texSrgbOn = (tex.srgb == 1);
    full.displayName = stem;
    entries_[textureId.value] = full;
    ids.push_back(textureId.value);

    int count = 0;
    for (const importmeta::SpriteSlice& sp : tex.sprites) {
        Entry e;
        e.def.texture = textureId;
        e.def.rect[0] = sp.x;
        e.def.rect[1] = sp.y;
        e.def.rect[2] = sp.w;
        e.def.rect[3] = sp.h;
        e.def.pivot[0] = sp.pivotX;
        e.def.pivot[1] = sp.pivotY;
        e.def.border[0] = sp.borderL;
        e.def.border[1] = sp.borderB;
        e.def.border[2] = sp.borderR;
        e.def.border[3] = sp.borderT;
        e.def.ppu = ppu;
        e.def.filter = tex.filter;
        e.def.texSrgbOn = (tex.srgb == 1);
        std::snprintf(e.def.name, sizeof(e.def.name), "%s", sp.name);
        e.displayName = stem + "#" + sp.name;
        const AssetID id = SliceId(texPath, sp.id);
        // 同じ id が .meta に複数あれば後勝ち (手編集・マージ事故の救済)
        if (entries_.contains(id.value) && id.value != textureId.value) {
            MYE_LOG_WARN("sprite id %u is duplicated in the .meta (the later one wins): %s", sp.id,
                         WideToUtf8(texPath).c_str());
        } else {
            ids.push_back(id.value);
            ++count;
        }
        entries_[id.value] = std::move(e);
    }
    return count;
}

const SpriteDef* SpriteLibrary::Get(AssetID id)
{
    if (id.IsNull()) {
        return nullptr;
    }
    auto it = entries_.find(id.value);
    if (it != entries_.end()) {
        return &it->second.def;
    }
    if (unresolved_.contains(id.value)) {
        return nullptr;
    }
    // テクスチャ自身の AssetID は GUID。パスを引ければ遅延登録する
    const std::wstring path = assetguid::ResolvePath(id.value);
    if (!path.empty() && AssetDatabase::ClassifyPath(path) == AssetType::Texture) {
        Register(path);
        it = entries_.find(id.value);
        if (it != entries_.end()) {
            return &it->second.def;
        }
    }
    unresolved_.insert(id.value);
    return nullptr;
}

void SpriteLibrary::Enumerate(std::vector<SpriteListing>& out) const
{
    out.clear();
    out.reserve(entries_.size());
    for (const auto& [key, e] : entries_) {
        out.push_back({ AssetID{ key }, e.displayName });
    }
    // 表示名が同じ (別フォルダの同名テクスチャ) ときは ID で決める
    std::sort(out.begin(), out.end(), [](const SpriteListing& a, const SpriteListing& b) {
        return a.displayName != b.displayName ? a.displayName < b.displayName : a.id.value < b.id.value;
    });
}

const SpriteDef* SpriteLibrary::FindByName(const std::wstring& texPath, const char* name)
{
    const AssetID textureId{ assetkey::Resolve(NormalizePathKey(texPath)) };
    if (!idsByTexture_.contains(textureId.value)) {
        Register(texPath);
    }
    auto it = idsByTexture_.find(textureId.value);
    if (it == idsByTexture_.end() || name == nullptr) {
        return nullptr;
    }
    for (uint64_t id : it->second) {
        const Entry& e = entries_.at(id);
        if (id != textureId.value && std::string(e.def.name) == name) {
            return &e.def;
        }
    }
    return nullptr;
}

void SpriteLibrary::Clear()
{
    entries_.clear();
    idsByTexture_.clear();
    unresolved_.clear();
}

namespace spritelib {

namespace {
SpriteLibrary* sLibrary = nullptr;
} // namespace

void Install(SpriteLibrary* lib)
{
    sLibrary = lib;
}

SpriteLibrary* Library()
{
    return sLibrary;
}

} // namespace spritelib

} // namespace mye
