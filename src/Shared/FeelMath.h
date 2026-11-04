#pragma once
// 手触り (M94、ADR-031) の共有数学: イージング / カーブ / ばね / 値ノイズ / 手触りの記述子。
//
// GameLogic.dll とエンジンが**同じ関数**を使うための header-only の inline と POD
// (MathPod.h と同じ規則: STL / DirectXMath / エンジンヘッダを含めない)。
//
// ★決定論: ここの関数は sim (ヒットストップ後のスロー回復、スクリプトの移動) からも呼ばれうる。
//   CRT の sin / pow / exp は実装依存なので使わず、四則演算と固定次数の多項式だけで組む
//   (/fp:precise の下でどのビルドでも同じビット列になる)。MyeSinRad (ScriptAPI.h) とは
//   独立に持つ — あちらは「1 命令も変えるな」の制約付きで、replay の視点角に効いている。
//
// ★番号 (MYE_EASE_* / MYE_CURVE_*) は .feel.json と actions.json に名前で書き、
//   スナップショットとハッシュには数値で載る。**末尾追加のみ**で、既存の値は変えない。

#include <stdint.h>

#include "Shared/MathPod.h"

// ---- イージングの種類 ----
enum : int32_t {
    MYE_EASE_LINEAR = 0,
    MYE_EASE_IN_QUAD,
    MYE_EASE_OUT_QUAD,
    MYE_EASE_INOUT_QUAD,
    MYE_EASE_IN_CUBIC,
    MYE_EASE_OUT_CUBIC,
    MYE_EASE_INOUT_CUBIC,
    MYE_EASE_IN_QUART,
    MYE_EASE_OUT_QUART,
    MYE_EASE_INOUT_QUART,
    MYE_EASE_IN_QUINT,
    MYE_EASE_OUT_QUINT,
    MYE_EASE_INOUT_QUINT,
    MYE_EASE_IN_SINE,
    MYE_EASE_OUT_SINE,
    MYE_EASE_INOUT_SINE,
    MYE_EASE_IN_EXPO,
    MYE_EASE_OUT_EXPO,
    MYE_EASE_INOUT_EXPO,
    MYE_EASE_IN_BACK,
    MYE_EASE_OUT_BACK,
    MYE_EASE_INOUT_BACK,
    MYE_EASE_IN_ELASTIC,
    MYE_EASE_OUT_ELASTIC,
    MYE_EASE_INOUT_ELASTIC,
    MYE_EASE_IN_BOUNCE,
    MYE_EASE_OUT_BOUNCE,
    MYE_EASE_INOUT_BOUNCE,
    MYE_EASE_STEP, // t < 1 では 0、t >= 1 で 1 (保持してから切り替える)
    MYE_EASE_COUNT
};

// .feel.json / actions.json に書く名前 (MYE_EASE_* と同じ並び)。読み書きの両方がこの表だけを見る
inline const char* MyeEaseName(int32_t ease)
{
    static const char* const kNames[MYE_EASE_COUNT] = {
        "Linear",    "InQuad",    "OutQuad",     "InOutQuad",   "InCubic",     "OutCubic",
        "InOutCubic", "InQuart",  "OutQuart",    "InOutQuart",  "InQuint",     "OutQuint",
        "InOutQuint", "InSine",   "OutSine",     "InOutSine",   "InExpo",      "OutExpo",
        "InOutExpo",  "InBack",   "OutBack",     "InOutBack",   "InElastic",   "OutElastic",
        "InOutElastic", "InBounce", "OutBounce", "InOutBounce", "Step",
    };
    return (ease >= 0 && ease < MYE_EASE_COUNT) ? kNames[ease] : "Linear";
}

// 名前 → MYE_EASE_*。大文字小文字は区別する。不明な名前は -1 (呼び出し側が警告して Linear に倒す)
inline int32_t MyeEaseFromName(const char* name)
{
    if (name == nullptr) {
        return -1;
    }
    for (int32_t e = 0; e < MYE_EASE_COUNT; ++e) {
        const char* a = MyeEaseName(e);
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return e;
        }
    }
    return -1;
}

// MyeCurve::mode
enum : int32_t {
    MYE_CURVE_PRESET = 0, // from + (to - from) * Ease(ease, t)
    MYE_CURVE_CUSTOM = 1, // keys の Hermite 補間 (カーブエディタで描く)
};

#define MYE_CURVE_MAX_KEYS 8
#define MYE_EASE_BACK_DEFAULT 1.70158f // Back の行き過ぎ量の既定 (Penner の値、約 10% 行き過ぎる)

// カーブの制御点 1 個。t は [0,1] の正規化時刻、接線は「値 / t」の傾き
struct MyeCurveKey {
    float t = 0.0f;
    float value = 0.0f;
    float inTangent = 0.0f;
    float outTangent = 0.0f;
};

// 時間 [0,1] → 値 のカーブ。preset (種類 + 数値) と custom (制御点) の 2 通りで書ける。
// 全メンバ 4 バイトで詰め物が無い (状態としてバイト列のままハッシュされる)。
// keys は t 昇順・t は [0,1] に収まっていること — 読み込み側 (Sanitize) が正規化する
struct MyeCurve {
    int32_t mode = MYE_CURVE_PRESET;
    int32_t ease = MYE_EASE_LINEAR;
    float from = 0.0f;
    float to = 1.0f;
    float param = 0.0f; // Back の行き過ぎ量 (0 = MYE_EASE_BACK_DEFAULT)。他の種類は使わない
    int32_t keyCount = 0;
    MyeCurveKey keys[MYE_CURVE_MAX_KEYS] = {};
};
static_assert(sizeof(MyeCurveKey) == 16, "MyeCurveKey layout");
static_assert(sizeof(MyeCurve) == 24 + 16 * MYE_CURVE_MAX_KEYS, "MyeCurve layout (no padding)");

inline MyeCurve MyeMakeCurve(int32_t ease, float from, float to)
{
    MyeCurve c;
    c.mode = MYE_CURVE_PRESET;
    c.ease = ease;
    c.from = from;
    c.to = to;
    return c;
}

// ---- 決定論の初等関数 (多項式のみ) ----
constexpr float kMyeFeelPi = 3.14159265358979f;

inline float MyeFeelClamp01(float t)
{
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// 任意の x の sin。2π の整数倍を引いて [-π, π] へ寄せ、さらに [-π/2, π/2] へ折り返して
// 9 次のテイラーで評価する (誤差 4e-6 未満、FeelMathSelfTest が固定。手触りの用途には十分)
inline float MyeFeelSin(float x)
{
    constexpr float kTwoPi = kMyeFeelPi * 2.0f;
    const float turns = x / kTwoPi;
    // 四捨五入の整数化 (|x| は手触りの用途では高々数百 rad)
    const int32_t n = static_cast<int32_t>(turns + (turns >= 0.0f ? 0.5f : -0.5f));
    x -= static_cast<float>(n) * kTwoPi;
    if (x > kMyeFeelPi * 0.5f) {
        x = kMyeFeelPi - x;
    } else if (x < -kMyeFeelPi * 0.5f) {
        x = -kMyeFeelPi - x;
    }
    const float x2 = x * x;
    return x
        * (1.0f
           + x2
               * (-1.0f / 6.0f
                  + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

inline float MyeFeelCos(float x)
{
    return MyeFeelSin(x + kMyeFeelPi * 0.5f);
}

// 2^x。整数部は 2 倍 / 半分の繰り返し (厳密)、小数部 [0,1) は e^(f ln2) の 7 次テイラー
// (相対誤差 1e-6 未満)。x は [-126, 126] にクランプする
inline float MyeFeelExp2(float x)
{
    if (x < -126.0f) {
        x = -126.0f;
    } else if (x > 126.0f) {
        x = 126.0f;
    }
    int32_t n = static_cast<int32_t>(x);
    if (static_cast<float>(n) > x) {
        --n; // 負の数の切り捨てを floor にする
    }
    const float f = x - static_cast<float>(n);
    const float y = f * 0.693147180559945f;
    float r = 1.0f + y * (1.0f + y * (0.5f + y * (1.0f / 6.0f + y * (1.0f / 24.0f
              + y * (1.0f / 120.0f + y * (1.0f / 720.0f + y * (1.0f / 5040.0f)))))));
    for (; n > 0; --n) {
        r *= 2.0f;
    }
    for (; n < 0; ++n) {
        r *= 0.5f;
    }
    return r;
}

// ---- イージング ----
inline float MyeEaseOutBounce(float t)
{
    constexpr float kN = 7.5625f;
    constexpr float kD = 2.75f;
    if (t < 1.0f / kD) {
        return kN * t * t;
    }
    if (t < 2.0f / kD) {
        t -= 1.5f / kD;
        return kN * t * t + 0.75f;
    }
    if (t < 2.5f / kD) {
        t -= 2.25f / kD;
        return kN * t * t + 0.9375f;
    }
    t -= 2.625f / kD;
    return kN * t * t + 0.984375f;
}

// 種類 ease で t (自動で [0,1] にクランプ) を写す。端点は Back / Elastic を含めて 0 と 1。
// param は Back の行き過ぎ量 (0 以下 = 既定)。不明な種類は Linear
inline float MyeEase(int32_t ease, float t, float param = 0.0f)
{
    t = MyeFeelClamp01(t);
    const float u = 1.0f - t;
    switch (ease) {
    case MYE_EASE_IN_QUAD: return t * t;
    case MYE_EASE_OUT_QUAD: return 1.0f - u * u;
    case MYE_EASE_INOUT_QUAD:
        return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * u * u;
    case MYE_EASE_IN_CUBIC: return t * t * t;
    case MYE_EASE_OUT_CUBIC: return 1.0f - u * u * u;
    case MYE_EASE_INOUT_CUBIC:
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * u * u * u;
    case MYE_EASE_IN_QUART: return t * t * t * t;
    case MYE_EASE_OUT_QUART: return 1.0f - u * u * u * u;
    case MYE_EASE_INOUT_QUART:
        return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * u * u * u * u;
    case MYE_EASE_IN_QUINT: return t * t * t * t * t;
    case MYE_EASE_OUT_QUINT: return 1.0f - u * u * u * u * u;
    case MYE_EASE_INOUT_QUINT:
        return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - 16.0f * u * u * u * u * u;
    case MYE_EASE_IN_SINE: return 1.0f - MyeFeelCos(t * kMyeFeelPi * 0.5f);
    case MYE_EASE_OUT_SINE: return MyeFeelSin(t * kMyeFeelPi * 0.5f);
    case MYE_EASE_INOUT_SINE: return 0.5f - 0.5f * MyeFeelCos(t * kMyeFeelPi);
    case MYE_EASE_IN_EXPO: return t <= 0.0f ? 0.0f : MyeFeelExp2(10.0f * t - 10.0f);
    case MYE_EASE_OUT_EXPO: return t >= 1.0f ? 1.0f : 1.0f - MyeFeelExp2(-10.0f * t);
    case MYE_EASE_INOUT_EXPO:
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t >= 1.0f) {
            return 1.0f;
        }
        return t < 0.5f ? 0.5f * MyeFeelExp2(20.0f * t - 10.0f)
                        : 1.0f - 0.5f * MyeFeelExp2(-20.0f * t + 10.0f);
    case MYE_EASE_IN_BACK:
    case MYE_EASE_OUT_BACK:
    case MYE_EASE_INOUT_BACK: {
        const float c1 = param > 0.0f ? param : MYE_EASE_BACK_DEFAULT;
        const float c3 = c1 + 1.0f;
        if (ease == MYE_EASE_IN_BACK) {
            return c3 * t * t * t - c1 * t * t;
        }
        if (ease == MYE_EASE_OUT_BACK) {
            const float s = t - 1.0f;
            return 1.0f + c3 * s * s * s + c1 * s * s;
        }
        const float c2 = c1 * 1.525f;
        if (t < 0.5f) {
            const float s = 2.0f * t;
            return 0.5f * (s * s * ((c2 + 1.0f) * s - c2));
        }
        const float s = 2.0f * t - 2.0f;
        return 0.5f * (s * s * ((c2 + 1.0f) * s + c2) + 2.0f);
    }
    case MYE_EASE_IN_ELASTIC:
    case MYE_EASE_OUT_ELASTIC:
    case MYE_EASE_INOUT_ELASTIC: {
        if (t <= 0.0f) {
            return 0.0f;
        }
        if (t >= 1.0f) {
            return 1.0f;
        }
        constexpr float kC4 = (2.0f * kMyeFeelPi) / 3.0f;
        constexpr float kC5 = (2.0f * kMyeFeelPi) / 4.5f;
        if (ease == MYE_EASE_IN_ELASTIC) {
            return -MyeFeelExp2(10.0f * t - 10.0f) * MyeFeelSin((t * 10.0f - 10.75f) * kC4);
        }
        if (ease == MYE_EASE_OUT_ELASTIC) {
            return MyeFeelExp2(-10.0f * t) * MyeFeelSin((t * 10.0f - 0.75f) * kC4) + 1.0f;
        }
        if (t < 0.5f) {
            return -0.5f * MyeFeelExp2(20.0f * t - 10.0f) * MyeFeelSin((20.0f * t - 11.125f) * kC5);
        }
        return 0.5f * MyeFeelExp2(-20.0f * t + 10.0f) * MyeFeelSin((20.0f * t - 11.125f) * kC5) + 1.0f;
    }
    case MYE_EASE_IN_BOUNCE: return 1.0f - MyeEaseOutBounce(1.0f - t);
    case MYE_EASE_OUT_BOUNCE: return MyeEaseOutBounce(t);
    case MYE_EASE_INOUT_BOUNCE:
        return t < 0.5f ? 0.5f * (1.0f - MyeEaseOutBounce(1.0f - 2.0f * t))
                        : 0.5f * (1.0f + MyeEaseOutBounce(2.0f * t - 1.0f));
    case MYE_EASE_STEP: return t >= 1.0f ? 1.0f : 0.0f;
    default: return t; // MYE_EASE_LINEAR と未知の値
    }
}

// a から b へ ease で補間する糖衣
inline float MyeEaseLerp(int32_t ease, float a, float b, float t, float param = 0.0f)
{
    return a + (b - a) * MyeEase(ease, t, param);
}

// ---- カーブの評価 ----
// t は [0,1] にクランプする。custom で keyCount == 0 なら 0、1 なら定数。
// 制御点の外側は端の値を保持する (外挿しない)
inline float MyeEvalCurve(const MyeCurve& c, float t)
{
    t = MyeFeelClamp01(t);
    if (c.mode != MYE_CURVE_CUSTOM) {
        return c.from + (c.to - c.from) * MyeEase(c.ease, t, c.param);
    }
    const int32_t n = c.keyCount < 0 ? 0 : (c.keyCount > MYE_CURVE_MAX_KEYS ? MYE_CURVE_MAX_KEYS : c.keyCount);
    if (n == 0) {
        return 0.0f;
    }
    if (n == 1 || t <= c.keys[0].t) {
        return c.keys[0].value;
    }
    if (t >= c.keys[n - 1].t) {
        return c.keys[n - 1].value;
    }
    int32_t i = 0;
    while (i + 1 < n - 1 && t >= c.keys[i + 1].t) {
        ++i;
    }
    const MyeCurveKey& k0 = c.keys[i];
    const MyeCurveKey& k1 = c.keys[i + 1];
    const float span = k1.t - k0.t;
    if (span <= 0.0f) {
        return k1.value; // 同時刻の 2 点 = 段差
    }
    const float s = (t - k0.t) / span;
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    return h00 * k0.value + h10 * span * k0.outTangent + h01 * k1.value + h11 * span * k1.inTangent;
}

// custom カーブの接線を滑らかにそろえる (Catmull-Rom 流: 両隣を結ぶ傾き、端は片側の傾き)。
// 入と出は同じ値になる (点で折れない)。カーブエディタの「接線をなめらかに」が呼ぶ
inline void MyeAutoTangents(MyeCurve& c)
{
    const int32_t n = c.keyCount < 0 ? 0 : (c.keyCount > MYE_CURVE_MAX_KEYS ? MYE_CURVE_MAX_KEYS : c.keyCount);
    for (int32_t i = 0; i < n; ++i) {
        const MyeCurveKey& a = c.keys[i > 0 ? i - 1 : i];
        const MyeCurveKey& b = c.keys[i + 1 < n ? i + 1 : i];
        const float dt = b.t - a.t;
        const float slope = dt > 0.0f ? (b.value - a.value) / dt : 0.0f;
        c.keys[i].inTangent = slope;
        c.keys[i].outTangent = slope;
    }
}

// ---- ばね ----
// 減衰付きばねを dt だけ進める。陰的 (後退) オイラーなので周波数と dt に依らず発散しない。
//   frequencyHz  : 固有振動数 (大きいほど速く追いつく)
//   dampingRatio : 1 = 臨界減衰 (行き過ぎない)、1 未満 = 行き過ぎて揺れる、1 超 = 粘る
inline void MyeSpringStep(float& x, float& v, float target, float frequencyHz, float dampingRatio, float dt)
{
    const float omega = 2.0f * kMyeFeelPi * (frequencyHz > 0.0f ? frequencyHz : 0.0f);
    const float k = omega * omega;
    const float c = 2.0f * (dampingRatio > 0.0f ? dampingRatio : 0.0f) * omega;
    const float denom = 1.0f + dt * c + dt * dt * k;
    v = (v - dt * k * (x - target)) / denom;
    x += dt * v;
}

inline void MyeSpringStep3(MyeVec3& x, MyeVec3& v, const MyeVec3& target, float frequencyHz,
                           float dampingRatio, float dt)
{
    MyeSpringStep(x.x, v.x, target.x, frequencyHz, dampingRatio, dt);
    MyeSpringStep(x.y, v.y, target.y, frequencyHz, dampingRatio, dt);
    MyeSpringStep(x.z, v.z, target.z, frequencyHz, dampingRatio, dt);
}

// 臨界減衰で target へ寄せる (Unity の SmoothDamp と同じ式、Game Programming Gems 4 の
// 1/(1+x+0.48x^2+0.235x^3) による exp(-x) の近似)。smoothTime は到達までのおよその秒数
inline float MyeSmoothDamp(float current, float target, float& velocity, float smoothTime, float dt)
{
    const float st = smoothTime > 0.0001f ? smoothTime : 0.0001f;
    const float omega = 2.0f / st;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = current - target;
    const float temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    float result = target + (change + temp) * e;
    // 行き過ぎ防止 (target を跨いだら target に止める)
    if ((target - current > 0.0f) == (result > target)) {
        result = target;
        velocity = 0.0f;
    }
    return result;
}

// ---- 値ノイズ (乱数を引かない揺れの源) ----
// 32bit の整数ハッシュ (lowbias32、Chris Wellons)。乱数列を消費しないので、
// 揺れを足しても world.Rng() の列は 1 つもずれない
inline uint32_t MyeFeelHash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// [-1, 1] の 1 次元値ノイズ。格子点 (整数) ごとにハッシュで値を決め、smoothstep で繋ぐ。
// x は 0 以上を前提とする (揺れの時刻 × 周波数)
inline float MyeValueNoise(uint32_t seed, float x)
{
    if (x < 0.0f) {
        x = 0.0f;
    }
    const uint32_t i = static_cast<uint32_t>(x);
    const float f = x - static_cast<float>(i);
    const float s = f * f * (3.0f - 2.0f * f);
    constexpr float kInv = 1.0f / 8388608.0f; // 2^-23
    const float a = static_cast<float>(MyeFeelHash32(seed ^ MyeFeelHash32(i)) >> 9) * kInv * 2.0f - 1.0f;
    const float b = static_cast<float>(MyeFeelHash32(seed ^ MyeFeelHash32(i + 1u)) >> 9) * kInv * 2.0f - 1.0f;
    return a + (b - a) * s;
}

// ---- 手触りの記述子 ----
// 1 回の「手触り」(ヒット、着地、被弾…) が鳴らすものの全部。.feel.json の 1 ファイル =
// この構造体 1 個。スクリプトからは PlayFeel (名前) か PlayFeelDesc (コードで組んだもの) で鳴らす。
// 全メンバ 4 バイトで詰め物が無い (発火時にコピーされ、sim 状態としてバイト列のまま
// ハッシュ・スナップショットされる)。各チャネルは *Ticks == 0 で無効。
//
// 時間 (hitStop / slow) は sim を止める・遅くする = シミュレーション結果に効く。
// 揺れ・キック・振動・画面効果は描画専用で、状態と経過 tick の純関数として毎フレーム評価される。
// intensity (発火時の強さ) は描画専用チャネルの振幅だけに掛かる (時間には掛からない)
struct MyeFeelDesc {
    // ---- 時間 (sim) ----
    int32_t hitStopTicks = 0;        // ゲート対象 (物理/アニメ/衝突/粒子/AI) を止める tick 数
    int32_t slowPercent = 100;       // スローの速さ [0,100]。100 = スロー無し
    int32_t slowHoldTicks = 0;       // slowPercent を保つ tick 数 (ヒットストップの後から数える)
    int32_t slowRecoverTicks = 0;    // 100% へ戻るまでの tick 数 (0 = 即座に戻る)
    int32_t slowRecoverEase = MYE_EASE_OUT_QUAD; // 戻り方
    // ---- カメラの揺れ (描画) ----
    int32_t shakeTicks = 0;
    float shakePosAmp = 0.0f;        // 位置の振幅 [m] (カメラ局所の xyz)
    float shakeRotAmpDeg = 0.0f;     // 回転の振幅 [度] (pitch / yaw / roll)
    float shakeFrequencyHz = 20.0f;  // ノイズの速さ
    MyeCurve shakeEnvelope = MyeMakeCurve(MYE_EASE_OUT_QUAD, 1.0f, 0.0f); // 振幅の時間変化
    // ---- カメラのキック (描画): 一方向へ押してから戻る ----
    int32_t kickTicks = 0;
    MyeVec3 kickOffset = {};         // カメラ局所の押し出し [m] (z+ = 前)
    float kickPitchDeg = 0.0f;       // 跳ね上げ [度] (正 = 上を向く)
    float kickFovDeg = 0.0f;         // 画角の差分 [度] (正 = 広がる)
    MyeCurve kickCurve = MyeMakeCurve(MYE_EASE_OUT_CUBIC, 1.0f, 0.0f);
    // ---- パッドの振動 (描画 = 出力レーン) ----
    int32_t rumbleTicks = 0;
    float rumbleLow = 0.0f;          // 低周波モーター [0,1]
    float rumbleHigh = 0.0f;         // 高周波モーター [0,1]
    MyeCurve rumbleCurve = MyeMakeCurve(MYE_EASE_OUT_QUAD, 1.0f, 0.0f);
    // ---- 画面効果のパルス (描画) ----
    int32_t flashTicks = 0;
    float flashVignette = 0.0f;      // 周辺減光の加算
    float flashChromAberration = 0.0f; // 色収差の加算
    float flashExposure = 0.0f;      // 露出倍率の加算 (0.5 = 1.5 倍)
    float flashSaturation = 0.0f;    // 彩度の加算 (-1 = 白黒)
    MyeColor flashColor = { 1.0f, 1.0f, 1.0f, 0.0f }; // 乗算フィルタへ寄せる色、a = 寄せる重み [0,1]
    MyeCurve flashCurve = MyeMakeCurve(MYE_EASE_OUT_QUAD, 1.0f, 0.0f);
};
static_assert(sizeof(MyeFeelDesc) % 4 == 0, "MyeFeelDesc must stay 4-byte packed");
