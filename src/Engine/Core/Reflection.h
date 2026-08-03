#pragma once
#include <cstdint>
#include <cstddef>

namespace mye {

// リフレクションのフィールド型 (閉集合)。
// この 1 つのメタデータ形式を以下の 4 システムが共用する (計画の中核方針):
//   - Inspector の widget 自動生成 (M2)
//   - シーン JSON シリアライズ (M2/M3)
//   - GameLogic.dll リロード時の状態移行 (M4)
//   - ワールド状態ハッシュ (M6)
// 型を追加する場合は 4 者すべてに対応を入れること。任意型の汎用対応はしない。
enum class FieldType : uint8_t {
    Float,
    Int32,
    UInt32,
    UInt64,
    Bool,
    Float2,
    Float3,
    Float4,
    Quat,      // float4 (x,y,z,w) — Inspector ではオイラー角表示
    Color,     // float4 RGBA — Inspector では ColorEdit
    EntityRef, // EntityID
    AssetRef,  // AssetID
    String64,  // char[64] 固定長
    Float4x4,  // 派生値表示用 (シリアライズ対象外が普通)
    String256, // char[256] 固定長 (M34: UI/TextMesh の日本語テキスト用。末尾追加)
};

// フィールドのバイトサイズ
uint32_t FieldTypeSize(FieldType t);

enum FieldFlags : uint32_t {
    kFieldNone = 0,
    kFieldReadOnly = 1u << 0,    // Inspector で編集不可
    kFieldNoSerialize = 1u << 1, // シーン保存対象外 (派生値・内部状態)
    kFieldHidden = 1u << 2,      // Inspector 非表示
};

struct FieldDesc {
    const char* name = nullptr;
    FieldType type = FieldType::Float;
    uint32_t offset = 0;
    uint32_t flags = kFieldNone;
    // ---- Inspector 用メタデータ (M8 追加、ABI 非依存 — Shared の MyeScriptField とは別物) ----
    // 0/nullptr は「型ごとの既定」を意味する。シリアライズ/ハッシュ/DLL 移行には影響しない
    float dragSpeed = 0.0f;      // ImGui::DragFloat の速度 (0 = 既定 0.05)
    float minVal = 0.0f;         // minVal==maxVal のときレンジ無効 (クランプしない)
    float maxVal = 0.0f;
    const char* tooltip = nullptr;
    // Inspector の日本語表示名 (M47c)。nullptr なら name をそのまま出す。
    // name は JSON キー / DLL リロードの移行キーなので触れない — これは表示専用の別スロット。
    // **必ず末尾に置くこと**: MYE_FIELD / MYE_FIELD_FLAGS は先頭 4 メンバの位置指定初期化、
    // Components.cpp の生 FieldDesc{...} も同様で、途中に挿すと全部ずれる
    const char* displayName = nullptr;
};

// 表示名を後付けする合成ヘルパ (M47c)。既存の MYE_FIELD 系 4 マクロを 1 文字も変えずに
// 包める。jp を先頭にして可変引数で受けるのは、Components.cpp に
// `FieldDesc{ "param0", ... }` のようなマクロを使わない記述があり、プリプロセッサが
// `{}` をグループ化しない (= 引数が分割されてしまう) ため。
//
//   MYE_JP("位置", MYE_FIELD(LocalTransform, position, Float3))
//   MYE_JP("範囲", MYE_FIELD_RANGE(LightComponent, range, Float, 0.0f, 100.0f))
constexpr FieldDesc WithJp(FieldDesc d, const char* jp)
{
    d.displayName = jp;
    return d;
}

} // namespace mye

// 使用例: MYE_FIELD(LocalTransform, position, Float3)
#define MYE_FIELD(T, member, ftype) \
    ::mye::FieldDesc{ #member, ::mye::FieldType::ftype, static_cast<uint32_t>(offsetof(T, member)), ::mye::kFieldNone }

#define MYE_FIELD_FLAGS(T, member, ftype, fflags) \
    ::mye::FieldDesc{ #member, ::mye::FieldType::ftype, static_cast<uint32_t>(offsetof(T, member)), (fflags) }

// レンジ付き (Inspector でスライダ/クランプ)。C++20 指定初期化子で末尾メタだけ設定
#define MYE_FIELD_RANGE(T, member, ftype, lo, hi)                                                \
    ::mye::FieldDesc { .name = #member, .type = ::mye::FieldType::ftype,                          \
                       .offset = static_cast<uint32_t>(offsetof(T, member)), .minVal = (lo),     \
                       .maxVal = (hi) }

// ツールチップ付き
#define MYE_FIELD_TIP(T, member, ftype, tip)                                                     \
    ::mye::FieldDesc { .name = #member, .type = ::mye::FieldType::ftype,                          \
                       .offset = static_cast<uint32_t>(offsetof(T, member)), .tooltip = (tip) }

// 日本語表示名を付ける (M47c)。上の 4 マクロと生の FieldDesc{...} のどれでも包める
#define MYE_JP(jp, ...) ::mye::WithJp(__VA_ARGS__, jp)
