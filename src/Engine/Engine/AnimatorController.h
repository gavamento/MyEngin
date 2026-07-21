#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace mye {

class World;
class AnimationLibrary;

// 遷移条件の比較演算 (整数パラメータに対して。決定論)
enum class CondOp : int32_t { Gt = 0, Ge = 1, Lt = 2, Le = 3, Eq = 4, Ne = 5 };

struct ControllerParam {
    std::string name; // params index に対応 (0..3)
};

struct ControllerState {
    std::string name;
    std::string clipPath;   // .anim.json への相対パス (controller ファイルからの相対)
    uint64_t clipHash = 0;  // 解決済み AnimationClip ハッシュ (AnimationLibrary のキー)
    int32_t speed = 1;      // 1 tick あたりの進み tick 数
    int32_t loop = 1;       // 0=末尾停止 1=ループ
};

struct ControllerCondition {
    int32_t param = 0; // params index (0..3)
    CondOp op = CondOp::Gt;
    int32_t value = 0;
};

struct ControllerTransition {
    int32_t from = -1;       // -1 = Any State
    int32_t to = 0;
    int32_t duration = 8;    // ブレンド長 (tick)。0 は 1 に丸める
    int32_t hasExitTime = 0; // 1=state が末尾に達したときのみ遷移可
    std::vector<ControllerCondition> conditions; // 全て満たせば遷移 (AND)
};

struct ControllerAsset {
    uint64_t hash = 0;
    std::string name;
    std::wstring path;
    int32_t defaultState = 0;
    std::vector<ControllerParam> parameters;
    std::vector<ControllerState> states;
    std::vector<ControllerTransition> transitions;
};

// 列挙 1 件 (AssetRef ピッカー / Asset Browser 用)
struct ControllerEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .controller.json の管理 (AnimationLibrary 範型)
class ControllerLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // clipPath を解決して clipHash を埋める。失敗時 0
    uint64_t Register(const std::wstring& path, ControllerAsset asset); // 返り値 = hash
    bool SaveToFile(uint64_t hash) const;

    const ControllerAsset* Get(uint64_t hash) const;
    ControllerAsset* GetMutable(uint64_t hash);
    bool Contains(uint64_t hash) const { return controllers_.find(hash) != controllers_.end(); }
    std::vector<ControllerEntry> Enumerate() const;

    static nlohmann::json ToJson(const ControllerAsset& c);
    // FromJson は clipPath を読むが clipHash は解決しない (LoadFromFile が baseDir 相対で解決する)
    static bool FromJson(const nlohmann::json& j, ControllerAsset& out);

private:
    std::unordered_map<uint64_t, ControllerAsset> controllers_;
};

// AnimatorControllerComponent を評価してポーズを適用し、状態/遷移を進める (M22)。
// AnimatorControllerComponent 非存在シーンでは完全 no-op (既存シーンのリプレイ不変)。
// 時刻は tick、ブレンド係数は transitionTick/duration の整数比 → 決定論。
class AnimatorControllerSystem {
public:
    void Update(World& world, const ControllerLibrary& controllers, const AnimationLibrary& clips);
};

} // namespace mye
