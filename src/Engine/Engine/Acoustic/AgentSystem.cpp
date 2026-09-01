//====================================================================================
//                          AgentSystem.cpp
//  MyEngine/ 秋田蓮音                                                      09/01/2026
//                                          敵の共通 FSM とセンサーの実装
//====================================================================================
#include "Engine/Engine/Acoustic/AgentSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h"

namespace mye {
namespace {

// 走査 1 件ぶん。**entity.index 昇順**に並べてから処理する (規則 7)
struct Agent {
    EntityID entity = kNullEntity;
    AgentBrainComponent* brain = nullptr;
    CharacterControllerComponent* cc = nullptr;
    AcousticListenerComponent* ear = nullptr; // 無ければ耳が無い
    LightSeekerComponent* eye = nullptr;      // 無ければ目が無い
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// 光 1 個 (センサーの入力)。ライトは走査のたびに集め直す — エージェントが 1 体でも
// 居れば必ず全部見るので、キャッシュしても得が無いうえ隠れた状態になる
struct LightSample {
    EntityID entity = kNullEntity;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float intensity = 0.0f;
};

// 巡回の再目標づけ間隔と半径。**AgentBrain のフィールドにしていない**のは、
// 性格づけに使う値ではなく「立ち止まらないための下限」だから (増やす理由が出たら足す)
constexpr int32_t kPatrolRepathTicks = 120;
constexpr float kPatrolRadiusM = 4.0f;

float Dist2(float ax, float ay, float az, float bx, float by, float bz)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

void AgentSystem::Update(World& world, AcousticField& field, uint64_t tick)
{
    lastAgents_ = 0;

    // ---- 存在ゲート ----
    // AgentBrain を持つエンティティを集める。0 件ならここから先は 1 命令も走らないし、
    // **world.Rng() も 1 draw も引かない** (引くと既存の全リプレイが乱数列ごとずれる)
    std::vector<Agent> agents;
    {
        const ComponentTypeId req[] = { AgentBrainComponent::sTypeId,
                                        CharacterControllerComponent::sTypeId,
                                        WorldMatrixComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int bi = arch.FindTypeIndex(AgentBrainComponent::sTypeId);
            const int ci = arch.FindTypeIndex(CharacterControllerComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            const int li = arch.FindTypeIndex(AcousticListenerComponent::sTypeId);
            const int si = arch.FindTypeIndex(LightSeekerComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                Agent a;
                a.entity = e;
                a.brain = static_cast<AgentBrainComponent*>(arch.GetPtr(bi, row));
                a.cc = static_cast<CharacterControllerComponent*>(arch.GetPtr(ci, row));
                const auto& wm =
                    static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                a.x = wm.m[3][0];
                a.y = wm.m[3][1];
                a.z = wm.m[3][2];
                if (li >= 0) {
                    a.ear = static_cast<AcousticListenerComponent*>(arch.GetPtr(li, row));
                }
                if (si >= 0) {
                    a.eye = static_cast<LightSeekerComponent*>(arch.GetPtr(si, row));
                }
                agents.push_back(a);
            }
        });
    }
    if (agents.empty()) {
        return;
    }
    MYE_PROFILE_SCOPE("agent");
    std::sort(agents.begin(), agents.end(),
              [](const Agent& a, const Agent& b) { return a.entity.index < b.entity.index; });
    lastAgents_ = static_cast<uint32_t>(agents.size());

    // ---- 航法グリッド。**流れ場は毎 tick 捨てる** (判断 6) ----
    nav_.Sync(field);
    nav_.BeginTick();

    // ---- 光センサーの入力を 1 回だけ集める (entity.index 昇順) ----
    std::vector<LightSample> lights;
    {
        bool anyEye = false;
        for (const Agent& a : agents) {
            anyEye = anyEye || (a.eye != nullptr);
        }
        if (anyEye) {
            const ComponentTypeId req[] = { LightComponent::sTypeId,
                                            WorldMatrixComponent::sTypeId };
            world.ForEachArchetype(req, [&](Archetype& arch) {
                const int gi = arch.FindTypeIndex(LightComponent::sTypeId);
                const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
                for (uint32_t row = 0; row < arch.Count(); ++row) {
                    const EntityID e = arch.EntityAt(row);
                    if (!IsEntityActive(world, e)) {
                        continue;
                    }
                    const auto* lc = static_cast<const LightComponent*>(arch.GetPtr(gi, row));
                    // ★平行光は「置かれた光」ではないので見ない (太陽に向かって歩き出す)
                    if (lc->type == 0) {
                        continue;
                    }
                    const auto& wm =
                        static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                    LightSample s;
                    s.entity = e;
                    s.x = wm.m[3][0];
                    s.y = wm.m[3][1];
                    s.z = wm.m[3][2];
                    s.intensity = lc->intensity;
                    lights.push_back(s);
                }
            });
            std::sort(lights.begin(), lights.end(),
                      [](const LightSample& a, const LightSample& b) {
                          return a.entity.index < b.entity.index;
                      });
        }
    }

    Pcg32& rng = world.Rng();

    for (Agent& a : agents) {
        AgentBrainComponent& b = *a.brain;

        // 初回だけ現在地を home に焼く。★フラグを足さずに済ませるため
        //   「home が厳密に原点なら未設定」と決めている (デモは明示的に入れている)
        if (b.home.x == 0.0f && b.home.y == 0.0f && b.home.z == 0.0f) {
            b.home = { a.x, a.y, a.z };
            b.target = b.home;
        }

        // ---- センサー ----
        // 光: attractRadius 内で**最強**を選ぶ。同値は entity.index の小さいほうが勝つ
        // (走査が昇順なので「厳密に大きいときだけ更新」で自動的にそうなる)
        if (a.eye != nullptr) {
            a.eye->nearestLight = kNullEntity;
            a.eye->nearestPos = { 0.0f, 0.0f, 0.0f };
            a.eye->nearestStrength = 0.0f;
            const float r2 = a.eye->attractRadius * a.eye->attractRadius;
            for (const LightSample& s : lights) {
                if (s.intensity < a.eye->minIntensity) {
                    continue;
                }
                if (Dist2(a.x, a.y, a.z, s.x, s.y, s.z) > r2) {
                    continue;
                }
                if (s.intensity > a.eye->nearestStrength) {
                    a.eye->nearestStrength = s.intensity;
                    a.eye->nearestPos = { s.x, s.y, s.z };
                    a.eye->nearestLight = s.entity;
                }
            }
        }

        // 音: loseTicks の窓の中に到達があるか。★**音が光に勝つ** — 音は減衰窓を持つ
        //   事象で、光は常在。逆にすると設置光の前を通るたびに音を無視することになる
        // ★`lastHeardTick != 0` を必ず先に見ること。鏡の「まだ一度も聞いていない」は 0 で、
        //   **tick 0 ではそれが「今 tick に聞いた」と区別できない** — 実際、最初の実装は
        //   全個体が tick 0 で警戒状態に入っていた (probe で発見)
        const bool heardNow =
            (a.ear != nullptr) && a.ear->lastHeardTick != 0 && a.ear->lastHeardTick == tick;
        const uint64_t age =
            (a.ear != nullptr && a.ear->lastHeardTick != 0) ? (tick - a.ear->lastHeardTick) : ~0ull;
        const bool soundFresh =
            (a.ear != nullptr) && a.ear->lastHeardTick != 0
            && age <= static_cast<uint64_t>((b.loseTicks > 0) ? b.loseTicks : 0);
        const bool lightVisible = (a.eye != nullptr) && a.eye->nearestStrength > 0.0f;

        // ---- 状態遷移 ----
        // ★遷移は**整数の比較だけ**で決める (stateTicks と閾値)。距離で決める所は
        //   航法グリッドのセル一致に落としてある (ReachedTarget)
        const int32_t prevState = b.state;
        switch (b.state) {
        case kAgentPatrol:
        case kAgentReturn:
            if (heardNow) {
                b.state = kAgentAlert; // 何か聞こえた → まず**止まって黙る**
            } else if (lightVisible) {
                b.state = kAgentChase;
                b.target = a.eye->nearestPos;
            } else if (b.state == kAgentReturn) {
                const int fi = nav_.BuildFlowField(b.home.x, b.home.y, b.home.z);
                if (fi >= 0 && nav_.ReachedTarget(fi, a.x, a.y, a.z)) {
                    b.state = kAgentPatrol;
                }
            } else if ((b.stateTicks % kPatrolRepathTicks) == 0) {
                // ★巡回は home の**周りを歩く**。目標を home に固定すると、home に着いた
                //   瞬間に立ち止まって二度と動かない = 「巡回」という名前だけが残る。
                //   乱数を引くのはエージェントが居るときだけなので既存シーンは無風
                const float ang = rng.NextFloat01() * 6.2831853f;
                const float rad = rng.NextFloat01() * kPatrolRadiusM;
                b.target = { b.home.x + std::cos(ang) * rad, b.home.y,
                             b.home.z + std::sin(ang) * rad };
            }
            break;
        case kAgentAlert:
            // 企画 §6-3: 警戒中は**自分の波を 1 本も出さない**ので、プレイヤーからは
            // 「敵の位置が急に分からなくなる」= 音を立てた代償が絵として出る
            if (b.stateTicks >= b.alertTicks) {
                b.state = kAgentChase;
                if (a.ear != nullptr && a.ear->lastHeardTick != 0) {
                    b.target = a.ear->lastHeardPos;
                }
            }
            break;
        case kAgentChase:
            if (heardNow) {
                b.target = a.ear->lastHeardPos; // 追いながら更新
            } else if (lightVisible && !soundFresh) {
                b.target = a.eye->nearestPos;
            } else if (!soundFresh && !lightVisible) {
                b.state = kAgentSearch; // 手がかりが切れた
            }
            break;
        case kAgentSearch:
            if (heardNow) {
                b.state = kAgentChase;
                b.target = a.ear->lastHeardPos;
            } else if (b.stateTicks >= b.searchTicks) {
                b.state = kAgentReturn;
                b.target = b.home;
            } else if ((b.stateTicks % 45) == 0) {
                // ★**乱数を引くのはここだけ**。探索の不規則さが企画 §6-4 の
                //   「調べ終えた場所を覚えている」の裏返しで、まっすぐ往復すると
                //   プレイヤーから完全に読めてしまう。
                //   エージェントが居るときしか引かないので、既存シーンの乱数列は無風
                const float ang = rng.NextFloat01() * 6.2831853f;
                const float rad = 1.5f + rng.NextFloat01() * 3.0f;
                b.target = { b.target.x + std::cos(ang) * rad, b.target.y,
                             b.target.z + std::sin(ang) * rad };
            }
            break;
        default:
            b.state = kAgentPatrol;
            break;
        }
        if (b.state != prevState) {
            b.stateTicks = 0;
        } else {
            ++b.stateTicks;
        }

        // ---- 移動 ----
        float vx = 0.0f;
        float vz = 0.0f;
        if (b.state != kAgentAlert) {
            const int fi = nav_.BuildFlowField(b.target.x, b.target.y, b.target.z);
            float dx = 0.0f, dz = 0.0f;
            if (fi >= 0 && nav_.SampleDirection(fi, a.x, a.y, a.z, dx, dz)) {
                const float speed = (b.state == kAgentChase) ? b.runSpeed : b.walkSpeed;
                vx = dx * speed;
                vz = dz * speed;
            }
        }
        // ★moveInput は「水平 m/s・保持」なので毎 tick 上書きする。
        //   同じエンティティにスクリプトが付いていると AI が勝つ (フェーズ 3.4 は
        //   スクリプト層の後) — 仕様として AgentSystem.h に明記してある
        a.cc->moveInput = { vx, 0.0f, vz };

        // ---- 自分の音 ----
        // ★警戒中は出さない (企画 §6-3)。分周のカウンタは sim 状態なので、
        //   出さない tick も**進めない** (進めると警戒明けの位相が警戒時間に依存する)
        if (b.state != kAgentAlert && b.emitEveryTicks > 0 && b.emitLoudness > 0.0f) {
            ++b.emitPhase;
            if (b.emitPhase >= b.emitEveryTicks) {
                b.emitPhase = 0;
                // 到達距離は音量に比例させる (材質を持たないので 1 式で決める)。
                // tone 1 = 敵の音として固定 — 音色で「誰が鳴らしたか」が読める
                field.Emit(a.entity, a.x, a.y, a.z, b.emitLoudness, b.emitLoudness * 18.0f, 1u, 2u,
                           tick);
            }
        }
    }
}

} // namespace mye
