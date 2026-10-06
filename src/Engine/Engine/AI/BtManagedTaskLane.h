//====================================================================================
//                          BtManagedTaskLane.h
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          BT の C# タスクを動かす別レーンの窓口 (M85)
//====================================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "nlohmann/json.hpp"

namespace mye {

// RunTask の phase。Enter = ノードに入った tick (新しいインスタンスで OnStart)、Tick = 以降の毎 tick (OnTick)、
// Abort = 実行中に Abort された (OnAbort。インスタンスを捨てる)
namespace btmanagedphase {
enum : int32_t {
    kEnter = 0,
    kTick = 1,
    kAbort = 2,
};
} // namespace btmanagedphase

// RunTask の戻り値。0 / 1 / 2 は MyeBtStatus (Running / Success / Failure)、これは「その名前のクラスが無い」
constexpr int32_t kBtManagedUnknownClass = -1;

// C# タスクの実体 (managed 側のインスタンス) を持つ別レーン。実装は ManagedHost。
// BehaviorTreeSystem は null の間 (記録・検証・Net・再シムで C# レーンが止まっている) CsTask を Failure にする。
// インスタンスは World の外にあり、巻き戻しでは戻らない (決定論の保証外)
class BtManagedTaskLane {
public:
    virtual ~BtManagedTaskLane() = default;

    // owner の木のノード nodeIndex (展開後の実行木の添字) の C# タスク className を 1 手進める。
    // fieldsJson: 新しいインスタンスを作った直後 (OnStart の前) に書くフィールドの値 (JSON オブジェクト。空 = 何も書かない)。
    // インスタンスを作らない呼び出しでは使われない。Abort では空
    // 戻り値は MyeBtStatus か kBtManagedUnknownClass。Abort の戻り値は使わない
    virtual int32_t RunTask(EntityID owner, int32_t nodeIndex, const std::string& className, int32_t phase, uint64_t tick,
                            const std::string& fieldsJson) = 0;
};

// C# タスクのフィールドの型。MyeScripting.dll の一覧 (ScriptRuntime.BtTaskCatalog) が返す "type" の文字列と対応する
enum class BtManagedFieldType : int32_t {
    Bool,
    Int,
    Float,
    String,
    Vector3,
};

// [BtTask] クラスの public なインスタンスフィールド 1 つの記述子。defaultValue はインスタンスを 1 つ作って読んだ値 (taskFields と同じ JSON の形)
struct BtManagedTaskField {
    std::string name;
    BtManagedFieldType type = BtManagedFieldType::Int;
    nlohmann::json defaultValue;
};

// JSON の値 value が type のフィールドに書けるか。C# 側 (ScriptRuntime.TryReadBtField) と同じ規則:
// int は整数値の数、float は数、Vector3 は数 3 つの配列
inline bool BtManagedFieldAccepts(BtManagedFieldType type, const nlohmann::json& value)
{
    switch (type) {
    case BtManagedFieldType::Bool: return value.is_boolean();
    case BtManagedFieldType::Int: {
        if (value.is_number_integer()) {
            return true;
        }
        if (!value.is_number()) {
            return false;
        }
        const double number = value.get<double>();
        return number == std::floor(number) && number >= -2147483648.0 && number <= 2147483647.0;
    }
    case BtManagedFieldType::Float: return value.is_number();
    case BtManagedFieldType::String: return value.is_string();
    case BtManagedFieldType::Vector3:
        return value.is_array() && value.size() == 3
               && std::all_of(value.begin(), value.end(), [](const nlohmann::json& element) { return element.is_number(); });
    }
    return false;
}

// [BtTask] クラス 1 つの記述子 (BT 窓のピッカーとフィールド欄の元)
struct BtManagedTaskClass {
    std::string name; // FullName。CsTask の params.class
    std::vector<BtManagedTaskField> fields;
};

} // namespace mye
