//====================================================================================
//                          PerceptionSystem.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          AI の知覚 (視覚・聴覚・ダメージ・接触・予測)
//====================================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Rendering/DebugDraw.h"

namespace mye {

class World;
struct SolidContact;
struct AIPerceptionComponent;

// 相手の陣営への態度 (UE の ETeamAttitude)
enum class PerceptionAttitude : uint8_t {
    Friendly, // 同じ陣営
    Neutral,
    Hostile,  // hostileMask のビットが立つ陣営
};

// 観る側から見た陣営 targetFaction の態度。範囲外の陣営は中立
PerceptionAttitude PerceptionAttitudeOf(const AIPerceptionComponent& perception, int32_t targetFaction);

// 態度が detectEnemies / detectNeutrals / detectFriendlies で知覚対象になるか
bool PerceptionDetects(const AIPerceptionComponent& perception, PerceptionAttitude attitude);

// 音を鳴らす (UE の ReportNoiseEvent)。sim の中 (スクリプトの Update / LateUpdate / OnCollision など) から呼ぶ。
// hearingMode = Distance の AIPerception ごとに、目の位置との距離で減衰させた音量
// loudness * (1 - 距離 / range) を求め、hearingRange 以内かつ hearingThreshold 以上なら保留欄に書く
// (同じ tick に複数届いたら強い方、同じなら先に届いた方)。次の PerceptionSystem::Update が消費する。
// 位置は呼んだ時点のワールド行列 (前の tick の確定値) で測る。instigator は鳴らした者 (無ければ kNullEntity)。
// Acoustic モードの AIPerception には届かない (壁で回り込む音は AcousticEmitter で波を立てる。耳が拾えば聞こえる)。
// 戻り値は聞こえた AIPerception の数
int PerceptionReportNoise(World& world, const float pos[3], float loudness, float range, EntityID instigator);

// ダメージを知らせる (UE の ReportDamageEvent)。victim の AIPerception (damageEnabled) の保留欄に書く。
// 同じ tick に複数あれば量の大きい方。victim に AIPerception が無ければ false
bool PerceptionReportDamage(World& world, EntityID victim, EntityID instigator, float amount, const float hitPos[3]);

// observer から target が今見えるか (Update の視覚と同じ規則: 陣営・距離・視野角・視線。見失う距離は
// 前の tick に見ていたときだけ。視線の本数の上限は無い)。sim 状態を変えない。tick は今の tick 番号
bool PerceptionCanSee(World& world, EntityID observer, EntityID target, uint64_t tick);

// 計測用 (sim 状態ではない)
struct PerceptionStats {
    int perceivers = 0;    // 直近の Update で処理した AIPerception の数
    int losRays = 0;       // 直近の Update で撃った視線のレイの数
    int losColliders = 0;  // 視線の判定に集めたコライダーの数
    double updateUs = 0.0; // 直近の Update 全体
};

// AIPerception を持つエンティティの知覚を毎 tick 更新する。
// 状態は全部コンポーネント (AIPerception の percepts と保留欄) にあり、このクラスは持たない =
// スナップショット・ハッシュは World の通常経路で済む。AIPerception が 1 つも無いシーンでは走査だけで何もしない
class PerceptionSystem {
public:
    // tick ごとに呼ぶ (stepSim の中、音響 + AgentSystem の後・NavSystem の前)。
    // contacts は前の tick の物理のソリッド接触 (接触の感覚に使う)。読むワールド行列も前の tick の確定値
    void Update(World& world, uint64_t tick, float dt, const std::vector<SolidContact>& contacts);

    // drawDebug の立った AIPerception の線 (見えている相手への線・最後の位置・予測位置) を out へ足す。描画レーン
    void AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const;

    const PerceptionStats& Stats() const { return stats_; }

private:
    PerceptionStats stats_;
};

} // namespace mye
