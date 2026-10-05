//====================================================================================
//                          BehaviorTreeLibrary.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリー資産 (.bt.json) とノード種類の表 (M85)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace mye {

constexpr int kBtMaxNodes = 1024;  // 1 アセットのノード数の上限
constexpr int kBtMaxDepth = 64;    // 根からの深さの上限 (実行器は再帰で降りるのでスタックを守る)
constexpr int kBtMaxStepsPerTick = 256; // 1 体 1 tick のノード訪問の手数の上限 (超えたらその tick はそこで止めて次の tick に続ける)
constexpr int kBtMaxTicksParam = 216000; // tick 数のパラメータの上限 (60 Hz で 1 時間)

// ノードの種類。値は btNodeTypes の添字で、ファイルには名前で保存する (並べ替えても保存形式は変わらない)
enum class BtNodeKind : uint8_t {
    Selector,
    Sequence,
    SimpleParallel,
    Wait,
    Count,
};

// エディタのパレットの分類 (UE の Composites / Decorators / Tasks に相当)
enum class BtNodeCategory : uint8_t {
    Composite,
    Task,
};

enum class BtParamType : uint8_t {
    Int,
    Float,
    Bool,
    Enum, // 値は enumNames の添字、ファイルには名前で保存する
};

// ノードのパラメータ 1 つの記述。エディタ (パラメータ欄) と読み書き (範囲の丸め) が同じ表を引く
struct BtParamDesc {
    const char* name;
    BtParamType type;
    float defaultValue;                  // Enum は添字
    float minValue;                      // Int / Float の範囲
    float maxValue;
    const char* const* enumNames;        // Enum のときだけ
    int enumCount;
};

constexpr int kBtUnlimitedChildren = -1;

struct BtNodeTypeInfo {
    BtNodeKind kind;
    const char* name;                    // ファイルの "type"
    BtNodeCategory category;
    int minChildren;
    int maxChildren;                     // kBtUnlimitedChildren = 上限なし
    const BtParamDesc* params;
    int paramCount;
};

const BtNodeTypeInfo& BtNodeTypeOf(BtNodeKind kind);
const BtNodeTypeInfo* BtFindNodeType(const std::string& name); // 無ければ nullptr

// パラメータの値 1 つ。Int / Bool / Enum は i、Float は f
struct BtParamValue {
    int32_t i = 0;
    float f = 0.0f;
};

// SimpleParallel の finishMode
namespace btparallelfinish {
enum : int32_t {
    kImmediate = 0, // メインが終わったら背景を Abort する
    kDelayed = 1,   // メインが終わっても背景が終わるまで待つ
};
} // namespace btparallelfinish

struct BtNodeDef {
    int32_t id = 0;                      // ファイル内で一意 (>= 0)。ライブ表示と ABI の「実行中ノード」はこの値
    BtNodeKind kind = BtNodeKind::Selector;
    std::vector<BtParamValue> params;    // 種類の params と同じ長さ・同じ並び
    std::vector<int32_t> childIds;       // 左から右 = 優先順
    float pos[2] = {};                   // エディタの表示位置 (実行には使わない)

    // ---- BtLinkAsset が childIds から作る導出値 ----
    std::vector<int32_t> children;       // nodes の添字
    int32_t parent = -1;                 // nodes の添字。根と、どこにもつながっていないノードは -1
};

// ビヘイビアツリー 1 本。値はワールドハッシュに入れない (ファイルの中身は provenance の contentHash が守る。
// 実行状態は BehaviorTreeSystem が持つ)。読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ
struct BehaviorTreeAsset {
    uint64_t hash = 0;                   // = GUID (BehaviorTreeLibrary のキー)
    std::string name;
    std::wstring path;
    uint64_t blackboard = 0;             // 使う .bb.json の GUID。0 = ブラックボードなし
    int32_t rootId = -1;                 // -1 = 根なし (何も実行しない)
    std::vector<BtNodeDef> nodes;        // ファイルの順 (エディタの往復で並びを変えない)

    // ---- BtLinkAsset が作る導出値 ----
    int32_t rootIndex = -1;              // nodes の添字
    int32_t stateSlotCount = 0;          // 実行状態 (BtNodeState) の欄の数。今はノード数と同じ

    // id からノードの添字。無ければ -1
    int FindNode(int32_t id) const;
};

// childIds から children / parent / rootIndex を作り、木として成り立つか検査する。
// 成り立たない (id の重複・存在しない子・子の数の違反・親が 2 つ・循環・深すぎ・根に親がある) なら false。
// BehaviorTreeLibrary::FromJson とエディタの保存前の検査が使う
bool BtLinkAsset(BehaviorTreeAsset& asset);

struct BehaviorTreeEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .bt.json の管理 (NavFilterLibrary と同じ形)。EngineLoop / HeadlessSim が所有し behaviortree:: で注入
class BehaviorTreeLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, BehaviorTreeAsset asset); // 返り値 = hash。同じ GUID なら置き換える

    // 置き換わった後も、実行中のエンティティが古い木を持ち続けて後始末できるよう shared_ptr で渡す。
    // 実行器は「持っている木 != 今の登録」で読み直しを検出する
    std::shared_ptr<const BehaviorTreeAsset> GetShared(uint64_t hash) const;
    const BehaviorTreeAsset* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return assets_.find(hash) != assets_.end(); }
    std::vector<BehaviorTreeEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const BehaviorTreeAsset& asset);
    // 読み値は Sanitize 済みで返す。木でない JSON ("behaviortree" キー無し)・未知の type・壊れた参照・循環は false
    static bool FromJson(const nlohmann::json& j, BehaviorTreeAsset& out);

private:
    std::unordered_map<uint64_t, std::shared_ptr<const BehaviorTreeAsset>> assets_;
};

// モジュール注入 (navfilter:: と同じ流儀)。所有者が起動時に Install し、終了時にライブラリ破棄前に外す。メインスレッド専用
namespace behaviortree {
void Install(BehaviorTreeLibrary* lib);
BehaviorTreeLibrary* Library();              // 未接続 = nullptr
} // namespace behaviortree

} // namespace mye
