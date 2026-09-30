#pragma once
#include <cstdint>

namespace mye {

// 世代付きハンドル (engine_spec.md 4.2)。
// スロットは再利用されるが generation がインクリメントされるため、
// 破棄済みエンティティへの古いハンドルは無効として検出できる。
struct EntityID {
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;

    bool IsNull() const { return index == kInvalidIndex; }

    friend bool operator==(EntityID a, EntityID b)
    {
        return a.index == b.index && a.generation == b.generation;
    }
    friend bool operator!=(EntityID a, EntityID b) { return !(a == b); }
};

inline constexpr EntityID kNullEntity = {};

// コンポーネント型 ID (ComponentRegistry への登録順で決まる)
using ComponentTypeId = uint32_t;
inline constexpr ComponentTypeId kInvalidComponentType = 0xFFFFFFFFu;

// アセット参照 (正規化相対パスの FNV-1a 64bit ハッシュ)。
// 生ポインタでのアセット保持は禁止 (spec 8.2) — 常にこの ID を介して参照する
struct AssetID {
    uint64_t value = 0;
    bool IsNull() const { return value == 0; }
    friend bool operator==(AssetID a, AssetID b) { return a.value == b.value; }
    friend bool operator!=(AssetID a, AssetID b) { return a.value != b.value; }
};

} // namespace mye
