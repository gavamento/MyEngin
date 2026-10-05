//====================================================================================
//                          BtTaskRegistry.h
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          BT の C++ タスクの登録表 (GameLogic.dll の記述子の写し、M85)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

#include "Shared/ScriptTypes.h"

namespace mye {

constexpr uint32_t kBtCppTaskMaxStateBytes = 112; // タスクの状態 (T) の上限。超えたタスクは登録されない
constexpr uint32_t kBtCppTaskMaxFields = 32;      // REGISTER_SCRIPT と同じ上限
constexpr int kBtCppTaskExtraBytes = 128;         // CppTask ノード 1 つの追加状態 (BtCppTaskHeader + 状態)

// CppTask の追加状態の先頭 (BT 節に生バイトで入る。パディングを持たない 16 バイト)。この後ろに状態 (T) が続く
struct BtCppTaskHeader {
    uint64_t layoutHash = 0; // 入ったときのタスクの layoutHash。0 = 入っていない
    uint32_t stateSize = 0;  // 入ったときのタスクの stateSize
    uint32_t reserved = 0;
};
static_assert(sizeof(BtCppTaskHeader) == 16, "BtCppTaskHeader はパディングなしの 16 バイト (BT 節の生バイトに入る)");
static_assert(sizeof(BtCppTaskHeader) + kBtCppTaskMaxStateBytes == static_cast<size_t>(kBtCppTaskExtraBytes),
              "CppTask の追加状態 = ヘッダ + 状態の上限");

// タスクのフィールド 1 つ (MyeScriptField の写し。文字列は持ち主のコピー)
struct BtTaskField {
    std::string name;
    int32_t type = MYE_FIELD_FLOAT; // MyeFieldType
    uint32_t offset = 0;
    std::string displayName;        // 空 = name
    float rangeMin = 0.0f;          // 両方 0 = 範囲指定なし
    float rangeMax = 0.0f;
};

// 登録されたタスク 1 種。関数ポインタは DLL 内を指し、ホットリロードのたびに差し替わる
struct BtTaskType {
    std::string name;
    uint32_t stateSize = 0;
    uint64_t layoutHash = 0;
    std::vector<BtTaskField> fields;
    void (*construct)(void* dst) = nullptr;
    int32_t (*onStart)(void* state, MyeBtTaskContext* ctx) = nullptr;
    int32_t (*onTick)(void* state, MyeBtTaskContext* ctx) = nullptr;
    void (*onAbort)(void* state, MyeBtTaskContext* ctx) = nullptr;
};

// BT の C++ タスクの登録表。ScriptHost が LoadModule のたびに更新し、BehaviorTreeSystem が実行時に引く。
// 名前で引き直すので、DLL を差し替えても木の側は何も変えない。メインスレッド専用
class BtTaskRegistry {
public:
    // module の btTasks を取り込む。今回の DLL に無い名前は登録から外す。stateSize が上限を超える / 整列が 16 を超える /
    // フィールドが範囲外 (未知の型・状態からはみ出す・32 個超) / 同名の重複はそのタスクだけ警告して取り込まない。
    // api はタスクのコンテキストに入れる (MyeBtTaskContext::api)
    void Update(const MyeEngineApi* api, const MyeBtTaskDesc* descs, uint32_t count);
    // 全部外す (DLL を解放する前)
    void Clear();

    // 登録されているタスク。無ければ nullptr
    const BtTaskType* Find(std::string_view name) const;
    // 名前昇順
    std::vector<const BtTaskType*> Enumerate() const;
    const MyeEngineApi* Api() const { return api_; }
    // Update / Clear のたびに増える (消えた名前の警告を出し直す目安)
    uint64_t Generation() const { return generation_; }

private:
    std::map<std::string, BtTaskType, std::less<>> types_;
    const MyeEngineApi* api_ = nullptr;
    uint64_t generation_ = 0;
};

// フィールド 1 つぶんのバイト数。未対応の型は 0
uint32_t BtTaskFieldSize(int32_t type);
// state (stateSize バイト) の field へ JSON の値を書く。型に合わない値・未対応の型 (EntityRef) は書かず false
bool BtTaskWriteField(const BtTaskField& field, const nlohmann::json& value, void* state);
// state の field を JSON にする。未対応の型は null
nlohmann::json BtTaskReadField(const BtTaskField& field, const void* state);
// 登録フィールドに入らないバイト (登録していないメンバとパディング) を 0 にする。状態のバイト列は BT 節とワールドハッシュに入るので、
// Debug / Release でパディングの中身が違っても同じ値になるようにする。state は stateSize バイト以上
void BtTaskCanonicalizeState(const BtTaskType& task, void* state);
// state を construct の既定値にし、fields (JSON のオブジェクト) の値を名前で上書きして BtTaskCanonicalizeState までかける。
// state は stateSize バイト以上
void BtTaskInitState(const BtTaskType& task, const nlohmann::json& fields, void* state);

} // namespace mye
