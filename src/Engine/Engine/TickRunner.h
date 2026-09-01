#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/EngineLoop.h" // EngineContext / EngineConfig / IEngineApp

namespace mye {

class Scene;
class InputActions;
class ScriptHost;
class ManagedHost;
class AnimationSystem;
class AnimationLibrary;
class AnimatorControllerSystem;
class ControllerLibrary;
class SkinningSystem;
class PartFollowSystem;
class EffectSystem;
class PhysicsSystem;
class XpbdBackend;
class AcousticField;
class AgentSystem;
class TransformSystem;
class CollisionSystem;
class ParticleSystem;
class VfxRenderer;
class AudioSystem;
class AudioSourceSystem;
class SoundLibrary;
class PrefabLibrary;
class ReplayRecorder;
class ReplayPlayer;
class Pcg32;
struct RenderResources;
struct PrevWorldStore;
struct SolidContact;
struct ScriptAudioEvent;
struct EffectSpawnRequest;
struct DebugLineCmd;

// 固定 tick 1 回分の実行に必要な参照束 (M52d、決定台帳 2)。
//
// 通常 tick / タイムトラベル再シム (M52e) / ロールバック再シム (M52i) が
// **同一の RunOneTick** を通るための入り口。経路ごとに tick を書き直すと
// 「再シムのときだけ挙動が違う」種類のバグが必ず入るので、ここは 1 本に保つこと。
// 抽出の合格条件は「3 ペアの replay_verify がビット一致すること」ただ 1 つ。
//
// null を許すもの = その経路では走らせないもの:
//   app               … エディタ更新 (再シムでは呼ばない。simulateScripts は呼び出し側が決める)
//   recorder / player … 記録・照合 (再シムでは両方 null = 記録も照合もしない)
//   prevWorld         … 描画補間用の採取 (再シムでは描かないので不要)
//   lastTickSimulated / exitCode … 呼び出し側の観測点
struct TickServices {
    EngineContext* ctx = nullptr;
    const EngineConfig* config = nullptr;
    Scene* scene = nullptr;
    IEngineApp* app = nullptr;

    // 入力 (M51d / M52g)
    InputActions* inputActions = nullptr;
    // **kMaxPlayers 本のレーン配列**の先頭 (1 本ではない)。消費するのは ctx->playerCount 本で、
    // 残りは Evaluate が毎 tick ゼロへ落とす
    InputSnapshot* prevTickInput = nullptr;

    // スクリプト層
    ScriptHost* scriptHost = nullptr;
    ManagedHost* managedHost = nullptr;

    // システム層
    AnimationSystem* animationSystem = nullptr;
    AnimationLibrary* animLibrary = nullptr;
    AnimatorControllerSystem* controllerSystem = nullptr;
    ControllerLibrary* controllerLibrary = nullptr;
    SkinningSystem* skinningSystem = nullptr;
    PartFollowSystem* partFollowSystem = nullptr;
    EffectSystem* effectSystem = nullptr;
    PhysicsSystem* physicsSystem = nullptr;
    XpbdBackend* xpbd = nullptr; // M60'b: 変形体の粒子池 (PhysicsSystem::Update へ渡す)
    // M65a: 音響の場 (ECS 外 sim 状態の 3 例目)。フェーズ 3.4 で Sync/Advance する。
    // null = 音響を回さない (World 単体の selftest 経路)
    AcousticField* acoustic = nullptr;
    // M65f: 敵の思考 (流れ場 + 共通 FSM)。フェーズ 3.4 の音響伝播の**直後**に回す —
    // 物理 (3.6) より前なので、書いた moveInput が同じ tick で効く。
    // null = AI を回さない (World 単体の selftest 経路)
    AgentSystem* agentSystem = nullptr;
    TransformSystem* transformSystem = nullptr;
    CollisionSystem* collisionSystem = nullptr;
    ParticleSystem* particleSystem = nullptr;
    VfxRenderer* vfxRenderer = nullptr;
    RenderResources* resources = nullptr;

    // tick 内で積んで tick 内で捌くバッファ
    std::vector<SolidContact>* solidContacts = nullptr;
    std::vector<EffectSpawnRequest>* effectQueue = nullptr;
    std::vector<DebugLineCmd>* debugLines = nullptr;
    std::vector<ScriptAudioEvent>* audioQueue = nullptr;

    // 出力レーン (ハッシュ後にしか触らない)
    AudioSystem* audioSystem = nullptr;
    AudioSourceSystem* audioSources = nullptr;
    SoundLibrary* soundLibrary = nullptr;
    Pcg32* audioScriptRng = nullptr;
    // 再生ハンドルの予約カウンタ (M45)。シーン遷移で 0 へ戻す = sim 側の採番列
    uint64_t* audioHandleSeq = nullptr;

    // 遅延要求 (tick 末のセーフポイントで消費)
    std::wstring* pendingScene = nullptr;
    int* pendingSaveSlot = nullptr;
    int* pendingLoadSlot = nullptr;

    // アセット / パス
    PrefabLibrary* prefabLibrary = nullptr;
    const std::wstring* assetsRoot = nullptr;
    const std::wstring* saveDir = nullptr;

    // リプレイ
    ReplayRecorder* recorder = nullptr;
    ReplayPlayer* player = nullptr;

    // ネットのロックステップ中 (M52h)。record/verify と**同じ決定論の境界**を要求する:
    // C# レーンは巻き戻せず 2 台で同じ列を回す保証も無いので停止し、LoadGame は禁止する
    // (LoadScene は許可 — record/verify と同じ扱い)。
    // ★オーディオは止めない。record/verify で止めているのは「1 フレームに 64 tick 回る」
    //   早送り実行だからで、決定論のためではない (音は tick 末のハッシュより後の出力レーン)。
    //   ネットは実時間で回るので、ここを止めると理由なく無音になる
    bool netLockstep = false;

    // 再シム中 (M52e タイムトラベルのシーク / M52i ロールバック) は true。
    // **過去に一度起きたことをもう一度なぞっている**状態なので、外へ出す側 (オーディオ /
    // セーブ書出) と巻き戻せない側 (C# レーン) を record/verify と同じ条件で抑止する。
    // ★入力側 (LoadGame / LoadScene) は抑止しない — 抑止すると元の tick 列と違う世界に
    //   なって必ずハッシュが割れる。「読むもの」と「書き出すもの」を混同しないこと
    bool resim = false;

    // 観測点 (呼び出し側のフレーム処理が読む)
    PrevWorldStore* prevWorld = nullptr;
    bool* lastTickSimulated = nullptr;
    int* exitCode = nullptr;
};

// tick 1 回 (フェーズ 3 → 3.5 → 3.6 → 4 → 5 → 7 → tick 末の出力レーン) を回し、
// 最後に ctx.tickIndex を 1 進める。
// ★ctx.inputs[0..playerCount) は**呼び出し側が確定させてから**渡すこと — ライブ入力 /
//   合成入力 / .rep の記録入力 / タイムトラベルのリング入力を RunOneTick 側で
//   区別しないための取り決め (決定台帳 5)。
void RunOneTick(TickServices& ts);

} // namespace mye
