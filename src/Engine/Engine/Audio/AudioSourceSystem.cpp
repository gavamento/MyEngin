#include "Engine/Engine/Audio/AudioSourceSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Audio/SynthCore.h" // WriteWavToFile (--modal-wav-dump、M76h の耳確認用)
#include "Engine/Engine/Modal/ModalSoundLibrary.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"

using namespace DirectX;

namespace mye {
namespace {

AudioVec3 PositionOf(const XMFLOAT4X4& m)
{
    return AudioVec3{ m._41, m._42, m._43 };
}

// 行列の基底から正規直交な (前, 上) を取り出す。非一様スケールや歪んだ行列でも
// X3DAudio の「正規化済み・直交」要求を満たせるようにグラム・シュミットで直す
void OrientationOf(const XMFLOAT4X4& m, AudioVec3& outForward, AudioVec3& outUp)
{
    XMVECTOR fwd = XMVectorSet(m._31, m._32, m._33, 0.0f); // +Z 行 = 前 (左手系)
    XMVECTOR up = XMVectorSet(m._21, m._22, m._23, 0.0f);  // +Y 行 = 上
    if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-12f) {
        fwd = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }
    fwd = XMVector3Normalize(fwd);
    // up から前方向成分を抜いて直交化する
    up = XMVectorSubtract(up, XMVectorScale(fwd, XMVectorGetX(XMVector3Dot(up, fwd))));
    if (XMVectorGetX(XMVector3LengthSq(up)) < 1e-12f) {
        // 前が真上/真下を向いていて up が潰れた場合の退避
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        up = XMVectorSubtract(up, XMVectorScale(fwd, XMVectorGetX(XMVector3Dot(up, fwd))));
        if (XMVectorGetX(XMVector3LengthSq(up)) < 1e-12f) {
            up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        }
    }
    up = XMVector3Normalize(up);
    XMFLOAT3 f;
    XMFLOAT3 u;
    XMStoreFloat3(&f, fwd);
    XMStoreFloat3(&u, up);
    outForward = { f.x, f.y, f.z };
    outUp = { u.x, u.y, u.z };
}

// 指定エンティティをそのままリスナーにする (v8 SetListenerEntity)。
// 死んでいる / 非アクティブ / WorldMatrix 無しなら false を返して自動探索へ落とす —
// **リスナーを失って全部無音になるのが一番困る**ため
bool ListenerFromEntity(World& world, EntityID e, AudioListenerState& out, EntityID& outEntity)
{
    if (e.IsNull() || !world.IsAlive(e) || !IsEntityActive(world, e)) {
        return false;
    }
    const auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    if (wm == nullptr) {
        return false;
    }
    out.position = PositionOf(wm->value);
    OrientationOf(wm->value, out.forward, out.up);
    out.velocity = {};
    outEntity = e;
    return true;
}

// リスナーを探す。AudioListenerComponent (enabled != 0 かつエンティティが active) のうち
// **entity.index が最小のもの** を使い、無ければ primary カメラへ落とす
// (SkyboxComponent / FogComponent の「最初の active な 1 個」と同じ規約。
//  ForEachArchetype の走査順はアーキタイプ生成順なので、index で明示的に選ぶ)。
bool FindListener(World& world, AudioListenerState& out, EntityID& outEntity)
{
    EntityID best = kNullEntity;
    XMFLOAT4X4 bestWorld = {};

    const ComponentTypeId req[] = { AudioListenerComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int li = arch.FindTypeIndex(AudioListenerComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* l = static_cast<const AudioListenerComponent*>(arch.GetPtr(li, row));
            if (!l->enabled) {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (!best.IsNull() && best.index <= e.index) {
                continue;
            }
            if (!IsEntityActive(world, e)) {
                continue;
            }
            best = e;
            bestWorld = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
        }
    });

    if (best.IsNull()) {
        // フォールバック: primary カメラ (無ければ最初に見つかったカメラ)。
        // RenderSystem のカメラ探索と同じ規則にして「見ている所で聞こえる」を保つ
        const ComponentTypeId camReq[] = { CameraComponent::sTypeId,
                                           WorldMatrixComponent::sTypeId };
        bool primaryFound = false;
        world.ForEachArchetype(camReq, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
            const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                if (primaryFound) {
                    return;
                }
                const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
                const EntityID e = arch.EntityAt(row);
                if (!best.IsNull() && !c->isPrimary) {
                    continue;
                }
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                best = e;
                bestWorld = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                primaryFound = c->isPrimary;
            }
        });
    }

    outEntity = best;
    if (best.IsNull()) {
        out = {}; // リスナーが 1 つも無い = 原点で前を向いているものとして扱う
        return false;
    }
    out.position = PositionOf(bestWorld);
    OrientationOf(bestWorld, out.forward, out.up);
    out.velocity = {};
    return true;
}

// M68a: 音響の調整卓を 1 個だけ選ぶ。**entity.index 最小の active かつ enabled** —
// AcousticVolume / Skybox / Fog と同じ規約 (ForEachArchetype の走査順はアーキタイプの
// 生成順なので、index で明示的に選ばないと「シーンの組み方で音が変わる」)
const AcousticAudioComponent* FindAcousticAudio(World& world)
{
    const AcousticAudioComponent* best = nullptr;
    EntityID bestEntity = kNullEntity;
    const ComponentTypeId req[] = { AcousticAudioComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(AcousticAudioComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* c = static_cast<const AcousticAudioComponent*>(arch.GetPtr(ci, row));
            if (!c->enabled) {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (!bestEntity.IsNull() && bestEntity.index <= e.index) {
                continue;
            }
            if (!IsEntityActive(world, e)) {
                continue;
            }
            bestEntity = e;
            best = c;
        }
    });
    return best;
}

// log 用のエンティティ名 (無ければ空文字)。**tick 内では安定**なので借用でよい
const char* EntityNameOf(World& world, EntityID e)
{
    const auto* n = world.GetComponent<NameComponent>(e);
    return (n != nullptr) ? n->value : "";
}

} // namespace

// ---------------------------------------------------------------------------
// 純関数: アセット既定 + コンポーネント上書き
// ---------------------------------------------------------------------------

void MakeSourcePlay(const SoundAsset& asset, const AudioSourceComponent& src,
                    const AudioSystem& audio, int variationIndex, float volJitter,
                    float pitchJitter, PlayDesc& outDesc, AudioSpatial& outSpatial)
{
    // 2D 部分 (クリップ / バス / 揺らぎ / ループ / 優先度) は M45c の 1 本を通す
    outDesc = MakePlayDesc(asset, variationIndex, volJitter, pitchJitter, audio);

    constexpr float kMinPitch = 1.0f / AudioSystem::kMaxFreqRatio;
    const float volMul = src.volume > 0.0f ? src.volume : 0.0f; // NaN もここで 0 に落ちる
    const float pitchMul = src.pitch > 0.0f ? src.pitch : 1.0f;
    outDesc.volume = std::clamp(outDesc.volume * volMul, 0.0f, 1.0f);
    if (src.mute != 0) {
        outDesc.volume = 0.0f;
    }
    outDesc.pitch = std::clamp(outDesc.pitch * pitchMul, kMinPitch, AudioSystem::kMaxFreqRatio);
    if (src.loop >= 0) {
        outDesc.loop = src.loop != 0;
    }
    if (src.priority >= 0) {
        outDesc.priority = src.priority;
    }
    if (src.bus[0] != '\0') {
        const int b = audio.FindBus(src.bus);
        if (b >= 0) {
            outDesc.bus = b; // 解決できない名前はアセット既定のまま (黙って無音にしない)
        }
    }

    outSpatial = {};
    if (src.overrideAttenuation != 0) {
        outSpatial.spatialBlend = src.spatialBlend;
        outSpatial.minDistance = src.minDistance;
        outSpatial.maxDistance = src.maxDistance;
        outSpatial.rolloff = src.rolloff;
        outSpatial.dopplerScale = src.dopplerScale;
        outSpatial.reverbSend = src.reverbSend;
    } else {
        outSpatial.spatialBlend = asset.spatialBlend;
        outSpatial.minDistance = asset.minDistance;
        outSpatial.maxDistance = asset.maxDistance;
        outSpatial.rolloff = static_cast<int>(asset.rolloff);
        outSpatial.dopplerScale = asset.dopplerScale;
        outSpatial.reverbSend = asset.reverbSend;
    }
    outSpatial.spatialBlend = std::clamp(outSpatial.spatialBlend, 0.0f, 1.0f);
    outSpatial.pitch = outDesc.pitch; // ドップラーはこの比に乗る
}

// ---------------------------------------------------------------------------
// システム
// ---------------------------------------------------------------------------

void AudioSourceSystem::Reset(AudioSystem& audio)
{
    states_.clear();
    listenerVel_ = {};
    listenerEntity_ = kNullEntity;
    // シーンが替われば EntityID の意味も替わる。指定を残すと別のオブジェクトが
    // リスナーになりかねないので自動へ戻す
    listenerOverride_ = kNullEntity;
    lastTickValid_ = false;
    // M68a: シーンが替われば占有もリスナーも別物。**配列は捨てず valid だけ落とす**
    // (次の Update が焼き直す)。統計は run 全体の診断値なので残す
    acProbe_.valid = false;
    // M68b: 積んだままの一発再生は**捨てる** (次のシーンで前のシーンの足音が鳴る)。
    // 残響の上書きも一緒に降ろす — 新しいシーンの開放度が決まるまでは資産のプリセットが正
    pendingShots_.clear();
    roomValid_ = false;
    roomApplied_ = false;
    for (bool& w : unknownToneWarned_) {
        w = false; // 新しいシーンでは設定ミスをもう一度知らせる (PartFollowSystem と同じ流儀)
    }
    // M76f: 積んだままの衝突インパクトと cooldown 側テーブルも捨てる (前のシーンの
    // 発音元 EntityID はこのシーンでは意味が変わる)。クリップ池 (modalSlots_) は
    // 「鳴っている音を切らない」ための予約に過ぎないので触らない (自然に期限切れになる)
    pendingModalImpacts_.clear();
    modalStates_.clear();
    audio.ClearReverbOverride();
}

void AudioSourceSystem::PushWaveShot(const PendingWaveShot& shot)
{
    if (static_cast<int>(pendingShots_.size()) >= kMaxPendingShots) {
        ++acStats_.shotsDropped; // 溢れは黙って捨てる。数だけ summary に出す
        return;
    }
    pendingShots_.push_back(shot);
}

void AudioSourceSystem::PushModalImpact(const PendingModalImpact& impact)
{
    if (static_cast<int>(pendingModalImpacts_.size()) >= kMaxPendingModalImpacts) {
        ++modalStats_.dropped;
        return;
    }
    pendingModalImpacts_.push_back(impact);
}

AudioSourceSystem::SourceState& AudioSourceSystem::StateFor(EntityID e)
{
    // 音源はせいぜい数十個 (voice 上限が 64) なので線形探索で足りる。
    // unordered_map の反復を持ち込まない = spec 11.2 規則 7 の警告も出ない
    for (SourceState& s : states_) {
        if (s.entity == e) {
            return s;
        }
    }
    states_.push_back(SourceState{});
    states_.back().entity = e;
    return states_.back();
}

AudioSourceSystem::ModalEntityState& AudioSourceSystem::ModalStateFor(EntityID e)
{
    // EntityID 昇順の sorted vector + 二分探索 (index を優先、同値なら generation)。
    // index だけで比較すると、破棄されたエンティティのスロットを別世代が再利用したとき
    // 別物の cooldown を引き継いでしまう
    const auto less = [](const ModalEntityState& a, EntityID b) {
        if (a.entity.index != b.index) {
            return a.entity.index < b.index;
        }
        return a.entity.generation < b.generation;
    };
    auto it = std::lower_bound(modalStates_.begin(), modalStates_.end(), e, less);
    if (it != modalStates_.end() && it->entity == e) {
        return *it;
    }
    ModalEntityState st;
    st.entity = e;
    it = modalStates_.insert(it, st);
    return *it;
}

void AudioSourceSystem::Sweep(AudioSystem& audio)
{
    size_t write = 0;
    for (size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].seen) {
            if (write != i) {
                states_[write] = states_[i];
            }
            ++write;
        } else if (states_[i].voice.Valid()) {
            audio.Stop(states_[i].voice); // 消えた音源 (破棄/コンポーネント削除) の音を残さない
        }
    }
    states_.resize(write);
}

bool AudioSourceSystem::StartSource(AudioSystem& audio, const SoundAsset& asset,
                                    const AudioSourceComponent& src, SourceState& st,
                                    const AudioVec3& pos)
{
    st.variationIndex = PickVariationIndex(asset, rng_.NextU32());
    if (st.variationIndex < 0) {
        return false; // 鳴らせるバリエーションが 1 つも無いアセット
    }
    st.volJitter = rng_.Range(-1.0f, 1.0f);
    st.pitchJitter = rng_.Range(-1.0f, 1.0f);
    PlayDesc desc;
    AudioSpatial spatial;
    MakeSourcePlay(asset, src, audio, st.variationIndex, st.volJitter, st.pitchJitter, desc,
                   spatial);
    // 初回の定位も込みで渡す (Start() 前に適用されるので出だしが無定位にならない)
    spatial.position = pos;
    spatial.velocity = {}; // 生成直後は静止扱い (初速をでっち上げない)
    desc.spatial = spatial.spatialBlend > 0.0f ? &spatial : nullptr;
    st.voice = audio.Play(desc);
    st.vel = {};
    st.shape = {}; // M68a: 鳴らし直しは遮蔽の平滑化も仕切り直す (次の整形でスナップ)
    return st.voice.Valid();
}

bool AudioSourceSystem::PlayEntity(World& world, AudioSystem& audio, const SoundLibrary& sounds,
                                   EntityID e)
{
    const auto* src = world.GetComponent<AudioSourceComponent>(e);
    if (src == nullptr) {
        return false;
    }
    const SoundAsset* asset = sounds.Get(src->sound.value);
    if (asset == nullptr) {
        return false; // サウンド未割当 (Inspector で空のまま)
    }
    SourceState& st = StateFor(e);
    // このフレームの Update より前に状態を作ることがあるので、掃除で消されないようにする
    st.seen = true;
    // 明示的に鳴らした以上、playOnAwake の再発火は不要
    st.started = true;

    if (asset->stream) {
        SoundAsset a = *asset; // コンポーネント側の音量上書きだけ載せる (Update と同じ規則)
        const float vol = src->mute ? 0.0f : std::clamp(src->volume, 0.0f, 1.0f);
        a.volume = std::clamp(a.volume * vol, 0.0f, 1.0f);
        return PlayMusicSound(audio, a, kMusicDefaultFadeSeconds);
    }

    if (st.voice.Valid()) {
        audio.Stop(st.voice); // 鳴らし直し (Unity の AudioSource.Play と同じ挙動)
        st.voice = {};
    }
    AudioVec3 pos{};
    if (const auto* wm = world.GetComponent<WorldMatrixComponent>(e)) {
        pos = PositionOf(wm->value);
    }
    return StartSource(audio, *asset, *src, st, pos);
}

bool AudioSourceSystem::StopEntity(World& world, AudioSystem& audio, const SoundLibrary& sounds,
                                   EntityID e, float fadeSeconds)
{
    const auto* src = world.GetComponent<AudioSourceComponent>(e);
    if (src == nullptr) {
        return false;
    }
    SourceState& st = StateFor(e);
    st.seen = true;
    st.started = true; // 止めた音を playOnAwake が鳴らし直さないように

    const SoundAsset* asset = sounds.Get(src->sound.value);
    if (asset != nullptr && asset->stream) {
        // BGM レーンは 1 本しかないので「この音源の曲だけ止める」は原理的にできない。
        // 0 秒指定でもプツッと切らない (M45f の kMusicStopFadeSeconds と同じ規約)
        audio.StopMusic(fadeSeconds > 0.0f ? fadeSeconds : kMusicStopFadeSeconds);
        return true;
    }
    if (!st.voice.Valid()) {
        return false;
    }
    // フェード中の voice の面倒は AudioSystem::Update が見る。こちら側は手を離す
    audio.Stop(st.voice, fadeSeconds);
    st.voice = {};
    return true;
}

void AudioSourceSystem::Update(World& world, AudioSystem& audio, const SoundLibrary& sounds,
                               uint64_t tickIndex, float fixedDt, bool simulateScripts)
{
    // ★M68b: 一発再生のキューは**どの early return よりも先に**取り出して空にする。
    //   ここを return の後ろに置くと、記録/検証中に積まれた波が溜まり続けて、
    //   サスペンドが明けた瞬間に数十発が一斉に鳴る (A17)。捨てるのが正しい —
    //   「鳴らなかった音」は決定論レーンに何の影響も無い
    std::vector<PendingWaveShot> shots;
    shots.swap(pendingShots_);
    // M76f: モーダル衝突音のキューも同じ理由で同じ場所で空にする (受け入れ条件 8:
    // suspend 中の Update でキューが空になること)
    std::vector<PendingModalImpact> modalImpacts;
    modalImpacts.swap(pendingModalImpacts_);

    // ★決定論契約 2: 記録/検証中は 3D 計算も playOnAwake も一切走らせない。
    //   検証中は 1 フレームで最大 64 tick 回るので、ここを開けると計算量も発音も暴れる。
    //   状態はそのまま残す (サスペンドが明けたら続きから鳴らせるように)
    if (!audio.IsReady() || audio.IsSuspended()) {
        return;
    }
    // ★0-tick フレームは丸ごと省く。WorldMatrix は tick 内でしか更新されないので入力が
    //   前フレームと 1 ビットも変わらず、計算しても同じ結果にしかならない。Runtime は
    //   vsync 無効で数千 fps 回るため、ここを開けると X3DAudioCalculate が
    //   「音源数 × 数千回/秒」走って CPU を無駄に食う (sim は 60Hz でしか動かない)
    if (lastTickValid_ && tickIndex == lastTick_) {
        return;
    }
    // ★M68a: 平滑化の歩幅は lastTick_ を**上書きする前**に取る (最小 1)。
    //   追いつきフレームでは 1 フレームで複数 tick 進むので、ここを 1 固定にすると
    //   実時間の遮蔽の追従がフレームレートに依存してしまう
    const float acDTicks = (lastTickValid_ && tickIndex > lastTick_)
        ? static_cast<float>(tickIndex - lastTick_)
        : 1.0f;
    lastTick_ = tickIndex;
    lastTickValid_ = true;

    for (SourceState& s : states_) {
        s.seen = false;
    }

    // ---- リスナー ----
    AudioListenerState listener;
    EntityID listenerEntity = kNullEntity;
    // v8 SetListenerEntity の指定が生きていればそれを使い、駄目なら通常の探索へ落とす
    bool haveListener = ListenerFromEntity(world, listenerOverride_, listener, listenerEntity);
    if (!haveListener) {
        haveListener = FindListener(world, listener, listenerEntity);
    }
    if (!(listenerEntity == listenerEntity_)) {
        listenerVel_ = {}; // リスナーが替わったら速度推定を仕切り直す (瞬間移動扱いにしない)
        listenerEntity_ = listenerEntity;
    }
    if (haveListener) {
        UpdateVelocitySample(listenerVel_, listener.position, tickIndex, fixedDt);
        listener.velocity = listenerVel_.velocity;
    }
    audio.SetListener(listener);

    // ---- 音響 × オーディオ (M68a): リスナー場 ----
    // ★リスナーが確定した**後**でなければ焼けない (原点が耳のセルそのもの)。
    //   逆に音源ループより**前**に済ませておかないと、同じフレームの voice が
    //   1 本目と 2 本目で違う場を見ることになる
    const AcousticAudioComponent* acComp = FindAcousticAudio(world);
    const bool acOn = acComp != nullptr && acousticField_ != nullptr;
    if (acOn) {
        if (haveListener) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool rebuilt =
                UpdateAcousticProbe(*acousticField_, *acComp, listener.position, acProbe_);
            if (rebuilt) {
                const float ms = static_cast<float>(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                              - t0)
                        .count());
                ++acStats_.rebuilds;
                acStats_.probeMsLast = ms;
                acStats_.probeMsTotal += ms;
            }
        } else {
            acProbe_.valid = false; // 耳が 1 つも無いシーンは整形しない
        }
        ++acStats_.ticks;
        acStats_.boxCells = static_cast<int32_t>(acProbe_.valid ? acProbe_.BoxCells() : 0);
        acStats_.openness = acProbe_.valid ? acProbe_.openness : 0.0f;
    } else {
        acProbe_.valid = false; // 調整卓が消えた / 場が繋がっていない = 何も主張しない
    }
    // ★有効な AcousticAudio が無い / 場が繋がっていない tick は **Bypass 相当** =
    //   spatial に 1 バイトも触らない (= AcousticAudio の無いシーンと 1 ビットも変わらない)
    acStats_.active = acOn && acProbe_.valid;
    const bool acLog = acOn && acousticLogTicks_ > 0
        && tickIndex < static_cast<uint64_t>(acousticLogTicks_);

    // ---- 部屋の残響 (M68b): 開放度 → 2 プリセット間の連続補間 ----
    // ★**段階切替にしない**のがユーザー決定 U2。廊下と部屋で響きが切り替わると
    //   境目で「カチッ」と鳴って世界が嘘になるので、開放度から t を作って
    //   I3DL2 の 13 パラメータを丸ごと混ぜる。
    // ★平滑化は gain/lpf とは**別の半減期** (roomSmoothTicks、既定 18 = 300ms)。
    //   reverb APO のパラメータ更新は重いので |Δt| > 0.01 でしか撃たない (spec S12)
    if (acStats_.active) {
        const float target = RoomBlend(acProbe_.openness, acComp->openSmall, acComp->openLarge);
        const float half = static_cast<float>(acComp->roomSmoothTicks);
        if (!roomValid_ || !(half > 0.0f)) {
            roomT_ = target; // 初回 / スナップ指定は即座に合わせる
            roomValid_ = true;
        } else {
            const float alpha = 1.0f - std::pow(0.5f, acDTicks / half);
            roomT_ += (target - roomT_) * alpha;
        }
        if (!roomApplied_ || std::fabs(roomT_ - roomAppliedT_) > 0.01f) {
            audio.SetReverbOverride(
                LerpReverbParams(AudioSystem::PresetReverbParams(acComp->reverbSmall),
                                 AudioSystem::PresetReverbParams(acComp->reverbLarge), roomT_));
            roomAppliedT_ = roomT_;
            roomApplied_ = true;
        }
    } else {
        // 調整卓が無い / 耳がグリッド外 = **資産のプリセットへ戻す**。
        // 上書きを掛けっぱなしにすると、音響ボリュームを出た瞬間の響きが固まる
        roomValid_ = false;
        roomApplied_ = false;
        roomT_ = 0.0f;
        audio.ClearReverbOverride();
    }
    acStats_.roomT = roomT_;

    // ---- 音源 ----
    const ComponentTypeId req[] = { AudioSourceComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(AudioSourceComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* src = static_cast<const AudioSourceComponent*>(arch.GetPtr(si, row));
            const XMFLOAT4X4& wm =
                static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            const EntityID e = arch.EntityAt(row);

            SourceState& st = StateFor(e);
            st.seen = true;

            const SoundAsset* asset = sounds.Get(src->sound.value);
            const bool usable = asset != nullptr && IsEntityActive(world, e);
            if (!usable) {
                // 未割当 / 非アクティブ: 鳴っていれば止め、**started を戻す** —
                // 再アクティブ化で playOnAwake がもう一度効く (Unity の OnEnable 相当)
                if (st.voice.Valid()) {
                    audio.Stop(st.voice);
                    st.voice = {};
                }
                st.started = false;
                st.vel = {};
                st.shape = {};
                continue;
            }

            const AudioVec3 pos = PositionOf(wm);

            // ---- BGM (stream) は voice プールではなくストリーミングレーンで鳴らす (M45f) ----
            // 定位も減衰もドップラーも掛からない (BGM は 2D)。**レーンは 1 本**なので、
            // stream の AudioSource を 2 つ置くと後勝ちでクロスフェードする
            if (asset->stream) {
                if (simulateScripts && src->playOnAwake && !st.started) {
                    const float vol = src->mute ? 0.0f : std::clamp(src->volume, 0.0f, 1.0f);
                    SoundAsset a = *asset; // コンポーネント側の音量上書きだけ載せる
                    a.volume = std::clamp(a.volume * vol, 0.0f, 1.0f);
                    // 失敗 (ファイル未解決) なら started を立てずに次フレーム再挑戦する
                    st.started = PlayMusicSound(audio, a, kMusicDefaultFadeSeconds);
                }
                st.vel = {};
                st.shape = {}; // BGM は 2D レーン = 遮蔽の対象外
                continue;
            }

            // ---- 再生開始 (playOnAwake)。経路は PlayEntity と共有の StartSource 1 本 ----
            if (simulateScripts && src->playOnAwake && !st.started && !st.voice.Valid()) {
                // クリップが未ロードの間は失敗する → started を立てずに次フレーム再挑戦する。
                // ただしバリエーションが 1 つも無いアセットは打ち切る (毎フレーム試さない)
                st.started = StartSource(audio, *asset, *src, st, pos) || st.variationIndex < 0;
            }

            if (!st.voice.Valid()) {
                continue;
            }

            // ---- 実効パラメータを毎フレーム引き直す (Inspector の編集を即反映する) ----
            PlayDesc desc;
            AudioSpatial spatial;
            MakeSourcePlay(*asset, *src, audio, st.variationIndex, st.volJitter, st.pitchJitter,
                           desc, spatial);

            // ---- 速度推定 (ドップラー用) ----
            if (spatial.dopplerScale > 0.0f) {
                UpdateVelocitySample(st.vel, pos, tickIndex, fixedDt);
                spatial.velocity = st.vel.velocity;
            } else {
                // ドップラー無効: 推定ごとスキップする (位置だけ追随させて、
                // 後で有効化されたときに巨大な差分を食わないようにする)
                st.vel.position = pos;
                st.vel.tick = tickIndex;
                st.vel.velocity = {};
                st.vel.valid = true;
            }
            spatial.position = pos;

            // ---- 遮蔽・回折の整形 (M68a)。**規則はこの 1 本だけ** ----
            // 一発再生 (M68b の波) も同じ関数を通る = 「テストが見ている規則」と
            // 「実際に鳴らしている規則」が構造的に一致する
            if (acOn) {
                float acGain = 1.0f;
                AcousticShapeInfo info;
                ShapeAcousticSpatial(*acousticField_, acProbe_, *acComp, listener.position, pos,
                                     spatial, acGain, &st.shape, acDTicks, &info);
                desc.volume = std::clamp(desc.volume * acGain, 0.0f, 1.0f);
                ++acStats_.shaped;
                ++acStats_.classCount[static_cast<int>(info.cls)];
                if (acLog) {
                    MYE_LOG_INFO("[acaudio] t=%llu kind=voice src=%u name=%s class=%s "
                                 "dPath=%.2f dLine=%.2f dReal=%.2f lpf=%.3f gain=%.3f open=%.2f "
                                 "room=%.2f",
                                 static_cast<unsigned long long>(tickIndex), e.index,
                                 EntityNameOf(world, e), AcousticPathClassName(info.cls),
                                 static_cast<double>(info.dPath), static_cast<double>(info.dLine),
                                 static_cast<double>(info.dReal), static_cast<double>(info.lpf),
                                 static_cast<double>(info.gain),
                                 static_cast<double>(acProbe_.valid ? acProbe_.openness : 0.0f),
                                 static_cast<double>(roomT_));
                }
            }

            audio.SetVoiceVolume(st.voice, desc.volume);
            if (spatial.spatialBlend > 0.0f) {
                if (!audio.ApplyVoiceSpatial(st.voice, spatial)) {
                    st.voice = {}; // 鳴り終わった / スティールされた
                }
            } else {
                audio.SetVoicePitch(st.voice, desc.pitch);
            }
        }
    });

    // ---- 鳴る波 (M68b): この tick に生まれた波を一発再生する ----
    // ★probe を焼き直した**後**に流すので、同じフレームに生まれた波でも
    //   「今の耳から見た経路」で整形される (これが tick 側 push / フレーム側 drain の理由)。
    // ★調整卓が無い / 場が繋がっていない run では**捨てる**。上の swap で既に
    //   キューは空なので、ここを素通りするだけで溜まらない
    if (acOn) {
        for (const PendingWaveShot& shot : shots) {
            PlayDesc desc;
            AudioSpatial spatial;
            AcousticShapeInfo info;
            const WaveShotResult r =
                MakeWaveShotPlay(*acousticField_, acProbe_, *acComp, shot, listener.position,
                                 audio, sounds, rng_, desc, spatial, &info);
            if (r != WaveShotResult::Played) {
                if (r == WaveShotResult::UnknownKey) {
                    ++acStats_.shotsUnknownKey;
                    // ★tone ごとに 1 回だけ警告する。毎歩出すと足音のたびにログが埋まる
                    if (shot.tone < 4u && !unknownToneWarned_[shot.tone]) {
                        unknownToneWarned_[shot.tone] = true;
                        MYE_LOG_WARN("[audio] unknown sound key for acoustic tone %u "
                                     "(AcousticAudio.toneSound%u)",
                                     shot.tone, shot.tone);
                    }
                } else {
                    ++acStats_.shotsSkipped;
                }
                continue;
            }
            ++acStats_.shots;
            ++acStats_.shaped;
            ++acStats_.classCount[static_cast<int>(info.cls)];
            if (acLog) {
                // ★src は**発音元エンティティ** (無ければ -1)。名前は引かない — drain は
                //   tick の後なので、鳴らした主体が既に破棄されていることがある
                //   (借用するなら IsAlive 検査が要る)。tone は音色の識別に足りる
                const int src =
                    shot.source.IsNull() ? -1 : static_cast<int>(shot.source.index);
                // key = 積む側が決めた名前キーのハッシュ (0 = tone マップ)。名前は引かない
                MYE_LOG_INFO("[acaudio] t=%llu kind=shot src=%d name=tone%u key=%016llx class=%s "
                             "dPath=%.2f dLine=%.2f dReal=%.2f lpf=%.3f gain=%.3f open=%.2f "
                             "room=%.2f",
                             static_cast<unsigned long long>(tickIndex), src, shot.tone,
                             static_cast<unsigned long long>(shot.soundKey),
                             AcousticPathClassName(info.cls), static_cast<double>(info.dPath),
                             static_cast<double>(info.dLine), static_cast<double>(info.dReal),
                             static_cast<double>(info.lpf), static_cast<double>(info.gain),
                             static_cast<double>(acProbe_.valid ? acProbe_.openness : 0.0f),
                             static_cast<double>(roomT_));
            }
            // ★戻り値を捨てない。無効ハンドル = voice が立たなかった (クリップ未ロード /
            //   suspend / 枯渇) ので数える。ここを見ていないと「ログは 51 行出ているのに
            //   1 音も鳴っていない」が緑のまま通る
            if (!audio.Play(desc).Valid()) {
                ++acStats_.shotsPlayFailed;
            }
        }
    }

    // ---- Deep-Modal 衝突音 (M76f) ----
    // ★wave shot と違い **acOn を要求しない** — ModalSound は AcousticAudio の有無と
    //   無関係に鳴る (置いてあるのが「衝突音」であって「聴覚シム」ではないため)。
    //   acOn のときだけ ShapeAcousticSpatial で遮蔽・回折を足す
    {
        const bool modalLog =
            modalLogTicks_ > 0 && tickIndex < static_cast<uint64_t>(modalLogTicks_);
        int32_t shotsThisTick = 0;
        for (const PendingModalImpact& impact : modalImpacts) {
            if (shotsThisTick >= kMaxModalShotsPerTick) {
                break; // 残りは次フレームへ持ち越さず捨てる (AcousticField::DrainImpacts と同じ規律)
            }
            ++modalStats_.impacts;

            ModalShotResult result = ModalShotResult::NotReady; // 全分岐で書き換わる (規則 3: 宣言時初期化)
            ModalShotInfo info{};
            AcousticShapeInfo shapeInfo{}; // 既定 = Bypass/gain=1 (acOn=false ならそのままログに出る)
            int32_t slot = -1;

            const ModalSoundComponent* comp =
                world.IsAlive(impact.source) ? world.GetComponent<ModalSoundComponent>(impact.source)
                                             : nullptr;
            if (comp == nullptr) {
                // 発音元 / コンポーネントが push の後に消えた (drain は tick の後なので稀に起きる)。
                // 専用の結果値は無いので NotReady 側へ畳む (notReady バケットと同じ意味 —
                // 「今回は鳴らせなかった」)
                result = ModalShotResult::NotReady;
                ++modalStats_.notReady;
            } else {
                ModalEntityState& est = ModalStateFor(impact.source);
                if (ModalOnCooldown(est.everShot, est.lastShotTick, comp->cooldownTicks, impact.tick)) {
                    result = ModalShotResult::Cooldown;
                    ++modalStats_.cooldown;
                } else if (modalLibrary_ == nullptr) {
                    result = ModalShotResult::NoModel;
                    ++modalStats_.notReady;
                } else {
                    // M76j: 合成メッシュはパーツ列が要るので発音元から引き直す (impact は POD のまま)。
                    // push から drain までに構成が変わって ID が食い違ったら、今回は鳴らさない
                    const ModalMeshRef ref = ResolveModalMesh(world, impact.source, *comp);
                    ModalState state = (ref.id == impact.mesh) ? modalLibrary_->Request(ref)
                                                               : ModalState::Missing;
                    // NoModel (.dmnet 未ロード) は BakeSync を呼んでも同じ理由で失敗するだけなので
                    // 焼き直さない。呼ぶと state が Failed に上書きされ、r= のログが
                    // 「NotReady」と「NoModel」を取り違える (集計バケットは同じでも診断行の意味が変わる)
                    if (state != ModalState::Ready && state != ModalState::NoModel && modalSyncBake_
                        && ref.id == impact.mesh) {
                        const auto t0 = std::chrono::steady_clock::now();
                        const bool ok = modalLibrary_->BakeSync(ref);
                        const float ms = static_cast<float>(
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
                        ++modalStats_.bakes;
                        modalStats_.bakeMsTotal += ms;
                        state = ok ? ModalState::Ready : ModalState::Failed;
                    }
                    if (state != ModalState::Ready) {
                        result = (state == ModalState::NoModel) ? ModalShotResult::NoModel
                                                                : ModalShotResult::NotReady;
                        ++modalStats_.notReady; // NoModel も同じバケットへ畳む (summary に専用欄が無い)
                        if (!modalNotReadyWarned_) {
                            modalNotReadyWarned_ = true;
                            MYE_LOG_WARN(
                                "[modal] mesh 0x%016llx is not baked yet (the wave shot plays "
                                "instead) -- run --modal-bake once (composite meshes of model "
                                "hierarchies are baked at runtime only; --modal-sync-bake makes it "
                                "synchronous), or wait for the async worker",
                                static_cast<unsigned long long>(impact.mesh.value));
                        }
                    } else {
                        const ModalFeatureMap* fm = modalLibrary_->Get(impact.mesh);
                        const DmNetHeader* hdr = modalLibrary_->Header();
                        if (fm == nullptr || hdr == nullptr) {
                            result = ModalShotResult::NoModel;
                            ++modalStats_.notReady;
                        } else {
                            const auto* col = world.GetComponent<ColliderComponent>(impact.source);
                            const PhysMat* mat =
                                col != nullptr ? physmat::Resolve(col->physMaterial) : nullptr;
                            float scale = 1.0f;
                            if (const auto* wm =
                                    world.GetComponent<WorldMatrixComponent>(impact.source)) {
                                scale = ModalWorldScaleOfLongestAxis(fm->frame, wm->value);
                            }
                            AudioClip clip;
                            AudioSpatial spatial;
                            result = MakeModalShotPlay(*fm, *hdr, impact, *comp, mat, scale, clip,
                                                       spatial, &info);
                            if (result == ModalShotResult::BelowMin) {
                                ++modalStats_.belowMin;
                            } else {
                                // --modal-wav-dump (M76h の耳確認用調査ツール): クリップ池 /
                                // Play() の成否に関わらず、実際に合成できた PCM をそのまま書く
                                // (プール満杯や再生失敗も「音は作れていた」ことの証拠として残す)。
                                // 後段の std::move(clip) より前で書くこと — 移動後は空になる
                                if (!modalWavDumpDir_.empty()) {
                                    wchar_t wavPath[512];
                                    std::swprintf(wavPath, 512, L"%ls\\shot_%04d_%llu_%u.wav",
                                                  modalWavDumpDir_.c_str(), modalWavDumpCounter_++,
                                                  static_cast<unsigned long long>(impact.tick),
                                                  impact.source.index);
                                    if (!WriteWavToFile(clip, wavPath)) {
                                        MYE_LOG_WARN("[modal] --modal-wav-dump: failed to write %ls",
                                                     wavPath);
                                    }
                                }
                                // --modal-face-probe (M76h の耳確認用調査ツール): 実衝突は常に
                                // 重力方向 (同じ面) にしか当たらないため、「面で音が変わる」を
                                // 実測するにはこの合成し直しが要る。Inspector の
                                // FireModalPreviewFace (InspectorWindow.cpp) と同じ式 (ローカル
                                // AABB 面中心 + 内向き法線 × 力積) をヘッドレスで 6 面ぶん回す —
                                // 幾何の式だけの小さな複製 (BuildModes/MakeModalShotPlay 自体は
                                // 呼び直すだけで 2 本目を書いていない)。実際の再生・クリップ池には
                                // 一切触れない (書き出し専用)
                                if (modalFaceProbe_ && !modalFaceProbeDone_
                                    && !modalWavDumpDir_.empty()) {
                                    modalFaceProbeDone_ = true;
                                    // Inspector の面打ちプレビューと同じ既定衝撃力を使う
                                    // (kModalPreviewDefaultImpulse、ModalAudio.h。旧 kProbeImpulse
                                    // はここだけの独自定数だったため sub-10 H で一本化した —
                                    // reviewer round 1 指摘 2: 「4.0 では 6 面すべて BelowMin」の
                                    // 是正がヘッドレス側だけ先に直り、Inspector 側が据え置かれていた)。
                                    // ★[追加] `MYE_MODAL_PROBE_IMPULSE` (計測用の環境変数、
                                    // `MYE_MODAL_THREADS` と同型) で上書きできる — sub-10 の
                                    // dBFS×J 較正表は実際の物理バウンドだけでは J の低い側
                                    // (0.35〜100 N・s) を作れないため、この経路で任意の J を
                                    // 実際の推論結果に対して振れるようにした (CLI フラグにしない
                                    // 理由は engine_spec の CLI 表を増やすほどの恒久機能ではないため)
                                    float probeImpulse = kModalPreviewDefaultImpulse;
                                    if (const char* env = std::getenv("MYE_MODAL_PROBE_IMPULSE")) {
                                        const float v = static_cast<float>(std::atof(env));
                                        if (v > 0.0f) {
                                            probeImpulse = v;
                                        }
                                    }
                                    static const wchar_t* kFaceNames[6] = { L"px", L"nx", L"py",
                                                                            L"ny", L"pz", L"nz" };
                                    for (int face = 0; face < 6; ++face) {
                                        const int axis = face / 2;
                                        const bool positive = (face % 2) == 0;
                                        PendingModalImpact probeImpact = impact;
                                        for (int a = 0; a < 3; ++a) {
                                            probeImpact.localPoint[a] =
                                                (a == axis)
                                                    ? (positive ? fm->frame.aabbMax[a]
                                                                : fm->frame.aabbMin[a])
                                                    : 0.5f * (fm->frame.aabbMin[a]
                                                              + fm->frame.aabbMax[a]);
                                        }
                                        probeImpact.k[0] = probeImpact.k[1] = probeImpact.k[2] = 0.0f;
                                        // sub-10 A: CollectModalImpacts と同じ圧縮カーブを通す
                                        probeImpact.k[axis] =
                                            (positive ? -1.0f : 1.0f) * ModalImpulseCurve(probeImpulse);
                                        probeImpact.excessImpulse = probeImpulse;
                                        AudioClip probeClip;
                                        AudioSpatial probeSpatial;
                                        ModalShotInfo probeInfo;
                                        const ModalShotResult probeResult = MakeModalShotPlay(
                                            *fm, *hdr, probeImpact, *comp, mat, scale, probeClip,
                                            probeSpatial, &probeInfo);
                                        if (probeResult != ModalShotResult::BelowMin) {
                                            wchar_t probePath[512];
                                            std::swprintf(
                                                probePath, 512, L"%ls\\probe_%016llx_%ls.wav",
                                                modalWavDumpDir_.c_str(),
                                                static_cast<unsigned long long>(impact.mesh.value),
                                                kFaceNames[face]);
                                            if (!WriteWavToFile(probeClip, probePath)) {
                                                MYE_LOG_WARN("[modal] --modal-face-probe: failed "
                                                             "to write %ls",
                                                             probePath);
                                            }
                                        }
                                    }
                                }
                                // クリップ池: ラウンドロビンで次のスロットを 1 つ取る。
                                // まだ鳴っている予定 (endTick > now) なら切らずに諦める
                                slot = nextModalSlot_;
                                nextModalSlot_ = (nextModalSlot_ + 1) % kModalClipSlots;
                                if (modalSlots_[slot].endTick > tickIndex) {
                                    result = ModalShotResult::PoolFull;
                                    ++modalStats_.poolFull;
                                } else {
                                    const uint64_t lenTicks = static_cast<uint64_t>(std::ceil(
                                        static_cast<double>(info.lenSec)
                                        / static_cast<double>(fixedDt)));
                                    modalSlots_[slot].endTick =
                                        tickIndex + (std::max<uint64_t>)(lenTicks, 1);
                                    char nameBuf[32];
                                    std::snprintf(nameBuf, sizeof(nameBuf), "modal://slot#%d", slot);
                                    const AssetID clipId{ HashStr(nameBuf) };
                                    audio.RegisterClip(clipId, std::move(clip), nameBuf);
                                    PlayDesc desc;
                                    desc.clip = clipId;
                                    desc.bus = AudioSystem::kBusSe;
                                    desc.volume = 1.0f;
                                    desc.priority = 128;
                                    spatial.reverbSend = acOn ? acComp->waveReverbSend : 0.0f;
                                    if (acOn) {
                                        float gain = 1.0f;
                                        ShapeAcousticSpatial(*acousticField_, acProbe_, *acComp,
                                                             listener.position, spatial.position,
                                                             spatial, gain, nullptr, 1.0f,
                                                             &shapeInfo);
                                        desc.volume = std::clamp(desc.volume * gain, 0.0f, 1.0f);
                                    }
                                    desc.spatial = &spatial;
                                    if (!audio.Play(desc).Valid()) {
                                        result = ModalShotResult::PlayFailed;
                                        ++modalStats_.playFailed;
                                    } else {
                                        result = ModalShotResult::Played;
                                        ++modalStats_.played;
                                        est.lastShotTick = impact.tick;
                                        est.everShot = true;
                                        ++shotsThisTick;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (modalLog) {
                MYE_LOG_INFO(
                    "[modal] t=%llu src=%u mesh=%016llx cell=%d,%d,%d slot=%d k=%.3f,%.3f,%.3f "
                    "J=%.3f modes=%d f0=%.1f f1=%.1f peak=%.2f len=%.3f class=%s gain=%.3f r=%s",
                    static_cast<unsigned long long>(impact.tick), impact.source.index,
                    static_cast<unsigned long long>(impact.mesh.value), info.cellX, info.cellY,
                    info.cellZ, slot, static_cast<double>(impact.k[0]),
                    static_cast<double>(impact.k[1]), static_cast<double>(impact.k[2]),
                    static_cast<double>(impact.excessImpulse), info.modeCount,
                    static_cast<double>(info.f0Hz), static_cast<double>(info.f1Hz),
                    static_cast<double>(info.peakDb), static_cast<double>(info.lenSec),
                    AcousticPathClassName(shapeInfo.cls), static_cast<double>(shapeInfo.gain),
                    ModalShotResultName(result));
            }
        }
    }

    Sweep(audio);
}

} // namespace mye
