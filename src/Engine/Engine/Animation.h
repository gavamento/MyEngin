#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Core/Reflection.h"

namespace mye {

class World;

// 補間方式 (linear / step のみ — カーブエディタは範囲外)
enum class AnimInterp : int32_t { Linear = 0, Step = 1 };

// キーフレーム。value は最大 4 float (Float/Float2/Float3/Float4/Quat/Color を一様に格納)
struct AnimKey {
    int32_t tick = 0;
    std::array<float, 4> value{ { 0.0f, 0.0f, 0.0f, 0.0f } };
};

// 1 トラック = (対象エンティティ, コンポーネント, フィールド) に対するキー列。
// target = アニメータサブツリー内の DFS index (0 = アニメータ自身)。
// component/field 名は保存/表示用、comp/offset/type は LoadFromJson で解決した実行時値
struct AnimTrack {
    uint64_t target = 0;
    std::string component;
    std::string field;
    AnimInterp interp = AnimInterp::Linear;
    ComponentTypeId comp = kInvalidComponentType; // 未解決なら実行時スキップ
    uint32_t offset = 0;
    FieldType type = FieldType::Float;
    uint32_t compCount = 1; // float 要素数 (1/2/3/4)
    std::vector<AnimKey> keys; // tick 昇順
};

struct AnimationClipAsset {
    uint64_t hash = 0;
    std::string name;
    std::wstring path;
    int32_t lengthTicks = 60;
    std::vector<AnimTrack> tracks;
};

// 列挙 1 件 (AssetRef ピッカー / Asset Browser 用)
struct AnimClipEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み AnimationClip の管理 (Mesh/Prefab ライブラリと同じ役割)
class AnimationLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path);            // 失敗時 0
    uint64_t Register(const std::wstring& path, AnimationClipAsset clip); // 返り値 = hash
    bool SaveToFile(uint64_t hash) const;

    const AnimationClipAsset* Get(uint64_t hash) const;
    AnimationClipAsset* GetMutable(uint64_t hash);
    bool Contains(uint64_t hash) const { return clips_.find(hash) != clips_.end(); }
    std::vector<AnimClipEntry> Enumerate() const;

    // JSON <-> AnimationClipAsset (FromJson は component/field を解決する)
    static nlohmann::json ToJson(const AnimationClipAsset& clip);
    static bool FromJson(const nlohmann::json& j, AnimationClipAsset& out);

private:
    std::unordered_map<uint64_t, AnimationClipAsset> clips_;
};

// track を時刻 timeTicks でサンプルして comp のフィールドへ書く (Linear/Step、Quat は slerp)
void SampleTrackInto(void* comp, const AnimTrack& track, int32_t timeTicks);

// track を時刻 timeTicks でサンプルして 4 float バッファへ書く (書き込み先を選べる版)。
// M22 のステートブレンドで from/to のポーズを合成するのに使う。補間は整数比 → 決定論
void SampleTrackValues(const AnimTrack& track, int32_t timeTicks, float out[4]);

// FieldType の float 要素数 (Float=1 … Float4/Quat/Color=4、非対応型は 0)
uint32_t FieldFloatCount(FieldType t);

// clip を animator サブツリーへ適用 (timeTicks の位置でサンプル)。M22 の controller からも使う
void ApplyClipPose(World& w, EntityID animator, const AnimationClipAsset& clip, int32_t time);

// from→to のポーズをブレンド (blend=0→from, 1→to)。まず from を完全適用し、to の各トラックで
// 現在値 (=from) から to 値へ補間 (Quat は slerp)。blend は整数比 (遷移 tick/duration) → 決定論。
// 両クリップが同じフィールドを animate する前提 (idle↔walk 等)
void ApplyClipPoseBlended(World& w, EntityID animator, const AnimationClipAsset& from,
                          int32_t timeFrom, const AnimationClipAsset& to, int32_t timeTo, float blend);

class AnimationSystem {
public:
    // 全 AnimatorComponent を clip でサンプルして対象へ書き、timeTicks を進める。
    // AnimatorComponent 非存在シーンでは完全 no-op (spec: 既存シーンのリプレイ不変)
    void Update(World& world, const AnimationLibrary& lib);

    // 編集プレビュー: 1 つの animator を指定 tick でサンプルする (timeTicks は進めない)
    void Evaluate(World& world, const AnimationLibrary& lib, EntityID animator, int32_t timeTicks);
};

} // namespace mye
