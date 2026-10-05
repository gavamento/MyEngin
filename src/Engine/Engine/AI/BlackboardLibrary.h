//====================================================================================
//                          BlackboardLibrary.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ブラックボード資産 (.bb.json、M85)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// ブラックボードのキーの型 (UE の Blackboard Key のうち Bool / Int / Float / Vector / Object 相当だけ)
enum class BbType : uint8_t {
    Bool,
    Int,
    Float,
    Vector,
    Entity,
};

constexpr int kBbMaxKeys = 64;          // 1 アセットのキー数の上限
constexpr size_t kBbMaxNameBytes = 63;  // キー名・eventName の最大バイト数

const char* BbTypeName(BbType type);
bool BbTypeFromName(const std::string& name, BbType& out);

// 実行時の値 1 つ。型は BlackboardAsset のキーが持つ。isSet が 0 の間は他のフィールドを読まない (0 のまま)。
// BT 節とハッシュに入る sim 状態
struct BbValue {
    uint8_t isSet = 0;
    int32_t i = 0;                // Bool (0 / 1) と Int
    float f = 0.0f;
    float v[3] = {};              // Vector
    EntityID entity = kNullEntity;
};

struct BbKeyDef {
    std::string name;
    BbType type = BbType::Bool;
    BbValue initial;              // isSet = 0 なら初期値なし (未設定から始まる)
    std::string eventName;        // 空でなければ、この名前のイベントが届いたらこのキーへ書く (M85 sub-05 以降)
};

// ブラックボード 1 つ。BehaviorTreeComponent 1 個につき、この定義から作った値の列が 1 つ付く。
// 決定論: 値はワールドハッシュに入れない (BT の実行状態が持つ値だけが sim 状態。ファイルの中身は provenance の contentHash が守る)。
// 読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ
struct BlackboardAsset {
    uint64_t hash = 0;            // = GUID (BlackboardLibrary のキー)
    std::string name;
    std::wstring path;
    std::vector<BbKeyDef> keys;   // 順序がそのまま値の列の添字

    // 名前からキーの添字。無ければ -1
    int FindKey(const std::string& keyName) const;
};

struct BlackboardEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .bb.json の管理 (NavFilterLibrary と同じ形)。EngineLoop / HeadlessSim が所有し blackboard:: で注入
class BlackboardLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, BlackboardAsset asset); // 返り値 = hash

    // Register で置き換わった後も、実行中の BT が古い定義を持ち続けられるよう shared_ptr で渡す
    std::shared_ptr<const BlackboardAsset> GetShared(uint64_t hash) const;
    const BlackboardAsset* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return assets_.find(hash) != assets_.end(); }
    std::vector<BlackboardEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const BlackboardAsset& asset);
    // 読み値は Sanitize 済みで返す。ブラックボードでない JSON ("blackboard" キー無し)・キーの重複・名前の不正・型の不明は false
    static bool FromJson(const nlohmann::json& j, BlackboardAsset& out);

private:
    std::unordered_map<uint64_t, std::shared_ptr<const BlackboardAsset>> assets_;
};

// モジュール注入 (navfilter:: と同じ流儀)。所有者が起動時に Install し、終了時にライブラリ破棄前に外す。メインスレッド専用
namespace blackboard {
void Install(BlackboardLibrary* lib);
BlackboardLibrary* Library();                // 未接続 = nullptr
} // namespace blackboard

} // namespace mye
