#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Audio/AcousticAudio.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/ModalAudio.h"

namespace mye {

class World;
class SoundLibrary;
// M68a: 場は読むだけ。**include の向きは Engine/Audio → Engine/Acoustic の一方向**で、
// このヘッダはポインタしか持たないので前方宣言で足りる (実体は AcousticAudio.h 経由)
class AcousticField;
struct SoundAsset;
struct AudioSourceComponent;
// M76f: モデルの実体は Engine/Modal 側。ここもポインタしか持たないので前方宣言で足りる
// (include の向き Engine/Audio → Engine/Modal は許容、ModalAudio.h 冒頭コメント参照)
class ModalSoundLibrary;

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

    // ---- 音響 × オーディオ (M68a) ----
    // 音響の場を注ぐ。**配線点は EngineLoop の 1 箇所だけ** (AssetPreviewCache が持つ
    // 別インスタンスは誰も埋めないので、サムネイル用の音が遮蔽されることがない)。
    // null なら整形も probe も一切走らない = 既存の挙動そのまま
    void SetAcousticField(const AcousticField* field) { acousticField_ = field; }
    // --acoustic-audio-log N: tick < N のあいだ整形の結果を 1 行ずつ標準出力へ。
    // **耳を使わずに配管を検査する唯一の口** (reviewer のレシピはこれを数える)
    void SetAcousticAudioLog(int ticks) { acousticLogTicks_ = ticks; }
    // ★型名 AcousticAudioStats と同名のメンバ関数にすると、クラス内でその型名が
    //   隠れる (以後 mye:: 修飾が必須になる) ので、アクセサ側の名前を短くしてある
    const AcousticAudioStats& AcousticStats() const { return acStats_; }

    // ---- 鳴る波 (M68b) ----
    // 1 フレームに溜めておける一発再生の上限。★超過は**捨てて数える** — 溜め続けると
    //   検証や一時停止が明けた瞬間に数百発が一斉に鳴る (voice 64 本を全部食う)
    static constexpr int kMaxPendingShots = 64;
    // 「この tick に生まれた波」を積む。**TickRunner の !resim ブロックからだけ**呼ぶ
    // (フレーム単位で AcousticField を舐めると、kMaxTicksPerFrame = 5 のフレームで
    //  4 tick しか生きない衝撃波を取りこぼす)
    void PushWaveShot(const PendingWaveShot& shot);
    // セルフテスト用。キューが Update / Reset で確実に空になることを外から見るため
    size_t PendingShotCount() const { return pendingShots_.size(); }

    // シーン遷移 / Play 停止で呼ぶ。鳴っている音を止めるのは呼び出し側 (AudioSystem::StopAll)。
    // ★M68b で AudioSystem を取るようになった: 残響の上書きは AudioSystem 側に載っているので、
    //   シーンを捨てるときに一緒に降ろさないと「前のシーンの部屋の響き」が残る
    void Reset(AudioSystem& audio);

    // ---- Deep-Modal 衝突音 (M76f) ----
    // 配線点は EngineLoop の 1 箇所だけ (SetAcousticField と同じ流儀)。null なら
    // CollectModalImpacts で積まれたキューはここで捨てられる (常に NoModel 扱い)
    void SetModalLibrary(ModalSoundLibrary* lib) { modalLibrary_ = lib; }
    // --modal-audio-log N: tick < N のあいだ 1 impact = 1 行を標準出力へ + 終了時に summary
    void SetModalAudioLog(int ticks) { modalLogTicks_ = ticks; }
    // --modal-sync-bake: Request() が Ready を返さないメッシュを BakeSync() で同期的に焼く
    // (--modal-demo の byte 一致検証用。既定は非同期ワーカー任せ)
    void SetModalSyncBake(bool sync) { modalSyncBake_ = sync; }
    const ModalAudioStats& ModalStats() const { return modalStats_; }
    // 1 フレームに溜めておける衝突インパクトの上限。超過は**捨てて数える**
    // (kMaxPendingShots と同じ理由 — 検証明けの一斉再生を防ぐ)
    static constexpr int kMaxPendingModalImpacts = 64;
    // TickRunner の !resim ブロックからだけ呼ぶ (PushWaveShot と同じ契約)
    void PushModalImpact(const PendingModalImpact& impact);
    size_t PendingModalImpactCount() const { return pendingModalImpacts_.size(); }

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
        // M68a: 遮蔽/回折の平滑化。**vel と全く同じ扱い** — 音源が使えなくなったら
        // 一緒に {} へ落とす (残すと鳴らし直しの 1 フレーム目が前の遮蔽値で始まる)
        AcousticShapeState shape;
    };

    // M76f: 発音元 1 体ぶんの「最後に鳴らした tick」(cooldown 判定用)。
    // ★EntityID 昇順の**sorted vector** (二分探索) — 音源の states_ と違い、
    //   接触音は同時に居る発音元の数が読めないため探索コストを抑えておく
    struct ModalEntityState {
        EntityID entity = kNullEntity;
        uint64_t lastShotTick = 0;
        bool everShot = false; // false の間は cooldown を判定しない (初回は必ず通す)
    };

    SourceState& StateFor(EntityID e);
    ModalEntityState& ModalStateFor(EntityID e);
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

    // ---- 音響 × オーディオ (M68a)。**全部 ECS の外**の側テーブル ----
    const AcousticField* acousticField_ = nullptr;
    AcousticProbe acProbe_;
    AcousticAudioStats acStats_;
    int acousticLogTicks_ = 0;
    // ---- 部屋の残響 / 鳴る波 (M68b) ----
    std::vector<PendingWaveShot> pendingShots_;
    float roomT_ = 0.0f;         // 平滑化後の補間パラメータ
    bool roomValid_ = false;     // false = 次の更新でスナップする
    float roomAppliedT_ = 0.0f;  // 最後に SetReverbOverride した t
    bool roomApplied_ = false;   // 一度も適用していないうちは |Δt| を見ずに撃つ
    // tone ごとの「鍵が引けない」警告の抑制 (毎歩ログを埋めない)
    bool unknownToneWarned_[4] = {};

    // ---- Deep-Modal 衝突音 (M76f)。**全部 ECS の外**の側テーブル (SourceState と同型) ----
    ModalSoundLibrary* modalLibrary_ = nullptr;
    int modalLogTicks_ = 0;
    bool modalSyncBake_ = false;
    ModalAudioStats modalStats_;
    std::vector<PendingModalImpact> pendingModalImpacts_;
    std::vector<ModalEntityState> modalStates_; // EntityID 昇順 (sorted vector)
    static constexpr int kMaxModalShotsPerTick = 4;
    static constexpr int kModalClipSlots = 32;
    // 回転プールの 1 スロット分。endTick は「自前で見積もった終了予定 tick」—
    // 実際に再生が終わったかは問わない (鳴っている音を切らないための保守的な予約)
    struct ModalClipSlotState {
        uint64_t endTick = 0;
    };
    ModalClipSlotState modalSlots_[kModalClipSlots];
    int32_t nextModalSlot_ = 0;
    bool modalNotReadyWarned_ = false; // 「--modal-bake を促す」WARN は 1 回だけ
};

} // namespace mye
