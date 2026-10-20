//====================================================================================
//                          SpriteAnim.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                  スプライト列クリップ (.spriteanim.json) と登録表
//====================================================================================
#include "Engine/Engine/Animation/SpriteAnim.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.spriteanim.json" → "X.spriteanim"
    const std::string suf = ".spriteanim";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

} // namespace

// ==== SpriteAnim ====

void SpriteAnim::RebuildDerived()
{
    frameEnds.clear();
    frameEnds.reserve(frames.size());
    int32_t total = 0;
    for (const SpriteAnimFrame& f : frames) {
        total += f.ticks;
        frameEnds.push_back(total);
    }
    lengthTicks = total;
    nameHash = name.empty() ? 0 : HashStr(name);
}

int32_t SpriteAnimFrameAt(const SpriteAnim& anim, int32_t time)
{
    if (anim.frameEnds.empty()) {
        return -1;
    }
    const int32_t t = std::clamp(time, 0, anim.lengthTicks - 1);
    // frameEnds[i] > t となる最初の i = t を含むコマ (frameEnds は単調増加)
    const auto it = std::upper_bound(anim.frameEnds.begin(), anim.frameEnds.end(), t);
    return static_cast<int32_t>(it - anim.frameEnds.begin());
}

// ==== SpriteAnimLibrary ====

uint64_t SpriteAnimLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / 改名済みアセットは .meta の GUID がキー (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t SpriteAnimLibrary::Register(const std::wstring& path, SpriteAnim anim)
{
    const uint64_t hash = HashForPath(path);
    anim.hash = hash;
    anim.path = path;
    if (anim.name.empty()) {
        anim.name = NameFromPath(path);
    }
    anim.RebuildDerived();
    anims_[hash] = std::move(anim);
    return hash;
}

uint64_t SpriteAnimLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[spriteanim] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    SpriteAnim a;
    if (!FromJson(j, a)) {
        MYE_LOG_WARN("[spriteanim] not a sprite animation: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(a));
}

const SpriteAnim* SpriteAnimLibrary::Get(uint64_t hash) const
{
    const auto it = anims_.find(hash);
    return it != anims_.end() ? &it->second : nullptr;
}

std::vector<SpriteAnimEntry> SpriteAnimLibrary::Enumerate() const
{
    std::vector<SpriteAnimEntry> out;
    out.reserve(anims_.size());
    for (const auto& [hash, anim] : anims_) {
        out.push_back(SpriteAnimEntry{ hash, anim.name });
    }
    // 表示名の昇順 (同名は hash 順) に並べ直す
    std::sort(out.begin(), out.end(), [](const SpriteAnimEntry& a, const SpriteAnimEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json SpriteAnimLibrary::ToJson(const SpriteAnim& a)
{
    json j;
    j["engine"] = "MyEngine";
    j["spriteanim"] = 1;
    j["name"] = a.name;
    json frames = json::array();
    for (const SpriteAnimFrame& f : a.frames) {
        frames.push_back({ { "sprite", f.sprite }, { "ticks", f.ticks } });
    }
    j["frames"] = std::move(frames);
    return j;
}

bool SpriteAnimLibrary::FromJson(const json& j, SpriteAnim& out)
{
    if (!j.is_object() || !j.contains("spriteanim")) {
        return false; // 種別キー必須 (別の資産を黙ってスプライト列に読まない)
    }
    out.name = (j.contains("name") && j["name"].is_string()) ? j["name"].get<std::string>() : std::string();
    out.frames.clear();
    if (j.contains("frames") && j["frames"].is_array()) {
        for (const json& fj : j["frames"]) {
            if (!fj.is_object()) {
                continue;
            }
            SpriteAnimFrame f;
            if (fj.contains("sprite") && fj["sprite"].is_number_integer()) {
                f.sprite = fj["sprite"].get<uint64_t>();
            }
            if (fj.contains("ticks") && fj["ticks"].is_number_integer()) {
                // int64 で読んで丸める (巨大値の int32 切り詰めで負や 0 にならないように)
                f.ticks = static_cast<int32_t>(
                    std::clamp<int64_t>(fj["ticks"].get<int64_t>(), 1, kMaxSpriteAnimFrameTicks));
            }
            out.frames.push_back(f);
        }
    }
    Sanitize(out);
    return true;
}

void SpriteAnimLibrary::Sanitize(SpriteAnim& a)
{
    if (a.frames.size() > kMaxSpriteAnimFrames) {
        MYE_LOG_WARN("[spriteanim] %zu frames; only the first %zu are kept", a.frames.size(), kMaxSpriteAnimFrames);
        a.frames.resize(kMaxSpriteAnimFrames);
    }
    for (SpriteAnimFrame& f : a.frames) {
        f.ticks = std::clamp(f.ticks, 1, kMaxSpriteAnimFrameTicks);
    }
    a.RebuildDerived();
}

// ==== spriteanim:: モジュール注入 ====

namespace spriteanim {

namespace {
SpriteAnimLibrary* sLibrary = nullptr;
} // namespace

void Install(SpriteAnimLibrary* lib)
{
    sLibrary = lib;
}

SpriteAnimLibrary* Library()
{
    return sLibrary;
}

const SpriteAnim* Resolve(AssetID id)
{
    if (sLibrary == nullptr || id.IsNull()) {
        return nullptr;
    }
    return sLibrary->Get(id.value);
}

} // namespace spriteanim

} // namespace mye
