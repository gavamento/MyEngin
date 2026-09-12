#pragma once
// GameLogic.dll 側のスクリプト定義 API (engine_spec.md 5.2)。
//
//   struct PlayerController : Script<PlayerController> {
//       float moveSpeed = 5.0f;   // 状態はエンジン側 ECS に置かれる (リフレクション登録)
//       int32_t jumpCount = 0;
//       void Update(MyeUpdateContext& ctx) { ... }
//   };
//   REGISTER_SCRIPT(PlayerController, FIELDS(moveSpeed, jumpCount));
//
// 規則:
//   - 状態は trivially copyable であること (コンパイル時に強制される)
//   - 登録フィールドのみ DLL リロードを跨いで保存される。global/static 変数の永続は保証しない
//   - Start / Update / LateUpdate は任意 (定義したものだけ呼ばれる)

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <type_traits>
#include <vector>

#include "Shared/ScriptTypes.h"

// ---- 型 → MyeFieldType 変換 ----
template <typename T> struct MyeTypeOf;
template <> struct MyeTypeOf<float>       { static constexpr int32_t value = MYE_FIELD_FLOAT; };
template <> struct MyeTypeOf<int32_t>     { static constexpr int32_t value = MYE_FIELD_INT32; };
template <> struct MyeTypeOf<uint32_t>    { static constexpr int32_t value = MYE_FIELD_UINT32; };
template <> struct MyeTypeOf<uint64_t>    { static constexpr int32_t value = MYE_FIELD_UINT64; };
template <> struct MyeTypeOf<bool>        { static constexpr int32_t value = MYE_FIELD_BOOL; };
template <> struct MyeTypeOf<MyeVec2>     { static constexpr int32_t value = MYE_FIELD_FLOAT2; };
template <> struct MyeTypeOf<MyeVec3>     { static constexpr int32_t value = MYE_FIELD_FLOAT3; };
template <> struct MyeTypeOf<MyeVec4>     { static constexpr int32_t value = MYE_FIELD_FLOAT4; };
template <> struct MyeTypeOf<MyeQuat>     { static constexpr int32_t value = MYE_FIELD_QUAT; };
template <> struct MyeTypeOf<MyeColor>    { static constexpr int32_t value = MYE_FIELD_COLOR; };
template <> struct MyeTypeOf<MyeEntityId> { static constexpr int32_t value = MYE_FIELD_ENTITYREF; };

// ---- CRTP 基底 (マーカー) ----
template <typename T>
struct Script {
};

namespace mye_script_detail {

inline std::vector<MyeScriptDesc>& Registry()
{
    static std::vector<MyeScriptDesc> registry;
    return registry;
}

template <typename T>
void (*GetStartFn())(void*, MyeUpdateContext*)
{
    if constexpr (requires(T t, MyeUpdateContext& c) { t.Start(c); }) {
        return [](void* s, MyeUpdateContext* c) { static_cast<T*>(s)->Start(*c); };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetUpdateFn())(void*, MyeUpdateContext*)
{
    if constexpr (requires(T t, MyeUpdateContext& c) { t.Update(c); }) {
        return [](void* s, MyeUpdateContext* c) { static_cast<T*>(s)->Update(*c); };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetLateUpdateFn())(void*, MyeUpdateContext*)
{
    if constexpr (requires(T t, MyeUpdateContext& c) { t.LateUpdate(c); }) {
        return [](void* s, MyeUpdateContext* c) { static_cast<T*>(s)->LateUpdate(*c); };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetTriggerEnterFn())(void*, MyeUpdateContext*, MyeEntityId)
{
    if constexpr (requires(T t, MyeUpdateContext& c, MyeEntityId o) { t.OnTriggerEnter(c, o); }) {
        return [](void* s, MyeUpdateContext* c, MyeEntityId o) {
            static_cast<T*>(s)->OnTriggerEnter(*c, o);
        };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetTriggerExitFn())(void*, MyeUpdateContext*, MyeEntityId)
{
    if constexpr (requires(T t, MyeUpdateContext& c, MyeEntityId o) { t.OnTriggerExit(c, o); }) {
        return [](void* s, MyeUpdateContext* c, MyeEntityId o) {
            static_cast<T*>(s)->OnTriggerExit(*c, o);
        };
    } else {
        return nullptr;
    }
}

// ---- ソリッド衝突コールバック (v4 予約、M28c で配信開始) ----
// 使い方: void OnCollisionEnter(MyeUpdateContext& ctx, MyeEntityId other, MyeVec3 normal);
//         void OnCollisionStay(MyeUpdateContext& ctx, MyeEntityId other);
//         void OnCollisionExit(MyeUpdateContext& ctx, MyeEntityId other);

template <typename T>
void (*GetCollisionEnterFn())(void*, MyeUpdateContext*, MyeEntityId, MyeVec3)
{
    if constexpr (requires(T t, MyeUpdateContext& c, MyeEntityId o, MyeVec3 n) {
                      t.OnCollisionEnter(c, o, n);
                  }) {
        return [](void* s, MyeUpdateContext* c, MyeEntityId o, MyeVec3 n) {
            static_cast<T*>(s)->OnCollisionEnter(*c, o, n);
        };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetCollisionStayFn())(void*, MyeUpdateContext*, MyeEntityId)
{
    if constexpr (requires(T t, MyeUpdateContext& c, MyeEntityId o) { t.OnCollisionStay(c, o); }) {
        return [](void* s, MyeUpdateContext* c, MyeEntityId o) {
            static_cast<T*>(s)->OnCollisionStay(*c, o);
        };
    } else {
        return nullptr;
    }
}

template <typename T>
void (*GetCollisionExitFn())(void*, MyeUpdateContext*, MyeEntityId)
{
    if constexpr (requires(T t, MyeUpdateContext& c, MyeEntityId o) { t.OnCollisionExit(c, o); }) {
        return [](void* s, MyeUpdateContext* c, MyeEntityId o) {
            static_cast<T*>(s)->OnCollisionExit(*c, o);
        };
    } else {
        return nullptr;
    }
}

inline uint64_t LayoutHash(const MyeScriptField* fields, uint32_t count)
{
    // FNV-1a (Engine/Core/Hash.h と同じ定数 — Shared はエンジンヘッダを включできないため再掲)
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const void* data, size_t size) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    for (uint32_t i = 0; i < count; ++i) {
        for (const char* c = fields[i].name; *c; ++c) {
            mix(c, 1);
        }
        mix(&fields[i].type, sizeof(fields[i].type));
        mix(&fields[i].offset, sizeof(fields[i].offset));
    }
    return h;
}

template <typename T>
MyeScriptDesc MakeDesc(const char* name, const MyeScriptField* fields, uint32_t fieldCount)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "script state must be trivially copyable (POD fields only)");
    static_assert(alignof(T) <= 16, "script state alignment must be <= 16");
    MyeScriptDesc d = {};
    d.name = name;
    d.stateSize = sizeof(T);
    d.stateAlign = alignof(T);
    d.fields = fields;
    d.fieldCount = fieldCount;
    d.layoutHash = LayoutHash(fields, fieldCount);
    d.construct = [](void* dst) { new (dst) T(); };
    d.start = GetStartFn<T>();
    d.update = GetUpdateFn<T>();
    d.lateUpdate = GetLateUpdateFn<T>();
    d.onTriggerEnter = GetTriggerEnterFn<T>();
    d.onTriggerExit = GetTriggerExitFn<T>();
    d.onCollisionEnter = GetCollisionEnterFn<T>();
    d.onCollisionStay = GetCollisionStayFn<T>();
    d.onCollisionExit = GetCollisionExitFn<T>();
    return d;
}

struct Registrar {
    explicit Registrar(const MyeScriptDesc& d) { Registry().push_back(d); }
};

} // namespace mye_script_detail

// ---- フィールド列挙マクロ (最大 32 個。/Zc:preprocessor 必須) ----
//
// 素の名前と「メタデータ付き」を同じ FIELDS() に混ぜて書ける (M70d):
//
//     REGISTER_SCRIPT(PlayerController,
//         FIELDS(MYE_F_JP(moveSpeed, "移動速度"),                    // 表示名だけ
//                MYE_F_RANGE(jumpPower, "跳躍力", 0.0f, 20.0f),      // 表示名 + スライダ範囲
//                jumpCount));                                        // 従来どおりの素の名前
//
// 仕掛けは「メタデータ付きの項目だけ**括弧で包まれたタプル**へ展開する」こと。
// MYE_SF が括弧の有無で分岐する (MYE_SF_IS_PAREN) ので、**既存の書き方は 1 文字も
// 変えずに通る**。表示名に nullptr を渡せば「名前をそのまま出す」= 範囲だけの指定になる。
//
// ★メタデータは layoutHash に混ざらない (LayoutHash は name/type/offset だけ) ので、
//   表示名やスライダ範囲を書き換えても DLL リロードの状態移行は走らない = 調整中の値が飛ばない。
// ★エンジン側の受け取りは ScriptHost.cpp の FieldDescFromScriptField 1 本。
#define MYE_F_JP(m, jp) (m, jp, 0.0f, 0.0f)
#define MYE_F_RANGE(m, jp, lo, hi) (m, jp, lo, hi)

// 引数が括弧で包まれているかの判定 (プリプロセッサの定石)。
// `MYE_SF_PROBE x` は x が `(…)` のときだけ関数マクロとして展開されて `~, 1,` になり、
// そうでなければ 1 つのトークン列のまま残る — 2 番目の要素を拾えば 1 / 0 が得られる
#define MYE_SF_PROBE(...) ~, 1,
#define MYE_SF_PICK2(a, b, ...) b
#define MYE_SF_IS_PAREN_I(...) MYE_SF_PICK2(__VA_ARGS__)
#define MYE_SF_IS_PAREN(x) MYE_SF_IS_PAREN_I(MYE_SF_PROBE x, 0, )

#define MYE_SF_MAKE(T, m, jp, lo, hi)                                                            \
    { #m, MyeTypeOf<std::remove_cv_t<decltype(T::m)>>::value, (uint32_t)offsetof(T, m),          \
      jp, lo, hi },
// タプルの括弧を外して MYE_SF_MAKE へ渡す。**1 段の間接**が要る —
// MYE_SF_MAKE(T, MYE_SF_UNWRAP m) と直接書くと「引数 2 個」で解釈されて足りなくなる
#define MYE_SF_UNWRAP(...) __VA_ARGS__
#define MYE_SF_MAKE_I(...) MYE_SF_MAKE(__VA_ARGS__)
#define MYE_SF_ENTRY_0(T, m) MYE_SF_MAKE(T, m, nullptr, 0.0f, 0.0f)
#define MYE_SF_ENTRY_1(T, m) MYE_SF_MAKE_I(T, MYE_SF_UNWRAP m)
#define MYE_SF(T, m) MYE_SF_CAT(MYE_SF_ENTRY_, MYE_SF_IS_PAREN(m))(T, m)
#define MYE_SF_1(T, m) MYE_SF(T, m)
#define MYE_SF_2(T, m, ...) MYE_SF(T, m) MYE_SF_1(T, __VA_ARGS__)
#define MYE_SF_3(T, m, ...) MYE_SF(T, m) MYE_SF_2(T, __VA_ARGS__)
#define MYE_SF_4(T, m, ...) MYE_SF(T, m) MYE_SF_3(T, __VA_ARGS__)
#define MYE_SF_5(T, m, ...) MYE_SF(T, m) MYE_SF_4(T, __VA_ARGS__)
#define MYE_SF_6(T, m, ...) MYE_SF(T, m) MYE_SF_5(T, __VA_ARGS__)
#define MYE_SF_7(T, m, ...) MYE_SF(T, m) MYE_SF_6(T, __VA_ARGS__)
#define MYE_SF_8(T, m, ...) MYE_SF(T, m) MYE_SF_7(T, __VA_ARGS__)
#define MYE_SF_9(T, m, ...) MYE_SF(T, m) MYE_SF_8(T, __VA_ARGS__)
#define MYE_SF_10(T, m, ...) MYE_SF(T, m) MYE_SF_9(T, __VA_ARGS__)
#define MYE_SF_11(T, m, ...) MYE_SF(T, m) MYE_SF_10(T, __VA_ARGS__)
#define MYE_SF_12(T, m, ...) MYE_SF(T, m) MYE_SF_11(T, __VA_ARGS__)
#define MYE_SF_13(T, m, ...) MYE_SF(T, m) MYE_SF_12(T, __VA_ARGS__)
#define MYE_SF_14(T, m, ...) MYE_SF(T, m) MYE_SF_13(T, __VA_ARGS__)
#define MYE_SF_15(T, m, ...) MYE_SF(T, m) MYE_SF_14(T, __VA_ARGS__)
#define MYE_SF_16(T, m, ...) MYE_SF(T, m) MYE_SF_15(T, __VA_ARGS__)
// M70d: 16 → 32。16 が上限だったせいで AudioDemo が 9 個のキーのエッジ検出を
// int32_t のビットへ畳んでいた (= 上限が設計を歪めていた) ので倍にした
#define MYE_SF_17(T, m, ...) MYE_SF(T, m) MYE_SF_16(T, __VA_ARGS__)
#define MYE_SF_18(T, m, ...) MYE_SF(T, m) MYE_SF_17(T, __VA_ARGS__)
#define MYE_SF_19(T, m, ...) MYE_SF(T, m) MYE_SF_18(T, __VA_ARGS__)
#define MYE_SF_20(T, m, ...) MYE_SF(T, m) MYE_SF_19(T, __VA_ARGS__)
#define MYE_SF_21(T, m, ...) MYE_SF(T, m) MYE_SF_20(T, __VA_ARGS__)
#define MYE_SF_22(T, m, ...) MYE_SF(T, m) MYE_SF_21(T, __VA_ARGS__)
#define MYE_SF_23(T, m, ...) MYE_SF(T, m) MYE_SF_22(T, __VA_ARGS__)
#define MYE_SF_24(T, m, ...) MYE_SF(T, m) MYE_SF_23(T, __VA_ARGS__)
#define MYE_SF_25(T, m, ...) MYE_SF(T, m) MYE_SF_24(T, __VA_ARGS__)
#define MYE_SF_26(T, m, ...) MYE_SF(T, m) MYE_SF_25(T, __VA_ARGS__)
#define MYE_SF_27(T, m, ...) MYE_SF(T, m) MYE_SF_26(T, __VA_ARGS__)
#define MYE_SF_28(T, m, ...) MYE_SF(T, m) MYE_SF_27(T, __VA_ARGS__)
#define MYE_SF_29(T, m, ...) MYE_SF(T, m) MYE_SF_28(T, __VA_ARGS__)
#define MYE_SF_30(T, m, ...) MYE_SF(T, m) MYE_SF_29(T, __VA_ARGS__)
#define MYE_SF_31(T, m, ...) MYE_SF(T, m) MYE_SF_30(T, __VA_ARGS__)
#define MYE_SF_32(T, m, ...) MYE_SF(T, m) MYE_SF_31(T, __VA_ARGS__)
#define MYE_SF_NARGS(...)                                                                        \
    MYE_SF_NARGS_I(__VA_ARGS__, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,  \
                   16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define MYE_SF_NARGS_I(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16,    \
                       _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30,     \
                       _31, _32, N, ...)                                                         \
    N
#define MYE_SF_CAT(a, b) MYE_SF_CAT_I(a, b)
#define MYE_SF_CAT_I(a, b) a##b
#define MYE_SF_FOREACH(T, ...) MYE_SF_CAT(MYE_SF_, MYE_SF_NARGS(__VA_ARGS__))(T, __VA_ARGS__)

#define FIELDS(...) __VA_ARGS__

#define REGISTER_SCRIPT(T, ...)                                                                  \
    static const MyeScriptField T##_mye_fields[] = { MYE_SF_FOREACH(T, __VA_ARGS__) };           \
    static const ::mye_script_detail::Registrar T##_mye_registrar(                               \
        ::mye_script_detail::MakeDesc<T>(#T, T##_mye_fields,                                     \
                                         (uint32_t)(sizeof(T##_mye_fields) / sizeof(MyeScriptField))))

#define REGISTER_SCRIPT_NO_FIELDS(T)                                                             \
    static const ::mye_script_detail::Registrar T##_mye_registrar(                               \
        ::mye_script_detail::MakeDesc<T>(#T, nullptr, 0))

// ---- スクリプト用ユーティリティ (DLL 内で完結。境界は越えない) ----

// 名前 → ハッシュ。FNV-1a 64bit で **Engine/Core/Hash.h の HashStr と同一の定数**
// (MyePartTag と同じ再掲。一致は SchemaSelfTest が機械検査している)。
// ★M70d でこの節の先頭へ移動 (MyeGameObject が WorldMatrix を引くのに使うため)
inline constexpr uint64_t MyeNameHash(const char* name)
{
    uint64_t h = 14695981039346656037ull;
    if (name != nullptr) {
        for (const char* c = name; *c != '\0'; ++c) {
            h ^= static_cast<unsigned char>(*c);
            h *= 1099511628211ull;
        }
    }
    return h;
}

inline void MyeLogf(const MyeUpdateContext& ctx, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ctx.api->Log(ctx.api->engine, MYE_LOG_LEVEL_INFO, buf);
}

// Unity 風の薄いラッパ
struct MyeGameObject {
    MyeEntityId id = {};
    const MyeEngineApi* api = nullptr;

    explicit operator bool() const { return api && api->IsAlive(api->engine, id) != 0; }

    MyeVec3 GetLocalPosition() const
    {
        MyeVec3 v;
        api->GetLocalPosition(api->engine, id, &v);
        return v;
    }
    void SetLocalPosition(MyeVec3 v) const { api->SetLocalPosition(api->engine, id, v); }
    void SetLocalRotation(MyeQuat q) const { api->SetLocalRotation(api->engine, id, q); }
    // M70d (dogfooding #18): 回転とスケールは**書けるのに読めない**非対称だった。
    // ABI スロットは v1 から 6 本とも埋まっているので、足りなかったのは糖衣だけ
    MyeQuat GetLocalRotation() const
    {
        MyeQuat q;
        api->GetLocalRotation(api->engine, id, &q);
        return q;
    }
    MyeVec3 GetLocalScale() const
    {
        MyeVec3 v;
        api->GetLocalScale(api->engine, id, &v);
        return v;
    }
    void SetLocalScale(MyeVec3 v) const { api->SetLocalScale(api->engine, id, v); }

    // ワールド位置 (M70d)。親を持つエンティティで「自分が実際に居る場所」を知る唯一の口。
    //
    // ★親が無ければ**ローカル位置をそのまま返す** (定義上つねに同値)。これは速さのためでは
    //   なく正しさのため — WorldMatrix は生成時から単位行列で存在するので、
    //   「まだ TransformSystem が回っていない」と「本当に原点に居る」を行列からは
    //   区別できない。親なしだけでも常に厳密な答えを返せるようにしておくと、
    //   Start から呼んでも黙って原点にならない (足音の鳴る場所がここに乗っている)。
    // ★親があるときは WorldMatrix を読む。**Start (フェーズ 3) は TransformSystem
    //   (フェーズ 4) より前**なので、シーンを読み込んだ最初の tick では原点が返る。
    //   位置に依存する処理は Update に置いて 2 tick 目以降で走らせること
    //   (dogfooding #3 と EngineAPI.h の空間クエリ節と同じ罠)。
    MyeVec3 GetWorldPosition() const
    {
        MyeEntityId parent = {};
        const int32_t gotParent =
            api->GetComponentField(api->engine, id, MyeNameHash("Hierarchy"),
                                   MyeNameHash("parent"), &parent, (int32_t)sizeof(parent),
                                   nullptr);
        if (gotParent != (int32_t)sizeof(parent) || MyeEntityIdIsNull(parent)) {
            return GetLocalPosition();
        }
        // WorldMatrix.value は Float4x4 (行優先)。平行移動は 4 行目 = 添字 12/13/14
        float m[16] = {};
        const int32_t got = api->GetComponentField(api->engine, id, MyeNameHash("WorldMatrix"),
                                                   MyeNameHash("value"), m, (int32_t)sizeof(m),
                                                   nullptr);
        if (got != (int32_t)sizeof(m)) {
            return GetLocalPosition();
        }
        return MyeVec3{ m[12], m[13], m[14] };
    }
    void Destroy() const { api->DestroyGameObject(api->engine, id); }
};

inline MyeGameObject MyeSelf(const MyeUpdateContext& ctx)
{
    return { ctx.self, ctx.api };
}

// ---- 角度まわり (M70d、dogfooding #7)。ABI 追加なし = ヘッダ内で完結 ----
//
// `Shared/` は DirectXMath を持ち込めない (DLL 境界規則) ので、これが無いと
// ゲーム側が XMQuaternionRotationRollPitchYaw 相当を手で書くことになる。
// 式を間違えるとカメラだけが静かに壊れる、という一番気づけない形で出る。
//
// ★**CRT の sinf / cosf は使わない**。`Physics\AeroSampling.cpp` の注記が正本で、
//   「std::sin / std::cos は CRT 実装依存でビットが動きうる」。ここで作った回転は
//   ハッシュ対象のフィールドへそのまま入るので、CRT 依存を挟むと
//   「別の Windows で .rep が再生できない」種類の壊れ方になる。乗算と加算だけの
//   多項式なら /fp:precise の下でどのビルドでも厳密に同じビット列になる。
constexpr float kMyePi = 3.14159265358979f;
constexpr float kMyeDeg2Rad = kMyePi / 180.0f;

// sin(x)。**前提: |x| <= 3pi/2** (sin(x)=sin(pi-x) の対称性で [-pi/2, pi/2] へ 1 回だけ
// 折り返し、9 次のテイラーで評価する。この区間の誤差は 1e-9 未満)。
// ★実装は WatcherFpsCamera (M65g) が持っていたものを**1 命令も変えずに**引き上げた —
//   変えると同スクリプトの視点角が動いて replay 7 ペア目が割れる
inline float MyeSinRad(float x)
{
    if (x > kMyePi * 0.5f) {
        x = kMyePi - x;
    } else if (x < -kMyePi * 0.5f) {
        x = -kMyePi - x;
    }
    const float x2 = x * x;
    return x
        * (1.0f
           + x2
               * (-1.0f / 6.0f
                  + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}
inline float MyeCosRad(float x) { return MyeSinRad(x + kMyePi * 0.5f); }

// 角度を [-180, 180] へ折り返す (加減算だけ = 決定論)。
// MyeSinRad の前提 |x| <= 3pi/2 を満たすために MyeQuatFromEuler が必ず通す
inline float MyeWrapDeg(float deg)
{
    while (deg > 180.0f) {
        deg -= 360.0f;
    }
    while (deg < -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

// オイラー角 (度) → 四元数。**エンジンの GameObject::SetLocalRotationEuler
// (= XMQuaternionRotationRollPitchYaw) と同じ規約** — 適用順は roll(Z) → pitch(X) → yaw(Y)。
// 一致は SchemaSelfTest が DirectXMath と照合して機械検査している
inline MyeQuat MyeQuatFromEuler(float pitchDeg, float yawDeg, float rollDeg = 0.0f)
{
    const float hp = MyeWrapDeg(pitchDeg) * kMyeDeg2Rad * 0.5f;
    const float hy = MyeWrapDeg(yawDeg) * kMyeDeg2Rad * 0.5f;
    const float hr = MyeWrapDeg(rollDeg) * kMyeDeg2Rad * 0.5f;
    const float sp = MyeSinRad(hp), cp = MyeCosRad(hp);
    const float sy = MyeSinRad(hy), cy = MyeCosRad(hy);
    const float sr = MyeSinRad(hr), cr = MyeCosRad(hr);
    return MyeQuat{ sp * cy * cr + cp * sy * sr, cp * sy * cr - sp * cy * sr,
                    cp * cy * sr - sp * sy * cr, cp * cy * cr + sp * sy * sr };
}

// 四元数の前方向 (+Z を回した結果)。「向いている方へ進む / 撃つ」の唯一の導き方。
// 正規化された四元数を前提にする (SetLocalRotation に入れる値は常にそう)
inline MyeVec3 MyeForwardOf(MyeQuat q)
{
    return MyeVec3{ 2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.y * q.z - q.w * q.x),
                    1.0f - 2.0f * (q.x * q.x + q.y * q.y) };
}

// ---- オーディオ (v8、M45g)。ABI 追加なしの糖衣 (呼び先は EngineAPI.h のスロットそのもの) ----
// key は .sound.json の名前 (無ければ .wav / .ogg のファイル名 stem)。
// **write-only** — 再生位置や再生中判定を取る手段は意図的に存在しない (EngineAPI.h 参照)
inline uint64_t MyePlaySound(const MyeUpdateContext& ctx, const char* key, float volume = 1.0f,
                             float pitch = 1.0f)
{
    return ctx.api->PlaySound2(ctx.api->engine, key, volume, pitch);
}

inline uint64_t MyePlaySoundAt(const MyeUpdateContext& ctx, const char* key, MyeVec3 worldPos,
                               float volume = 1.0f)
{
    return ctx.api->PlaySoundAt(ctx.api->engine, key, worldPos, volume);
}

// 自分の位置で 3D 再生する (足音・衝突音など)。
// ★M70d で実バグを修正: v8 から **ローカル位置をワールド位置として**渡していたので、
//   親を持つエンティティ (車輪・手に持った物・キャラの子ボーン) では鳴る場所がずれていた。
//   ずれは「親のワールド位置ぶん」なので、原点付近の親では気づけない
inline uint64_t MyePlaySoundHere(const MyeUpdateContext& ctx, const char* key, float volume = 1.0f)
{
    return MyePlaySoundAt(ctx, key, MyeSelf(ctx).GetWorldPosition(), volume);
}

inline void MyeStopVoice(const MyeUpdateContext& ctx, uint64_t handle, float fadeSeconds = 0.0f)
{
    ctx.api->StopVoice(ctx.api->engine, handle, fadeSeconds);
}

inline void MyeSetBusVolume(const MyeUpdateContext& ctx, const char* busName, float volume)
{
    ctx.api->SetBusVolume(ctx.api->engine, busName, volume);
}

inline void MyePlayMusic(const MyeUpdateContext& ctx, const char* key, float fadeSeconds = 1.0f,
                         bool loop = true)
{
    ctx.api->PlayMusic(ctx.api->engine, key, fadeSeconds, loop ? 1 : 0);
}

inline void MyeStopMusic(const MyeUpdateContext& ctx, float fadeSeconds = 1.0f)
{
    ctx.api->StopMusic(ctx.api->engine, fadeSeconds);
}

// ---- 部位 (ソケット) (v9、M48h)。ABI 追加なしの糖衣 + タグ名ハッシュ ----
//
// 使い方 (`SetEffect(leg)` 相当 = アセットが公開した場所にランタイムが物を付ける):
//     const MyeEntityId hand = MyeFindPart(ctx, enemy, "Hips/HandR");
//     if (!MyeEntityIdIsNull(hand)) { ctx.api->Instantiate(ctx.api->engine, "fx_fire", {}, hand); }

// タグ名 → タグ ID。FNV-1a 64bit で、**Engine/Core/Hash.h の HashStr と同一の定数**。
// Shared はエンジンヘッダを include できないので再掲する (LayoutHash と同じ扱い)。
// 一致は PartSelfTest が MyePartTag == Parts::TagOf で機械検査している
inline constexpr uint64_t MyePartTag(const char* name)
{
    if (name == nullptr || *name == '\0') {
        return 0ull; // 無名タグ = 0 (Parts::TagOf("") と同じ)
    }
    uint64_t h = 14695981039346656037ull;
    for (const char* c = name; *c != '\0'; ++c) {
        h ^= static_cast<unsigned char>(*c);
        h *= 1099511628211ull;
    }
    return h;
}

// root から '/' 区切りの名前パスで部位を引く。見つからなければ null id
inline MyeEntityId MyeFindPart(const MyeUpdateContext& ctx, MyeEntityId root, const char* path)
{
    return ctx.api->FindPart(ctx.api->engine, root, path);
}

// 自分のサブツリーから引く省略形
inline MyeEntityId MyeFindPart(const MyeUpdateContext& ctx, const char* path)
{
    return ctx.api->FindPart(ctx.api->engine, ctx.self, path);
}

// タグ名一致の部位を DFS 順に集める。戻り値は **切り捨て前の総ヒット数**
// (書けた件数は min(戻り値, cap)。戻り値 > cap ならバッファが足りていない)
inline int32_t MyeFindPartsByTag(const MyeUpdateContext& ctx, MyeEntityId root, const char* tagName,
                                 MyeEntityId* out, int32_t cap)
{
    return ctx.api->FindPartsByTag(ctx.api->engine, root, MyePartTag(tagName), out, cap);
}

// タグ名一致の部位を 1 個だけ引く (最初のヒット)。無ければ null id
inline MyeEntityId MyeFindPartByTag(const MyeUpdateContext& ctx, MyeEntityId root,
                                    const char* tagName)
{
    MyeEntityId hit = {};
    MyeFindPartsByTag(ctx, root, tagName, &hit, 1);
    return hit; // ヒット 0 件なら既定値 = null id のまま
}

// child を root の部位へ取り付ける。取り付けは既存 SetParent (ABI 追加なし)。
// 部位が見つからなければ何もせず false — 「黙って原点に付く」より落ちる方を選ぶ
inline bool MyeAttachToPart(const MyeUpdateContext& ctx, MyeEntityId child, MyeEntityId root,
                            const char* path)
{
    const MyeEntityId part = MyeFindPart(ctx, root, path);
    if (MyeEntityIdIsNull(part)) {
        return false;
    }
    ctx.api->SetParent(ctx.api->engine, child, part);
    return true;
}

// 部位ボリューム (Part + PartBounds) へのレイキャスト (v10、M49)。部位ダメージ判定用。
// root null id = シーン全体、tagName null/空 = 全部位。dir は**正規化済み**であること。
// ヒットで true、outHit.entity が部位 (同距離は低 index が勝つ = 決定論)
inline bool MyeRaycastParts(const MyeUpdateContext& ctx, MyeEntityId root, const char* tagName,
                            MyeVec3 origin, MyeVec3 dir, float maxDist, MyeRaycastHit& outHit)
{
    return ctx.api->RaycastParts(ctx.api->engine, root, MyePartTag(tagName), origin, dir, maxDist,
                                 &outHit)
        != 0;
}

// ---- 汎用フィールドアクセス (v11、M50d)。スキーマ codegen の呼び先 ----
//
// コンポーネント名 / フィールド名の FNV-1a 64bit ハッシュで、任意の登録コンポーネント
// (組込み / スキーマ / C++ スクリプト) のフィールドを値コピーで読み書きする。
// 型付きの入口は生成ヘッダ (<project>\cache\Generated\SchemaComponents.gen.h) が
// スキーマごとに提供する — 手書きでここを直接呼ぶのは probe / 一時実験くらいのはず。
// ★C# スクリプト状態 (非決定論レーン) は読み書きとも 0 が返る (EngineAPI.h の契約)

// ★MyeNameHash の定義は「スクリプト用ユーティリティ」節の先頭へ移した (M70d) —
//   MyeGameObject::GetWorldPosition が WorldMatrix を汎用フィールドアクセスで読むため、
//   ここより前で必要になった

// 生スロットの糖衣。戻り値は Get = 実サイズ / Set = 1 (0 = 無し/不一致)
inline int32_t MyeGetComponentField(const MyeUpdateContext& ctx, MyeEntityId e, uint64_t comp,
                                    uint64_t field, void* buf, int32_t bufSize,
                                    int32_t* outType = nullptr)
{
    return ctx.api->GetComponentField(ctx.api->engine, e, comp, field, buf, bufSize, outType);
}

inline int32_t MyeSetComponentField(const MyeUpdateContext& ctx, MyeEntityId e, uint64_t comp,
                                    uint64_t field, const void* buf, int32_t size)
{
    return ctx.api->SetComponentField(ctx.api->engine, e, comp, field, buf, size);
}

// 型付き糖衣。T はフィールドと**同サイズ**の trivially copyable 型 (float / int32_t /
// MyeVec3 ...)。サイズ不一致はエンジン側が 0 で拒否する (型違いの静かな破壊を防ぐ)
template <typename T>
inline bool MyeGetField(const MyeUpdateContext& ctx, MyeEntityId e, uint64_t comp, uint64_t field,
                        T& out)
{
    static_assert(std::is_trivially_copyable_v<T>, "field value must be a POD");
    return MyeGetComponentField(ctx, e, comp, field, &out, static_cast<int32_t>(sizeof(T)))
        == static_cast<int32_t>(sizeof(T));
}

template <typename T>
inline bool MyeSetField(const MyeUpdateContext& ctx, MyeEntityId e, uint64_t comp, uint64_t field,
                        const T& v)
{
    static_assert(std::is_trivially_copyable_v<T>, "field value must be a POD");
    return MyeSetComponentField(ctx, e, comp, field, &v, static_cast<int32_t>(sizeof(T))) != 0;
}

// ---- ゲーム内 UI ヒットテスト (M21) ----
// UI 描画はエンジン (UIElementComponent) が行うが、ボタン操作は **決定論のため
// InputSnapshot のマウス経由** で判定する (ABI 追加なし = bump 不要)。verify では記録された
// マウスで再現されるため replay 一致。
//
// ★M70c で解消: 矩形は**キャンバス座標** (基準 1920x1080、UIElement の x/y/w/h と同じ
//   土俵) で、マウスも MouseCanvasPos 経由のキャンバス座標になった。
//   さらに「矩形を手書きしない」経路 (MyeUIClicked) が下に増えている — 新しく書くなら
//   そちらを使うこと。
// キャンバス座標のマウス位置 (v16、M70b/M70c)。UI の引数はすべてこの座標系
inline void MyeMouseCanvasPos(const MyeUpdateContext& ctx, float& outX, float& outY)
{
    ctx.api->MouseCanvasPos(ctx.api->engine, &outX, &outY);
}

inline bool MyeMouseInRect(const MyeUpdateContext& ctx, MyeUIRect r)
{
    float fx = 0.0f, fy = 0.0f;
    MyeMouseCanvasPos(ctx, fx, fy);
    return fx >= r.x && fx < r.x + r.w && fy >= r.y && fy < r.y + r.h;
}

// 左ボタンを rect 内で押した瞬間に true。prevDown は呼び出し側スクリプトがフィールドで
// 保持する (エッジ検出。登録フィールドなら DLL リロードを跨いで状態維持)。
// ★**新しく書くなら MyeUIClicked を使うこと** — こちらは矩形を手書きする形なので、
//   UIElement 側のレイアウトを変えると黙って食い違う (M70c で潰したのがまさにこれ)
inline bool MyeButtonClicked(const MyeUpdateContext& ctx, MyeUIRect r, int32_t& prevDown)
{
    const int down = ctx.api->MouseButton(ctx.api->engine, 0);
    const bool clicked = down && !prevDown && MyeMouseInRect(ctx, r);
    prevDown = down;
    return clicked;
}

// ---- v16 (M70c): エンジンが持つ UI の対話状態 ----
// 矩形はエンジンが解決し、判定もエンジンが tick 中 (スクリプト層より前) に済ませてある。
// スクリプトは結果を読むだけ = **矩形の二重管理が無くなる**。
enum MyeUIButtonBits : uint32_t {
    MyeUIButtonHovered = 1u << 0,
    MyeUIButtonPressed = 1u << 1,
    MyeUIButtonClicked = 1u << 2, // 1 tick だけ立つ (離した瞬間 / Submit した瞬間)
    MyeUIButtonFocused = 1u << 3,
};

inline uint32_t MyeUIButtonState(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return ctx.api->UIButtonState(ctx.api->engine, id);
}
inline bool MyeUIHovered(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return (MyeUIButtonState(ctx, id) & MyeUIButtonHovered) != 0;
}
inline bool MyeUIPressed(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return (MyeUIButtonState(ctx, id) & MyeUIButtonPressed) != 0;
}
// クリック (マウスで離した / フォーカス中に UINavSubmit)。**この tick だけ true**
inline bool MyeUIClicked(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return (MyeUIButtonState(ctx, id) & MyeUIButtonClicked) != 0;
}
inline bool MyeUIFocused(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return (MyeUIButtonState(ctx, id) & MyeUIButtonFocused) != 0;
}
inline MyeEntityId MyeUIGetFocused(const MyeUpdateContext& ctx)
{
    return ctx.api->UIGetFocused(ctx.api->engine);
}
// null id でフォーカスを外す。focusable でない要素は false (何も変わらない)
inline bool MyeUISetFocused(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return ctx.api->UISetFocused(ctx.api->engine, id) != 0;
}
// 解決済みのキャンバス矩形 (アンカー・親子 space 適用後)。UIElement 非所持は false
inline bool MyeGetUIRect(const MyeUpdateContext& ctx, MyeEntityId id, MyeUIRect& out)
{
    return ctx.api->GetUIRect(ctx.api->engine, id, &out) != 0;
}
// セーブスロットから PersistStore だけ読む (シーンは動かさない、dogfooding #16)
inline bool MyeLoadPersist(const MyeUpdateContext& ctx, int slot)
{
    return ctx.api->LoadPersist(ctx.api->engine, slot) != 0;
}

// ---- v17 (M71a): 現在のシーンの識別 ----
// 今ロードされているシーンの sceneName を buf へ取り出す。戻り値は NUL を除く実バイト数で、
// **cap が足りなくても実長を返す** (切り詰めを呼び側が判定できる)。
// ★分岐は MyeNameHash(buf) == MyeNameHash("Stage1") で書ける — MyeNameHash は
//   constexpr 関数だが実行時の文字列にもそのまま使える (新しいハッシュ関数は要らない)
inline int32_t MyeGetSceneName(const MyeUpdateContext& ctx, char* buf, int32_t cap)
{
    return ctx.api->GetSceneName(ctx.api->engine, buf, cap);
}

// ---- v12 (M51h): 入力アクション / UI 拡張 / ゲームフロー / パッド振動 ----

inline int32_t MyeGetMouseWheel(const MyeUpdateContext& ctx)
{
    return ctx.api->GetMouseWheel(ctx.api->engine);
}

// アクションマップ (assets\input\actions.json)。名前は MyeNameHash で 64bit 化して引く。
// 未定義名は常に false / 0
inline bool MyeActionHeld(const MyeUpdateContext& ctx, const char* name)
{
    return (ctx.api->GetActionState(ctx.api->engine, MyeNameHash(name)) & 1u) != 0;
}
inline bool MyeActionPressed(const MyeUpdateContext& ctx, const char* name)
{
    return (ctx.api->GetActionState(ctx.api->engine, MyeNameHash(name)) & 2u) != 0;
}
inline bool MyeActionReleased(const MyeUpdateContext& ctx, const char* name)
{
    return (ctx.api->GetActionState(ctx.api->engine, MyeNameHash(name)) & 4u) != 0;
}
inline float MyeAxis(const MyeUpdateContext& ctx, const char* name)
{
    return ctx.api->GetAxisValue(ctx.api->engine, MyeNameHash(name));
}

// UI 書込 (write-only)。w/h・anchor 以降の負値は「現値維持」(EngineAPI.h の keep 意味論)。
// M75a 以降は RectTransform (anchoredPosition / sizeDelta / 一致アンカー / basis) へ書く
inline bool MyeSetUIRect(const MyeUpdateContext& ctx, MyeEntityId id, float x, float y,
                         float w = -1.0f, float h = -1.0f)
{
    return ctx.api->SetUIRect(ctx.api->engine, id, x, y, w, h) != 0;
}
inline bool MyeSetUILayout(const MyeUpdateContext& ctx, MyeEntityId id, int32_t anchor,
                           int32_t space = -1, int32_t clipChildren = -1, int32_t align = -1,
                           int32_t wrap = -1)
{
    return ctx.api->SetUILayout(ctx.api->engine, id, anchor, space, clipChildren, align, wrap)
        != 0;
}
inline bool MyeSetUITexture(const MyeUpdateContext& ctx, MyeEntityId id, const char* textureKey)
{
    return ctx.api->SetUITexture(ctx.api->engine, id, textureKey) != 0;
}
// **キャンバス座標**でのヒットテスト (M70b。描画と同じ土俵)。無ヒットは null id
// (MyeEntityIdIsNull で判定)。★MousePos は実 px なのでそのまま渡さないこと —
// キャンバス座標のマウス (MouseCanvasPos) は M70c で足す
inline MyeEntityId MyeUIHitTest(const MyeUpdateContext& ctx, float x, float y)
{
    return ctx.api->UIHitTest(ctx.api->engine, x, y);
}

// ゲームフロー (M51g)。ポーズ/スケールが止めるのはアニメ/物理/衝突/パーティクルで、
// スクリプト自身は動き続ける (だからここから解除できる)
inline void MyeSetPaused(const MyeUpdateContext& ctx, bool paused)
{
    int p = 0, s = 100;
    ctx.api->GetTimeControl(ctx.api->engine, &p, &s);
    ctx.api->SetTimeControl(ctx.api->engine, paused ? 1 : 0, s);
}
inline bool MyeIsPaused(const MyeUpdateContext& ctx)
{
    int p = 0, s = 100;
    ctx.api->GetTimeControl(ctx.api->engine, &p, &s);
    return p != 0;
}
inline void MyeSetTimeScale(const MyeUpdateContext& ctx, int32_t percent)
{
    int p = 0, s = 100;
    ctx.api->GetTimeControl(ctx.api->engine, &p, &s);
    ctx.api->SetTimeControl(ctx.api->engine, p, percent);
}
inline int32_t MyeGetTimeScale(const MyeUpdateContext& ctx)
{
    int p = 0, s = 100;
    ctx.api->GetTimeControl(ctx.api->engine, &p, &s);
    return s;
}

// PersistStore (シーン跨ぎ永続)。key は名前を MyeNameHash した 64bit
inline bool MyePersistSet(const MyeUpdateContext& ctx, const char* key, const void* data,
                          int32_t size)
{
    return ctx.api->PersistSet(ctx.api->engine, MyeNameHash(key), data, size) != 0;
}
// 戻り値は実バイト数 (-1 = 不在)。buf へは min(実サイズ, cap) が書かれる
inline int32_t MyePersistGet(const MyeUpdateContext& ctx, const char* key, void* buf, int32_t cap)
{
    return ctx.api->PersistGet(ctx.api->engine, MyeNameHash(key), buf, cap);
}
// 型付き糖衣。サイズ厳密一致で読めたときだけ true (POD 前提)
template <typename T>
inline bool MyePersistSetValue(const MyeUpdateContext& ctx, const char* key, const T& v)
{
    static_assert(std::is_trivially_copyable_v<T>, "persist value must be a POD");
    return MyePersistSet(ctx, key, &v, static_cast<int32_t>(sizeof(T)));
}
template <typename T>
inline bool MyePersistGetValue(const MyeUpdateContext& ctx, const char* key, T& out)
{
    static_assert(std::is_trivially_copyable_v<T>, "persist value must be a POD");
    return MyePersistGet(ctx, key, &out, static_cast<int32_t>(sizeof(T)))
        == static_cast<int32_t>(sizeof(T));
}
// int/float の省略形 (不在・型違いは def を返す)
inline int32_t MyePersistGetInt(const MyeUpdateContext& ctx, const char* key, int32_t def = 0)
{
    int32_t v = 0;
    return MyePersistGetValue(ctx, key, v) ? v : def;
}
inline float MyePersistGetFloat(const MyeUpdateContext& ctx, const char* key, float def = 0.0f)
{
    float v = 0.0f;
    return MyePersistGetValue(ctx, key, v) ? v : def;
}

// セーブ/ロード (tick 末に消費、同 tick 内は後勝ち)。LoadGame は record/verify 中 no-op
inline void MyeSaveGame(const MyeUpdateContext& ctx, int slot = 0)
{
    ctx.api->SaveGame(ctx.api->engine, slot);
}
inline void MyeLoadGame(const MyeUpdateContext& ctx, int slot = 0)
{
    ctx.api->LoadGame(ctx.api->engine, slot);
}

// パッド振動 (出力レーン、0..1)。record/verify 中とフォーカス喪失中はエンジンが 0 に落とす
inline void MyeSetPadVibration(const MyeUpdateContext& ctx, float left, float right)
{
    ctx.api->SetPadVibration(ctx.api->engine, left, right);
}

// ---- v15 (M64a): マウスルック ----

// この tick に積まれた生マウスデルタ (Raw Input のカウント、下向きが正)。
// InputSnapshot 由来なので record/verify では記録値が返る = 決定論レーン。
// ★**単位は生カウントで DPI は機種依存**。感度をコードへ直書きすると
//   マウスを替えただけで別のゲームになる — 調整値を通して割ること
inline void MyeMouseDelta(const MyeUpdateContext& ctx, int32_t& dx, int32_t& dy)
{
    dx = 0;
    dy = 0;
    ctx.api->GetMouseDelta(ctx.api->engine, &dx, &dy);
}

// カーソルのロック (0 = 通常 / 1 = クライアント矩形へ閉じ込めて非表示)。**出力レーン** —
// 要求を書くだけで、record/verify 中・フォーカス喪失中・スクラブ中はエンジンが解除する。
// ★Escape でもエンジンが手放す (エディタで Play 中に Stop を押せなくなるのを防ぐ最後の
//   逃げ道)。**再ロックには「0 を出し直してから 1」が要る** — 毎 tick 1 を書くだけの
//   実装が Escape を握り潰さないようにこうしてある。作法は「ポーズしたら 0、再開で 1」
inline void MyeSetCursorMode(const MyeUpdateContext& ctx, int mode)
{
    ctx.api->SetCursorMode(ctx.api->engine, mode);
}

// ---- v13 (M52i): ネット対戦の状態 + 入力アクションのレーン指定版 ----

// ★Net* が返すのは**機種依存の値** (自分がどちら側か / 実時間 / 巻き戻し回数)。
//   読んだ値を sim 状態へ書き戻すと 2 台のワールドハッシュが割れる — 表示・カメラ・
//   UI の判断にだけ使うこと (詳細は EngineAPI.h の v13 の注記)。
//   誤用は M52i の desync 検出が捕まえて desync バンドルを吐いて止まる
inline bool MyeNetIsConnected(const MyeUpdateContext& ctx)
{
    return ctx.api->NetIsConnected(ctx.api->engine) != 0;
}
// 自分が動かすレーン。ネット非使用なら 0
inline uint32_t MyeNetLocalPlayer(const MyeUpdateContext& ctx)
{
    return ctx.api->NetLocalPlayer(ctx.api->engine);
}
// セッションのレーン数。ネット非使用なら 1
inline uint32_t MyeNetPlayerCount(const MyeUpdateContext& ctx)
{
    return ctx.api->NetPlayerCount(ctx.api->engine);
}
inline float MyeNetPingMs(const MyeUpdateContext& ctx)
{
    return ctx.api->NetPingMs(ctx.api->engine);
}
inline uint64_t MyeNetRollbackCount(const MyeUpdateContext& ctx)
{
    return ctx.api->NetRollbackCount(ctx.api->engine);
}

// レーン指定のアクション/軸。**これは決定論の内側** (記録済み入力の純関数) なので
// sim 状態へそのまま書いてよい。player は 0..kMaxPlayers-1、範囲外は 0
inline bool MyeActionHeldFor(const MyeUpdateContext& ctx, const char* name, uint32_t player)
{
    return (ctx.api->GetActionForPlayer(ctx.api->engine, MyeNameHash(name), player) & 1u) != 0;
}
inline bool MyeActionPressedFor(const MyeUpdateContext& ctx, const char* name, uint32_t player)
{
    return (ctx.api->GetActionForPlayer(ctx.api->engine, MyeNameHash(name), player) & 2u) != 0;
}
inline bool MyeActionReleasedFor(const MyeUpdateContext& ctx, const char* name, uint32_t player)
{
    return (ctx.api->GetActionForPlayer(ctx.api->engine, MyeNameHash(name), player) & 4u) != 0;
}
inline float MyeAxisFor(const MyeUpdateContext& ctx, const char* name, uint32_t player)
{
    return ctx.api->GetAxisForPlayer(ctx.api->engine, MyeNameHash(name), player);
}

// ---- v14 (M59k): 超リアル物理 (M59) の入口 ----

// コンポーネントの付け外し。M59 の機能は「付けたら効く」存在ゲートなので、
// ランタイムの ON/OFF はここが入口 (Aero を付けて空気抵抗を出す 等)。
// ★構造変更 = アーキタイプ移動なので**毎 tick の付け外しは非推奨**。常用する ON/OFF は
//   「付けたまま bool フィールドを MyeSetComponentField で倒す」ほうが桁違いに安い。
// ★**スクリプトから呼ぶ Add / Remove はどちらも tick 末に適用される** (ADR-005)。
//   Has が答えるのは常に「この tick の頭の状態」— 付けた直後は false、外した直後は true。
//   同じ tick 内で結果を見に行かないこと (見えるのは次の tick から)
inline bool MyeRemoveComponent(const MyeUpdateContext& ctx, MyeEntityId id, const char* name)
{
    return ctx.api->RemoveComponentByName(ctx.api->engine, id, name) != 0;
}
inline bool MyeHasComponent(const MyeUpdateContext& ctx, MyeEntityId id, const char* name)
{
    return ctx.api->HasComponentByName(ctx.api->engine, id, name) != 0;
}

// 作用点付きの力 (1 tick 分)。端を押せば回る = 並進と回転が同時に入る。
// MyeAddForce + MyeAddTorque を自分で合成するより安全 (質量と慣性が 1 回だけ解決される)
inline bool MyeAddForceAtPosition(const MyeUpdateContext& ctx, MyeEntityId id, MyeVec3 force,
                                  MyeVec3 worldPoint)
{
    return ctx.api->AddForceAtPosition(ctx.api->engine, id, force, worldPoint) != 0;
}

// 今 tick の接触の詳細 (代表点 / 法線 / 法線インパルス合計)。
// ★**OnCollisionEnter / OnCollisionStay / LateUpdate からしか実データが返らない**。
//   Update は物理より前のフェーズなので必ず false が返る (決定論の要請 —
//   詳細は EngineAPI.h の v14 の注記)。
//   典型的な使い方は OnCollisionEnter で impulse を見て着地音の音量を決める、など
inline bool MyeGetContactInfo(const MyeUpdateContext& ctx, MyeEntityId self, MyeEntityId other,
                              MyeContactInfo& out)
{
    return ctx.api->GetContactInfo(ctx.api->engine, self, other, &out) != 0;
}

// その点の風速 (m/s、ワールド)。PhysicsEnvironment を置いていなければ false + 無風。
// point は M59 では読まれない (一様定常風) が、乱流を足すときに ABI を
// もう一度上げずに済ませるため引数に取ってある
inline bool MyeSampleWind(const MyeUpdateContext& ctx, MyeVec3 point, MyeVec3& outWind)
{
    return ctx.api->SampleWind(ctx.api->engine, point, &outWind) != 0;
}

// ワールド XZ の地形表面の高さ (ワールド Y) と面法線。**当たる地形**を引くので
// 描画の LOD やスカートの影響を受けない。地形コライダーの範囲外は false
inline bool MyeSampleTerrainHeight(const MyeUpdateContext& ctx, float x, float z, float& outHeight,
                                   MyeVec3& outNormal)
{
    return ctx.api->SampleTerrainHeight(ctx.api->engine, x, z, &outHeight, &outNormal) != 0;
}

// スリープ (M59h)。力・速度を触るスロットは自動で起こすので、明示的に呼ぶのは
// 「近くで何かが起きたから念のため起こす」ような外部要因のときだけ
inline bool MyeWakeRigidbody(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return ctx.api->WakeRigidbody(ctx.api->engine, id) != 0;
}
inline bool MyeIsSleeping(const MyeUpdateContext& ctx, MyeEntityId id)
{
    return ctx.api->IsSleeping(ctx.api->engine, id) != 0;
}
