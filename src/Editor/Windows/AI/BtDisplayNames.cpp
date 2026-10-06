//====================================================================================
//                          BtDisplayNames.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                  ビヘイビアツリーの識別子を UI に出すときの表示名
//====================================================================================
#include "Editor/Windows/AI/BtDisplayNames.h"

#include <string_view>
#include <unordered_map>

#include "Engine/Core/Localization/Localization.h"

namespace mye {

namespace {

struct BtNameEntry {
    const char* identifier;
    StrId text;
};

// 同じ識別子は種類が違っても同じ表示名にする (例: "target" はキー欄とパラメータの両方にある)
constexpr BtNameEntry kBtNames[] = {
    // ノードの種類
    { "Selector", StrId::BtName_Selector },
    { "Sequence", StrId::BtName_Sequence },
    { "SimpleParallel", StrId::BtName_SimpleParallel },
    { "Wait", StrId::BtName_Wait },
    { "MoveTo", StrId::BtName_MoveTo },
    { "RotateTo", StrId::BtName_RotateTo },
    { "SetBlackboard", StrId::BtName_SetBlackboard },
    { "ClearBlackboard", StrId::BtName_ClearBlackboard },
    { "FindRandomPoint", StrId::BtName_FindRandomPoint },
    { "FindNearestTarget", StrId::BtName_FindNearestTarget },
    { "SearchArea", StrId::BtName_SearchArea },
    { "FindTarget", StrId::BtName_FindTarget },
    { "SendEvent", StrId::BtName_SendEvent },
    { "PlayAnimation", StrId::BtName_PlayAnimation },
    { "SubTree", StrId::BtName_SubTree },
    { "Patrol", StrId::BtName_Patrol },
    { "CppTask", StrId::BtName_CppTask },
    { "CsTask", StrId::BtName_CsTask },
    // Decorator
    { "BlackboardCondition", StrId::BtName_BlackboardCondition },
    { "Invert", StrId::BtName_Invert },
    { "Cooldown", StrId::BtName_Cooldown },
    { "Repeat", StrId::BtName_Repeat },
    { "Timeout", StrId::BtName_Timeout },
    // パラメータ
    { "finishMode", StrId::BtName_finishMode },
    { "ticks", StrId::BtName_ticks },
    { "randomDeviation", StrId::BtName_randomDeviation },
    { "acceptanceRadius", StrId::BtName_acceptanceRadius },
    { "observeTarget", StrId::BtName_observeTarget },
    { "failOnStuck", StrId::BtName_failOnStuck },
    { "navFilter", StrId::BtName_navFilter },
    { "angularSpeedDeg", StrId::BtName_angularSpeedDeg },
    { "toleranceDeg", StrId::BtName_toleranceDeg },
    { "source", StrId::BtName_source },
    { "boolValue", StrId::BtName_boolValue },
    { "intValue", StrId::BtName_intValue },
    { "floatValue", StrId::BtName_floatValue },
    { "vector", StrId::BtName_vector },
    { "radius", StrId::BtName_radius },
    { "sight", StrId::BtName_sight },
    { "hearing", StrId::BtName_hearing },
    { "damage", StrId::BtName_damage },
    { "touch", StrId::BtName_touch },
    { "currentlySensedOnly", StrId::BtName_currentlySensedOnly },
    { "usePrediction", StrId::BtName_usePrediction },
    { "pointCount", StrId::BtName_pointCount },
    { "enemies", StrId::BtName_enemies },
    { "neutrals", StrId::BtName_neutrals },
    { "friendlies", StrId::BtName_friendlies },
    { "tagMask", StrId::BtName_tagMask },
    { "eventName", StrId::BtName_eventName },
    { "target", StrId::BtName_target },
    { "state", StrId::BtName_state },
    { "durationTicks", StrId::BtName_durationTicks },
    { "waitForEnd", StrId::BtName_waitForEnd },
    { "tree", StrId::BtName_tree },
    { "task", StrId::BtName_task },
    { "class", StrId::BtName_class },
    { "query", StrId::BtName_query },
    { "abort", StrId::BtName_abort },
    { "count", StrId::BtName_count },
    // キー欄
    { "route", StrId::BtName_route },
    { "center", StrId::BtName_center },
    { "result", StrId::BtName_result },
    { "position", StrId::BtName_position },
    { "origin", StrId::BtName_origin },
    { "endTarget", StrId::BtName_endTarget },
    { "key", StrId::BtName_key },
    { "sourceKey", StrId::BtName_sourceKey },
    // 選択肢
    { "Immediate", StrId::BtName_Immediate },
    { "Delayed", StrId::BtName_Delayed },
    { "Constant", StrId::BtName_Constant },
    { "Self", StrId::BtName_Self },
    { "Copy", StrId::BtName_Copy },
    { "All", StrId::BtName_All },
    { "Entity", StrId::BtName_Entity },
    { "IsSet", StrId::BtName_IsSet },
    { "IsNotSet", StrId::BtName_IsNotSet },
    { "Equal", StrId::BtName_Equal },
    { "NotEqual", StrId::BtName_NotEqual },
    { "Less", StrId::BtName_Less },
    { "LessEqual", StrId::BtName_LessEqual },
    { "Greater", StrId::BtName_Greater },
    { "GreaterEqual", StrId::BtName_GreaterEqual },
    { "None", StrId::BtName_None },
    { "LowerPriority", StrId::BtName_LowerPriority },
    { "Both", StrId::BtName_Both },
};

const StrId* FindName(const char* identifier)
{
    static const std::unordered_map<std::string_view, StrId> table = [] {
        std::unordered_map<std::string_view, StrId> map;
        for (const BtNameEntry& entry : kBtNames) {
            map.emplace(entry.identifier, entry.text);
        }
        return map;
    }();
    if (identifier == nullptr) {
        return nullptr;
    }
    const auto it = table.find(identifier);
    return it != table.end() ? &it->second : nullptr;
}

} // namespace

const char* BtDisplayName(const char* identifier)
{
    const StrId* id = FindName(identifier);
    return id != nullptr ? Tr(*id) : (identifier != nullptr ? identifier : "");
}

std::string BtShortName(const char* identifier)
{
    // 日本語の表示名は「日本語名 (識別子)」の形なので、括弧の前だけを使う
    const std::string full = BtDisplayName(identifier);
    const size_t paren = full.find(" (");
    return paren != std::string::npos ? full.substr(0, paren) : full;
}

std::string BtFieldLabel(const char* identifier)
{
    return std::string(BtDisplayName(identifier)) + "##" + (identifier != nullptr ? identifier : "");
}

} // namespace mye
