#include "Engine/Engine/Animation.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;
using namespace DirectX;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.anim.json" -> "X.anim"
    const std::string suf = ".anim";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

// アニメータサブツリーを DFS 順に収集 (index 0 = animator 自身)
void DfsCollect(World& w, EntityID root, std::vector<EntityID>& out)
{
    std::function<void(EntityID)> visit = [&](EntityID e) {
        out.push_back(e);
        auto* h = w.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            auto* ch = w.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
}

// clip を animator サブツリーへ適用 (timeTicks の位置でサンプル)
void ApplyPose(World& w, EntityID animator, const AnimationClipAsset& clip, int32_t time)
{
    std::vector<EntityID> idx;
    DfsCollect(w, animator, idx);
    for (const AnimTrack& t : clip.tracks) {
        if (t.comp == kInvalidComponentType || t.keys.empty()) {
            continue;
        }
        if (t.target >= idx.size()) {
            continue;
        }
        void* comp = w.GetComponentRaw(idx[static_cast<size_t>(t.target)], t.comp);
        if (comp) {
            SampleTrackInto(comp, t, time);
        }
    }
}

// timeTicks を speed 分進める (loop で巻き戻し / 非 loop で末尾停止)
void AdvanceTime(AnimatorComponent* a, int32_t length)
{
    if (length <= 0) {
        return;
    }
    a->timeTicks += a->speed;
    if (a->timeTicks >= length) {
        if (a->loop) {
            a->timeTicks %= length;
        } else {
            a->timeTicks = length;
            a->playing = 0;
        }
    } else if (a->timeTicks < 0) {
        if (a->loop) {
            a->timeTicks = ((a->timeTicks % length) + length) % length;
        } else {
            a->timeTicks = 0;
            a->playing = 0;
        }
    }
}

} // namespace

uint32_t FieldFloatCount(FieldType t)
{
    switch (t) {
    case FieldType::Float:  return 1;
    case FieldType::Float2: return 2;
    case FieldType::Float3: return 3;
    case FieldType::Float4:
    case FieldType::Quat:
    case FieldType::Color:  return 4;
    default:                return 0;
    }
}

void SampleTrackInto(void* comp, const AnimTrack& t, int32_t time)
{
    if (t.keys.empty()) {
        return;
    }
    float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (time <= t.keys.front().tick) {
        const auto& v = t.keys.front().value;
        for (int i = 0; i < 4; ++i) {
            out[i] = v[i];
        }
    } else if (time >= t.keys.back().tick) {
        const auto& v = t.keys.back().value;
        for (int i = 0; i < 4; ++i) {
            out[i] = v[i];
        }
    } else {
        size_t k = 0;
        for (; k + 1 < t.keys.size(); ++k) {
            if (time < t.keys[k + 1].tick) {
                break;
            }
        }
        const AnimKey& a = t.keys[k];
        const AnimKey& b = t.keys[k + 1];
        if (t.interp == AnimInterp::Step || b.tick == a.tick) {
            for (int i = 0; i < 4; ++i) {
                out[i] = a.value[i];
            }
        } else {
            // 補間係数は整数の比 → プラットフォーム非依存 (spec 11.3)
            const float f = static_cast<float>(time - a.tick) / static_cast<float>(b.tick - a.tick);
            if (t.type == FieldType::Quat) {
                const XMVECTOR qa = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(a.value.data()));
                const XMVECTOR qb = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(b.value.data()));
                XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(out), XMQuaternionSlerp(qa, qb, f));
            } else {
                for (int i = 0; i < 4; ++i) {
                    out[i] = a.value[i] + (b.value[i] - a.value[i]) * f;
                }
            }
        }
    }

    float* dst = reinterpret_cast<float*>(static_cast<uint8_t*>(comp) + t.offset);
    for (uint32_t i = 0; i < t.compCount; ++i) {
        dst[i] = out[i];
    }
}

// ==== AnimationLibrary ====

uint64_t AnimationLibrary::HashForPath(const std::wstring& path)
{
    return HashStr(WideToUtf8(NormalizePathKey(path)));
}

json AnimationLibrary::ToJson(const AnimationClipAsset& clip)
{
    json root;
    root["engine"] = "MyEngine";
    root["anim"] = 1;
    root["name"] = clip.name;
    root["lengthTicks"] = clip.lengthTicks;
    json tracks = json::array();
    for (const AnimTrack& t : clip.tracks) {
        json tj;
        tj["target"] = t.target;
        tj["component"] = t.component;
        tj["field"] = t.field;
        tj["interp"] = (t.interp == AnimInterp::Step) ? "step" : "linear";
        const uint32_t n = t.compCount ? t.compCount : 1;
        json keys = json::array();
        for (const AnimKey& k : t.keys) {
            json kj;
            kj["t"] = k.tick;
            json v = json::array();
            for (uint32_t i = 0; i < n; ++i) {
                v.push_back(k.value[i]);
            }
            kj["v"] = std::move(v);
            keys.push_back(std::move(kj));
        }
        tj["keys"] = std::move(keys);
        tracks.push_back(std::move(tj));
    }
    root["tracks"] = std::move(tracks);
    return root;
}

bool AnimationLibrary::FromJson(const json& j, AnimationClipAsset& out)
{
    if (!j.contains("tracks") || !j["tracks"].is_array()) {
        return false;
    }
    out.name = j.value("name", std::string());
    out.lengthTicks = j.value("lengthTicks", 60);
    out.tracks.clear();
    const ComponentRegistry& reg = ComponentRegistry::Get();

    for (const json& tj : j["tracks"]) {
        AnimTrack t;
        t.target = tj.value("target", 0ull);
        t.component = tj.value("component", std::string());
        t.field = tj.value("field", std::string());
        t.interp = (tj.value("interp", std::string("linear")) == "step") ? AnimInterp::Step
                                                                          : AnimInterp::Linear;
        // component/field を解決 (実行時に必要)。両方揃わなければ comp=invalid で実行時スキップ
        bool resolved = false;
        t.comp = reg.FindByName(t.component);
        if (t.comp != kInvalidComponentType) {
            for (const FieldDesc& f : reg.Desc(t.comp).fields) {
                if (t.field == f.name) {
                    t.offset = f.offset;
                    t.type = f.type;
                    t.compCount = FieldFloatCount(f.type);
                    resolved = t.compCount > 0;
                    break;
                }
            }
        }
        if (!resolved) {
            t.comp = kInvalidComponentType;
        }
        if (tj.contains("keys") && tj["keys"].is_array()) {
            for (const json& kj : tj["keys"]) {
                AnimKey k;
                k.tick = kj.value("t", 0);
                if (kj.contains("v") && kj["v"].is_array()) {
                    const json& v = kj["v"];
                    for (size_t i = 0; i < v.size() && i < 4; ++i) {
                        k.value[i] = v[i].get<float>();
                    }
                    if (!resolved && t.compCount == 0) {
                        t.compCount = static_cast<uint32_t>(v.size() < 4 ? v.size() : 4);
                    }
                }
                t.keys.push_back(k);
            }
        }
        if (t.compCount == 0) {
            t.compCount = 1;
        }
        std::sort(t.keys.begin(), t.keys.end(),
                  [](const AnimKey& x, const AnimKey& y) { return x.tick < y.tick; });
        out.tracks.push_back(std::move(t));
    }
    return true;
}

uint64_t AnimationLibrary::Register(const std::wstring& path, AnimationClipAsset clip)
{
    const uint64_t hash = HashForPath(path);
    clip.hash = hash;
    clip.path = path;
    if (clip.name.empty()) {
        clip.name = NameFromPath(path);
    }
    clips_[hash] = std::move(clip);
    return hash;
}

uint64_t AnimationLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json root;
    try {
        f >> root;
    } catch (const json::exception& ex) {
        MYE_LOG_WARN("anim parse failed: %s (%s)", WideToUtf8(path).c_str(), ex.what());
        return 0;
    }
    AnimationClipAsset clip;
    if (!FromJson(root, clip)) {
        MYE_LOG_WARN("anim load: no tracks array in %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, std::move(clip));
}

bool AnimationLibrary::SaveToFile(uint64_t hash) const
{
    auto it = clips_.find(hash);
    if (it == clips_.end()) {
        return false;
    }
    const json root = ToJson(it->second);
    std::error_code ec;
    fs::create_directories(fs::path(it->second.path).parent_path(), ec);
    std::ofstream f(fs::path(it->second.path), std::ios::binary);
    if (!f) {
        return false;
    }
    const std::string text = root.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

const AnimationClipAsset* AnimationLibrary::Get(uint64_t hash) const
{
    auto it = clips_.find(hash);
    return (it != clips_.end()) ? &it->second : nullptr;
}

AnimationClipAsset* AnimationLibrary::GetMutable(uint64_t hash)
{
    auto it = clips_.find(hash);
    return (it != clips_.end()) ? &it->second : nullptr;
}

std::vector<AnimClipEntry> AnimationLibrary::Enumerate() const
{
    std::vector<AnimClipEntry> out;
    out.reserve(clips_.size());
    for (const auto& [h, c] : clips_) {
        out.push_back({ c.hash, c.name });
    }
    std::sort(out.begin(), out.end(),
              [](const AnimClipEntry& x, const AnimClipEntry& y) { return x.name < y.name; });
    return out;
}

// ==== AnimationSystem ====

void AnimationSystem::Update(World& world, const AnimationLibrary& lib)
{
    const ComponentTypeId req[] = { AnimatorComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ai = arch.FindTypeIndex(AnimatorComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            auto* anim = static_cast<AnimatorComponent*>(arch.GetPtr(ai, row));
            if (!anim->playing) {
                continue;
            }
            const AnimationClipAsset* clip = lib.Get(anim->clip.value);
            if (!clip) {
                continue;
            }
            ApplyPose(world, e, *clip, anim->timeTicks); // 現在位置のポーズを適用
            AdvanceTime(anim, clip->lengthTicks);        // 次 tick へ進める
        }
    });
}

void AnimationSystem::Evaluate(World& world, const AnimationLibrary& lib, EntityID animator,
                               int32_t timeTicks)
{
    auto* a = world.GetComponent<AnimatorComponent>(animator);
    if (!a) {
        return;
    }
    const AnimationClipAsset* clip = lib.Get(a->clip.value);
    if (clip) {
        ApplyPose(world, animator, *clip, timeTicks);
    }
}

} // namespace mye
