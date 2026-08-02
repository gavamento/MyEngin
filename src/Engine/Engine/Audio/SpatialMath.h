#pragma once
#include <cmath>
#include <cstdint>

// 3D オーディオの純関数だけを置くヘッダ。XAudio2 にも X3DAudio にも ECS にも依存しないので
// ヘッドレス selftest から直接叩ける (Audio/VoicePolicy.h・Renderer/FrustumCull.h・
// Renderer/PostFxMath.h・Particles/ParticleCurves.h と同じ流儀)。
//
// **ここは決定論レーンの外** — sim はこの結果を一切読まない (読んだ瞬間にリプレイが壊れる)。

namespace mye {

// 位置/速度の最小 POD。DirectXMath を AudioSystem のヘッダへ引き込まないために持つ
// (このサブシステムは意図的に依存を薄く保っている)
struct AudioVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 距離減衰カーブの 1 点。distance は **maxDistance で正規化された 0..1** で、
// X3DAUDIO_DISTANCE_CURVE_POINT の規約そのもの (CurveDistanceScaler = maxDistance を渡す前提)
struct AudioCurvePoint {
    float distance = 0.0f;
    float dsp = 1.0f;
};

inline constexpr int kMaxRolloffCurvePoints = 16;

// rolloff は SoundRolloff (Audio/SoundAsset.h) と同じ並び。ヘッダ依存を作らないので int で受ける。
//   0 = Logarithmic : gain = minD / d       (Unity の Logarithmic と同じ「逆数」カーブ)
//   1 = Linear      : gain = (maxD - d) / (maxD - minD)
//   2 = Inverse     : gain = (minD / d)^2   (逆二乗 = 物理的なパワー減衰)
// いずれも d <= minD で 1.0。**d >= maxD では必ず 0 を返す** — X3DAudio はカーブ最終点の値を
// それ以降ずっと保持するので、ここを 0 にしないと音が無限遠まで聞こえ続ける
// (Logarithmic の素の値は maxD でも minD/maxD > 0 のまま)。
inline float RolloffGain(int rolloff, float minDistance, float maxDistance, float d)
{
    const float minD = minDistance > 1e-4f ? minDistance : 1e-4f;
    const float maxD = maxDistance > minD * 1.0001f ? maxDistance : minD * 1.0001f;
    if (!(d > minD)) { // NaN もここで最大音量に落ちる (手編集された .sound.json 対策)
        return 1.0f;
    }
    if (d >= maxD) {
        return 0.0f;
    }
    switch (rolloff) {
    case 1: // Linear
        return (maxD - d) / (maxD - minD);
    case 2: { // Inverse (逆二乗)
        const float r = minD / d;
        return r * r;
    }
    default: // 0 = Logarithmic (逆数)
        return minD / d;
    }
}

// 正規化距離 0..1 のカーブ点列を組む。戻り値 = 書き込んだ点数 (2 以上)。
//
// 点の取り方: **減衰の始まる位置 (minD/maxD) を必ず 1 点として置き、そこから先は等比に刻む**。
// 1/d 系のカーブは対数距離で見ると直線なので、等比刻みなら少ない点数でも誤差が小さい。
// 均等刻みだと minD/maxD が小さいとき (例 1m/50m = 0.02) に近距離の急降下が
// 最初の 1 区間へ丸ごと潰れ、近くの音の減衰が黙って線形になってしまう。
//
// X3DAudio は距離が**狭義単調増加**であること・先頭 0.0 / 末尾 1.0 であることを要求するので、
// t0 は [1e-3, 0.5] にクランプしてから使う。
inline int BuildRolloffCurve(int rolloff, float minDistance, float maxDistance, AudioCurvePoint* out,
                             int maxPoints)
{
    if (out == nullptr || maxPoints < 2) {
        return 0;
    }
    const int n = maxPoints < kMaxRolloffCurvePoints ? maxPoints : kMaxRolloffCurvePoints;
    const float minD = minDistance > 1e-4f ? minDistance : 1e-4f;
    const float maxD = maxDistance > minD * 1.0001f ? maxDistance : minD * 1.0001f;
    float t0 = minD / maxD;
    if (t0 < 1e-3f) {
        t0 = 1e-3f;
    }
    if (t0 > 0.5f) {
        t0 = 0.5f;
    }

    out[0] = { 0.0f, 1.0f };
    if (n < 4) { // 等比刻みの区間が取れない退化ケース (selftest が小さい配列を渡したとき)
        if (n == 2) {
            out[1] = { 1.0f, 0.0f };
            return 2;
        }
        out[1] = { t0, 1.0f };
        out[2] = { 1.0f, 0.0f };
        return 3;
    }

    out[1] = { t0, 1.0f };
    const int last = n - 1; // last >= 3 なので下の指数は必ず正
    const float ratio = std::pow(1.0f / t0, 1.0f / static_cast<float>(last - 1));
    float t = t0;
    for (int i = 2; i < last; ++i) {
        t *= ratio;
        out[i].distance = t;
        out[i].dsp = RolloffGain(rolloff, minD, maxD, t * maxD);
    }
    out[last] = { 1.0f, 0.0f };
    return n;
}

// ---- ドップラー用の速度推定 -------------------------------------------------
//
// **フレームではなく tick でサンプリングする**のが要点。sim は 60Hz でしか進まないのに
// Runtime は vsync 無効で数千 fps 回るため、フレーム差分を取ると「位置は前 tick のまま・
// dt だけ極小」になって速度が発散/ノイズ化する。tick 差分なら 60fps でも 6500fps でも
// 厳密に同じ値が出るし、1 フレームで複数 tick 進む追いつきフレームでも正しい。

struct VelocitySample {
    AudioVec3 position;  // 最後にサンプルした位置
    AudioVec3 velocity;  // 平滑化済み速度 (m/s)
    uint64_t tick = 0;   // 最後にサンプルした tickIndex
    bool valid = false;  // false = 初回 (速度 0 で立ち上げる)
};

// 1 tick でこれ以上動いたら瞬間移動 (SetLocalPosition / Instantiate / LoadScene) とみなして
// 速度を 0 に落とす。入れないとテレポートの度にピッチが飛ぶ
inline constexpr float kTeleportMetersPerTick = 5.0f;
// 速度平滑化の半減期 (tick)。**整数 tick 基準**なので実フレームレートに依存しない
inline constexpr float kVelocityHalfLifeTicks = 3.0f;

inline void UpdateVelocitySample(VelocitySample& s, const AudioVec3& pos, uint64_t tick,
                                 float fixedDt)
{
    if (!s.valid || tick < s.tick) {
        // 初回、または tick が巻き戻った (シーン再ロード / リプレイ開始) → 速度 0 で仕切り直す
        s.position = pos;
        s.velocity = {};
        s.tick = tick;
        s.valid = true;
        return;
    }
    if (tick == s.tick) {
        return; // 0-tick フレーム: 前回の速度をそのまま使う (再計算するとノイズになる)
    }

    const float dTicks = static_cast<float>(tick - s.tick);
    const float dx = pos.x - s.position.x;
    const float dy = pos.y - s.position.y;
    const float dz = pos.z - s.position.z;

    AudioVec3 raw;
    const float dt = dTicks * fixedDt;
    const float limit = kTeleportMetersPerTick * dTicks;
    if (dt > 1e-6f && (dx * dx + dy * dy + dz * dz) <= limit * limit) {
        raw = { dx / dt, dy / dt, dz / dt };
    } // テレポート判定に掛かったら raw = 0 のまま = ドップラー無し

    const float alpha = 1.0f - std::pow(0.5f, dTicks / kVelocityHalfLifeTicks);
    s.velocity.x += (raw.x - s.velocity.x) * alpha;
    s.velocity.y += (raw.y - s.velocity.y) * alpha;
    s.velocity.z += (raw.z - s.velocity.z) * alpha;
    s.position = pos;
    s.tick = tick;
}

} // namespace mye
