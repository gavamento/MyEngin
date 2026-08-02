#include "Engine/Engine/Audio/AudioSourceSystem.h"

#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Audio/SoundAsset.h"

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
            if (l->enabled == 0) {
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
                if (!best.IsNull() && c->isPrimary == 0) {
                    continue;
                }
                if (!IsEntityActive(world, e)) {
                    continue;
                }
                best = e;
                bestWorld = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
                primaryFound = c->isPrimary != 0;
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

void AudioSourceSystem::Reset()
{
    states_.clear();
    listenerVel_ = {};
    listenerEntity_ = kNullEntity;
    lastTickValid_ = false;
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

void AudioSourceSystem::Update(World& world, AudioSystem& audio, const SoundLibrary& sounds,
                               uint64_t tickIndex, float fixedDt, bool simulateScripts)
{
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
    lastTick_ = tickIndex;
    lastTickValid_ = true;

    for (SourceState& s : states_) {
        s.seen = false;
    }

    // ---- リスナー ----
    AudioListenerState listener;
    EntityID listenerEntity = kNullEntity;
    const bool haveListener = FindListener(world, listener, listenerEntity);
    if (!(listenerEntity == listenerEntity_)) {
        listenerVel_ = {}; // リスナーが替わったら速度推定を仕切り直す (瞬間移動扱いにしない)
        listenerEntity_ = listenerEntity;
    }
    if (haveListener) {
        UpdateVelocitySample(listenerVel_, listener.position, tickIndex, fixedDt);
        listener.velocity = listenerVel_.velocity;
    }
    audio.SetListener(listener);

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
                continue;
            }

            const AudioVec3 pos = PositionOf(wm);

            // ---- 再生開始 (playOnAwake) ----
            if (simulateScripts && src->playOnAwake != 0 && !st.started && !st.voice.Valid()) {
                st.variationIndex = PickVariationIndex(*asset, rng_.NextU32());
                if (st.variationIndex < 0) {
                    // 鳴らせるバリエーションが 1 つも無いアセット。毎フレーム試し続けない
                    st.started = true;
                } else {
                    st.volJitter = rng_.Range(-1.0f, 1.0f);
                    st.pitchJitter = rng_.Range(-1.0f, 1.0f);
                    PlayDesc desc;
                    AudioSpatial spatial;
                    MakeSourcePlay(*asset, *src, audio, st.variationIndex, st.volJitter,
                                   st.pitchJitter, desc, spatial);
                    // 初回の定位も込みで渡す (Start() 前に適用されるので出だしが無定位にならない)
                    spatial.position = pos;
                    spatial.velocity = {}; // 生成直後は静止扱い (初速をでっち上げない)
                    desc.spatial = spatial.spatialBlend > 0.0f ? &spatial : nullptr;
                    st.voice = audio.Play(desc);
                    // クリップが未ロードの間は失敗する → started を立てずに次フレーム再挑戦する
                    st.started = st.voice.Valid();
                    st.vel = {};
                }
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

    Sweep(audio);
}

} // namespace mye
