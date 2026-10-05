//====================================================================================
//                          BehaviorTreeLibrary.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリー資産の読み書きとノード種類の表
//====================================================================================
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

const char* const kFinishModeNames[] = { "Immediate", "Delayed" };

const BtParamDesc kParallelParams[] = {
    { "finishMode", BtParamType::Enum, static_cast<float>(btparallelfinish::kImmediate), 0.0f, 1.0f,
      kFinishModeNames, 2 },
};

const BtParamDesc kWaitParams[] = {
    { "ticks", BtParamType::Int, 60.0f, 0.0f, static_cast<float>(kBtMaxTicksParam), nullptr, 0 },
    { "randomDeviation", BtParamType::Int, 0.0f, 0.0f, static_cast<float>(kBtMaxTicksParam), nullptr, 0 },
};

// BtNodeKind の並びと同じ順に並べる (BtNodeTypeOf が添字で引く)
const BtNodeTypeInfo kNodeTypes[] = {
    { BtNodeKind::Selector, "Selector", BtNodeCategory::Composite, 0, kBtUnlimitedChildren, nullptr, 0 },
    { BtNodeKind::Sequence, "Sequence", BtNodeCategory::Composite, 0, kBtUnlimitedChildren, nullptr, 0 },
    { BtNodeKind::SimpleParallel, "SimpleParallel", BtNodeCategory::Composite, 2, 2, kParallelParams, 1 },
    { BtNodeKind::Wait, "Wait", BtNodeCategory::Task, 0, 0, kWaitParams, 2 },
};
static_assert(sizeof(kNodeTypes) / sizeof(kNodeTypes[0]) == static_cast<size_t>(BtNodeKind::Count),
              "kNodeTypes を BtNodeKind の全値ぶん並べる");

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.bt.json" -> "X.bt"
    const std::string suf = ".bt";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

std::string GuidToHex(uint64_t guid)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(guid));
    return std::string(buf);
}

BtParamValue DefaultParam(const BtParamDesc& desc)
{
    BtParamValue value;
    if (desc.type == BtParamType::Float) {
        value.f = desc.defaultValue;
    } else {
        value.i = static_cast<int32_t>(desc.defaultValue);
    }
    return value;
}

// params の 1 項目を読む。型が違う・列挙名が未知は false (手書きの綴り間違いを黙って既定値にしない)。範囲外は丸める
bool ReadParam(const BtParamDesc& desc, const json& j, BtParamValue& out)
{
    switch (desc.type) {
    case BtParamType::Int: {
        if (!j.is_number_integer()) {
            return false;
        }
        const int64_t v = j.get<int64_t>();
        out.i = static_cast<int32_t>(
            (std::clamp)(v, static_cast<int64_t>(desc.minValue), static_cast<int64_t>(desc.maxValue)));
        return true;
    }
    case BtParamType::Float: {
        if (!j.is_number()) {
            return false;
        }
        const float v = j.get<float>();
        if (!std::isfinite(v)) {
            return false;
        }
        out.f = (std::clamp)(v, desc.minValue, desc.maxValue);
        return true;
    }
    case BtParamType::Bool:
        if (!j.is_boolean()) {
            return false;
        }
        out.i = j.get<bool>() ? 1 : 0;
        return true;
    case BtParamType::Enum: {
        if (!j.is_string()) {
            return false;
        }
        const std::string name = j.get<std::string>();
        for (int i = 0; i < desc.enumCount; ++i) {
            if (name == desc.enumNames[i]) {
                out.i = i;
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

json WriteParam(const BtParamDesc& desc, const BtParamValue& value)
{
    switch (desc.type) {
    case BtParamType::Int: return value.i;
    case BtParamType::Float: return value.f;
    case BtParamType::Bool: return value.i != 0;
    case BtParamType::Enum:
        return desc.enumNames[(std::clamp)(value.i, 0, desc.enumCount - 1)];
    }
    return nullptr;
}

} // namespace

const BtNodeTypeInfo& BtNodeTypeOf(BtNodeKind kind)
{
    return kNodeTypes[static_cast<size_t>(kind)];
}

const BtNodeTypeInfo* BtFindNodeType(const std::string& name)
{
    for (const BtNodeTypeInfo& info : kNodeTypes) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

int BehaviorTreeAsset::FindNode(int32_t id) const
{
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool BtLinkAsset(BehaviorTreeAsset& asset)
{
    const size_t count = asset.nodes.size();
    if (count > static_cast<size_t>(kBtMaxNodes)) {
        return false;
    }

    // id の一意性 (整列してから隣どうしを比べる = 反復順に依らない)
    std::vector<std::pair<int32_t, int32_t>> byId; // (id, nodes の添字)
    byId.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const BtNodeDef& node = asset.nodes[i];
        if (node.id < 0 || static_cast<size_t>(node.kind) >= static_cast<size_t>(BtNodeKind::Count)
            || node.params.size() != static_cast<size_t>(BtNodeTypeOf(node.kind).paramCount)) {
            return false;
        }
        byId.emplace_back(node.id, static_cast<int32_t>(i));
    }
    std::sort(byId.begin(), byId.end());
    for (size_t i = 1; i < byId.size(); ++i) {
        if (byId[i - 1].first == byId[i].first) {
            return false;
        }
    }
    const auto indexOf = [&byId](int32_t id) {
        const auto it = std::lower_bound(byId.begin(), byId.end(), std::pair<int32_t, int32_t>{ id, -1 });
        return it != byId.end() && it->first == id ? it->second : -1;
    };

    // 子の参照と親の数
    std::vector<std::vector<int32_t>> children(count);
    std::vector<int32_t> parent(count, -1);
    for (size_t i = 0; i < count; ++i) {
        const BtNodeDef& node = asset.nodes[i];
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        const int childCount = static_cast<int>(node.childIds.size());
        if (childCount < info.minChildren || (info.maxChildren != kBtUnlimitedChildren && childCount > info.maxChildren)) {
            return false;
        }
        for (const int32_t childId : node.childIds) {
            const int32_t child = indexOf(childId);
            if (child < 0 || child == static_cast<int32_t>(i) || parent[static_cast<size_t>(child)] != -1) {
                return false; // 存在しない子・自分自身・親が 2 つ
            }
            parent[static_cast<size_t>(child)] = static_cast<int32_t>(i);
            children[i].push_back(child);
        }
    }

    // 根
    int32_t rootIndex = -1;
    if (asset.rootId != -1) {
        rootIndex = indexOf(asset.rootId);
        if (rootIndex < 0 || parent[static_cast<size_t>(rootIndex)] != -1) {
            return false;
        }
    }

    // 循環と深さ: 親をたどって根へ着くまでの段数。循環の中のノードはどこへも着かない
    for (size_t i = 0; i < count; ++i) {
        int steps = 0;
        int32_t at = static_cast<int32_t>(i);
        while (parent[static_cast<size_t>(at)] != -1) {
            at = parent[static_cast<size_t>(at)];
            if (++steps >= kBtMaxDepth) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        asset.nodes[i].children = std::move(children[i]);
        asset.nodes[i].parent = parent[i];
    }
    asset.rootIndex = rootIndex;
    asset.stateSlotCount = static_cast<int32_t>(count);
    return true;
}

uint64_t BehaviorTreeLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / リネーム済みの資産は .meta の GUID がキー (NavFilterLibrary と同じ)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t BehaviorTreeLibrary::Register(const std::wstring& path, BehaviorTreeAsset asset)
{
    const uint64_t hash = HashForPath(path);
    asset.hash = hash;
    asset.path = path;
    if (asset.name.empty()) {
        asset.name = NameFromPath(path);
    }
    assets_[hash] = std::make_shared<const BehaviorTreeAsset>(std::move(asset));
    return hash;
}

uint64_t BehaviorTreeLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[behaviortree] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    BehaviorTreeAsset asset;
    if (!FromJson(j, asset)) {
        MYE_LOG_WARN("[behaviortree] not a valid behavior tree: %s", WideToUtf8(path).c_str());
        return 0;
    }
    asset.name = NameFromPath(path);
    return Register(path, std::move(asset));
}

std::shared_ptr<const BehaviorTreeAsset> BehaviorTreeLibrary::GetShared(uint64_t hash) const
{
    const auto it = assets_.find(hash);
    return it != assets_.end() ? it->second : nullptr;
}

const BehaviorTreeAsset* BehaviorTreeLibrary::Get(uint64_t hash) const
{
    const auto it = assets_.find(hash);
    return it != assets_.end() ? it->second.get() : nullptr;
}

std::vector<BehaviorTreeEntry> BehaviorTreeLibrary::Enumerate() const
{
    std::vector<BehaviorTreeEntry> out;
    out.reserve(assets_.size());
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        out.push_back(BehaviorTreeEntry{ it->first, it->second->name });
    }
    std::sort(out.begin(), out.end(), [](const BehaviorTreeEntry& a, const BehaviorTreeEntry& b) {
        return a.name != b.name ? a.name < b.name : a.hash < b.hash;
    });
    return out;
}

json BehaviorTreeLibrary::ToJson(const BehaviorTreeAsset& asset)
{
    json j;
    j["engine"] = "MyEngine";
    j["behaviortree"] = 1;
    j["name"] = asset.name;
    j["blackboard"] = asset.blackboard != 0 ? GuidToHex(asset.blackboard) : std::string();
    j["root"] = asset.rootId;
    json nodes = json::array();
    for (const BtNodeDef& node : asset.nodes) {
        const BtNodeTypeInfo& info = BtNodeTypeOf(node.kind);
        json n;
        n["id"] = node.id;
        n["type"] = info.name;
        json params = json::object();
        for (int i = 0; i < info.paramCount && static_cast<size_t>(i) < node.params.size(); ++i) {
            params[info.params[i].name] = WriteParam(info.params[i], node.params[static_cast<size_t>(i)]);
        }
        n["params"] = std::move(params);
        n["decorators"] = json::array();
        n["children"] = node.childIds;
        n["pos"] = json::array({ node.pos[0], node.pos[1] });
        nodes.push_back(std::move(n));
    }
    j["nodes"] = std::move(nodes);
    return j;
}

bool BehaviorTreeLibrary::FromJson(const json& j, BehaviorTreeAsset& out)
{
    if (!j.is_object() || !j.contains("behaviortree")) {
        return false; // 種別キー必須 (別の JSON 資産を黙って木に読まない)
    }
    BehaviorTreeAsset asset;
    if (j.contains("name") && j["name"].is_string()) {
        asset.name = j["name"].get<std::string>();
    }
    if (j.contains("blackboard") && j["blackboard"].is_string()) {
        const std::string hex = j["blackboard"].get<std::string>();
        asset.blackboard = hex.empty() ? 0 : std::strtoull(hex.c_str(), nullptr, 16);
    }
    if (j.contains("root")) {
        if (!j["root"].is_number_integer()) {
            return false;
        }
        asset.rootId = j["root"].get<int32_t>();
    }
    if (j.contains("nodes")) {
        const json& nodes = j["nodes"];
        if (!nodes.is_array() || nodes.size() > static_cast<size_t>(kBtMaxNodes)) {
            return false;
        }
        for (const json& n : nodes) {
            if (!n.is_object() || !n.contains("id") || !n["id"].is_number_integer() || !n.contains("type")
                || !n["type"].is_string()) {
                return false;
            }
            const BtNodeTypeInfo* info = BtFindNodeType(n["type"].get<std::string>());
            if (info == nullptr) {
                return false; // 未知の type
            }
            BtNodeDef node;
            node.id = n["id"].get<int32_t>();
            node.kind = info->kind;
            for (int i = 0; i < info->paramCount; ++i) {
                node.params.push_back(DefaultParam(info->params[i]));
            }
            if (n.contains("params")) {
                if (!n["params"].is_object()) {
                    return false;
                }
                for (int i = 0; i < info->paramCount; ++i) {
                    if (n["params"].contains(info->params[i].name)
                        && !ReadParam(info->params[i], n["params"][info->params[i].name], node.params[static_cast<size_t>(i)])) {
                        return false;
                    }
                }
            }
            if (n.contains("decorators")) {
                // この版に Decorator の種類は無い。あるなら未知の type と同じく読めない
                if (!n["decorators"].is_array() || !n["decorators"].empty()) {
                    return false;
                }
            }
            if (n.contains("children")) {
                if (!n["children"].is_array()) {
                    return false;
                }
                for (const json& child : n["children"]) {
                    if (!child.is_number_integer()) {
                        return false;
                    }
                    node.childIds.push_back(child.get<int32_t>());
                }
            }
            if (n.contains("pos") && n["pos"].is_array() && n["pos"].size() == 2 && n["pos"][0].is_number()
                && n["pos"][1].is_number()) {
                const float x = n["pos"][0].get<float>();
                const float y = n["pos"][1].get<float>();
                node.pos[0] = std::isfinite(x) ? x : 0.0f;
                node.pos[1] = std::isfinite(y) ? y : 0.0f;
            }
            asset.nodes.push_back(std::move(node));
        }
    }
    if (!BtLinkAsset(asset)) {
        return false;
    }
    out = std::move(asset);
    return true;
}

namespace behaviortree {

namespace {
BehaviorTreeLibrary* sLibrary = nullptr;
} // namespace

void Install(BehaviorTreeLibrary* lib)
{
    sLibrary = lib;
}

BehaviorTreeLibrary* Library()
{
    return sLibrary;
}

} // namespace behaviortree

} // namespace mye
