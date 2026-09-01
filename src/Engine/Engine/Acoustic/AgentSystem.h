//====================================================================================
//                          AgentSystem.h
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          敵の共通 FSM とセンサー（聴覚／光）
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Engine/Acoustic/AcousticNav.h"

namespace mye {

class World;
class AcousticField;
class Pcg32;

// 敵の状態 (AgentBrainComponent::state の正本)。
// ★**5 状態固定**。増やすなら .json 資産化を先に検討すること (計画 判断 7)
enum AgentState : int32_t {
    kAgentPatrol = 0, // 巡回: home の周りを歩き、自分でも音を出す
    kAgentAlert = 1,  // 警戒: **止まって黙る**。企画 §6-3「波が止まる」の実体
    kAgentSearch = 2, // 探索: 最後に聞いた場所の周りを不規則に歩く
    kAgentChase = 3,  // 追跡: 走って刺激源へ向かう
    kAgentReturn = 4, // 帰還: home へ戻る
};

// 敵の思考 (M65f、計画 hushed-rippling-beacon)。
//
// ★**1 つの FSM に差し替え可能なセンサーを挿す**構造。企画の 2 種類の敵の違いは
//   「どのセンサーコンポーネントが載っているか」だけで、遷移表は共有する:
//     音の敵 = AcousticListenerComponent / 光の敵 = LightSeekerComponent。
//   両方載せると音が優先される (音は事象で減衰窓を持ち、光は常在だから)。
//
// ★**フェーズ 3.4 = スクリプト層 (3) の後・物理 (3.6) の前**で回す。つまり
//   同じエンティティにスクリプトが付いていると **AI が後から moveInput を上書きして勝つ**。
//   これは仕様。手で動かしたい敵には AgentBrain を付けないこと。
//
// ★存在ゲート: AgentBrainComponent が 1 個も無ければ最初の走査で return する。
//   `world.Rng()` を引くのも**エージェントが居るときだけ** — 居ないシーンで 1 draw でも
//   引くと、既存の全リプレイが乱数列ごとずれる。
class AgentSystem {
public:
    // field は const ではない — エージェント自身が音を出す (企画 §6-3「敵も音を立てる」)
    void Update(World& world, AcousticField& field, uint64_t tick);

    // シーン遷移時 (航法グリッドを捨てる)
    void Reset() { nav_.Reset(); }

    // 統計 (ProfilerWindow / 診断用)。**sim 状態ではない**
    uint32_t LastAgentCount() const { return lastAgents_; }
    int LastFieldCount() const { return nav_.FieldCount(); }
    const AcousticNav& Nav() const { return nav_; }

private:
    AcousticNav nav_;
    uint32_t lastAgents_ = 0;
};

} // namespace mye
