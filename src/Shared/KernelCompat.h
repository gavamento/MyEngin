//====================================================================================
//                          KernelCompat.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach カーネル (*.kernel.hlsl) の C++ 側 HLSL 互換層
//====================================================================================
#pragma once
// *.kernel.hlsl を C++ としてコンパイルするための HLSL 互換層 (header-only、D3D 非依存)。
//
// 使い方: 正本は assets/shaders/kernels/kernel_common.hlsli。カーネルを取り込む TU は
//   #include "Shared/KernelCompat.h"
//   namespace mye::kernel::cpu::<Name> { #include ".../<Name>.kernel.hlsl" }
// と書く。★名前空間を mye::kernel の内側にするのは、unqualified な sin / sqrt / abs などを
// CRT の同名関数より先にここの定義へ見つけさせるため (外側だと float 同士で曖昧になる)。
//
// 決定論: 要素ごとの演算列は「左から右のスカラー式」で固定し、/fp:precise の下で
// Debug/Release・ジョブ数に依らずビット一致させる。C# 版 (変換器) も同じ式にそろえること。
// 式の一覧: docs/adr 下書き (plans/m91-compute-foreach/sub-01.md 実装メモ)。

#include <bit>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "Shared/ScriptAPI.h" // MyeSinRad / MyeCosRad (★1 命令も変えない関数。ここで式を複製しない)

#define MYE_KERNEL_COMPAT_INCLUDED 1

namespace mye::kernel {

using uint = std::uint32_t;

// ---- ベクトル型 (成分 .x .y .z .w のみ。スウィズルは持たない) --------------------------

template <class T> struct Vec2 {
    using Scalar = T;
    static constexpr int kLanes = 2;
    T x, y;
    constexpr Vec2() = default; // 未初期化 (HLSL のローカル変数と同じ)。T{} は 0 になる
    constexpr Vec2(T s) : x(s), y(s) {}
    constexpr Vec2(T x_, T y_) : x(x_), y(y_) {}
    template <class U> constexpr explicit Vec2(const Vec2<U>& o)
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)) {}
};

template <class T> struct Vec3 {
    using Scalar = T;
    static constexpr int kLanes = 3;
    T x, y, z;
    constexpr Vec3() = default;
    constexpr Vec3(T s) : x(s), y(s), z(s) {}
    constexpr Vec3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3(const Vec2<T>& xy, T z_) : x(xy.x), y(xy.y), z(z_) {}
    constexpr Vec3(T x_, const Vec2<T>& yz) : x(x_), y(yz.x), z(yz.y) {}
    template <class U> constexpr explicit Vec3(const Vec3<U>& o)
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)) {}
};

template <class T> struct Vec4 {
    using Scalar = T;
    static constexpr int kLanes = 4;
    T x, y, z, w;
    constexpr Vec4() = default;
    constexpr Vec4(T s) : x(s), y(s), z(s), w(s) {}
    constexpr Vec4(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3<T>& xyz, T w_) : x(xyz.x), y(xyz.y), z(xyz.z), w(w_) {}
    constexpr Vec4(T x_, const Vec3<T>& yzw) : x(x_), y(yzw.x), z(yzw.y), w(yzw.z) {}
    constexpr Vec4(const Vec2<T>& xy, const Vec2<T>& zw) : x(xy.x), y(xy.y), z(zw.x), w(zw.y) {}
    template <class U> constexpr explicit Vec4(const Vec4<U>& o)
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)),
          w(static_cast<T>(o.w)) {}
};

using float2 = Vec2<float>;
using float3 = Vec3<float>;
using float4 = Vec4<float>;
using int2 = Vec2<int>;
using int3 = Vec3<int>;
using int4 = Vec4<int>;
using uint2 = Vec2<uint>;
using uint3 = Vec3<uint>;
using uint4 = Vec4<uint>;

template <class V>
concept Vector = requires {
    typename V::Scalar;
    V::kLanes;
};

namespace detail {

template <class V, class S> struct Rebind;
template <class T, class S> struct Rebind<Vec2<T>, S> { using type = Vec2<S>; };
template <class T, class S> struct Rebind<Vec3<T>, S> { using type = Vec3<S>; };
template <class T, class S> struct Rebind<Vec4<T>, S> { using type = Vec4<S>; };

// 成分ごとに f を適用する。結果の成分型は f の戻り値型 (sign が int を返すため)
template <Vector V, class F> constexpr auto Map(const V& a, F f)
{
    using R = typename Rebind<V, decltype(f(a.x))>::type;
    if constexpr (V::kLanes == 2) {
        return R(f(a.x), f(a.y));
    } else if constexpr (V::kLanes == 3) {
        return R(f(a.x), f(a.y), f(a.z));
    } else {
        return R(f(a.x), f(a.y), f(a.z), f(a.w));
    }
}

template <Vector V, class F> constexpr V Zip(const V& a, const V& b, F f)
{
    if constexpr (V::kLanes == 2) {
        return V(f(a.x, b.x), f(a.y, b.y));
    } else if constexpr (V::kLanes == 3) {
        return V(f(a.x, b.x), f(a.y, b.y), f(a.z, b.z));
    } else {
        return V(f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w));
    }
}

template <Vector V, class F> constexpr V Zip3(const V& a, const V& b, const V& c, F f)
{
    if constexpr (V::kLanes == 2) {
        return V(f(a.x, b.x, c.x), f(a.y, b.y, c.y));
    } else if constexpr (V::kLanes == 3) {
        return V(f(a.x, b.x, c.x), f(a.y, b.y, c.y), f(a.z, b.z, c.z));
    } else {
        return V(f(a.x, b.x, c.x), f(a.y, b.y, c.y), f(a.z, b.z, c.z), f(a.w, b.w, c.w));
    }
}

} // namespace detail

// ---- 演算子 (成分ごと。スカラーとの混在可。比較は定義しない = ベクトル同士の比較は禁止) --------

#define MYE_KERNEL_VEC_OP(op)                                                                   \
    template <Vector V> constexpr V operator op(const V& a, const V& b)                         \
    {                                                                                           \
        return detail::Zip(a, b, [](auto p, auto q) { return static_cast<typename V::Scalar>(p op q); }); \
    }                                                                                           \
    template <Vector V> constexpr V operator op(const V& a, std::type_identity_t<typename V::Scalar> s) \
    {                                                                                           \
        return a op V(s);                                                                       \
    }                                                                                           \
    template <Vector V> constexpr V operator op(std::type_identity_t<typename V::Scalar> s, const V& b) \
    {                                                                                           \
        return V(s) op b;                                                                       \
    }                                                                                           \
    template <Vector V> constexpr V& operator op##=(V& a, const V& b)                           \
    {                                                                                           \
        a = a op b;                                                                             \
        return a;                                                                               \
    }                                                                                           \
    template <Vector V> constexpr V& operator op##=(V& a, std::type_identity_t<typename V::Scalar> s) \
    {                                                                                           \
        a = a op V(s);                                                                          \
        return a;                                                                               \
    }

MYE_KERNEL_VEC_OP(+)
MYE_KERNEL_VEC_OP(-)
MYE_KERNEL_VEC_OP(*)
MYE_KERNEL_VEC_OP(/)
MYE_KERNEL_VEC_OP(%)
MYE_KERNEL_VEC_OP(&)
MYE_KERNEL_VEC_OP(|)
MYE_KERNEL_VEC_OP(^)
MYE_KERNEL_VEC_OP(<<)
MYE_KERNEL_VEC_OP(>>)

#undef MYE_KERNEL_VEC_OP

template <Vector V> constexpr V operator-(const V& a)
{
    return detail::Map(a, [](auto p) { return static_cast<typename V::Scalar>(-p); });
}

// ---- 組込み関数 (ホワイトリスト。spec 4.1.2) ----------------------------------------

inline constexpr float kHalfPi = kMyePi * 0.5f;
inline constexpr float kTwoPi = kMyePi * 2.0f;
inline constexpr float kInvTwoPi = 1.0f / kTwoPi;

// ---- スカラー ----

template <class T> requires std::is_arithmetic_v<T> constexpr T min(T a, T b) { return b < a ? b : a; }
template <class T> requires std::is_arithmetic_v<T> constexpr T max(T a, T b) { return a < b ? b : a; }
template <class T> requires std::is_arithmetic_v<T> constexpr T clamp(T x, T lo, T hi)
{
    return min(max(x, lo), hi);
}

// 符号ビットを落とす (GPU の abs と同じく -0.0f も +0.0f になる)
inline float abs(float x) { return std::bit_cast<float>(std::bit_cast<std::uint32_t>(x) & 0x7fffffffu); }
constexpr int abs(int x) { return x < 0 ? -x : x; }

inline float floor(float x) { return std::floor(x); }
inline float ceil(float x) { return std::ceil(x); }
inline float frac(float x) { return x - std::floor(x); }
inline float sqrt(float x) { return std::sqrt(x); } // IEEE 単精度の正しい丸め
// GPU では近似。CPU では厳密な除算 (spec 4.1.2)
inline float rsqrt(float x) { return 1.0f / std::sqrt(x); }
constexpr float rcp(float x) { return 1.0f / x; }
constexpr float saturate(float x) { return min(max(x, 0.0f), 1.0f); }
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float step(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }
constexpr float smoothstep(float lo, float hi, float x)
{
    const float t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}
// HLSL の sign は int を返す
constexpr int sign(float x) { return x > 0.0f ? 1 : (x < 0.0f ? -1 : 0); }
constexpr int sign(int x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }

// sin / cos は MyeSinRad / MyeCosRad (前提 |x| <= 3pi/2、cos は x in [-pi, pi]) の範囲内ならそのまま。
// 範囲外だけ x - 2pi * floor(x / 2pi + 0.5) で [-pi, pi] へ寄せる (GPU は任意の x を受けられるため)
inline float sin(float x)
{
    if (x > kMyePi * 1.5f || x < -kMyePi * 1.5f) {
        x = x - kTwoPi * std::floor(x * kInvTwoPi + 0.5f);
    }
    return MyeSinRad(x);
}
inline float cos(float x)
{
    if (x > kMyePi || x < -kMyePi) {
        x = x - kTwoPi * std::floor(x * kInvTwoPi + 0.5f);
    }
    return MyeCosRad(x);
}

// ---- ベクトル (成分ごと) ----

template <Vector V> constexpr V abs(const V& a)
{
    return detail::Map(a, [](auto p) { return abs(p); });
}
template <Vector V> V floor(const V& a) { return detail::Map(a, [](auto p) { return floor(p); }); }
template <Vector V> V ceil(const V& a) { return detail::Map(a, [](auto p) { return ceil(p); }); }
template <Vector V> V frac(const V& a) { return detail::Map(a, [](auto p) { return frac(p); }); }
template <Vector V> V sqrt(const V& a) { return detail::Map(a, [](auto p) { return sqrt(p); }); }
template <Vector V> V rsqrt(const V& a) { return detail::Map(a, [](auto p) { return rsqrt(p); }); }
template <Vector V> constexpr V rcp(const V& a) { return detail::Map(a, [](auto p) { return rcp(p); }); }
template <Vector V> constexpr V saturate(const V& a)
{
    return detail::Map(a, [](auto p) { return saturate(p); });
}
template <Vector V> V sin(const V& a) { return detail::Map(a, [](auto p) { return sin(p); }); }
template <Vector V> V cos(const V& a) { return detail::Map(a, [](auto p) { return cos(p); }); }
template <Vector V> constexpr auto sign(const V& a)
{
    return detail::Map(a, [](auto p) { return sign(p); });
}

template <Vector V> constexpr V min(const V& a, const V& b)
{
    return detail::Zip(a, b, [](auto p, auto q) { return min(p, q); });
}
template <Vector V> constexpr V max(const V& a, const V& b)
{
    return detail::Zip(a, b, [](auto p, auto q) { return max(p, q); });
}
template <Vector V> constexpr V min(const V& a, std::type_identity_t<typename V::Scalar> s) { return min(a, V(s)); }
template <Vector V> constexpr V max(const V& a, std::type_identity_t<typename V::Scalar> s) { return max(a, V(s)); }
template <Vector V> constexpr V clamp(const V& x, const V& lo, const V& hi) { return min(max(x, lo), hi); }
template <Vector V> constexpr V clamp(const V& x, std::type_identity_t<typename V::Scalar> lo,
                                      std::type_identity_t<typename V::Scalar> hi)
{
    return min(max(x, V(lo)), V(hi));
}
template <Vector V> constexpr V lerp(const V& a, const V& b, const V& t) { return a + (b - a) * t; }
template <Vector V> constexpr V lerp(const V& a, const V& b, std::type_identity_t<typename V::Scalar> t)
{
    return a + (b - a) * t;
}
template <Vector V> constexpr V step(const V& edge, const V& x)
{
    return detail::Zip(edge, x, [](auto e, auto p) { return step(e, p); });
}
template <Vector V> constexpr V step(std::type_identity_t<typename V::Scalar> edge, const V& x)
{
    return step(V(edge), x);
}
template <Vector V> constexpr V smoothstep(const V& lo, const V& hi, const V& x)
{
    return detail::Zip3(lo, hi, x, [](auto l, auto h, auto p) { return smoothstep(l, h, p); });
}
template <Vector V> constexpr V smoothstep(std::type_identity_t<typename V::Scalar> lo,
                                           std::type_identity_t<typename V::Scalar> hi, const V& x)
{
    return smoothstep(V(lo), V(hi), x);
}

// 内積。成分の積を x, y, z, w の順に左から足す ((x*x' + y*y') + z*z') + w*w')
template <Vector V> constexpr typename V::Scalar dot(const V& a, const V& b)
{
    if constexpr (V::kLanes == 2) {
        return a.x * b.x + a.y * b.y;
    } else if constexpr (V::kLanes == 3) {
        return (a.x * b.x + a.y * b.y) + a.z * b.z;
    } else {
        return ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
    }
}
constexpr float3 cross(const float3& a, const float3& b)
{
    return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
template <Vector V> float length(const V& v) { return sqrt(dot(v, v)); }
template <Vector V> float distance(const V& a, const V& b) { return length(a - b); }
// v * rsqrt(dot(v, v))。長さ 0 は GPU と同じく NaN になる (書き手が避ける)
template <Vector V> V normalize(const V& v) { return v * rsqrt(dot(v, v)); }

// ---- 乱数代わりの整数ハッシュ (PCG の出力関数)。kernel_common.hlsli にも同じ式がある ----

constexpr uint HashU32(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
// 上位 24 bit を [0, 1) へ。float で厳密に表せるので CPU/GPU でビット一致する
constexpr float HashToUnitFloat(uint h) { return static_cast<float>(h >> 8u) * (1.0f / 16777216.0f); }

// ---- バッファのビュー ----------------------------------------------------------------

struct KernelLauncher; // 実行器 (Engine/Compute/KernelRunner.h) だけが KernelId を作れる

// 要素番号。RW の添字にできるのはこの型だけで、id + 1 のような計算結果 (uint) は添字にできない。
// 読み取りの用途 (乱数の種、float への変換) には uint として使える
class KernelId {
public:
    constexpr operator uint() const { return value_; }

private:
    friend struct KernelLauncher;
    constexpr explicit KernelId(uint v) : value_(v) {}
    uint value_;
};

// RWStructuredBuffer<T>。自分の要素 (KernelId) だけを書く約束 (ADR-028)
template <class T> class RWView {
public:
    constexpr RWView() = default;
    constexpr RWView(T* data, uint count) : data_(data), count_(count) {}
    constexpr T& operator[](KernelId id) const { return data_[static_cast<uint>(id)]; }
    template <class I> T& operator[](I) const = delete; // KernelId 以外の添字はコンパイルエラー
    constexpr void GetDimensions(uint& count, uint& stride) const
    {
        count = count_;
        stride = static_cast<uint>(sizeof(T));
    }
    constexpr uint Count() const { return count_; }
    constexpr T* Data() const { return data_; }

private:
    T* data_ = nullptr;
    uint count_ = 0;
};

// StructuredBuffer<T>。範囲外の読み取りは 0 初期化の T (D3D11 と同じ)
template <class T> class ROView {
public:
    constexpr ROView() = default;
    constexpr ROView(const T* data, uint count) : data_(data), count_(count) {}
    constexpr T operator[](uint i) const { return i < count_ ? data_[i] : T{}; }
    constexpr void GetDimensions(uint& count, uint& stride) const
    {
        count = count_;
        stride = static_cast<uint>(sizeof(T));
    }
    constexpr uint Count() const { return count_; }

private:
    const T* data_ = nullptr;
    uint count_ = 0;
};

} // namespace mye::kernel
