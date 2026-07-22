#pragma once

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// 合成エフェクトのライフサイクル駆動 (M32e)。EngineLoop のフェーズ 3.5 (アニメ後・物理前) で
// simulateScripts 時のみ呼ばれる。EffectComponent を持つエンティティの経過 tick を進め:
//   - duration 到達で子サブツリーのエミッタ放出を停止 (ParticleEmitter.playing / Trail.emitting)
//   - looping なら巻き戻して子エミッタ + Animator を再開
//   - autoDestroy なら duration+linger 経過で自エンティティ (子孫ごと) を破棄 (tick 末構造変更)
// 全て整数 tick + コンポーネント書き込みのみ = 決定論。EffectComponent 非存在シーンは完全 no-op。
class EffectSystem {
public:
    void Update(World& world);

    // エフェクトを先頭から再生し直す (M32f、ABI RestartEffect / ループ再開で共用):
    // elapsedTicks=0, playing=1 にし、サブツリーの子エミッタ放出 + Animator を再開する。
    static void RestartEffect(World& world, EntityID root);
};

} // namespace mye
