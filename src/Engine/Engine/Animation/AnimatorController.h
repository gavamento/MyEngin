#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class World;
class AnimationLibrary;
class SkinnedModelLibrary;
struct SkinnedModel;

// 遷移条件の比較演算 (決定論)
enum class CondOp : int32_t { Gt = 0, Ge = 1, Lt = 2, Le = 3, Eq = 4, Ne = 5 };

// パラメータの型 (M89c)。値は AnimatorControllerComponent::params に int32 で入り、Float はビット列。
// ★値は MyeAnimatorParamType (EngineAPI.h) と同じ
enum class ControllerParamType : int32_t {
    Int = 0,
    Float = 1,
    Bool = 2,    // 0 / 1
    Trigger = 3, // 1 = 立っている。条件に使った遷移が採用された tick に 0 へ戻る (消費)
};

struct ControllerParam {
    std::string name; // 並び順 = params の index (0..kMaxParams-1)
    ControllerParamType type = ControllerParamType::Int; // v1 (型の無い) アセットは全部 Int
};

struct ControllerState {
    std::string name;
    // 旧形式 (M39a 以前) の .anim.json 相対パス。読み込み後方互換のためだけに残る —
    // 保存は clipHash (= GUID) の数値参照で行い、リネーム/移動に追従する
    std::string clipPath;
    uint64_t clipHash = 0;  // 解決済み AnimationClip ハッシュ = GUID (AnimationLibrary のキー)
    int32_t speed = 1;      // 1 tick あたりの進み tick 数
    int32_t loop = 1;       // 0=末尾停止 1=ループ
    // 骨クリップ (M89b、.controller.json v2 の "skel":{"clip":"Walk"})。SkinnedModel のクリップ名で、
    // 駆動する SkinnedMesh ごとに FindClipByHash で index を引く。空 = 骨を駆動しないステート
    std::string skelClip;
    uint64_t skelClipHash = 0; // HashStr(skelClip)。skelClip が空なら 0
};

// 比べ方は参照するパラメータの型で決まる (宣言の無い index は Int):
// Int = params と value を op で / Float = params のビット列を float に戻して floatValue と op で /
// Bool = (params != 0) と (value != 0) を op で / Trigger = params != 0 なら真 (op と値は見ない)
struct ControllerCondition {
    int32_t param = 0; // params index (0..kMaxParams-1)
    CondOp op = CondOp::Gt;
    int32_t value = 0;
    float floatValue = 0.0f; // Float 型のパラメータと比べる値 (他の型では使わない)
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
    // FromJson の "clip" は両対応 (M39a): 数値 = GUID → clipHash 直接 / 文字列 = 旧相対パス →
    // clipPath に読むだけで clipHash は解決しない (LoadFromFile が baseDir 相対で解決する)
    static bool FromJson(const nlohmann::json& j, ControllerAsset& out);

private:
    std::unordered_map<uint64_t, ControllerAsset> controllers_;
};

// state 名から index を引く。無ければ -1 (同名が複数なら先頭)
int32_t FindControllerState(const ControllerAsset& controller, const std::string& name);

// ---- 型付きパラメータ (M89c) ----
// 名前の FNV-1a 64bit (HashStr) でパラメータの index を引く。同名が複数なら先頭。
// kMaxParams 以降の宣言は値の置き場が無いので引かない。無ければ -1
int32_t FindControllerParam(const ControllerAsset& controller, uint64_t nameHash);
// index の型 (宣言が無ければ Int)
ControllerParamType ControllerParamTypeAt(const ControllerAsset& controller, int32_t index);

// entity の Animator のパラメータ (名前のハッシュ) へ bits を書く。宣言の型が type と違う /
// Animator・controller・名前が無ければ何も変えず false。Bool / Trigger は bits != 0 を 1 に揃えて書く
bool AnimatorSetParam(World& world, EntityID entity, uint64_t nameHash, ControllerParamType type, int32_t bits,
                      const ControllerLibrary& controllers);

// ---- 骨クリップの駆動 (M89b) ----
// controller を持つ entity が駆動する SkinnedMesh を前順 (親 → 子を兄弟順) で out へ入れ直す。
// 自分を含む部分木が対象で、別の AnimatorController を持つ子孫の部分木は、そちらが駆動するので含めない
void CollectDrivenSkinnedMeshes(World& world, EntityID controllerEntity, std::vector<EntityID>& out);

// 主 SkinnedMesh のモデル = 上の集合のうち entity index が最小のもの。ステートの長さ (hasExitTime・
// ループ・BT の waitForEnd) はこのモデルの骨クリップで決める。無い・モデルが未登録なら null
const SkinnedModel* MainSkinnedModel(World& world, EntityID controllerEntity, const SkinnedModelLibrary* models);

// ステートの 1 周の長さ (tick、speed では割らない)。骨クリップが mainModel から引ければその長さ
// (SkeletalClipTicks)、引けなければプロパティクリップの lengthTicks、どちらも無ければ 0 (時刻が進まない)。
// ★Animator と BT の PlayAnimation が同じこの関数を通す — 片方だけ骨の長さを知らないと、
//   遷移の終わりと waitForEnd の終わりが食い違う
int32_t ControllerStateLengthTicks(const ControllerState& state, const AnimationLibrary* clips,
                                   const SkinnedModel* mainModel);

// entity の Animator を stateIndex のステートへ強制的に移す (BT の PlayAnimation 用)。
// durationTicks > 0: 今のポーズからその tick 数で混ぜる遷移を始める (遷移中なら遷移先だけを差し替えて混ぜ直す)。
// 0 以下: currentState を即切り替えて再生位置を 0 に戻し、遷移を捨てる。
// Animator が無い・controller が未登録・stateIndex が範囲外なら何も変えず false
bool AnimatorPlay(World& world, EntityID entity, int32_t stateIndex, int32_t durationTicks, const ControllerLibrary& controllers);

// AnimatorControllerComponent を評価してポーズを適用し、状態/遷移を進める (M22)。
// AnimatorControllerComponent 非存在シーンでは完全 no-op (既存シーンのリプレイ不変)。
// 時刻は tick、ブレンド係数は transitionTick/duration の整数比 → 決定論。
// 遷移は宣言順に見て、最初に条件をすべて満たしたものを採用する。採用した遷移の条件が参照する
// Trigger 型のパラメータはその場で 0 に戻す (M89c。採用されなかった遷移は消費しない)。
//
// 骨クリップ (M89b): 骨クリップを持つステートがあるコントローラは、時刻を進めた後に、駆動する
// SkinnedMesh すべてへポーズプログラム (今のステート 1 層、遷移中は元と先の 2 層) を書いて
// poseClaim を立てる。SkinningSystem より前に呼ぶこと。
// - 骨クリップの無いステートは層を出さない。遷移の片側だけが骨クリップを持つなら、そちらを重み満杯で出す。
//   どちらも持たない tick は書かない = SkinnedMesh は旧経路 (clip / timeTicks) に戻る
// - entity が非アクティブの間は、プログラムを持っている SkinnedMesh の claim だけを立てて凍らせる
//   (旧経路の時計が裏で進んで、再びアクティブになった瞬間に別のポーズへ飛ばないため)
// - models が null (または SkinnedMesh のモデルが未登録) なら骨クリップの長さは引けない (0 = 進まない)
class AnimatorControllerSystem {
public:
    void Update(World& world, const ControllerLibrary& controllers, const AnimationLibrary& clips,
                const SkinnedModelLibrary* models = nullptr);

private:
    std::vector<EntityID> driven_; // 走査用の作業領域 (sim 状態ではない)
};

} // namespace mye
