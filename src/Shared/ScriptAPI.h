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

// ---- フィールド列挙マクロ (最大 16 個。/Zc:preprocessor 必須) ----
#define MYE_SF(T, m) { #m, MyeTypeOf<std::remove_cv_t<decltype(T::m)>>::value, (uint32_t)offsetof(T, m) },
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
#define MYE_SF_NARGS(...) MYE_SF_NARGS_I(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define MYE_SF_NARGS_I(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
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
    void Destroy() const { api->DestroyGameObject(api->engine, id); }
};

inline MyeGameObject MyeSelf(const MyeUpdateContext& ctx)
{
    return { ctx.self, ctx.api };
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

// 自分の位置で 3D 再生する (足音・衝突音など)
inline uint64_t MyePlaySoundHere(const MyeUpdateContext& ctx, const char* key, float volume = 1.0f)
{
    return MyePlaySoundAt(ctx, key, MyeSelf(ctx).GetLocalPosition(), volume);
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

// 名前 → ハッシュ。FNV-1a 64bit で **Engine/Core/Hash.h の HashStr と同一の定数**
// (MyePartTag と同じ再掲。一致は SchemaSelfTest が機械検査している)
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
// マウスで再現されるため replay 一致。rect は UIElementComponent と同じピクセル座標系 (左上原点)。
// anchor=0(左上) の要素なら x/y/w/h をそのまま渡せる (他 anchor は画面サイズ依存)。
struct MyeUIRect {
    float x, y, w, h;
};

inline bool MyeMouseInRect(const MyeUpdateContext& ctx, MyeUIRect r)
{
    int32_t mx = 0, my = 0;
    ctx.api->MousePos(ctx.api->engine, &mx, &my);
    const float fx = static_cast<float>(mx), fy = static_cast<float>(my);
    return fx >= r.x && fx < r.x + r.w && fy >= r.y && fy < r.y + r.h;
}

// 左ボタンを rect 内で押した瞬間に true。prevDown は呼び出し側スクリプトがフィールドで
// 保持する (エッジ検出。登録フィールドなら DLL リロードを跨いで状態維持)。
inline bool MyeButtonClicked(const MyeUpdateContext& ctx, MyeUIRect r, int32_t& prevDown)
{
    const int down = ctx.api->MouseButton(ctx.api->engine, 0);
    const bool clicked = down && !prevDown && MyeMouseInRect(ctx, r);
    prevDown = down;
    return clicked;
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

// UI 書込 (write-only)。w/h・anchor 以降の負値は「現値維持」(EngineAPI.h の keep 意味論)
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
// 基準解像度 (1920x1080) でのヒットテスト。無ヒットは null id (MyeEntityIdIsNull で判定)
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
