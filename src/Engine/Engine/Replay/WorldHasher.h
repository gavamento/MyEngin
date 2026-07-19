#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class CpuParticleBackend;

// ワールド状態ハッシュ (engine_spec.md 11.3)。
// 対象: 全エンティティ (index 昇順) の親リンク + シリアライズ対象コンポーネントの
//       登録フィールド (float はビットパターン)、ワールド RNG、CPU パーティクル状態。
// GPU パーティクルは描画出力扱いで除外 (比較モードで別途検証)。
// 実装を変更すると過去の .rep が検証不能になるため、変更時は ReplayFile の
// バージョンを上げること
uint64_t HashWorld(World& world, const CpuParticleBackend* cpuParticles);

// 乖離診断用: エンティティ毎のサブハッシュ (index 昇順)
struct EntityHash {
    EntityID entity;
    uint64_t hash;
};
void HashWorldDetailed(World& world, const CpuParticleBackend* cpuParticles,
                       std::vector<EntityHash>& outEntities, uint64_t& outTotal);

} // namespace mye
