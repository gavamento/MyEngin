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
#include "Engine/Engine/AI/BlackboardLibrary.h"
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

constexpr float kBtMaxRadius = 1000.0f; // 距離のパラメータの上限 (m)

// btmoveparam の並びと同じ
const BtParamDesc kMoveToParams[] = {
    { "acceptanceRadius", BtParamType::Float, 0.5f, 0.0f, kBtMaxRadius, nullptr, 0 },
    { "observeTarget", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "failOnStuck", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
    { "navFilter", BtParamType::Guid, 0.0f, 0.0f, 0.0f, nullptr, 0 },
};

// btrotateparam の並びと同じ
const BtParamDesc kRotateToParams[] = {
    { "angularSpeedDeg", BtParamType::Float, 0.0f, 0.0f, 3600.0f, nullptr, 0 },
    { "toleranceDeg", BtParamType::Float, 5.0f, 0.0f, 180.0f, nullptr, 0 },
};

const char* const kSetSourceNames[] = { "Constant", "Self", "Copy" };

// btsetparam の並びと同じ
const BtParamDesc kSetBlackboardParams[] = {
    { "source", BtParamType::Enum, static_cast<float>(btsetparam::kConstant), 0.0f, 2.0f, kSetSourceNames, 3 },
    { "boolValue", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
    { "intValue", BtParamType::Int, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
    { "floatValue", BtParamType::Float, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
    { "vectorX", BtParamType::Float, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
    { "vectorY", BtParamType::Float, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
    { "vectorZ", BtParamType::Float, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
};

// btrandomparam / btnearestparam / btsearchparam / btfindtargetparam の並びと同じ
const BtParamDesc kFindRandomPointParams[] = {
    { "radius", BtParamType::Float, 10.0f, 0.0f, kBtMaxRadius, nullptr, 0 },
};
const BtParamDesc kFindNearestTargetParams[] = {
    { "sight", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "hearing", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "damage", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "touch", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "currentlySensedOnly", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
};
const BtParamDesc kSearchAreaParams[] = {
    { "usePrediction", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
    { "radius", BtParamType::Float, 5.0f, 0.0f, kBtMaxRadius, nullptr, 0 },
    { "pointCount", BtParamType::Int, 4.0f, 1.0f, static_cast<float>(kBtMaxSearchPoints), nullptr, 0 },
    { "failOnStuck", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
};
const BtParamDesc kFindTargetParams[] = {
    { "radius", BtParamType::Float, 15.0f, 0.0f, kBtMaxRadius, nullptr, 0 },
    { "enemies", BtParamType::Bool, 1.0f, 0.0f, 1.0f, nullptr, 0 },
    { "neutrals", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
    { "friendlies", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
    { "tagMask", BtParamType::Mask, 0.0f, 0.0f, 0.0f, nullptr, 0 },
};

const char* const kSendTargetNames[] = { "Self", "All", "Entity" };

// btsendparam の並びと同じ
const BtParamDesc kSendEventParams[] = {
    { "eventName", BtParamType::String, 0.0f, 0.0f, 0.0f, nullptr, 0 },
    { "target", BtParamType::Enum, static_cast<float>(btsendtarget::kAll), 0.0f, 2.0f, kSendTargetNames, 3 },
    { "floatValue", BtParamType::Float, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
    { "intValue", BtParamType::Int, 0.0f, -kBtValueLimit, kBtValueLimit, nullptr, 0 },
};

// btplayparam / btsubtreeparam の並びと同じ
const BtParamDesc kPlayAnimationParams[] = {
    { "state", BtParamType::String, 0.0f, 0.0f, 0.0f, nullptr, 0 },
    { "durationTicks", BtParamType::Int, 8.0f, 0.0f, static_cast<float>(kBtMaxTicksParam), nullptr, 0 },
    { "waitForEnd", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
};
const BtParamDesc kSubTreeParams[] = {
    { "tree", BtParamType::Guid, 0.0f, 0.0f, 0.0f, nullptr, 0 },
};

// btpatrolparam の並びと同じ
const BtParamDesc kPatrolParams[] = {
    { "acceptanceRadius", BtParamType::Float, 0.5f, 0.0f, kBtMaxRadius, nullptr, 0 },
    { "failOnStuck", BtParamType::Bool, 0.0f, 0.0f, 1.0f, nullptr, 0 },
};

const char* const kTargetKeyNames[] = { "target" };
const char* const kPatrolKeyNames[] = { "route" };
const char* const kSendEventKeyNames[] = { "target", "vector" };
const char* const kFindRandomPointKeyNames[] = { "center", "result" };
const char* const kFindNearestTargetKeyNames[] = { "target", "position" };
const char* const kSearchAreaKeyNames[] = { "origin", "endTarget" };
const char* const kSetBlackboardKeyNames[] = { "key", "sourceKey" };
const char* const kClearBlackboardKeyNames[] = { "key" };

// BtNodeKind の並びと同じ順に並べる (BtNodeTypeOf が添字で引く)
const BtNodeTypeInfo kNodeTypes[] = {
    { BtNodeKind::Selector, "Selector", BtNodeCategory::Composite, 0, kBtUnlimitedChildren, nullptr, 0, nullptr, 0, 0 },
    { BtNodeKind::Sequence, "Sequence", BtNodeCategory::Composite, 0, kBtUnlimitedChildren, nullptr, 0, nullptr, 0, 0 },
    { BtNodeKind::SimpleParallel, "SimpleParallel", BtNodeCategory::Composite, 2, 2, kParallelParams, 1, nullptr, 0, 0 },
    { BtNodeKind::Wait, "Wait", BtNodeCategory::Task, 0, 0, kWaitParams, 2, nullptr, 0, 0 },
    { BtNodeKind::MoveTo, "MoveTo", BtNodeCategory::Task, 0, 0, kMoveToParams, 4, kTargetKeyNames, 1,
      static_cast<int>(sizeof(BtMoveToState)) },
    { BtNodeKind::RotateTo, "RotateTo", BtNodeCategory::Task, 0, 0, kRotateToParams, 2, kTargetKeyNames, 1,
      static_cast<int>(sizeof(BtRotateToState)) },
    { BtNodeKind::SetBlackboard, "SetBlackboard", BtNodeCategory::Task, 0, 0, kSetBlackboardParams, 7, kSetBlackboardKeyNames, 2, 0 },
    { BtNodeKind::ClearBlackboard, "ClearBlackboard", BtNodeCategory::Task, 0, 0, nullptr, 0, kClearBlackboardKeyNames, 1, 0 },
    { BtNodeKind::FindRandomPoint, "FindRandomPoint", BtNodeCategory::Ai, 0, 0, kFindRandomPointParams, 1, kFindRandomPointKeyNames, 2, 0 },
    { BtNodeKind::FindNearestTarget, "FindNearestTarget", BtNodeCategory::Ai, 0, 0, kFindNearestTargetParams, 5,
      kFindNearestTargetKeyNames, 2, 0 },
    { BtNodeKind::SearchArea, "SearchArea", BtNodeCategory::Ai, 0, 0, kSearchAreaParams, 4, kSearchAreaKeyNames, 2,
      static_cast<int>(sizeof(BtSearchAreaState)) },
    { BtNodeKind::FindTarget, "FindTarget", BtNodeCategory::Ai, 0, 0, kFindTargetParams, 5, kTargetKeyNames, 1, 0 },
    { BtNodeKind::SendEvent, "SendEvent", BtNodeCategory::Gameplay, 0, 0, kSendEventParams, 4, kSendEventKeyNames, 2, 0 },
    { BtNodeKind::PlayAnimation, "PlayAnimation", BtNodeCategory::Gameplay, 0, 0, kPlayAnimationParams, 3, nullptr, 0, 0 },
    // ファイルの SubTree は子を持たない。子 1 つ (取り込んだ部分木の根) は BtExpandSubTrees が実行用の木にだけ作る
    { BtNodeKind::SubTree, "SubTree", BtNodeCategory::Tree, 0, 1, kSubTreeParams, 1, nullptr, 0, 0 },
    { BtNodeKind::Patrol, "Patrol", BtNodeCategory::Task, 0, 0, kPatrolParams, 2, kPatrolKeyNames, 1,
      static_cast<int>(sizeof(BtPatrolState)) },
};
static_assert(sizeof(kNodeTypes) / sizeof(kNodeTypes[0]) == static_cast<size_t>(BtNodeKind::Count),
              "kNodeTypes を BtNodeKind の全値ぶん並べる");

const char* const kQueryNames[] = { "IsSet", "IsNotSet", "Equal", "NotEqual", "Less", "LessEqual", "Greater", "GreaterEqual" };
const char* const kAbortNames[] = { "None", "Self", "LowerPriority", "Both" };

constexpr float kBbCompareLimit = kBtValueLimit; // 比べる値の範囲

// btbbparam の並びと同じ
const BtParamDesc kBlackboardConditionParams[] = {
    { "query", BtParamType::Enum, static_cast<float>(btquery::kIsSet), 0.0f, 7.0f, kQueryNames, 8 },
    { "intValue", BtParamType::Int, 0.0f, -kBbCompareLimit, kBbCompareLimit, nullptr, 0 },
    { "floatValue", BtParamType::Float, 0.0f, -kBbCompareLimit, kBbCompareLimit, nullptr, 0 },
    { "abort", BtParamType::Enum, static_cast<float>(btabort::kNone), 0.0f, 3.0f, kAbortNames, 4 },
};

const BtParamDesc kCooldownParams[] = {
    { "ticks", BtParamType::Int, 60.0f, 0.0f, static_cast<float>(kBtMaxTicksParam), nullptr, 0 },
};

const BtParamDesc kRepeatParams[] = {
    { "count", BtParamType::Int, 2.0f, 0.0f, static_cast<float>(kBtMaxRepeatCount), nullptr, 0 }, // 0 = 無限
};

const BtParamDesc kTimeoutParams[] = {
    { "ticks", BtParamType::Int, 60.0f, 1.0f, static_cast<float>(kBtMaxTicksParam), nullptr, 0 },
};

// BtDecoratorKind の並びと同じ順に並べる
const BtDecoratorTypeInfo kDecoratorTypes[] = {
    { BtDecoratorKind::BlackboardCondition, "BlackboardCondition", kBlackboardConditionParams, 4, true },
    { BtDecoratorKind::Invert, "Invert", nullptr, 0, false },
    { BtDecoratorKind::Cooldown, "Cooldown", kCooldownParams, 1, false },
    { BtDecoratorKind::Repeat, "Repeat", kRepeatParams, 1, false },
    { BtDecoratorKind::Timeout, "Timeout", kTimeoutParams, 1, false },
};
static_assert(sizeof(kDecoratorTypes) / sizeof(kDecoratorTypes[0]) == static_cast<size_t>(BtDecoratorKind::Count),
              "kDecoratorTypes を BtDecoratorKind の全値ぶん並べる");

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
    } else if (desc.type == BtParamType::Guid || desc.type == BtParamType::Mask) {
        value.u = 0;
    } else if (desc.type == BtParamType::String) {
        value.s.clear();
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
    case BtParamType::Guid:
    case BtParamType::Mask: {
        if (!j.is_string()) {
            return false;
        }
        const std::string hex = j.get<std::string>();
        if (hex.empty()) {
            out.u = 0;
            return true;
        }
        char* end = nullptr;
        out.u = std::strtoull(hex.c_str(), &end, 16);
        return end != nullptr && *end == '\0' && hex.size() <= 16;
    }
    case BtParamType::String: {
        if (!j.is_string() || j.get<std::string>().size() > kBbMaxNameBytes) {
            return false;
        }
        out.s = j.get<std::string>();
        return true;
    }
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
    case BtParamType::Guid:
    case BtParamType::Mask: return value.u != 0 ? GuidToHex(value.u) : std::string();
    case BtParamType::String: return value.s;
    case BtParamType::Enum:
        return desc.enumNames[(std::clamp)(value.i, 0, desc.enumCount - 1)];
    }
    return nullptr;
}

// "params" オブジェクトを descs の並びで読む。無い項目は既定値。1 つでも読めなければ false
bool ReadParamList(const BtParamDesc* descs, int count, const json& owner, std::vector<BtParamValue>& out)
{
    out.clear();
    for (int i = 0; i < count; ++i) {
        out.push_back(DefaultParam(descs[i]));
    }
    if (!owner.contains("params")) {
        return true;
    }
    const json& params = owner["params"];
    if (!params.is_object()) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (params.contains(descs[i].name) && !ReadParam(descs[i], params[descs[i].name], out[static_cast<size_t>(i)])) {
            return false;
        }
    }
    return true;
}

json WriteParamList(const BtParamDesc* descs, int count, const std::vector<BtParamValue>& values)
{
    json params = json::object();
    for (int i = 0; i < count && static_cast<size_t>(i) < values.size(); ++i) {
        params[descs[i].name] = WriteParam(descs[i], values[static_cast<size_t>(i)]);
    }
    return params;
}

// "keys" オブジェクトを info.keyNames の並びで読む。無い項目は空 (未指定)。文字列でない・長すぎる名前は false
bool ReadKeyList(const BtNodeTypeInfo& info, const json& owner, std::vector<std::string>& out)
{
    out.assign(static_cast<size_t>(info.keyCount), std::string());
    if (!owner.contains("keys")) {
        return true;
    }
    const json& keys = owner["keys"];
    if (!keys.is_object()) {
        return false;
    }
    for (int i = 0; i < info.keyCount; ++i) {
        if (!keys.contains(info.keyNames[i])) {
            continue;
        }
        const json& value = keys[info.keyNames[i]];
        if (!value.is_string() || value.get<std::string>().size() > kBbMaxNameBytes) {
            return false;
        }
        out[static_cast<size_t>(i)] = value.get<std::string>();
    }
    return true;
}

bool ReadDecorator(const json& j, BtDecoratorDef& out)
{
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        return false;
    }
    const BtDecoratorTypeInfo* info = BtFindDecoratorType(j["type"].get<std::string>());
    if (info == nullptr) {
        return false; // 未知の Decorator
    }
    out = BtDecoratorDef{};
    out.kind = info->kind;
    if (info->hasKey) {
        if (!j.contains("key") || !j["key"].is_string()) {
            return false;
        }
        out.key = j["key"].get<std::string>();
    }
    return ReadParamList(info->params, info->paramCount, j, out.params);
}

// 親が Selector でない BlackboardCondition の LowerPriority / Both は実行時に Self になる。読み込み時にだけ知らせる
void WarnIgnoredLowerPriority(const BehaviorTreeAsset& asset)
{
    for (const BtNodeDef& node : asset.nodes) {
        const bool parentIsSelector = node.parent >= 0 && asset.nodes[static_cast<size_t>(node.parent)].kind == BtNodeKind::Selector;
        for (const BtDecoratorDef& deco : node.decorators) {
            const int32_t abort = deco.kind == BtDecoratorKind::BlackboardCondition ? deco.params[btbbparam::kAbort].i : btabort::kNone;
            if ((abort == btabort::kLowerPriority || abort == btabort::kBoth) && !parentIsSelector) {
                MYE_LOG_WARN("[behaviortree] node %d: LowerPriority / Both only works under a Selector; treated as Self", node.id);
            }
        }
    }
}

} // namespace

const BtDecoratorTypeInfo& BtDecoratorTypeOf(BtDecoratorKind kind)
{
    return kDecoratorTypes[static_cast<size_t>(kind)];
}

const BtDecoratorTypeInfo* BtFindDecoratorType(const std::string& name)
{
    for (const BtDecoratorTypeInfo& info : kDecoratorTypes) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

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
            || node.params.size() != static_cast<size_t>(BtNodeTypeOf(node.kind).paramCount)
            || node.decorators.size() > static_cast<size_t>(kBtMaxDecoratorsPerNode)
            || node.keys.size() != static_cast<size_t>(BtNodeTypeOf(node.kind).keyCount)) {
            return false;
        }
        for (const std::string& key : node.keys) {
            if (key.size() > kBbMaxNameBytes) {
                return false;
            }
        }
        const BtNodeTypeInfo& typeInfo = BtNodeTypeOf(node.kind);
        for (int p = 0; p < typeInfo.paramCount; ++p) {
            if (typeInfo.params[p].type == BtParamType::String && node.params[static_cast<size_t>(p)].s.size() > kBbMaxNameBytes) {
                return false;
            }
        }
        for (const BtDecoratorDef& deco : node.decorators) {
            if (static_cast<size_t>(deco.kind) >= static_cast<size_t>(BtDecoratorKind::Count)) {
                return false;
            }
            const BtDecoratorTypeInfo& info = BtDecoratorTypeOf(deco.kind);
            const bool keyOk = info.hasKey ? !deco.key.empty() && deco.key.size() <= kBbMaxNameBytes : deco.key.empty();
            if (!keyOk || deco.params.size() != static_cast<size_t>(info.paramCount)) {
                return false;
            }
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

    // 実行状態の欄: 先頭 count 個がノード、続いて Decorator を (ノードの並び, 上から) の順に 1 つずつ
    // 種類別の追加状態: ノードの並びに固定長の領域を順に割り当てる
    int32_t nextSlot = static_cast<int32_t>(count);
    int32_t nextExtra = 0;
    for (size_t i = 0; i < count; ++i) {
        asset.nodes[i].children = std::move(children[i]);
        asset.nodes[i].parent = parent[i];
        asset.nodes[i].extraOffset = nextExtra;
        nextExtra += BtNodeTypeOf(asset.nodes[i].kind).extraStateBytes;
        for (BtDecoratorDef& deco : asset.nodes[i].decorators) {
            deco.slot = nextSlot++;
        }
    }
    asset.rootIndex = rootIndex;
    asset.stateSlotCount = nextSlot;
    asset.extraStateBytes = nextExtra;
    return true;
}

namespace {

// 実行用の木の組み立て作業
struct ExpandWork {
    const BehaviorTreeLibrary& library;
    BtExpansion& result;
    BehaviorTreeAsset& out;   // 組み立て中の実行用の木 (nodes は source の写しに部分木を足していく)
    int64_t nextId;
    bool overflow = false;    // ノード数・id が上限を超えた (全体を諦める)
};

void WarnSubTree(const BehaviorTreeAsset& source, int32_t nodeId, const char* reason)
{
    MYE_LOG_WARN("[behaviortree] '%s' node %d: SubTree cannot be used (%s); it fails when run", source.name.c_str(), nodeId, reason);
}

// src の根から届くノードを前順で out へ足す。id は連番で振り直し、子の参照も振り直した id へ向ける。足した根の id を返す
int32_t AppendSubTreeNodes(ExpandWork& work, const BehaviorTreeAsset& src)
{
    std::vector<int32_t> order; // 前順
    std::vector<int32_t> stack{ src.rootIndex };
    while (!stack.empty()) {
        const int32_t at = stack.back();
        stack.pop_back();
        order.push_back(at);
        const std::vector<int32_t>& children = src.nodes[static_cast<size_t>(at)].children;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    std::vector<int32_t> newIdOf(src.nodes.size(), -1);
    for (const int32_t at : order) {
        newIdOf[static_cast<size_t>(at)] = static_cast<int32_t>(work.nextId++);
    }
    for (const int32_t at : order) {
        BtNodeDef node = src.nodes[static_cast<size_t>(at)];
        node.id = newIdOf[static_cast<size_t>(at)];
        node.childIds.clear();
        for (const int32_t child : src.nodes[static_cast<size_t>(at)].children) {
            node.childIds.push_back(newIdOf[static_cast<size_t>(child)]);
        }
        node.children.clear();
        node.parent = -1;
        work.out.nodes.push_back(std::move(node));
    }
    return newIdOf[static_cast<size_t>(src.rootIndex)];
}

// out.nodes の [begin, end) にある SubTree へ部分木を取り込む。level = この範囲の木が何段目の入れ子か (source = 0)
void ResolveSubTrees(ExpandWork& work, const BehaviorTreeAsset& source, size_t begin, size_t end, int level)
{
    for (size_t i = begin; i < end && !work.overflow; ++i) {
        if (work.out.nodes[i].kind != BtNodeKind::SubTree) {
            continue;
        }
        const int32_t nodeId = work.out.nodes[i].id;
        const uint64_t guid = work.out.nodes[i].params[btsubtreeparam::kTree].u;
        if (guid == 0) {
            WarnSubTree(source, nodeId, "no tree is set");
            continue;
        }
        if (level + 1 > kBtMaxSubTreeDepth) {
            WarnSubTree(source, nodeId, "nested deeper than the limit");
            continue;
        }
        std::shared_ptr<const BehaviorTreeAsset> sub = work.library.GetShared(guid);
        work.result.deps.push_back(BtSubTreeDep{ guid, sub });
        if (!sub) {
            WarnSubTree(source, nodeId, "the tree is not registered");
            continue;
        }
        if (sub->blackboard != source.blackboard) {
            WarnSubTree(source, nodeId, "its blackboard differs from the parent's");
            continue;
        }
        if (sub->rootIndex < 0) {
            WarnSubTree(source, nodeId, "the tree has no root");
            continue;
        }
        const size_t firstNew = work.out.nodes.size();
        const int32_t rootId = AppendSubTreeNodes(work, *sub);
        if (work.out.nodes.size() > static_cast<size_t>(kBtMaxNodes) || work.nextId > INT32_MAX) {
            work.overflow = true;
            return;
        }
        work.out.nodes[i].childIds = { rootId };
        ResolveSubTrees(work, source, firstNew, work.out.nodes.size(), level + 1);
    }
}

} // namespace

bool BtExpansion::IsCurrent(const BehaviorTreeLibrary& library, const std::shared_ptr<const BehaviorTreeAsset>& registered) const
{
    if (source != registered) {
        return false;
    }
    return std::all_of(deps.begin(), deps.end(), [&library](const BtSubTreeDep& dep) { return library.GetShared(dep.guid) == dep.asset; });
}

BtExpansion BtExpandSubTrees(const BehaviorTreeLibrary& library, std::shared_ptr<const BehaviorTreeAsset> source)
{
    BtExpansion result;
    result.source = source;
    result.tree = source;
    const bool hasSubTree = std::any_of(source->nodes.begin(), source->nodes.end(),
                                        [](const BtNodeDef& node) { return node.kind == BtNodeKind::SubTree; });
    if (!hasSubTree) {
        return result;
    }

    BehaviorTreeAsset expanded = *source;
    int64_t maxId = 0;
    for (const BtNodeDef& node : source->nodes) {
        maxId = (std::max)(maxId, static_cast<int64_t>(node.id));
    }
    ExpandWork work{ library, result, expanded, maxId + 1 };
    ResolveSubTrees(work, *source, 0, source->nodes.size(), 0);
    if (work.overflow || !BtLinkAsset(expanded)) {
        MYE_LOG_WARN("[behaviortree] '%s': the SubTrees make the tree too large or too deep; none of them is expanded (they fail when run)",
                     source->name.c_str());
        return result; // tree = source のまま。deps は残すので、部分木が小さく直れば作り直される
    }
    result.tree = std::make_shared<const BehaviorTreeAsset>(std::move(expanded));
    return result;
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
        n["params"] = WriteParamList(info.params, info.paramCount, node.params);
        if (info.keyCount > 0) {
            json keys = json::object();
            for (int i = 0; i < info.keyCount && static_cast<size_t>(i) < node.keys.size(); ++i) {
                keys[info.keyNames[i]] = node.keys[static_cast<size_t>(i)];
            }
            n["keys"] = std::move(keys);
        }
        json decorators = json::array();
        for (const BtDecoratorDef& deco : node.decorators) {
            const BtDecoratorTypeInfo& decoInfo = BtDecoratorTypeOf(deco.kind);
            json d;
            d["type"] = decoInfo.name;
            if (decoInfo.hasKey) {
                d["key"] = deco.key;
            }
            d["params"] = WriteParamList(decoInfo.params, decoInfo.paramCount, deco.params);
            decorators.push_back(std::move(d));
        }
        n["decorators"] = std::move(decorators);
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
            if (!ReadParamList(info->params, info->paramCount, n, node.params) || !ReadKeyList(*info, n, node.keys)) {
                return false;
            }
            if (n.contains("decorators")) {
                if (!n["decorators"].is_array() || n["decorators"].size() > static_cast<size_t>(kBtMaxDecoratorsPerNode)) {
                    return false;
                }
                for (const json& decoJson : n["decorators"]) {
                    BtDecoratorDef deco;
                    if (!ReadDecorator(decoJson, deco)) {
                        return false; // 未知の Decorator・型の違うパラメータ・キー名なし
                    }
                    node.decorators.push_back(std::move(deco));
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
    for (const BtNodeDef& node : asset.nodes) {
        if (node.kind == BtNodeKind::SubTree && !node.childIds.empty()) {
            return false; // ファイルの SubTree は子を持たない (子は実行用の木にだけ付く)
        }
    }
    WarnIgnoredLowerPriority(asset);
    out =std::move(asset);
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
