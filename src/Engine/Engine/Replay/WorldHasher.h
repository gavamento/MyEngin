#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class CpuParticleBackend;
struct TimeControl;
class PersistStore;

// ワールド状態ハッシュ (engine_spec.md 11.3)。
// 対象: 全エンティティ (index 昇順) の親リンク + シリアライズ対象コンポーネントの
//       登録フィールド (float はビットパターン)、ワールド RNG、TimeControl と
//       PersistStore (M51g、RNG の直後・キー昇順)、CPU パーティクル状態。
// GPU パーティクルは描画出力扱いで除外 (比較モードで別途検証)。
// time / persist が null の項は畳み込まない (World 単体の selftest 用。EngineLoop の
// 記録/検証は常に Scene の実体を渡す)。
// 実装を変更すると過去の .rep が検証不能になるため、変更時は ReplayFile の
// バージョンを上げること
uint64_t HashWorld(World& world, const CpuParticleBackend* cpuParticles,
                   const TimeControl* time = nullptr, const PersistStore* persist = nullptr);

// 乖離診断用: エンティティ毎のサブハッシュ (index 昇順)
struct EntityHash {
    EntityID entity;
    uint64_t hash;
};
void HashWorldDetailed(World& world, const CpuParticleBackend* cpuParticles,
                       std::vector<EntityHash>& outEntities, uint64_t& outTotal,
                       const TimeControl* time = nullptr, const PersistStore* persist = nullptr);

} // namespace mye
