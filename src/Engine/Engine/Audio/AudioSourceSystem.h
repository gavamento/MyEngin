#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Audio/AudioSystem.h"

namespace mye {

class World;
class SoundLibrary;
struct SoundAsset;
struct AudioSourceComponent;

// アセット既定 (.sound.json) に AudioSource コンポーネントの上書きを載せる **純関数**。
// XAudio2 にも ECS にも触れないので selftest がデバイス無しで規則を検証できる。
//
// 上書き規約 (Components.h の AudioSourceComponent と同じことをここで実装する。
// **規則は必ずこの 1 本だけ** — M45d でランタイムとアセット編集に同じ規則を二重実装して
// 「selftest が見ているコードと実際に鳴らしているコードが別物」になった前例がある):
//   - volume / pitch は乗算、mute で volume=0
//   - loop は -1 でアセット既定、priority は -1 でアセット既定
//   - bus は空文字列でアセット既定。**解決できない名前もアセット既定へ落とす**
//   - 3D 系は overrideAttenuation != 0 のときだけコンポーネント値を使う
//
// volJitter / pitchJitter は [-1,1] (再生開始時に 1 度だけ引いた値を使い回すこと。
// 毎フレーム引き直すと音が揺れ続ける)。バス名の解決だけは実行中のミキサーに問う
void MakeSourcePlay(const SoundAsset& asset, const AudioSourceComponent& src,
                    const AudioSystem& audio, int variationIndex, float volJitter,
                    float pitchJitter, PlayDesc& outDesc, AudioSpatial& outSpatial);

// AudioSource / AudioListener を毎フレーム 1 回処理して AudioSystem を駆動する。
//
// **ECS を読むのはここだけ** — AudioSystem 側は World を知らないままにしてある
// (ヘッドレス selftest がデバイスもワールドも無しに AudioSystem を構築できる性質を保つため)。
//
// **決定論レーンの外**: sim へは一切書き戻さない。乱数は専用の Pcg32 で、
// `world.Rng()` には絶対に触らない (RNG state はワールドハッシュ対象なので sim が壊れる)。
//
// 呼び出し位置は EngineLoop の「tick ループ後・フレーム末の transformSystem.Update() より前」で
// 固定 — そこで読める WorldMatrix が「直前 tick で確定した値」になっている必要がある
// (ドップラーの速度推定が tick 差分を前提にしているため)。
class AudioSourceSystem {
public:
    AudioSourceSystem() { rng_.Seed(0x4D796541754Full); } // "MyeAuO" — world.Rng() とは別系統

    void Update(World& world, AudioSystem& audio, const SoundLibrary& sounds, uint64_t tickIndex,
                float fixedDt, bool simulateScripts);

    // ---- スクリプト v8 (M45g) 由来の明示操作 ----
    // playOnAwake と**同じ状態キャッシュ / 同じ MakeSourcePlay** を通す (規則の二重実装を作らない)。
    // 呼び出し位置は EngineLoop のオーディオ drain (tick 末・ハッシュ後)。
    // PlayEntity は鳴っている音を鳴らし直す (Unity の AudioSource.Play と同じ)。
    // stream = true のアセットは BGM レーンへ回る (voice ハンドルは持たない)
    bool PlayEntity(World& world, AudioSystem& audio, const SoundLibrary& sounds, EntityID e);
    bool StopEntity(World& world, AudioSystem& audio, const SoundLibrary& sounds, EntityID e,
                    float fadeSeconds);
    // 3D リスナーを指定エンティティに固定する。kNullEntity = 自動
    // (AudioListener → primary カメラ)。指定が死んでいる/非アクティブなら自動へ落ちる
    void SetListenerOverride(EntityID e) { listenerOverride_ = e; }

    // シーン遷移 / Play 停止で呼ぶ。鳴っている音を止めるのは呼び出し側 (AudioSystem::StopAll)
    void Reset();

private:
    // 音源 1 つぶんの非決定論レーン状態。**コンポーネントには持たせない** —
    // シリアライズ対象になったり Undo/コピペで壊れたりするのを構造的に防ぐため
    struct SourceState {
        EntityID entity = kNullEntity;
        AudioHandle voice;
        VelocitySample vel;
        int variationIndex = 0;   // 再生開始時に抽選した結果 (毎フレーム引き直さない)
        float volJitter = 0.0f;   // 同上
        float pitchJitter = 0.0f; // 同上
        bool started = false;     // playOnAwake を撃ったか (非アクティブ化で戻る)
        bool seen = false;        // 今フレーム見かけたか (掃除用)
    };

    SourceState& StateFor(EntityID e);
    void Sweep(AudioSystem& audio);
    // 音源 1 つを実際に鳴らす。**playOnAwake とスクリプト PlayEntity が共有する唯一の経路**。
    // 戻り値 = voice が立ち上がったか (false かつ variationIndex >= 0 は「クリップ未ロード」
    // なので呼び出し側が次フレーム再挑戦してよい)
    bool StartSource(AudioSystem& audio, const SoundAsset& asset, const AudioSourceComponent& src,
                     SourceState& st, const AudioVec3& pos);

    std::vector<SourceState> states_;
    VelocitySample listenerVel_;
    EntityID listenerEntity_ = kNullEntity;
    EntityID listenerOverride_ = kNullEntity; // v8 SetListenerEntity (kNullEntity = 自動)
    uint64_t lastTick_ = 0;      // 0-tick フレームを丸ごと省くための直前 tick
    bool lastTickValid_ = false;
    Pcg32 rng_;
};

} // namespace mye
