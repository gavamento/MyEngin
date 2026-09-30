//====================================================================================
//                          TagNames.cpp
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          タグ名の表と RT のタグ設定の読み書き
//====================================================================================
#include "Engine/Engine/Scene/TagNames.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"

#include "nlohmann/json.hpp"

namespace mye {
namespace {

using json = nlohmann::json;

// project_settings.json を丸ごと読む (無い / 壊れていれば空オブジェクト)
json ReadSettings(const std::wstring& assetsRoot)
{
    std::ifstream f(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
    if (!f) {
        return json::object();
    }
    try {
        json j;
        f >> j;
        return j.is_object() ? j : json::object();
    } catch (const json::exception& e) {
        MYE_LOG_WARN("[tags] project_settings.json parse failed: %s", e.what());
        return json::object();
    }
}

// 他キーを壊さずに 1 キーだけ差し替えて書く (PhysicsLayerNames::Save と同じ read-modify-write)
bool WriteSettingsKey(const std::wstring& assetsRoot, const char* key, json value)
{
    json j = ReadSettings(assetsRoot);
    j[key] = std::move(value);
    std::ofstream out(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
    if (!out) {
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

// タグ番号の配列 → ビット集合。範囲外・非整数の要素は黙って落とす (手で壊した設定で起動を止めない)
uint64_t MaskFromIndexArray(const json& arr)
{
    uint64_t mask = 0;
    if (!arr.is_array()) {
        return 0;
    }
    for (const json& v : arr) {
        if (v.is_number_integer()) {
            const int64_t i = v.get<int64_t>();
            if (i >= 0 && i < kMaxTags) {
                mask |= 1ull << static_cast<uint32_t>(i);
            }
        }
    }
    return mask;
}

json IndexArrayFromMask(uint64_t mask)
{
    json arr = json::array();
    for (int i = 0; i < kMaxTags; ++i) {
        if ((mask >> i) & 1ull) {
            arr.push_back(i);
        }
    }
    return arr;
}

} // namespace

TagNames& TagNames::Get()
{
    static TagNames instance;
    return instance;
}

void TagNames::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    std::memset(names_, 0, sizeof(names_));
    const json j = ReadSettings(assetsRoot);
    if (!j.contains("tags") || !j["tags"].is_array()) {
        return;
    }
    const json& arr = j["tags"];
    for (int i = 0; i < kCount && i < static_cast<int>(arr.size()); ++i) {
        if (arr[i].is_string()) {
            std::snprintf(names_[i], kNameCapacity, "%s", arr[i].get<std::string>().c_str());
        }
    }
}

bool TagNames::Save(const std::wstring& assetsRoot) const
{
    // 末尾の空欄は落とす (64 個の "" を毎回ファイルに書かない)。途中の空欄は番号を保つために残す
    int last = -1;
    for (int i = 0; i < kCount; ++i) {
        if (names_[i][0] != '\0') {
            last = i;
        }
    }
    json arr = json::array();
    for (int i = 0; i <= last; ++i) {
        arr.push_back(std::string(names_[i]));
    }
    return WriteSettingsKey(assetsRoot, "tags", std::move(arr));
}

bool TagNames::DiffersFromDisk() const
{
    if (loadedRoot_.empty()) {
        return false; // 一度も読んでいない = この実行では何も編集していない
    }
    TagNames onDisk;
    onDisk.Load(loadedRoot_, true);
    return std::memcmp(names_, onDisk.names_, sizeof(names_)) != 0;
}

const char* TagNames::Name(int i) const
{
    return (i >= 0 && i < kCount) ? names_[i] : "";
}

const char* TagNames::Display(int i) const
{
    if (i < 0 || i >= kCount) {
        return "(out of range)";
    }
    if (names_[i][0] != '\0') {
        return names_[i];
    }
    // 空欄の表示名。呼び出しごとに書き換わる静的バッファは複数同時に使うと壊れるので、
    // 番号ごとに固定の文字列を 1 回だけ作って持つ
    static char fallback[kCount][12] = {};
    if (fallback[i][0] == '\0') {
        std::snprintf(fallback[i], sizeof(fallback[i]), "Tag %d", i);
    }
    return fallback[i];
}

char* TagNames::EditBuffer(int i)
{
    return (i >= 0 && i < kCount) ? names_[i] : nullptr;
}

int32_t TagNames::IndexOf(std::string_view name) const
{
    if (name.empty()) {
        return -1;
    }
    for (int i = 0; i < kCount; ++i) {
        if (name == std::string_view(names_[i])) {
            return i;
        }
    }
    return -1;
}

RtTagRules LoadRtTagRules(const std::wstring& assetsRoot)
{
    // ★旧形式 ("receivers" / "scene" = 対象を絞るフィルタ) は読まない。既定 OFF の規則と
    //   意味が逆向きで、読み替えると「絞り込みのつもりで付けたタグ」が黙って ON 規則になる
    RtTagRules r;
    const json j = ReadSettings(assetsRoot);
    if (j.contains("rayTracingTags") && j["rayTracingTags"].is_object()) {
        const json& rt = j["rayTracingTags"];
        auto read = [&rt](const char* key, uint64_t& out) {
            if (rt.contains(key)) {
                out = MaskFromIndexArray(rt[key]);
            }
        };
        read("sceneOn", r.sceneOn);
        read("sceneOff", r.sceneOff);
        read("receiverOn", r.receiverOn);
        read("receiverOff", r.receiverOff);
    }
    return r;
}

bool SaveRtTagRules(const std::wstring& assetsRoot, const RtTagRules& rules)
{
    json rt = json::object();
    rt["sceneOn"] = IndexArrayFromMask(rules.sceneOn);
    rt["sceneOff"] = IndexArrayFromMask(rules.sceneOff);
    rt["receiverOn"] = IndexArrayFromMask(rules.receiverOn);
    rt["receiverOff"] = IndexArrayFromMask(rules.receiverOff);
    return WriteSettingsKey(assetsRoot, "rayTracingTags", std::move(rt));
}

bool ParseTagIndexList(std::wstring_view text, uint64_t& out)
{
    uint64_t mask = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L',', pos);
        if (end == std::wstring_view::npos) {
            end = text.size();
        }
        const std::wstring_view item = text.substr(pos, end - pos);
        if (item.empty() || item.size() > 2) {
            return false; // 空要素 ("1,,2") や 3 桁以上は打ち間違い
        }
        int value = 0;
        for (wchar_t c : item) {
            if (c < L'0' || c > L'9') {
                return false;
            }
            value = value * 10 + static_cast<int>(c - L'0');
        }
        if (value >= kMaxTags) {
            return false;
        }
        mask |= 1ull << static_cast<uint32_t>(value);
        pos = end + 1;
    }
    out = mask;
    return true;
}

} // namespace mye
