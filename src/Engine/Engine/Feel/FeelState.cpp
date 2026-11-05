//====================================================================================
//                          FeelState.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          再生中の手触り (M94c、ADR-031) の sim 状態と、描画専用の出力の評価の実装
//====================================================================================
#include "Engine/Engine/Feel/FeelState.h"

#include <cmath>

#include "Engine/Engine/Loop/GameFlow.h"

namespace mye {

static_assert(static_cast<uint32_t>(kFeelChannelCamera) == static_cast<uint32_t>(MYE_FEEL_CHANNEL_CAMERA)
                  && static_cast<uint32_t>(kFeelChannelRumble) == static_cast<uint32_t>(MYE_FEEL_CHANNEL_RUMBLE)
                  && static_cast<uint32_t>(kFeelChannelFlash) == static_cast<uint32_t>(MYE_FEEL_CHANNEL_FLASH),
              "the v31 SetFeelOutputScale channel numbers are part of the ABI");

namespace {

constexpr float kTicksPerSecond = 60.0f;
constexpr float kMaxShakePosAmp = 10.0f;    // [m]
constexpr float kMaxShakeRotAmpDeg = 45.0f; // [度]
constexpr float kMaxShakeFrequencyHz = 120.0f;
constexpr float kMaxKickOffset = 10.0f;     // [m]
constexpr float kMaxKickAngleDeg = 45.0f;
constexpr float kMaxFlashAmount = 4.0f;
constexpr float kMaxIntensity = 10.0f;

float Finite(float v, float lo, float hi, float fallback = 0.0f)
{
    if (!std::isfinite(v)) {
        return fallback;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t ClampTicks(int32_t t)
{
    return t < 0 ? 0 : (t > kMaxFeelChannelTicks ? kMaxFeelChannelTicks : t);
}

// ハンドル = (serial << 4) | slot。serial は 1 から数え、27 bit で折り返す (0 は使わない)
constexpr uint32_t kSerialMask = 0x07FFFFFFu;
int32_t MakeHandle(uint32_t serial, uint32_t slot)
{
    return static_cast<int32_t>(((serial & kSerialMask) << 4) | (slot & 0xFu));
}

// 正規化時刻 [0,1) でのカーブの値。持続 0 か時刻が持続を過ぎたら 0
float ChannelEnvelope(const MyeCurve& curve, int32_t ticks, float tt)
{
    if (ticks <= 0 || tt >= static_cast<float>(ticks)) {
        return 0.0f;
    }
    return MyeEvalCurve(curve, tt / static_cast<float>(ticks));
}

} // namespace

void SanitizeFeelCurve(MyeCurve& c)
{
    if (c.mode != MYE_CURVE_CUSTOM) {
        c.mode = MYE_CURVE_PRESET;
    }
    if (c.ease < 0 || c.ease >= MYE_EASE_COUNT) {
        c.ease = MYE_EASE_LINEAR;
    }
    c.from = Finite(c.from, -1000.0f, 1000.0f);
    c.to = Finite(c.to, -1000.0f, 1000.0f);
    c.param = Finite(c.param, 0.0f, 10.0f);
    c.keyCount = c.keyCount < 0 ? 0 : (c.keyCount > MYE_CURVE_MAX_KEYS ? MYE_CURVE_MAX_KEYS : c.keyCount);
    for (int32_t i = 0; i < MYE_CURVE_MAX_KEYS; ++i) {
        MyeCurveKey& k = c.keys[i];
        if (i >= c.keyCount) {
            k = MyeCurveKey{}; // 使わない制御点は既定値 (状態の正規形)
            continue;
        }
        k.t = Finite(k.t, 0.0f, 1.0f);
        k.value = Finite(k.value, -1000.0f, 1000.0f);
        k.inTangent = Finite(k.inTangent, -10000.0f, 10000.0f);
        k.outTangent = Finite(k.outTangent, -10000.0f, 10000.0f);
    }
    // t 昇順の安定な挿入ソート (同時刻は元の順序を保つ = 段差の向きが変わらない)
    for (int32_t i = 1; i < c.keyCount; ++i) {
        const MyeCurveKey key = c.keys[i];
        int32_t j = i - 1;
        while (j >= 0 && c.keys[j].t > key.t) {
            c.keys[j + 1] = c.keys[j];
            --j;
        }
        c.keys[j + 1] = key;
    }
}

void SanitizeFeelDesc(MyeFeelDesc& d)
{
    d.hitStopTicks = ClampTicks(d.hitStopTicks);
    d.slowPercent = d.slowPercent < 0 ? 0 : (d.slowPercent > 100 ? 100 : d.slowPercent);
    d.slowHoldTicks = ClampTicks(d.slowHoldTicks);
    d.slowRecoverTicks = ClampTicks(d.slowRecoverTicks);
    if (d.slowRecoverEase < 0 || d.slowRecoverEase >= MYE_EASE_COUNT) {
        d.slowRecoverEase = MYE_EASE_LINEAR;
    }
    d.shakeTicks = ClampTicks(d.shakeTicks);
    d.shakePosAmp = Finite(d.shakePosAmp, 0.0f, kMaxShakePosAmp);
    d.shakeRotAmpDeg = Finite(d.shakeRotAmpDeg, 0.0f, kMaxShakeRotAmpDeg);
    d.shakeFrequencyHz = Finite(d.shakeFrequencyHz, 0.0f, kMaxShakeFrequencyHz, 20.0f);
    SanitizeFeelCurve(d.shakeEnvelope);
    d.kickTicks = ClampTicks(d.kickTicks);
    d.kickOffset.x = Finite(d.kickOffset.x, -kMaxKickOffset, kMaxKickOffset);
    d.kickOffset.y = Finite(d.kickOffset.y, -kMaxKickOffset, kMaxKickOffset);
    d.kickOffset.z = Finite(d.kickOffset.z, -kMaxKickOffset, kMaxKickOffset);
    d.kickPitchDeg = Finite(d.kickPitchDeg, -kMaxKickAngleDeg, kMaxKickAngleDeg);
    d.kickFovDeg = Finite(d.kickFovDeg, -kMaxKickAngleDeg, kMaxKickAngleDeg);
    SanitizeFeelCurve(d.kickCurve);
    d.rumbleTicks = ClampTicks(d.rumbleTicks);
    d.rumbleLow = Finite(d.rumbleLow, 0.0f, 1.0f);
    d.rumbleHigh = Finite(d.rumbleHigh, 0.0f, 1.0f);
    SanitizeFeelCurve(d.rumbleCurve);
    d.flashTicks = ClampTicks(d.flashTicks);
    d.flashVignette = Finite(d.flashVignette, -kMaxFlashAmount, kMaxFlashAmount);
    d.flashChromAberration = Finite(d.flashChromAberration, -kMaxFlashAmount, kMaxFlashAmount);
    d.flashExposure = Finite(d.flashExposure, -1.0f, kMaxFlashAmount);
    d.flashSaturation = Finite(d.flashSaturation, -1.0f, kMaxFlashAmount);
    d.flashColor.r = Finite(d.flashColor.r, 0.0f, kMaxFlashAmount, 1.0f);
    d.flashColor.g = Finite(d.flashColor.g, 0.0f, kMaxFlashAmount, 1.0f);
    d.flashColor.b = Finite(d.flashColor.b, 0.0f, kMaxFlashAmount, 1.0f);
    d.flashColor.a = Finite(d.flashColor.a, 0.0f, 1.0f);
    SanitizeFeelCurve(d.flashCurve);
}

MyeFeelDesc PresentationOnly(const MyeFeelDesc& desc)
{
    MyeFeelDesc d = desc;
    d.hitStopTicks = 0;
    d.slowPercent = 100;
    d.slowHoldTicks = 0;
    d.slowRecoverTicks = 0;
    return d;
}

int32_t FeelPresentationTicks(const MyeFeelDesc& d)
{
    int32_t t = d.shakeTicks;
    t = d.kickTicks > t ? d.kickTicks : t;
    t = d.rumbleTicks > t ? d.rumbleTicks : t;
    t = d.flashTicks > t ? d.flashTicks : t;
    return t;
}

int32_t FeelState::Play(const MyeFeelDesc& descIn, float intensity, uint64_t sourceKey, TimeControl& time)
{
    MyeFeelDesc desc = descIn;
    SanitizeFeelDesc(desc);
    // 時間効果は強さに依らない (intensity は描画チャネルだけに掛かる — ADR-031)
    if (desc.hitStopTicks > 0) {
        time.RequestFreeze(desc.hitStopTicks);
    }
    if (desc.slowPercent < 100) {
        time.RequestSlow(desc.slowPercent, desc.slowHoldTicks, desc.slowRecoverTicks, desc.slowRecoverEase);
    }
    const int32_t duration = FeelPresentationTicks(desc);
    if (duration <= 0) {
        return kFeelHandleTimeOnly;
    }
    // 空きの先頭。無ければ残り時間が最短のもの (同率は添字の小さい方) を置き換える
    uint32_t slot = kMaxFeelInstances;
    int32_t bestRemaining = 0;
    for (uint32_t i = 0; i < kMaxFeelInstances; ++i) {
        const FeelInstance& s = slots_[i];
        if (s.active == 0) {
            slot = i;
            break;
        }
        const int32_t remaining = FeelPresentationTicks(s.desc) - s.ageTicks;
        if (slot == kMaxFeelInstances || remaining < bestRemaining) {
            slot = i;
            bestRemaining = remaining;
        }
    }
    const uint32_t serial = nextSerial_;
    nextSerial_ = ((nextSerial_ + 1u) & kSerialMask) == 0u ? 1u : ((nextSerial_ + 1u) & kSerialMask);
    FeelInstance& inst = slots_[slot];
    inst = FeelInstance{};
    inst.active = 1;
    inst.serial = serial;
    inst.ageTicks = 0;
    inst.intensity = Finite(intensity, 0.0f, kMaxIntensity, 1.0f);
    inst.sourceKey = sourceKey;
    inst.desc = desc;
    return MakeHandle(serial, slot);
}

void FeelState::Stop(int32_t handle)
{
    if (handle == kFeelHandleInvalid) {
        for (FeelInstance& s : slots_) {
            s = FeelInstance{};
        }
        return;
    }
    if (handle <= 0) {
        return;
    }
    const uint32_t h = static_cast<uint32_t>(handle);
    const uint32_t slot = h & 0xFu;
    const uint32_t serial = h >> 4;
    if (slot < kMaxFeelInstances && slots_[slot].active != 0 && (slots_[slot].serial & kSerialMask) == serial) {
        slots_[slot] = FeelInstance{};
    }
}

void FeelState::Advance()
{
    for (FeelInstance& s : slots_) {
        if (s.active == 0) {
            continue;
        }
        ++s.ageTicks;
        if (s.ageTicks >= FeelPresentationTicks(s.desc)) {
            s = FeelInstance{}; // 空きは既定値 = 正規形 (ハッシュが空きの中身に依存しない)
        }
    }
}

void FeelState::Clear()
{
    for (FeelInstance& s : slots_) {
        s = FeelInstance{};
    }
    nextSerial_ = 1;
}

bool FeelState::HasHashableState() const
{
    return nextSerial_ != 1 || ActiveCount() != 0;
}

uint32_t FeelState::ActiveCount() const
{
    uint32_t n = 0;
    for (const FeelInstance& s : slots_) {
        n += s.active != 0 ? 1u : 0u;
    }
    return n;
}

namespace {

// 1 つの表の全件を out へ足し込む (EvaluateFeel の実体。sim の表と描画専用のプレビュー表の両方に使う)
void AccumulateFeel(const FeelState& state, float alpha, FeelFrameOutput& out, float flashColorSum[3])
{
    for (uint32_t i = 0; i < kMaxFeelInstances; ++i) {
        const FeelInstance& s = state.Slot(i);
        if (s.active == 0) {
            continue;
        }
        const MyeFeelDesc& d = s.desc;
        // 描画は tick N-1 と N の間 (α) を出している。最新 tick で発火した件 (age 0) は α=1 で経過 0
        float tt = static_cast<float>(s.ageTicks) + alpha - 1.0f;
        tt = tt < 0.0f ? 0.0f : tt;
        const float k = s.intensity;

        const float shake = ChannelEnvelope(d.shakeEnvelope, d.shakeTicks, tt) * k;
        if (shake != 0.0f && (d.shakePosAmp > 0.0f || d.shakeRotAmpDeg > 0.0f)) {
            const float x = tt / kTicksPerSecond * d.shakeFrequencyHz;
            const uint32_t seed = MyeFeelHash32(s.serial * 2654435761u);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                out.camPos[axis] += d.shakePosAmp * shake * MyeValueNoise(seed + axis, x);
                out.camRotDeg[axis] += d.shakeRotAmpDeg * shake * MyeValueNoise(seed + 3u + axis, x);
            }
            out.cameraActive = true;
        }
        const float kick = ChannelEnvelope(d.kickCurve, d.kickTicks, tt) * k;
        if (kick != 0.0f) {
            out.camPos[0] += d.kickOffset.x * kick;
            out.camPos[1] += d.kickOffset.y * kick;
            out.camPos[2] += d.kickOffset.z * kick;
            out.camRotDeg[0] -= d.kickPitchDeg * kick; // 正の kickPitch = 上を向く (pitch は下向きが正)
            out.fovDeg += d.kickFovDeg * kick;
            out.cameraActive = true;
        }
        const float rumble = ChannelEnvelope(d.rumbleCurve, d.rumbleTicks, tt) * k;
        if (rumble > 0.0f) {
            const float lo = d.rumbleLow * rumble;
            const float hi = d.rumbleHigh * rumble;
            out.rumbleLow = lo > out.rumbleLow ? lo : out.rumbleLow; // 重なりは max (足すと振り切れる)
            out.rumbleHigh = hi > out.rumbleHigh ? hi : out.rumbleHigh;
        }
        const float flash = ChannelEnvelope(d.flashCurve, d.flashTicks, tt) * k;
        if (flash != 0.0f) {
            out.vignette += d.flashVignette * flash;
            out.chromAberration += d.flashChromAberration * flash;
            out.exposure += d.flashExposure * flash;
            out.saturation += d.flashSaturation * flash;
            const float w = d.flashColor.a * flash;
            flashColorSum[0] += d.flashColor.r * w;
            flashColorSum[1] += d.flashColor.g * w;
            flashColorSum[2] += d.flashColor.b * w;
            out.flashWeight += w;
            out.flashActive = true;
        }
    }
}

} // namespace

FeelFrameOutput EvaluateFeel(const FeelState& state, float interpAlpha, const FeelOutputScale& scale,
                             const FeelState* preview)
{
    FeelFrameOutput out;
    const float alpha = interpAlpha < 0.0f ? 0.0f : (interpAlpha > 1.0f ? 1.0f : interpAlpha);
    float flashColorSum[3] = { 0.0f, 0.0f, 0.0f };
    AccumulateFeel(state, alpha, out, flashColorSum);
    if (preview != nullptr) {
        AccumulateFeel(*preview, alpha, out, flashColorSum);
    }
    if (out.flashWeight > 0.0f) {
        for (int c = 0; c < 3; ++c) {
            out.flashColor[c] = flashColorSum[c] / out.flashWeight; // 重みつき平均の色
        }
        out.flashWeight = out.flashWeight > 1.0f ? 1.0f : out.flashWeight;
    }
    // 利用者の出力倍率 (揺れ酔い・振動が苦手な人のため)
    const float cam = Finite(scale.scale[kFeelChannelCamera], 0.0f, 1.0f, 1.0f);
    const float rum = Finite(scale.scale[kFeelChannelRumble], 0.0f, 1.0f, 1.0f);
    const float fla = Finite(scale.scale[kFeelChannelFlash], 0.0f, 1.0f, 1.0f);
    for (int a = 0; a < 3; ++a) {
        out.camPos[a] *= cam;
        out.camRotDeg[a] *= cam;
    }
    out.fovDeg *= cam;
    out.cameraActive = out.cameraActive && cam > 0.0f;
    out.rumbleLow = Finite(out.rumbleLow * rum, 0.0f, 1.0f);
    out.rumbleHigh = Finite(out.rumbleHigh * rum, 0.0f, 1.0f);
    out.vignette *= fla;
    out.chromAberration *= fla;
    out.exposure *= fla;
    out.saturation *= fla;
    out.flashWeight *= fla;
    out.flashActive = out.flashActive && fla > 0.0f;
    return out;
}

} // namespace mye
