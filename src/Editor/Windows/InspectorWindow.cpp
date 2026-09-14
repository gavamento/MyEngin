#include "Editor/Windows/InspectorWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Editor/AssetOps.h"
#include "Editor/CameraPilot.h"
#include "Editor/ComponentClipboard.h"
#include "Editor/EditorWidgets.h"
#include "Engine/Core/AssetGuidResolver.h"
#include "Editor/EditorComponentCatalog.h"
#include "Editor/PartTagNames.h"
#include "Editor/PhysicsLayerNames.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h" // マテリアルプレビューの同一性キー (M53)
#include "Engine/Core/Localization.h"
#include "Engine/Engine/UI/UILayout.h" // M75a: RectTransform の解決済み矩形の読み取り表示
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: 自動レイアウトに上書きされている欄の表示
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ImGuiTheme.h"  // 見出しフォント (テーマ第 3 世代)
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClassCount (M67)
#include "Engine/Renderer/ReflectionClassJson.h" // reflectionClass の受理規則 (M67h)
#include "Engine/Renderer/RenderTypes.h" // kEmissiveMaxIntensity (M46i)
#include "Engine/Renderer/Skeleton.h"    // SkinnedModel のジョイント名 (M48i)

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

// ASCII 大文字小文字無視の部分一致 (Add Component の検索用。コンポーネント名は ASCII 前提)
bool ContainsIgnoreCase(const char* haystack, const char* needle)
{
    if (needle[0] == '\0') {
        return true;
    }
    const size_t nlen = std::strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        size_t i = 0;
        while (i < nlen && h[i]
               && std::tolower(static_cast<unsigned char>(h[i]))
                      == std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

// 直前に描画した widget の編集開始/確定を検出して Undo エントリにまとめる。
// activate (ドラッグ開始) で before を、deactivate-after-edit で after を記録 —
// ドラッグ全体が 1 エントリになる (transient マージ)。
// M40a: 複数 fileId を渡すとバッチ編集全体が 1 エントリになる (全対象の before/after)
void HandleEditUndoMulti(EngineContext& ctx, Selection& sel, UndoStack& undo,
                         const std::vector<uint64_t>& fids, const char* label)
{
    if (ImGui::IsItemActivated() && !undo.IsRecording()) {
        undo.BeginRecord(label, sel);
        for (uint64_t fid : fids) {
            undo.CaptureBefore(*ctx.scene, fid);
        }
    }
    if (undo.IsRecording()) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            for (uint64_t fid : fids) {
                undo.CaptureAfter(*ctx.scene, fid);
            }
            undo.EndRecord(sel);
        } else if (ImGui::IsItemDeactivated()) {
            undo.CancelRecord(); // 値を変えずに離した
        }
    }
}

void HandleEditUndo(EngineContext& ctx, Selection& sel, UndoStack& undo, uint64_t fid,
                    const char* label)
{
    const std::vector<uint64_t> one = { fid };
    HandleEditUndoMulti(ctx, sel, undo, one, label);
}

// XMQuaternionRotationRollPitchYaw 規約 (roll→pitch→yaw) の逆変換
XMFLOAT3 QuatToEulerDeg(const XMFLOAT4& q)
{
    const XMMATRIX m = XMMatrixRotationQuaternion(XMLoadFloat4(&q));
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);

    XMFLOAT3 euler;
    const float sinPitch = -f._32;
    if (std::fabs(sinPitch) > 0.9999f) {
        // ジンバル特異点
        euler.x = std::asin(sinPitch) * kRad2Deg;
        euler.y = std::atan2(-f._13, f._11) * kRad2Deg;
        euler.z = 0.0f;
    } else {
        euler.x = std::asin(sinPitch) * kRad2Deg;          // pitch
        euler.y = std::atan2(f._31, f._33) * kRad2Deg;     // yaw
        euler.z = std::atan2(f._12, f._22) * kRad2Deg;     // roll
    }
    return euler;
}

XMFLOAT4 EulerDegToQuat(const XMFLOAT3& euler)
{
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(euler.x * kDeg2Rad, euler.y * kDeg2Rad,
                                                       euler.z * kDeg2Rad));
    return q;
}

// エディタ側の enum ラベル表 (M28a)。リフレクション FieldType は閉集合のまま、
// (コンポーネント名, フィールド名) が一致した Int32 を Combo で描画する (sim 非影響)
// M47c: フィールドの表示名。name (英語) は JSON キー / DLL 移行キーなので触らず、
// FieldDesc::displayName を「表示だけ」に使う。
// widget では "表示名###英語名" にして ID を英語名に固定する — ImHashStr は "###" で
// ハッシュをシードへ戻すので、言語を切り替えても ID が変わらず、同一コンポーネント内で
// 表示名が重複しても衝突しない。displayName が無い (スクリプト由来の) フィールドは英名のまま
const char* FieldLabelText(const FieldDesc& f)
{
    return (f.displayName != nullptr && CurrentLanguage() != Lang::En) ? f.displayName : f.name;
}

const char* MakeFieldLabel(const FieldDesc& f, char* out, size_t cap)
{
    if (f.displayName != nullptr && CurrentLanguage() != Lang::En) {
        std::snprintf(out, cap, "%s###%s", f.displayName, f.name);
    } else {
        std::snprintf(out, cap, "%s", f.name);
    }
    return out;
}

struct EnumFieldLabels {
    const char* component;
    const char* field;
    const char* const* labels;
    int count;
    const char* const* ja = nullptr; // M47c: 日本語表示 (nullptr なら labels をそのまま出す)

    // 表示に使う配列。値は index なので、訳しても保存内容は変わらない
    const char* const* Display() const
    {
        return (ja != nullptr && CurrentLanguage() != Lang::En) ? ja : labels;
    }
};
constexpr const char* kColliderShapeLabels[] = { "Sphere", "Box",  "Capsule",
                                                 "Mesh",   "Terrain", "Convex" };
constexpr const char* kLightTypeLabels[] = { "Directional", "Point", "Spot" };
constexpr const char* kEmitterShapeLabels[] = { "Point", "Sphere", "Cone", "Box" };
constexpr const char* kBlendModeLabels[] = { "Additive", "Alpha", "Distortion" }; // M42d
// M61a: A群拡張の enum 3 種。
// simulationSpace は kForceSpaceLabels と同文だが、ConstantForce の並びと結合させない
// (片方の値域拡張がもう片方の表示を静かに変える事故を避ける) ため専用配列にする
constexpr const char* kPtclSimSpaceLabels[] = { "World", "Local" };
constexpr const char* kPtclTurbModeLabels[] = { "Vortex", "Noise" };
constexpr const char* kPtclEmitFromLabels[] = { "Default", "Volume", "Surface" };
// M63a: B群の enum は lightingMode の 1 種だけ。flipBlend / flipRandomStart /
// lightReceiveShadow / collisionFloor は 0/1 の素の DragInt のままにする
// (kOffOnLabels へ寄せると「オン/オフ」以上の意味を持つ日が来たとき値域を広げられない)
constexpr const char* kPtclLightModeLabels[] = { "Unlit", "Per Particle", "Per Pixel" };
constexpr const char* kUIKindLabels[] = { "Panel", "Text", "Button" };
constexpr const char* kUIAnchorLabels[] = { "TopLeft",    "TopCenter",    "TopRight",
                                            "MiddleLeft", "Center",       "MiddleRight",
                                            "BottomLeft", "BottomCenter", "BottomRight" };
constexpr const char* kForceSpaceLabels[] = { "World", "Local" };
constexpr const char* kBillboardLabels[] = { "Billboard", "BillboardY", "World" };
constexpr const char* kSkyboxModeLabels[] = { "Gradient", "Cubemap" };
constexpr const char* kFogModeLabels[] = { "Linear", "Exp", "Exp2" };
constexpr const char* kTonemapLabels[] = { "Passthrough", "ACES", "Reinhard" };
constexpr const char* kOffOnLabels[] = { "Off", "On" };
// M49: PartBounds は 0=Box (Collider の 0=Sphere と並びが違うので専用配列)
constexpr const char* kPartBoundsShapeLabels[] = { "Box", "Sphere" };
// M51f: UI オーサリング
constexpr const char* kUIFillModeLabels[] = { "Off", "Horizontal", "Vertical" };
// M75a: RectTransform.basis (旧 UIElement.space の後継。0=親 / 1=キャンバス で意味が反転)
constexpr const char* kUIBasisLabels[] = { "Parent", "Canvas" };
// M75a: アンカープリセット 4x4 (Unity の Anchor Presets と同じ並び。行 = 縦、列 = 横)
constexpr const char* kUIPresetColLabels[] = { "Left", "Center", "Right", "Stretch" };
constexpr const char* kUIPresetRowLabels[] = { "Top", "Middle", "Bottom", "Stretch" };
// M47c: 日本語表示。ACES / Reinhard / Exp2 のような固有名詞・数式名は英語のまま
constexpr const char* kColliderShapeJa[] = { "スフィア", "ボックス", "カプセル", "メッシュ",
                                             "地形",     "凸包" };
constexpr const char* kLightTypeJa[] = { "平行光", "ポイント", "スポット" };
constexpr const char* kEmitterShapeJa[] = { "点", "スフィア", "コーン", "ボックス" };
constexpr const char* kBlendModeJa[] = { "加算", "アルファ", "歪み" };
constexpr const char* kPtclSimSpaceJa[] = { "ワールド", "ローカル" }; // M61a
constexpr const char* kPtclTurbModeJa[] = { "渦", "ノイズ" };
constexpr const char* kPtclEmitFromJa[] = { "既定", "体積", "表面" };
constexpr const char* kPtclLightModeJa[] = { "なし", "粒子単位", "画素単位" }; // M63a
constexpr const char* kUIKindJa[] = { "パネル", "テキスト", "ボタン" };
constexpr const char* kUIAnchorJa[] = { "左上", "上中央", "右上", "左中央", "中央",
                                        "右中央", "左下", "下中央", "右下" };
constexpr const char* kForceSpaceJa[] = { "ワールド", "ローカル" };
constexpr const char* kBillboardJa[] = { "ビルボード", "ビルボード (Y 軸)", "ワールド" };
constexpr const char* kSkyboxModeJa[] = { "グラデーション", "キューブマップ" };
constexpr const char* kFogModeJa[] = { "線形", "Exp", "Exp2" };
constexpr const char* kTonemapJa[] = { "そのまま", "ACES", "Reinhard" };
constexpr const char* kOffOnJa[] = { "オフ", "オン" };
constexpr const char* kPartBoundsShapeJa[] = { "ボックス", "スフィア" };
constexpr const char* kUIFillModeJa[] = { "オフ", "水平 (左→右)", "垂直 (下→上)" };
constexpr const char* kUIBasisJa[] = { "親", "キャンバス" };
// M75c: UICanvas.scaleMode (Unity の Screen Match Mode と同じ並び)
constexpr const char* kUICanvasScaleLabels[] = { "Expand", "Shrink", "Match Width Or Height" };
constexpr const char* kUICanvasScaleJa[] = { "拡張 (Expand)", "縮小 (Shrink)", "幅/高さに合わせる" };
// M75e: 自動レイアウト (Unity の Layout Group / ContentSizeFitter と同じ並び)
constexpr const char* kUILayoutKindLabels[] = { "Horizontal", "Vertical", "Grid" };
constexpr const char* kUILayoutKindJa[] = { "水平", "垂直", "グリッド" };
constexpr const char* kUIGridCornerLabels[] = { "Upper Left", "Upper Right", "Lower Left",
                                                "Lower Right" };
constexpr const char* kUIGridCornerJa[] = { "左上", "右上", "左下", "右下" };
constexpr const char* kUIGridAxisLabels[] = { "Horizontal", "Vertical" };
constexpr const char* kUIGridAxisJa[] = { "横に埋める", "縦に埋める" };
constexpr const char* kUIGridConstraintLabels[] = { "Flexible", "Fixed Column Count",
                                                    "Fixed Row Count" };
constexpr const char* kUIGridConstraintJa[] = { "幅に合わせる", "列数を固定", "行数を固定" };
constexpr const char* kUIFitModeLabels[] = { "Unconstrained", "Min Size", "Preferred Size" };
constexpr const char* kUIFitModeJa[] = { "制約なし", "最小サイズ", "推奨サイズ" };
// M75f: ウィジェット (Unity の Selectable.Transition / Navigation.Mode / Slider.Direction と同じ並び)
constexpr const char* kUITransitionLabels[] = { "None", "Color Tint", "Sprite Swap" };
constexpr const char* kUITransitionJa[] = { "なし", "色 (Color Tint)", "画像の差し替え (Sprite Swap)" };
constexpr const char* kUINavModeLabels[] = { "None", "Horizontal", "Vertical", "Automatic",
                                             "Explicit" };
constexpr const char* kUINavModeJa[] = { "なし", "左右", "上下", "自動", "明示" };
constexpr const char* kUISliderDirLabels[] = { "Left To Right", "Right To Left", "Bottom To Top",
                                               "Top To Bottom" };
constexpr const char* kUISliderDirJa[] = { "左→右", "右→左", "下→上", "上→下" };
constexpr const char* kUIPresetColJa[] = { "左", "中央", "右", "伸縮" };
constexpr const char* kUIPresetRowJa[] = { "上", "中央", "下", "伸縮" };
// 番号の表 (Components.h) とラベルの件数を機械で揃える。種類を足してラベルを忘れると
// コンボが "(invalid)" 表示になり、エディタから選ぶ手段が消える (M60f で踏んだ)
static_assert(std::size(kColliderShapeLabels) == collidershape::kCount
              && std::size(kColliderShapeJa) == collidershape::kCount);
static_assert(std::size(kLightTypeLabels) == lighttype::kCount && std::size(kLightTypeJa) == lighttype::kCount);
static_assert(std::size(kPartBoundsShapeLabels) == 2 && partboundsshape::kSphere == 1);
constexpr EnumFieldLabels kEnumFields[] = {
    // M60f: 3 (Mesh) / 4 (Terrain) / 5 (Convex) までコンボに出す (これらは meshAsset を
    // 併せて指す必要がある)
    { "Collider", "shape", kColliderShapeLabels, collidershape::kCount, kColliderShapeJa },
    { "Light", "type", kLightTypeLabels, lighttype::kCount, kLightTypeJa },
    { "ParticleEmitter", "shape", kEmitterShapeLabels, 4, kEmitterShapeJa },
    { "ParticleEmitter", "blendMode", kBlendModeLabels, 3, kBlendModeJa },
    // M61a: A群拡張 (subframeEmission は playing/looping と同じく素の DragInt のまま)
    { "ParticleEmitter", "simulationSpace", kPtclSimSpaceLabels, 2, kPtclSimSpaceJa },
    { "ParticleEmitter", "turbulenceMode", kPtclTurbModeLabels, 2, kPtclTurbModeJa },
    { "ParticleEmitter", "emitFrom", kPtclEmitFromLabels, 3, kPtclEmitFromJa },
    // M63a: B群 = 描画表現力
    { "ParticleEmitter", "lightingMode", kPtclLightModeLabels, 3, kPtclLightModeJa },
    { "UIElement", "kind", kUIKindLabels, 3, kUIKindJa },
    { "UIElement", "align", kUIAnchorLabels, 9, kUIAnchorJa },
    { "UIElement", "fillMode", kUIFillModeLabels, 3, kUIFillModeJa },
    // M75a: RectTransform.anchorMin はコンボではなくプリセット 4x4 ピッカー + DragFloat2
    // (DrawField の特例) — 行はここに置かない
    { "RectTransform", "basis", kUIBasisLabels, 2, kUIBasisJa },
    { "UICanvas", "scaleMode", kUICanvasScaleLabels, 3, kUICanvasScaleJa }, // M75c
    // M75e: 自動レイアウト
    { "UILayoutGroup", "kind", kUILayoutKindLabels, 3, kUILayoutKindJa },
    { "UILayoutGroup", "childAlignment", kUIAnchorLabels, 9, kUIAnchorJa },
    { "UILayoutGroup", "controlChildWidth", kOffOnLabels, 2, kOffOnJa },
    { "UILayoutGroup", "controlChildHeight", kOffOnLabels, 2, kOffOnJa },
    { "UILayoutGroup", "forceExpandWidth", kOffOnLabels, 2, kOffOnJa },
    { "UILayoutGroup", "forceExpandHeight", kOffOnLabels, 2, kOffOnJa },
    { "UILayoutGroup", "reverseArrangement", kOffOnLabels, 2, kOffOnJa },
    { "UILayoutGroup", "startCorner", kUIGridCornerLabels, 4, kUIGridCornerJa },
    { "UILayoutGroup", "startAxis", kUIGridAxisLabels, 2, kUIGridAxisJa },
    { "UILayoutGroup", "constraint", kUIGridConstraintLabels, 3, kUIGridConstraintJa },
    { "UILayoutElement", "ignoreLayout", kOffOnLabels, 2, kOffOnJa },
    { "UIContentSizeFitter", "horizontalFit", kUIFitModeLabels, 3, kUIFitModeJa },
    { "UIContentSizeFitter", "verticalFit", kUIFitModeLabels, 3, kUIFitModeJa },
    // M75f: ウィジェット
    { "UISelectable", "interactable", kOffOnLabels, 2, kOffOnJa },
    { "UISelectable", "transition", kUITransitionLabels, 3, kUITransitionJa },
    { "UISelectable", "navigationMode", kUINavModeLabels, 5, kUINavModeJa },
    { "UIToggle", "isOn", kOffOnLabels, 2, kOffOnJa },
    { "UIToggleGroup", "allowSwitchOff", kOffOnLabels, 2, kOffOnJa },
    { "UISlider", "direction", kUISliderDirLabels, 4, kUISliderDirJa },
    { "UISlider", "wholeNumbers", kOffOnLabels, 2, kOffOnJa },
    { "UIElement", "clipChildren", kOffOnLabels, 2, kOffOnJa },
    { "UIElement", "wrap", kOffOnLabels, 2, kOffOnJa },
    { "ConstantForce", "relative", kForceSpaceLabels, 2, kForceSpaceJa },
    { "SpriteRenderer", "billboardMode", kBillboardLabels, 3, kBillboardJa },
    { "TextMesh", "billboardMode", kBillboardLabels, 3, kBillboardJa },
    { "Skybox", "mode", kSkyboxModeLabels, 2, kSkyboxModeJa },
    { "Fog", "mode", kFogModeLabels, 3, kFogModeJa },
    { "CameraPostFx", "tonemapMode", kTonemapLabels, 3, kTonemapJa },
    { "CameraPostFx", "bloomOn", kOffOnLabels, 2, kOffOnJa },
    { "CameraPostFx", "fxaaOn", kOffOnLabels, 2, kOffOnJa },
    { "PartBounds", "shape", kPartBoundsShapeLabels, 2, kPartBoundsShapeJa },
};

// ImGui::InputText は終端より後ろのバイトを掃除しない。一方 WorldHasher は登録フィールドを
// **FieldTypeSize 分まるごと**ハッシュする (WorldHasher.cpp) ので、文字列を短くしたときの
// 残骸が「JSON は同一なのに WorldHash だけ違う」を生む (M8 の NameComponent と同じ罠)。
// 編集直後に終端以降をゼロ埋めして、生バイトを保存内容と 1:1 にする
void ZeroStringTail(char* buf, size_t cap)
{
    const size_t n = std::strlen(buf);
    if (n + 1 < cap) {
        std::memset(buf + n + 1, 0, cap - n - 1);
    }
}

const EnumFieldLabels* FindEnumLabels(const char* component, const char* field)
{
    if (!component) {
        return nullptr;
    }
    for (const EnumFieldLabels& e : kEnumFields) {
        if (std::strcmp(e.component, component) == 0 && std::strcmp(e.field, field) == 0) {
            return &e;
        }
    }
    return nullptr;
}

// C# コンポーネントの 1 フィールドを raw バッファ上で描画する (型は managed から届く FieldType)
bool DrawManagedFieldWidget(const char* name, FieldType type, void* buf)
{
    switch (type) {
    case FieldType::Float: return ImGui::DragFloat(name, static_cast<float*>(buf), 0.05f);
    case FieldType::Int32: return ImGui::DragInt(name, static_cast<int*>(buf));
    case FieldType::UInt32: return ImGui::InputScalar(name, ImGuiDataType_U32, buf);
    case FieldType::UInt64: return ImGui::InputScalar(name, ImGuiDataType_U64, buf);
    case FieldType::Bool: {
        bool b = *static_cast<uint8_t*>(buf) != 0;
        if (ImGui::Checkbox(name, &b)) {
            *static_cast<uint8_t*>(buf) = b ? 1 : 0;
            return true;
        }
        return false;
    }
    case FieldType::Float2: return ImGui::DragFloat2(name, static_cast<float*>(buf), 0.05f);
    case FieldType::Float3: return ImGui::DragFloat3(name, static_cast<float*>(buf), 0.05f);
    case FieldType::Float4: return ImGui::DragFloat4(name, static_cast<float*>(buf), 0.05f);
    case FieldType::Quat: return ImGui::DragFloat4(name, static_cast<float*>(buf), 0.05f);
    case FieldType::Color: return ImGui::ColorEdit4(name, static_cast<float*>(buf));
    default: ImGui::TextDisabled("%s (unsupported)", name); return false;
    }
}

// C# スクリプトコンポーネントのフィールド描画。値は managed インスタンスが保持するため、
// ManagedHost 経由で get/set する (編集モードでは EnsureInstance で instance を用意)。
void DrawManagedComponentFields(EngineContext& ctx, ComponentTypeId t, void* comp, EntityID e)
{
    ManagedHost* mh = ctx.managedHost;
    const int32_t handle = mh->EnsureInstance(t, e, comp);
    if (handle == 0) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_NoManagedInst));
        return;
    }
    const auto* fields = mh->FieldsForComponent(t);
    if (!fields || fields->empty()) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_NoPublicFields));
        return;
    }
    for (size_t i = 0; i < fields->size(); ++i) {
        const ManagedHost::ManagedFieldInfo& f = (*fields)[i];
        uint8_t buf[16] = {};
        if (!mh->GetFieldValue(handle, static_cast<int>(i), buf, static_cast<int>(sizeof(buf)))) {
            continue;
        }
        if (DrawManagedFieldWidget(f.name.c_str(), f.type, buf)) {
            mh->SetFieldValue(handle, static_cast<int>(i), buf, static_cast<int>(sizeof(buf)));
        }
    }
}

// M60b: 関節の型 (jointtype::) ごとに Inspector へ出すフィールドを選ぶ。ソルバ側の type ディスパッチと**対**なので、
// 型を足したらここも足すこと — 出ているのに効かない行があるのが最悪の状態
bool JointFieldApplies(int32_t type, const char* name)
{
    const bool usesAxis = (type == jointtype::kHinge || type == jointtype::kSlider || type == jointtype::kCone);
    const bool usesAngular = (type != jointtype::kBall); // Ball だけ相対姿勢を拘束しない
    const bool usesLimit = (type == jointtype::kHinge || type == jointtype::kSlider || type == jointtype::kCone);
    const bool usesMotor = (type == jointtype::kHinge || type == jointtype::kSlider);
    if (std::strcmp(name, "axis") == 0) {
        return usesAxis;
    }
    if (std::strcmp(name, "restRotation") == 0) {
        return usesAngular;
    }
    if (std::strcmp(name, "useLimit") == 0 || std::strcmp(name, "limitMin") == 0
        || std::strcmp(name, "limitMax") == 0) {
        return usesLimit;
    }
    if (std::strcmp(name, "swingLimitDeg") == 0) {
        return type == jointtype::kCone;
    }
    if (std::strcmp(name, "motorTargetVelocity") == 0
        || std::strcmp(name, "motorMaxForce") == 0) {
        return usesMotor;
    }
    // M60d: Ball は角ブロックを 1 つも立てないので、破断トルクが働く余地が無い
    if (std::strcmp(name, "breakTorque") == 0) {
        return usesAngular;
    }
    return true;
}

} // namespace

namespace {

// マルチ選択の対象集合 (M40a): 生存する選択 fileId 群、primary 先頭。
// 表示値は primary のもの。編集は全対象へバッチ適用 (ギズモ操作は primary のみ)
InspectorTargets CollectInspectorTargets(EngineContext& ctx, const Selection& selection, uint64_t fid, EntityID e)
{
    World& world = ctx.scene->GetWorld();
    InspectorTargets tg;
    tg.fid = fid;
    tg.e = e;
    tg.fids.push_back(fid);
    tg.ents.push_back(e);
    for (uint64_t sfid : selection.ids) {
        if (sfid == fid) {
            continue;
        }
        GameObject g = ctx.scene->FindByFileId(sfid);
        if (g && world.IsAlive(g.Id())) {
            tg.fids.push_back(sfid);
            tg.ents.push_back(g.Id());
        }
    }
    tg.multi = tg.fids.size() > 1;
    // プレハブ所属判定 (青文字 / オーバーライド表示 / Revert・Apply に使う)
    tg.prefabRoot = Prefab::FindInstanceRoot(world, e);
    tg.isPrefabMember = !tg.prefabRoot.IsNull();
    return tg;
}

} // namespace

void InspectorWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                              AssetPreviewCache& preview)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Inspector), &open)) {
        ImGui::End();
        return;
    }
    // ---- アセット選択 (M40c): AssetBrowser タイルクリックで Inspector に情報表示 ----
    if (selection.HasAsset()) {
        DrawAssetInspector(ctx, selection, preview);
        ImGui::End();
        return;
    }

    World& world = ctx.scene->GetWorld();
    // 選択は fileId 保持 — 現フレームの EntityID に解決する
    const uint64_t fid = selection.primary;
    GameObject go = ctx.scene->FindByFileId(fid);
    const EntityID e = go ? go.Id() : kNullEntity;
    if (fid == 0 || !world.IsAlive(e)) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_NoSelection));
        ImGui::End();
        return;
    }

    const InspectorTargets tg = CollectInspectorTargets(ctx, selection, fid, e);
    DrawNameRow(ctx, selection, undo, tg);
    DrawPrefabBar(ctx, selection, undo, tg);
    ImGui::Separator();

    // ---- コンポーネント一覧 (アーキタイプの型リスト = TypeId 昇順) ----
    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        ImGui::End();
        return;
    }
    // 型リストをコピー (描画中の RemoveComponent でアーキタイプが変わっても安全に)
    std::vector<ComponentTypeId> types(arch->Types().begin(), arch->Types().end());
    for (ComponentTypeId t : types) {
        DrawComponent(ctx, selection, undo, tg, t);
    }

    DrawRemovedPrefabComponents(ctx, selection, undo, tg);
    DrawUnknownComponents(ctx, tg);
    DrawAddComponentPopup(ctx, selection, undo, tg);
    DrawScriptDropTarget(ctx, selection, undo, tg);
    ImGui::End();
}

// 名前欄と、対象の説明 1 行 (マルチ選択の件数 / Entity index:generation)
void InspectorWindow::DrawNameRow(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                  const InspectorTargets& tg)
{
    World& world = ctx.scene->GetWorld();
    const EntityID e = tg.e;
    if (auto* nc = world.GetComponent<NameComponent>(e)) {
        // 部位の構造ロック (M48f): プレハブメンバの部位はリネーム不可。Hierarchy 側だけ
        // 塞いでも Inspector から改名できたら穴になるので、ここも読み取り専用にする
        const bool partLocked = Parts::IsStructureLocked(world, e);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", nc->value, sizeof(nc->value),
                         partLocked ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
        if (partLocked) {
            ImGui::TextDisabled("%s", Tr(StrId::Hier_PartLockedShort));
        }
        // 編集開始時の名前を控え、確定時に正規化する (M48b)。
        // HandleEditUndo が CaptureAfter を撮る**前**に置くこと。ImGui のアイテム状態問い合わせは
        // 同フレーム内で冪等なので次行の判定は壊れない。
        // ★IsItemDeactivatedAfterEdit は Esc の revert でも真になるので、original と比較して
        //   「変更なし」を弾かないと取り消したはずの操作で名前が変わる
        if (ImGui::IsItemActivated()) {
            nameOriginal_ = nc->value;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            FinishRename(world, e, nc->value, nameOriginal_);
        }
        HandleEditUndo(ctx, selection, undo, tg.fid, "Rename");
        if (tg.isPrefabMember && Prefab::IsNameOverridden(*ctx.scene, *ctx.prefabs, e)) {
            ImGui::SameLine();
            ImGui::TextColored(themeColor::Prefab, "*"); // Prefab 色 = 「プレハブ由来」(名前は歴史的経緯)
        }
    }
    if (tg.multi) {
        ImGui::TextDisabled("%zu entities selected — edits apply to all (gizmo: primary only)",
                            tg.fids.size());
    } else {
        ImGui::TextDisabled("Entity %u:%u  (fileId %llu)", e.index, e.generation,
                            static_cast<unsigned long long>(tg.fid));
    }
}

// プレハブバー (Revert All / Apply All)。プレハブ由来のエンティティのときだけ
void InspectorWindow::DrawPrefabBar(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                    const InspectorTargets& tg)
{
    if (!tg.isPrefabMember) {
        return;
    }
    World& world = ctx.scene->GetWorld();
    auto* inst = world.GetComponent<PrefabInstanceComponent>(tg.prefabRoot);
    const PrefabAsset* asset = inst ? ctx.prefabs->Get(inst->prefabHash) : nullptr;
    ImGui::TextColored(themeColor::Prefab, "Prefab: %s", asset ? asset->name.c_str() : "(missing)");
    const uint64_t rootFid = ctx.scene->EnsureFileId(tg.prefabRoot);
    if (ImGui::SmallButton(Tr(StrId::Insp_RevertAll))) {
        undo.Record("Revert Prefab", *ctx.scene, selection, rootFid,
                    UndoStack::StructuralChanges::Apply, [&] {
            Prefab::RevertInstance(*ctx.scene, *ctx.prefabs, rootFid);
        });
    }
    ImGui::SameLine();
    // Apply は他インスタンス・アセットファイルも更新するため Undo 対象外 (Unity 同様)
    if (ImGui::SmallButton(Tr(StrId::Insp_ApplyAll))) {
        Prefab::ApplyInstance(*ctx.scene, *ctx.prefabs, rootFid);
        ctx.scene->GetWorld().ApplyStructuralChanges();
    }
}

// コンポーネント 1 型ぶん。表示するかの判定と、見出し・PushID / PopID はここで持ち、
// 中身は DrawComponentContextMenu / DrawComponentFields / DrawComponentNotes に任せる
void InspectorWindow::DrawComponent(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                    const InspectorTargets& tg, ComponentTypeId t)
{
    World& world = ctx.scene->GetWorld();
    const ComponentDesc& desc = ComponentRegistry::Get().Desc(t);
    if ((desc.flags & kComponentHidden) && t != LocalTransform::sTypeId) {
        return;
    }
    if (t == NameComponent::sTypeId) {
        return; // 上部で表示済み
    }
    // マルチ選択: 全対象が共通に持つコンポーネントだけ表示 (M40a)
    if (tg.multi) {
        for (size_t i = 1; i < tg.ents.size(); ++i) {
            if (!world.HasComponent(tg.ents[i], t)) {
                return;
            }
        }
    }
    InspectorComponentRow row;
    row.type = t;
    row.desc = &desc;
    // この型のバッチ対象 (fileId + コンポーネント実体、[0] = primary)。
    // フィールド編集中に構造変更は起きないためポインタはフレーム内有効
    for (size_t i = 0; i < tg.ents.size(); ++i) {
        if (void* c = world.GetComponentRaw(tg.ents[i], t)) {
            row.fids.push_back(tg.fids[i]);
            row.comps.push_back(c);
        }
    }
    row.managed = ctx.managedHost && ctx.managedHost->IsManagedComponent(t);
    // 構造上書きの状態 (M50c)。Added = インスタンスで追加された comp
    row.addedInInstance = tg.isPrefabMember
        && Prefab::ComponentOverrideState(*ctx.scene, *ctx.prefabs, tg.e, desc.name)
               == Prefab::CompOverride::Added;

    ImGui::PushID(static_cast<int>(t));
    const ComponentUiInfo& ui = ComponentUiFor(desc.name);
    // コンポーネント見出しは Semibold + 8% 増し (テーマ第 3 世代)。フィールド行と
    // 同じ書体・同じサイズだと「どこからが次のコンポーネントか」を色だけで探すことになる。
    // アイコンはカテゴリ色 — ImGui はラベルの部分着色ができないので、可視ラベルを
    // 空にして DrawItemIconLabel が矩形へ直接描く (PopFont より前に呼ぶこと)
    ImGui::PushFont(EditorHeadingFont(), ImGui::GetStyle().FontSizeBase * 1.08f);
    const bool openHeader = ImGui::CollapsingHeader((std::string("###") + desc.name).c_str(),
                                                    ImGuiTreeNodeFlags_DefaultOpen);
    DrawItemIconLabel(ui.icon, ComponentCategoryColor(ui.category),
                      ComponentDisplayName(desc.name), /*framed=*/true);
    ImGui::PopFont();
    if (row.addedInInstance) {
        // ヘッダ右端に「+」バッジ (M50c)。ヘッダは全幅アイテムなので SameLine では
        // 右端に置けない — アイテム矩形へ直接描く (折りたたみ中でも見える)
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(mx.x - ImGui::GetFontSize() - ImGui::GetStyle().FramePadding.x,
                   mn.y + ImGui::GetStyle().FramePadding.y),
            ImGui::GetColorU32(themeColor::Prefab), "+");
    }
    DrawComponentContextMenu(ctx, selection, undo, tg, row);
    if (openHeader) {
        void* comp = row.comps.empty() ? nullptr : row.comps[0];
        // C# スクリプトコンポーネント: フィールドは managed 側が保持 → 専用描画パス
        // (マルチ選択でも primary のみ編集 — managed 状態はエンティティ毎に独立)
        if (comp && row.managed) {
            DrawManagedComponentFields(ctx, t, comp, tg.e);
            ImGui::PopID();
            return;
        }
        if (comp) {
            DrawComponentFields(ctx, selection, undo, tg, row, comp);
        }
        DrawComponentNotes(ctx, tg, row);
    }
    ImGui::PopID();
}

// 見出しの右クリックメニュー (Copy / Paste / Reset / Remove / Revert Added Component)
void InspectorWindow::DrawComponentContextMenu(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                               const InspectorTargets& tg, const InspectorComponentRow& row)
{
    if (!ImGui::BeginPopupContextItem("##comp_ctx")) {
        return;
    }
    const ComponentDesc& desc = *row.desc;
    // 全対象の before/after を取り 1 Undo エントリにするバッチヘルパ (M40a)
    auto batchOp = [&](const char* label, auto&& mutate) {
        undo.Record(label, *ctx.scene, selection, row.fids, UndoStack::StructuralChanges::Apply, mutate);
    };
    // C# コンポーネントはフィールドが managed 側にあるため copy/paste/reset 対象外
    ComponentClipboard& clip = GetComponentClipboard();
    if (ImGui::MenuItem(Tr(StrId::Insp_CopyComponent), nullptr, false, !row.managed && !row.comps.empty())) {
        clip.componentName = desc.name;
        clip.fields = ComponentFieldsToJson(desc, row.comps[0]);
    }
    const bool canPaste = !row.managed && !clip.Empty() && clip.componentName == desc.name;
    if (ImGui::MenuItem(Tr(StrId::Insp_PasteValues), nullptr, false, canPaste)) {
        batchOp("Paste Component", [&] {
            for (void* c : row.comps) {
                ComponentFieldsFromJson(desc, c, clip.fields);
            }
        });
    }
    if (ImGui::MenuItem(Tr(StrId::Insp_ResetComponent), nullptr, false, !row.managed && desc.construct)) {
        batchOp("Reset Component", [&] {
            for (void* c : row.comps) {
                desc.construct(c); // 既定値の書き込み (placement new)
            }
        });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(Tr(StrId::Insp_RemoveComponent))) {
        World& world = ctx.scene->GetWorld();
        batchOp("Remove Component", [&] {
            for (EntityID te : tg.ents) {
                world.RemoveComponentRaw(te, row.type); // 基本コンポーネントは World 側で拒否
            }
        });
    }
    // インスタンスで追加した comp の取り消し (M50c)。Remove と結果は同じだが
    // レコードの "+C" キーも消える (RevertComponent 内)。primary のみ対象
    if (tg.isPrefabMember
        && ImGui::MenuItem(Tr(StrId::Insp_RevertAddedComp), nullptr, false, row.addedInInstance)) {
        undo.Record("Revert Added Component", *ctx.scene, selection, tg.fid,
                    UndoStack::StructuralChanges::None, [&] {
            Prefab::RevertComponent(*ctx.scene, *ctx.prefabs, tg.e, desc.name);
        });
    }
    ImGui::EndPopup();
}

// フィールド行 (リフレクションの表から自動生成) と、行ごとのプレハブオーバーライド表示。
// 末尾に RectTransform の解決済み矩形 (読み取り専用) を足す
void InspectorWindow::DrawComponentFields(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                          const InspectorTargets& tg, const InspectorComponentRow& row,
                                          void* comp)
{
    const ComponentDesc& desc = *row.desc;
    const EntityID e = tg.e;
    // M60b: 関節は type によって意味を持つフィールドが変わる。効かない行を
    // 並べておくのは「軸を弄っても何も起きない」という無言の嘘になるので、
    // その型に効くものだけ出す (Collider の shape 依存より粒度が細かいのは、
    // 関節が 1 コンポーネントで 5 種類を兼ねているため = 決定台帳 1 の代償)
    const bool isJoint = (std::strcmp(desc.name, "Joint") == 0);
    int32_t jointType = jointtype::kBall;
    if (isJoint) {
        for (const FieldDesc& tf : desc.fields) {
            if (std::strcmp(tf.name, "type") == 0) {
                std::memcpy(&jointType,
                            static_cast<const uint8_t*>(comp) + tf.offset,
                            sizeof(int32_t));
                break;
            }
        }
    }
    for (const FieldDesc& f : desc.fields) {
        if (f.flags & kFieldHidden) {
            continue;
        }
        if (isJoint && !JointFieldApplies(jointType, f.name)) {
            continue;
        }
        const bool changed =
            DrawField(ctx, desc.name, comp, f, e, selection, undo, row.fids, row.comps);
        // マルチ選択: primary で編集した値をフィールド単位で他対象へ伝播
        // (バイトコピー — POD リフレクション型のみなので安全)
        if (changed && row.comps.size() > 1 && !(f.flags & kFieldReadOnly)
            && f.type != FieldType::AssetRef && f.type != FieldType::EntityRef) {
            const uint32_t sz = FieldTypeSize(f.type);
            for (size_t i = 1; i < row.comps.size(); ++i) {
                std::memcpy(static_cast<uint8_t*>(row.comps[i]) + f.offset,
                            static_cast<const uint8_t*>(comp) + f.offset, sz);
            }
        }
        // 参照ピッカー / 衝突マスク (M36a) はポップアップ内で自前 Undo を記録するので除外
        const bool ownUndo = f.type == FieldType::AssetRef
            || f.type == FieldType::EntityRef
            || (std::strcmp(desc.name, "Collider") == 0
                && std::strcmp(f.name, "mask") == 0)
            // M59a2: 材料上書きチェックボックスもクリック即確定 = 自前 Undo
            || (std::strcmp(desc.name, "Collider") == 0
                && std::strcmp(f.name, "materialOverrideBits") == 0);
        // (M75a: RectTransform のアンカープリセット 4x4 は自前 Undo だが、同じ行の
        //  DragFloat2 が最後のアイテムなので通常経路のままでよい — DrawField 参照)
        if (!ownUndo) {
            HandleEditUndoMulti(ctx, selection, undo, row.fids, "Modify");
        }
        // ---- プレハブオーバーライド: 右クリック Revert/Apply + マーカー ----
        if (tg.isPrefabMember) {
            // ID は f.name を明示する (M51f)。既定 (最終アイテムの ID) だと
            // ラベルを TextUnformatted で締める field (mask / anchor 9-grid) が
            // ID=0 で IM_ASSERT に落ちる
            if (f.type != FieldType::AssetRef && f.type != FieldType::EntityRef
                && ImGui::BeginPopupContextItem(f.name)) {
                // 追加 comp ("+C") はベースにフィールドが無く RevertField が
                // no-op — 押せるのに何も起きないので disabled にする (M50c)。
                // 構造ごと戻すのはヘッダ右クリックの Revert Added Component
                const bool ov = Prefab::IsFieldOverridden(*ctx.scene, *ctx.prefabs, e,
                                                          desc.name, f)
                    && !row.addedInInstance;
                if (ImGui::MenuItem(Tr(StrId::Insp_RevertToPrefab), nullptr, false, ov)) {
                    undo.Record("Revert Field", *ctx.scene, selection, tg.fid,
                                UndoStack::StructuralChanges::None, [&] {
                        Prefab::RevertField(*ctx.scene, *ctx.prefabs, e, desc.name, f);
                    });
                }
                if (ImGui::MenuItem(Tr(StrId::Insp_ApplyToPrefab))) {
                    Prefab::ApplyInstance(*ctx.scene, *ctx.prefabs,
                                          ctx.scene->EnsureFileId(tg.prefabRoot));
                    ctx.scene->GetWorld().ApplyStructuralChanges();
                }
                ImGui::EndPopup();
            }
            if (Prefab::IsFieldOverridden(*ctx.scene, *ctx.prefabs, e, desc.name, f)) {
                ImGui::SameLine();
                ImGui::TextColored(themeColor::Prefab, "*");
            }
        }
    }
    // M75a: 解決済み矩形 (基準キャンバス上のキャンバス単位) を読み取り専用で出す。
    // Unity が駆動プロパティを灰色で見せるのと同じ役どころ — アンカーを伸縮に
    // したときに「今この要素は何 px なのか」が数値で分かる唯一の場所
    if (std::strcmp(desc.name, "RectTransform") == 0) {
        // M75c: 基準は project_settings の実効値。明示 Canvas の下の要素はその
        // Canvas の単位で出す (Unity の RectTransform の数値と同じ見え方)
        const uilayout::CanvasDesc& def = uilayout::DefaultCanvasDesc();
        const uilayout::UIRect rr = uilayout::ResolveRect(
            ctx.scene->GetWorld(), e, def.referenceW, def.referenceH);
        ImGui::BeginDisabled();
        ImGui::Text(Tr(StrId::Insp_UIResolvedRect), rr.x, rr.y, rr.w, rr.h);
        ImGui::EndDisabled();
        // M75e: 自動レイアウトに上書きされている欄を言葉で示す (Unity は駆動プロパティを
        // 灰色にする)。欄は編集できるままだが、並べられている / 合わせられている間は効かない
        const uint32_t driven = uilayout::LayoutDrivenBits(ctx.scene->GetWorld(), e);
        if ((driven & uilayout::kDrivenByGroup) != 0) {
            const bool dw = (driven & uilayout::kDrivenWidth) != 0;
            const bool dh = (driven & uilayout::kDrivenHeight) != 0;
            const StrId id = (dw && dh) ? StrId::Insp_UIDrivenGroupSize
                : dw                    ? StrId::Insp_UIDrivenGroupWidth
                : dh                    ? StrId::Insp_UIDrivenGroupHeight
                                        : StrId::Insp_UIDrivenGroupPos;
            ImGui::TextDisabled("%s", Tr(id));
        }
        if ((driven & uilayout::kDrivenByFitter) != 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_UIDrivenFitter));
        }
        if ((driven & uilayout::kDrivenBySlider) != 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_UIDrivenSlider)); // M75f
        }
    }
}

// 型ごとの付記 (フィールド行の下)。PartBounds の警告と、カメラの操縦ボタン
void InspectorWindow::DrawComponentNotes(EngineContext& ctx, const InspectorTargets& tg,
                                         const InspectorComponentRow& row)
{
    const ComponentDesc& desc = *row.desc;
    World& world = ctx.scene->GetWorld();
    // M50a: PartBounds 単独 (Part 無し) は RaycastParts の収集
    // ({Part, PartBounds, WorldMatrix} の同居アーキタイプ) から黙って外れるため警告
    if (std::strcmp(desc.name, "PartBounds") == 0
        && world.GetComponent<PartComponent>(tg.e) == nullptr) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_BoundsNoPart));
    }
    // カメラ操縦モードの入口。押すと SceneView の飛行操作 (右ドラッグ + WASDQE /
    // ホイール / 中ドラッグ) の**書き込み先**がエディタカメラからこのカメラへ移る。
    // 視点はエディタのまま = 外から見ながら置ける。
    // マルチ選択では出さない — 操縦できるのは 1 台だけで、「どれが対象か」が
    // ボタンからは読めなくなるため
    if (std::strcmp(desc.name, "Camera") == 0 && !tg.multi) {
        CameraPilotState& pilot = GetCameraPilot();
        const bool on = (pilot.fileId == tg.fid);
        // 「別モード」トグルなので地形ブラシと同じ mode 色 (統一規格)
        if (ToolbarToggle(Tr(on ? StrId::Insp_PilotStop : StrId::Insp_PilotCamera), on,
                          nullptr, /*mode=*/true)) {
            pilot.fileId = on ? 0 : tg.fid;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Insp_PilotHint));
    }
}

// 削除されたプレハブコンポーネント (M50c)。
// インスタンスで削除されたベース comp ("-C") の一覧 + Restore。一覧はレコード由来だが
// 実体・ベースと突き合わせ済み (RemovedPrefabComponents) なので no-op 行は出ない
void InspectorWindow::DrawRemovedPrefabComponents(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                                  const InspectorTargets& tg)
{
    if (!tg.isPrefabMember || tg.multi) {
        return;
    }
    const std::vector<std::string> removed =
        Prefab::RemovedPrefabComponents(*ctx.scene, *ctx.prefabs, tg.e);
    if (removed.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextColored(themeColor::Prefab, "%s", Tr(StrId::Insp_RemovedComps));
    for (const std::string& name : removed) {
        ImGui::PushID(name.c_str());
        ImGui::TextDisabled("%s %s", ComponentUiFor(name.c_str()).icon,
                            ComponentDisplayName(name.c_str()));
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(StrId::Insp_RestoreComp))) {
            undo.Record("Restore Component", *ctx.scene, selection, tg.fid,
                        UndoStack::StructuralChanges::None, [&] {
                Prefab::RevertComponent(*ctx.scene, *ctx.prefabs, tg.e, name.c_str());
            });
        }
        ImGui::PopID();
    }
}

// 未知のコンポーネント (M70a)。
// 型が引けていないので編集も削除もできないが、生 JSON のまま保持していて保存でも
// 消えない。**ここに出さないと「消えた」と思って作り直され、型が戻った瞬間に
// 二重になる**。マルチ選択では出さない (どの対象の話か行から読めないため)
void InspectorWindow::DrawUnknownComponents(EngineContext& ctx, const InspectorTargets& tg)
{
    if (tg.multi) {
        return;
    }
    const Scene::UnknownCompSet* unknown = ctx.scene->GetUnknownComponents(tg.fid);
    if (unknown == nullptr) {
        return;
    }
    ImGui::Separator();
    char header[96];
    std::snprintf(header, sizeof(header), Tr(StrId::Insp_UnknownComps),
                  static_cast<int>(unknown->size()));
    ImGui::TextDisabled("%s", header);
    for (const auto& [compName, raw] : *unknown) {
        ImGui::TextDisabled("    %s %s", ICON_FA_CIRCLE_QUESTION, compName.c_str());
    }
    ImGui::TextDisabled("%s", Tr(StrId::Insp_UnknownCompsHint));
}

// Add Component ボタンと、検索付きのポップアップ (カテゴリ順)
void InspectorWindow::DrawAddComponentPopup(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                            const InspectorTargets& tg)
{
    World& world = ctx.scene->GetWorld();
    const ComponentRegistry& reg = ComponentRegistry::Get();
    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Insp_AddComponent), ImVec2(-1, 0))) {
        addComponentFilter_[0] = '\0';
        ImGui::OpenPopup("##add_component");
    }
    if (!ImGui::BeginPopup("##add_component")) {
        return;
    }
    // 検索ボックス (開いた直後にフォーカス)
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##add_filter", ICON_FA_MAGNIFYING_GLASS " Search",
                             addComponentFilter_, sizeof(addComponentFilter_));
    ImGui::Separator();
    // カテゴリ順に列挙 (EditorComponentCatalog)。マッチ行のあるカテゴリだけ見出しを出す
    for (const char* cat : ComponentUiCategories()) {
        bool headerShown = false;
        for (ComponentTypeId t = 0; t < reg.Count(); ++t) {
            const ComponentDesc& desc = reg.Desc(t);
            if (desc.flags & kComponentHidden) {
                continue;
            }
            if (world.HasComponent(tg.e, t)) {
                continue;
            }
            const ComponentUiInfo& info = ComponentUiFor(desc.name);
            if (std::strcmp(info.category, cat) != 0) {
                continue;
            }
            if (!ContainsIgnoreCase(desc.name, addComponentFilter_)
                && !ContainsIgnoreCase(ComponentDisplayName(desc.name), addComponentFilter_)) {
                continue;
            }
            if (!headerShown) {
                ImGui::SeparatorText(ComponentCategoryLabel(cat));
                headerShown = true;
            }
            // アイコンはカテゴリ色の独立アイテムで描く (MenuItem ラベルは部分着色不可)。
            // FA は GlyphMinAdvanceX で等幅化済みなので列が揃う
            ImGui::PushStyleColor(ImGuiCol_Text, ComponentCategoryColor(info.category));
            ImGui::TextUnformatted(info.icon);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::MenuItem(ComponentDisplayName(desc.name))) {
                // マルチ選択: まだ持っていない全対象へ追加 (1 Undo エントリ、M40a)
                undo.BeginRecord("Add Component", selection);
                for (size_t i = 0; i < tg.ents.size(); ++i) {
                    if (!world.HasComponent(tg.ents[i], t)) {
                        undo.CaptureBefore(*ctx.scene, tg.fids[i]);
                    }
                }
                std::vector<uint64_t> addedFids;
                for (size_t i = 0; i < tg.ents.size(); ++i) {
                    if (!world.HasComponent(tg.ents[i], t)) {
                        world.AddComponentRaw(tg.ents[i], t);
                        addedFids.push_back(tg.fids[i]);
                    }
                }
                world.ApplyStructuralChanges();
                for (uint64_t af : addedFids) {
                    undo.CaptureAfter(*ctx.scene, af);
                }
                undo.EndRecord(selection);
            }
        }
    }
    ImGui::EndPopup();
}

// スクリプト D&D 受け皿 (M31): パネル残余をターゲット化して .cs をアタッチ。
// AssetBrowser/SceneView の .cs をここへドロップすると表示中エンティティに付与される
void InspectorWindow::DrawScriptDropTarget(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                           const InspectorTargets& tg)
{
    const ImVec2 dropAvail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(dropAvail.x, dropAvail.y > 48.0f ? dropAvail.y : 48.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::wstring path = Utf8ToWide(static_cast<const char*>(pa->Data));
            if (AssetDatabase::ClassifyPath(path) == AssetType::Script) {
                AttachScriptToEntity(ctx, selection, undo, path, tg.e);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

bool InspectorWindow::DrawField(EngineContext& ctx, const char* componentName, void* comp,
                                const FieldDesc& field, EntityID entity, Selection& selection,
                                UndoStack& undo, const std::vector<uint64_t>& fids,
                                const std::vector<void*>& comps)
{
    void* p = static_cast<uint8_t*>(comp) + field.offset;
    const bool readOnly = (field.flags & kFieldReadOnly) != 0;
    if (readOnly) {
        ImGui::BeginDisabled();
    }

    // FieldDesc メタデータ (M8): 0 は「既定」。min==max は範囲無効 (クランプ無し)
    const float speed = (field.dragSpeed > 0.0f) ? field.dragSpeed : 0.05f;
    const float lo = field.minVal;
    const float hi = field.maxVal;

    // M47c: 表示名 + "###" + 英語名。"###" 以降が ImGui の ID なので、言語を切り替えても
    // widget の ID は変わらず、同一コンポーネント内で表示名が重複しても衝突しない。
    // 英語モードや displayName 未設定 (スクリプト由来のフィールド) は name をそのまま使う
    const char* labelText = FieldLabelText(field); // 表示専用 (ID を持たない Text 系)
    char labelBuf[192];
    const char* label = MakeFieldLabel(field, labelBuf, sizeof(labelBuf));

    bool changed = false;
    switch (field.type) {
    case FieldType::Float:
        changed = ImGui::DragFloat(label, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Int32:
        // M60b: 関節の種類は「どの拘束ブロックを立てるか」のプリセットなので、
        // 生の整数ではなく名前で選ばせる。**並び順 = 値 0..4** で、順序を変えると
        // 既存シーンの意味が変わる (登録順で TypeId が決まるのと同じ性質)
        if (componentName && std::strcmp(componentName, "Joint") == 0
            && std::strcmp(field.name, "type") == 0) {
            const char* labels[5] = { Tr(StrId::Insp_JointBall), Tr(StrId::Insp_JointHinge),
                                      Tr(StrId::Insp_JointFixed), Tr(StrId::Insp_JointSlider),
                                      Tr(StrId::Insp_JointCone) };
            int v = *static_cast<int*>(p);
            if (v < 0 || v > 4) {
                v = -1; // 未知の値は黙って 0 へ潰さない (別バージョンのシーンを開いたとき)
            }
            changed = ImGui::Combo(label, &v, labels, 5);
            if (changed) {
                *static_cast<int*>(p) = v;
            }
        } else if (componentName && std::strcmp(componentName, "Collider") == 0
            && std::strcmp(field.name, "layer") == 0) {
            PhysicsLayerNames& ln = PhysicsLayerNames::Get();
            ln.Load(ctx.assetsRoot);
            const char* labels[PhysicsLayerNames::kCount];
            ln.BuildComboLabels(labels);
            int v = *static_cast<int*>(p);
            if (v < 0 || v >= PhysicsLayerNames::kCount) {
                v = -1;
            }
            changed = ImGui::Combo(label, &v, labels, PhysicsLayerNames::kCount);
            if (changed) {
                *static_cast<int*>(p) = v;
            }
        } else if (const EnumFieldLabels* ef = FindEnumLabels(componentName, field.name)) {
            int v = *static_cast<int*>(p);
            if (v < 0 || v >= ef->count) {
                v = -1; // 範囲外は "(invalid)" 表示 (値は選択されるまで保持)
            }
            changed = ImGui::Combo(label, &v, ef->Display(), ef->count);
            if (changed) {
                *static_cast<int*>(p) = v;
            }
        } else {
            changed = ImGui::DragInt(label, static_cast<int*>(p), 1.0f, static_cast<int>(lo),
                                     static_cast<int>(hi));
        }
        break;
    case FieldType::UInt32:
        // M36a: 衝突マスクはレイヤー名チェックリストのポップアップ (Undo は自前記録 —
        // 呼び出し側の HandleEditUndo からは除外されている)
        if (componentName && std::strcmp(componentName, "Collider") == 0
            && std::strcmp(field.name, "mask") == 0) {
            uint32_t& m = *static_cast<uint32_t*>(p);
            PhysicsLayerNames& ln = PhysicsLayerNames::Get();
            ln.Load(ctx.assetsRoot);
            char summary[48];
            if (m == 0xFFFFFFFFu) {
                std::snprintf(summary, sizeof(summary), "%s", Tr(StrId::Insp_Everything));
            } else if (m == 0u) {
                std::snprintf(summary, sizeof(summary), "%s", Tr(StrId::Insp_Nothing));
            } else {
                std::snprintf(summary, sizeof(summary), Tr(StrId::Insp_MaskMixed), m);
            }
            if (ImGui::Button(summary)) {
                ImGui::OpenPopup("##collider_mask");
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(labelText);
            if (ImGui::BeginPopup("##collider_mask")) {
                auto applyMask = [&](uint32_t next) {
                    // マルチ選択は全対象へバッチ適用 (1 Undo エントリ、M40a)
                    undo.Record("Modify Mask", *ctx.scene, selection, fids,
                                UndoStack::StructuralChanges::None, [&] {
                        for (void* c : comps) {
                            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(c) + field.offset) = next;
                        }
                    });
                    changed = true;
                };
                if (ImGui::SmallButton(Tr(StrId::Insp_MaskAll))) {
                    applyMask(0xFFFFFFFFu);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr(StrId::Insp_MaskNone))) {
                    applyMask(0u);
                }
                ImGui::Separator();
                for (int i = 0; i < PhysicsLayerNames::kCount; ++i) {
                    bool on = ((m >> i) & 1u) != 0u;
                    ImGui::PushID(i);
                    if (ImGui::Checkbox(ln.Name(i), &on)) {
                        applyMask(m ^ (1u << i));
                    }
                    ImGui::PopID();
                    if ((i % 2) == 0 && i + 1 < PhysicsLayerNames::kCount) {
                        ImGui::SameLine(180.0f); // 2 列表示
                    }
                }
                ImGui::EndPopup();
            }
        } else if (componentName && std::strcmp(componentName, "Collider") == 0
                   && std::strcmp(field.name, "materialOverrideBits") == 0) {
            // M59a2: 材料の上書きはプロパティ別チェックボックス (mask と同じ自前 Undo 方式 —
            // クリック即確定なので呼び出し側の HandleEditUndoMulti からは除外されている)
            uint32_t& bits = *static_cast<uint32_t*>(p);
            auto applyBits = [&](uint32_t next) {
                undo.Record("Modify Material Override", *ctx.scene, selection, fids,
                            UndoStack::StructuralChanges::None, [&] {
                    for (void* c : comps) {
                        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(c) + field.offset) = next;
                    }
                });
                changed = true;
            };
            ImGui::PushID(field.name);
            bool fricOn = (bits & kPhysMatOverrideFriction) != 0u;
            if (ImGui::Checkbox(Tr(StrId::Insp_PmOvFriction), &fricOn)) {
                applyBits(bits ^ kPhysMatOverrideFriction);
            }
            ImGui::SameLine();
            bool restOn = (bits & kPhysMatOverrideRestitution) != 0u;
            if (ImGui::Checkbox(Tr(StrId::Insp_PmOvRestitution), &restOn)) {
                applyBits(bits ^ kPhysMatOverrideRestitution);
            }
            ImGui::PopID();
        } else {
            changed = ImGui::InputScalar(label, ImGuiDataType_U32, p);
        }
        break;
    case FieldType::UInt64:
        // M48i: 部位タグは project_settings.json の名前表から選ぶ。
        // **保存されるのは名前ハッシュ (u64)** なので、表に無い ID もそのまま維持する
        // (別プロジェクトのシーンを開いたときに黙って 0 に潰さない)
        if (componentName && std::strcmp(componentName, "Part") == 0
            && std::strcmp(field.name, "tag") == 0) {
            PartTagNames& pt = PartTagNames::Get();
            pt.Load(ctx.assetsRoot);
            uint64_t& tag = *static_cast<uint64_t*>(p);
            char unknown[48];
            const char* preview = (tag == 0) ? Tr(StrId::Insp_PartTagNone) : pt.NameOf(tag);
            if (preview == nullptr) {
                std::snprintf(unknown, sizeof(unknown), Tr(StrId::Insp_PartTagUnknown),
                              static_cast<unsigned long long>(tag));
                preview = unknown;
            }
            if (ImGui::BeginCombo(label, preview)) {
                if (ImGui::Selectable(Tr(StrId::Insp_PartTagNone), tag == 0)) {
                    tag = 0;
                    changed = true;
                }
                for (int i = 0; i < pt.Count(); ++i) {
                    ImGui::PushID(i); // タグ名が重複していても ImGui ID を分ける
                    const uint64_t id = pt.Id(i);
                    if (ImGui::Selectable(pt.Name(i), id == tag)) {
                        tag = id;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                ImGui::TextDisabled("%s", Tr(StrId::Insp_PartTagHint));
                ImGui::EndCombo();
            }
        } else {
            changed = ImGui::InputScalar(label, ImGuiDataType_U64, p);
        }
        break;
    case FieldType::Bool: {
        bool b = *static_cast<uint8_t*>(p) != 0;
        if (ImGui::Checkbox(label, &b)) {
            *static_cast<uint8_t*>(p) = b ? 1 : 0;
            changed = true;
        }
        break;
    }
    case FieldType::Float2:
        if (componentName && std::strcmp(componentName, "RectTransform") == 0
            && std::strcmp(field.name, "anchorMin") == 0) {
            // M75a: Unity の Anchor Presets 相当の 4x4 (列 = 左/中/右/伸縮、行 = 上/中/下/伸縮)。
            // クリック即確定で anchorMin と anchorMax を**同時に**書くので Undo は自前で
            // 1 エントリ記録する (M51f の 9-grid と同型)。右隣の DragFloat2 (anchorMin の生値)
            // は最後のアイテムなので呼び出し側の HandleEditUndoMulti がそのまま効く
            const bool ja = CurrentLanguage() != Lang::En;
            const char* const* colNames = ja ? kUIPresetColJa : kUIPresetColLabels;
            const char* const* rowNames = ja ? kUIPresetRowJa : kUIPresetRowLabels;
            const auto* cur = static_cast<const RectTransformComponent*>(comp);
            ImGui::BeginGroup();
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    if (col > 0) {
                        ImGui::SameLine(0.0f, 3.0f);
                    }
                    ImGui::PushID(row * 4 + col);
                    const float minX = (col == 1) ? 0.5f : (col == 2) ? 1.0f : 0.0f;
                    const float maxX = (col == 3) ? 1.0f : minX;
                    const float minY = (row == 1) ? 0.5f : (row == 2) ? 1.0f : 0.0f;
                    const float maxY = (row == 3) ? 1.0f : minY;
                    const bool sel = cur->anchorMin.x == minX && cur->anchorMax.x == maxX
                        && cur->anchorMin.y == minY && cur->anchorMax.y == maxY;
                    if (sel) {
                        const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                        ImGui::PushStyleColor(ImGuiCol_Button, c);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
                    }
                    if (ImGui::Button("##preset", ImVec2(18, 18)) && !sel) {
                        undo.Record("Modify", *ctx.scene, selection, fids,
                                    UndoStack::StructuralChanges::None, [&] {
                            for (void* c2 : comps) {
                                auto* rt = static_cast<RectTransformComponent*>(c2);
                                rt->anchorMin = { minX, minY };
                                rt->anchorMax = { maxX, maxY };
                            }
                        });
                        changed = true;
                    }
                    if (sel) {
                        ImGui::PopStyleColor(2);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s / %s", rowNames[row], colNames[col]);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndGroup();
        }
        changed = ImGui::DragFloat2(label, static_cast<float*>(p), speed, lo, hi) || changed;
        break;
    case FieldType::Float3:
        // サイズの比率固定: チェック中は 1 軸の編集で他 2 軸を同率スケール。
        // チェックボックスは DragFloat3 より**先**に描く — 呼び出し側の HandleEditUndoMulti
        // は直前 1 アイテムしか見ないので、最後のアイテムを編集本体にしておく必要がある
        if (componentName && std::strcmp(componentName, "LocalTransform") == 0
            && std::strcmp(field.name, "scale") == 0) {
            ImGui::Checkbox("##scale_link", &scaleLinked_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", Tr(StrId::Insp_ScaleLink));
            }
            ImGui::SameLine(0.0f, 3.0f);
            auto* v = static_cast<XMFLOAT3*>(p);
            const XMFLOAT3 prev = *v;
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 3.0f);
            changed = ImGui::DragFloat3(label, &v->x, speed, lo, hi);
            const bool usingCache =
                scaleLinkEditing_ && scaleLinkEntity_ == entity && scaleLinkField_ == p;
            if (scaleLinked_ && changed) {
                const XMFLOAT3 base = usingCache ? scaleLinkBase_ : prev;
                int axis = -1; // Drag/入力は 1 フレーム 1 軸しか変えない
                if (v->x != prev.x) {
                    axis = 0;
                } else if (v->y != prev.y) {
                    axis = 1;
                } else if (v->z != prev.z) {
                    axis = 2;
                }
                if (axis >= 0) {
                    const float b = (&base.x)[axis];
                    const float n = (&v->x)[axis];
                    if (std::fabs(b) > 1e-12f) {
                        const float r = n / b;
                        for (int i = 0; i < 3; ++i) {
                            if (i != axis) {
                                (&v->x)[i] = (&base.x)[i] * r;
                            }
                        }
                    } else {
                        *v = { n, n, n }; // 基準 0 は比率が定義できない — 一様値に落とす
                    }
                }
            }
            if (ImGui::IsItemActive()) {
                if (!usingCache) {
                    scaleLinkBase_ = prev;
                    scaleLinkEntity_ = entity;
                    scaleLinkField_ = p;
                    scaleLinkEditing_ = true;
                }
            } else if (usingCache) {
                scaleLinkEditing_ = false;
            }
        } else {
            changed = ImGui::DragFloat3(label, static_cast<float*>(p), speed, lo, hi);
        }
        break;
    case FieldType::Float4:
        changed = ImGui::DragFloat4(label, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Quat: {
        // オイラー角 (度) で編集。編集中はキャッシュを使い往復変換ドリフトを防ぐ
        auto* q = static_cast<XMFLOAT4*>(p);
        XMFLOAT3 euler;
        const bool usingCache = eulerEditing_ && eulerCacheEntity_ == entity && eulerCacheField_ == p;
        euler = usingCache ? eulerCache_ : QuatToEulerDeg(*q);
        if (ImGui::DragFloat3(label, &euler.x, 0.5f)) {
            *q = EulerDegToQuat(euler);
            changed = true;
        }
        if (ImGui::IsItemActive()) {
            eulerEditing_ = true;
            eulerCache_ = euler;
            eulerCacheEntity_ = entity;
            eulerCacheField_ = p;
        } else if (usingCache) {
            eulerEditing_ = false;
        }
        break;
    }
    case FieldType::Color:
        changed = ImGui::ColorEdit4(label, static_cast<float*>(p));
        break;
    case FieldType::EntityRef:
        DrawEntityRef(ctx, field, p, selection, undo, fids, comps, field.offset);
        break;
    case FieldType::AssetRef:
        DrawAssetRef(ctx, field, p, selection, undo, fids, comps, field.offset);
        break;
    case FieldType::String64:
        // M48i: 部位のジョイントは供給元スケルトンの名前から選ぶ (M48a の名前保持が効く)。
        // モデルが解決できないときは自由入力にフォールバックする — 骨がまだ登録されていない
        // だけの場合に、既に入っている正しい名前を消させないため
        if (componentName && std::strcmp(componentName, "Part") == 0
            && std::strcmp(field.name, "joint") == 0) {
            char* joint = static_cast<char*>(p);
            World& w = ctx.scene->GetWorld();
            const EntityID src = Parts::ResolvePartSource(
                w, entity, static_cast<const PartComponent*>(comp)->source);
            const auto* sm = src.IsNull() ? nullptr : w.GetComponent<SkinnedMeshComponent>(src);
            const SkinnedModel* model =
                (sm && ctx.resources) ? ctx.resources->skinnedModels.Get(sm->model) : nullptr;
            if (model == nullptr) {
                changed = ImGui::InputText(label, joint, 64);
                if (changed) {
                    ZeroStringTail(joint, 64);
                }
                ImGui::TextDisabled("%s", Tr(StrId::Insp_PartNoSkin));
            } else {
                char missing[96];
                const char* preview = Tr(StrId::Insp_PartJointStatic);
                if (joint[0] != '\0') {
                    preview = joint;
                    if (model->FindJointByName(joint) < 0) {
                        std::snprintf(missing, sizeof(missing),
                                      Tr(StrId::Insp_PartJointMissing), joint);
                        preview = missing;
                    }
                }
                if (ImGui::BeginCombo(label, preview)) {
                    if (ImGui::Selectable(Tr(StrId::Insp_PartJointStatic), joint[0] == '\0')) {
                        std::memset(joint, 0, 64); // 生バイトが hash 対象なので末尾までゼロ埋め
                        changed = true;
                    }
                    for (size_t i = 0; i < model->joints.size(); ++i) {
                        const std::string& n = model->joints[i].name;
                        if (n.empty()) {
                            continue; // 無名ジョイントは FindJointByName の対象外
                        }
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(n.c_str(), n == joint)) {
                            std::memset(joint, 0, 64);
                            std::snprintf(joint, 64, "%s", n.c_str());
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                // v1 規約: 骨追従の部位は供給元の直子。破っていると実行時に黙って skip される
                if (joint[0] != '\0' && w.GetParent(entity) != src) {
                    ImGui::TextDisabled("%s", Tr(StrId::Insp_PartNotChild));
                }
            }
        } else {
            changed = ImGui::InputText(label, static_cast<char*>(p), 64);
            if (changed) {
                ZeroStringTail(static_cast<char*>(p), 64);
            }
        }
        break;
    case FieldType::String256:
        changed = ImGui::InputText(label, static_cast<char*>(p), 256);
        if (changed) {
            ZeroStringTail(static_cast<char*>(p), 256);
        }
        break;
    case FieldType::Float4x4: {
        const float* m = static_cast<const float*>(p);
        ImGui::Text("%s:", labelText);
        for (int r = 0; r < 4; ++r) {
            ImGui::Text("  %8.3f %8.3f %8.3f %8.3f", m[r * 4], m[r * 4 + 1], m[r * 4 + 2],
                        m[r * 4 + 3]);
        }
        break;
    }
    }

    if (field.tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", field.tooltip);
    }
    if (readOnly) {
        ImGui::EndDisabled();
    }
    return changed;
}

void InspectorWindow::DrawAssetInspector(EngineContext& ctx, Selection& selection,
                                         AssetPreviewCache& preview)
{
    namespace fs = std::filesystem;
    const std::wstring path = selection.assetPath;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_AssetGone));
        return;
    }
    const AssetType type = AssetDatabase::ClassifyPath(path);
    const std::string nameU = WideToUtf8(fs::path(path).filename().wstring());
    ImGui::TextUnformatted(nameU.c_str());
    ImGui::TextDisabled("%s asset", AssetDatabase::TypeName(type));
    const uint64_t guid = ctx.assetDb ? ctx.assetDb->GuidForPath(path, /*createIfMissing=*/false)
                                      : 0;
    if (guid != 0) {
        ImGui::TextDisabled("GUID %016llx", static_cast<unsigned long long>(guid));
    }
    // assets ルート相対で表示 (絶対パスは長すぎる)
    {
        const std::wstring key = NormalizePathKey(path);
        const std::wstring rootKey = NormalizePathKey(ctx.assetsRoot);
        std::string rel = WideToUtf8(path);
        if (key.size() > rootKey.size() && key.compare(0, rootKey.size(), rootKey) == 0) {
            rel = "assets" + WideToUtf8(key.substr(rootKey.size()));
        }
        ImGui::TextDisabled("%s", rel.c_str());
    }
    ImGui::Separator();

    // 選択パスが変わったら編集キャッシュを .meta / .mat.json から再読込
    if (assetEditPath_ != path) {
        assetEditPath_ = path;
        AssetMeta meta;
        AssetDatabase::ReadMeta(path + L".meta", meta);
        assetImportEdit_ = meta.tex;
        if (type == AssetType::Material) {
            LoadMaterialEdit(ctx, path); // M40d
        }
        if (type == AssetType::Sound) {
            LoadSoundEdit(path); // M45c
        }
        if (type == AssetType::PhysMat) {
            LoadPhysMatEdit(path); // M59a1
        }
        if ((type == AssetType::Actor || type == AssetType::Prefab) && ctx.prefabs) {
            // 選択が変わったときだけ登録を試す (エディタ起動後に外から置かれたファイルを拾う)。
            // 毎フレーム LoadFromFile すると不正ファイルで警告ログを撒き続ける
            const uint64_t h = PrefabLibrary::HashForPath(path);
            if (!ctx.prefabs->Contains(h)) {
                ctx.prefabs->LoadFromFile(path);
            }
        }
    }

    if (type == AssetType::Actor || type == AssetType::Prefab) {
        // 構成アセット (M48d): 読み取り専用の要約。中身の編集はミニシーン編集モード (M48k) で
        const PrefabAsset* a =
            ctx.prefabs ? ctx.prefabs->Get(PrefabLibrary::HashForPath(path)) : nullptr;
        if (a) {
            int roots = 0;
            for (const nlohmann::json& it : a->entities) {
                if (!it.contains("parent")) {
                    ++roots;
                }
            }
            ImGui::Text(Tr(StrId::Insp_ComposeSummary), static_cast<int>(a->entities.size()), roots);
            if (roots >= 2) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_ComposeMultiRoot));
            }
        } else {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_ComposeInvalid));
        }
        return;
    }

    if (type == AssetType::Material) {
        DrawMaterialInspector(ctx, path, preview); // M40d + M53 プレビュー
        return;
    }

    if (type == AssetType::Sound) {
        DrawSoundInspector(ctx, path); // M45c
        return;
    }

    if (type == AssetType::PhysMat) {
        DrawPhysMatInspector(path); // M59a1
        return;
    }

    if (type == AssetType::Audio) {
        // 素のクリップ: 情報表示 + 試聴。編集対象は .sound.json 側 (ここは読み取り専用)
        if (ctx.audio) {
            const AssetID id = ctx.audio->LoadClipFile(path); // 冪等 (--no-audio なら null)
            if (ImGui::Button(Tr(StrId::Insp_Preview), ImVec2(110, 0))) {
                if (!id.IsNull()) {
                    PlayDesc d;
                    d.clip = id;
                    ctx.audio->Play(d);
                } else {
                    MYE_LOG_WARN("audio device is not available (or clip failed to decode)");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr(StrId::Insp_StopAll), ImVec2(90, 0))) {
                ctx.audio->StopAll();
                ctx.audio->StopMusic(kMusicStopFadeSeconds); // BGM は別レーン (M45f)
            }
        }
        return;
    }

    if (type == AssetType::Texture) {
        // プレビュー (AssetBrowser と同じ非同期サムネ。1x1 はプレースホルダなのでスキップ)
        const AssetID texId = ctx.resources->textures.RequestLoadFileAsync(path);
        if (Texture* tex = ctx.resources->textures.Get(texId);
            tex && tex->srv && tex->width > 1) {
            const float availW = ImGui::GetContentRegionAvail().x;
            float w = (availW < 220.0f) ? availW : 220.0f;
            const float h = w * static_cast<float>(tex->height) / static_cast<float>(tex->width);
            ImGui::Image(reinterpret_cast<ImTextureID>(tex->srv.Get()), ImVec2(w, h));
            ImGui::TextDisabled("%d x %d%s", tex->width, tex->height,
                                tex->srgb ? "  (sRGB)" : "");
        }

        // ---- Import Settings (M39b の統合表示、M40c) ----
        ImGui::SeparatorText(Tr(StrId::Insp_ImportSettings));
        const char* srgbLabels[] = { "Auto (usage hint)", "sRGB (albedo)", "Linear (data)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo(Tr(StrId::Insp_Srgb), &assetImportEdit_.srgb, srgbLabels, 3);
        bool mips = assetImportEdit_.generateMips != 0;
        if (ImGui::Checkbox(Tr(StrId::Insp_GenerateMips), &mips)) {
            assetImportEdit_.generateMips = mips ? 1 : 0;
        }
        const char* compLabels[] = { "Auto (BC1/BC3)", "None (RGBA8)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo(Tr(StrId::Insp_CookCompress), &assetImportEdit_.compress, compLabels, 2);
        if (ImGui::Button(Tr(StrId::Common_Apply), ImVec2(90, 0))) {
            const std::wstring metaPath = path + L".meta";
            AssetDatabase::EnsureMeta(path); // 不在なら生成 (GUID 確定)
            AssetMeta meta;
            if (AssetDatabase::ReadMeta(metaPath, meta)) {
                meta.type = AssetType::Texture;
                meta.tex = assetImportEdit_;
                meta.version = 2;
                AssetDatabase::WriteMeta(metaPath, meta);
                // ロード済みならその場で再ロード (AssetID 不変 = 参照側の再解決不要)
                ctx.resources->textures.ReplaceFromFile(TextureLibrary::IdForFile(path), path);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
            AssetMeta meta;
            AssetDatabase::ReadMeta(path + L".meta", meta);
            assetImportEdit_ = meta.tex;
        }
    }
}

void InspectorWindow::LoadMaterialEdit(EngineContext& ctx, const std::wstring& path)
{
    matEdit_ = MaterialEditState{};
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    matEdit_.name = root.value("name", std::string());
    matEdit_.shader = root.value("shader", std::string("forward_lit"));
    if (root.contains("baseColor") && root["baseColor"].is_array()) {
        const nlohmann::json& c = root["baseColor"];
        for (size_t i = 0; i < c.size() && i < 4; ++i) {
            matEdit_.baseColor[i] = c[i].get<float>();
        }
    }
    matEdit_.metallic = root.value("metallic", 0.0f);
    matEdit_.roughness = root.value("roughness", 0.5f);
    matEdit_.emissive = root.value("emissive", 0.0f); // M46i (欠損 = 発光なし)
    // M67: 反射クラス。ParseMaterialJson と**同じ関数**を呼ぶ (規則は 1 本、M67h)
    // — ここで拾い方がずれると「Inspector に出る値」と「描画に効く値」が食い違う
    matEdit_.reflectionClass = ParseReflectionClassJson(root);
    matEdit_.transparent = root.value("transparent", false);
    // texture/normalMap: 数値 = GUID / 文字列 = 旧相対パス (GUID に変換して保持 —
    // 保存時は常に GUID 数値で書く = M39a の「次回保存で guid 書き」)
    auto readRef = [&](const char* key) -> uint64_t {
        if (!root.contains(key)) {
            return 0;
        }
        const nlohmann::json& node = root[key];
        if (node.is_number_unsigned() || node.is_number_integer()) {
            return node.get<uint64_t>();
        }
        if (node.is_string()) {
            const std::string rel = node.get<std::string>();
            if (rel.empty()) {
                return 0;
            }
            const std::wstring abs = ctx.assetsRoot + L"\\" + Utf8ToWide(rel);
            return ctx.assetDb ? ctx.assetDb->GuidForPath(abs, /*createIfMissing=*/false) : 0;
        }
        return 0;
    };
    matEdit_.textureGuid = readRef("texture");
    matEdit_.normalGuid = readRef("normalMap");
    matEdit_.valid = true;
}

std::string InspectorWindow::MaterialEditToJson(const std::wstring& path) const
{
    namespace fs = std::filesystem;
    nlohmann::json root;
    root["engine"] = "MyEngine";
    root["material"] = 1;
    root["name"] = matEdit_.name.empty() ? WideToUtf8(fs::path(path).stem().stem().wstring())
                                         : matEdit_.name;
    root["shader"] = matEdit_.shader;
    root["baseColor"] = { matEdit_.baseColor[0], matEdit_.baseColor[1], matEdit_.baseColor[2],
                          matEdit_.baseColor[3] };
    root["metallic"] = matEdit_.metallic;
    root["roughness"] = matEdit_.roughness;
    root["emissive"] = matEdit_.emissive;                  // M46i
    root["reflectionClass"] = matEdit_.reflectionClass;    // M67
    // サブ参照は GUID 数値で書く (M39a)。0 = 空文字列 (従来互換の「なし」)
    if (matEdit_.textureGuid != 0) {
        root["texture"] = matEdit_.textureGuid;
    } else {
        root["texture"] = "";
    }
    if (matEdit_.normalGuid != 0) {
        root["normalMap"] = matEdit_.normalGuid;
    } else {
        root["normalMap"] = "";
    }
    root["transparent"] = matEdit_.transparent;
    return root.dump(2);
}

void InspectorWindow::DrawMaterialInspector(EngineContext& ctx, const std::wstring& path,
                                            AssetPreviewCache& preview)
{
    namespace fs = std::filesystem;
    if (!matEdit_.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_MaterialFailed));
        return;
    }

    // ---- ライブプレビュー (M53) ----
    // 保存する JSON をそのまま Material に組み直して描く = 「保存したらこう見える」が保証される。
    // JSON 本文のハッシュを同一性キーにしているので、フィールドを足しても取りこぼしが出ない
    const std::string matJson = MaterialEditToJson(path);
    const uint64_t jsonHash = HashStr(matJson);
    if (jsonHash != matPreviewHash_) {
        Material built;
        if (MaterialLibrary::MaterialFromJsonText(matJson, ctx.resources->textures, ctx.assetsRoot,
                                                  built)) {
            matPreviewMat_ = built;
            matPreviewHash_ = jsonHash;
        }
    }
    if (matPreviewHash_ != 0) {
        const auto shape = static_cast<PreviewShape>(matPreviewShape_);
        ID3D11ShaderResourceView* srv =
            preview.GetOrRequestMaterial(ctx, matPreviewMat_, shape, matPreviewHash_);
        const float side = static_cast<float>(AssetPreviewCache::kPreviewSize);
        if (srv) {
            ImGui::Image(reinterpret_cast<ImTextureID>(srv), ImVec2(side, side));
        } else {
            ImGui::Dummy(ImVec2(side, side)); // 生成待ち (次フレームに絵が入る)
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        const char* shapeItems[] = { Tr(StrId::Insp_ShapeSphere), Tr(StrId::Insp_ShapeCube),
                                     Tr(StrId::Insp_ShapePlane) };
        static_assert(static_cast<int>(PreviewShape::Count) == 3,
                      "shapeItems は PreviewShape と同数にすること");
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo(Tr(StrId::Insp_PreviewShape), &matPreviewShape_, shapeItems, 3);
        ImGui::EndGroup();
        // 注記は画像の**下**に全幅で折り返す。右に置くと Inspector の幅で右端が切れる
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Tr(StrId::Insp_PreviewNote));
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText(Tr(StrId::Insp_Material));
    ImGui::TextDisabled("shader: %s", matEdit_.shader.c_str());
    ImGui::ColorEdit4("baseColor", matEdit_.baseColor);
    ImGui::SliderFloat(Tr(StrId::Mat_Metallic), &matEdit_.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Mat_Roughness), &matEdit_.roughness, 0.0f, 1.0f);
    // M46i: 自己発光。放射輝度 = baseColor * emissive。Deferred は G-Buffer へ
    // kEmissiveMaxIntensity 正規化で詰めるので、それを超える値は頭打ちになる
    ImGui::SliderFloat(Tr(StrId::Mat_Emissive), &matEdit_.emissive, 0.0f,
                       static_cast<float>(kEmissiveMaxIntensity));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Insp_TipEmissive));
    }
    // M67: 反射に映るときの品質クラス。**このマテリアルの面が反射像に写るときの扱い**で、
    // このマテリアルが「何を映すか」ではない (Inspector で最も誤解されやすい点なので
    // ツールチップで明示する)
    {
        const char* classItems[] = { Tr(StrId::ReflClass_Hero), Tr(StrId::ReflClass_Character),
                                     Tr(StrId::ReflClass_Vehicle), Tr(StrId::ReflClass_Prop),
                                     Tr(StrId::ReflClass_Default) };
        static_assert(sizeof(classItems) / sizeof(classItems[0]) == kRtReflClassCount,
                      "classItems は kRtReflClassCount と同数にすること");
        ImGui::Combo(Tr(StrId::Mat_ReflClass), &matEdit_.reflectionClass, classItems,
                     kRtReflClassCount);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_TipReflClass));
        }
    }
    ImGui::Checkbox(Tr(StrId::Insp_Transparent), &matEdit_.transparent);

    // テクスチャピッカー (GUID 参照、M39a)。assets 配下の画像をサムネ付きで列挙
    auto texPicker = [&](const char* label, uint64_t& guidRef) {
        ImGui::PushID(label);
        std::string cur = Tr(StrId::Insp_NoneItem);
        if (guidRef != 0) {
            const std::wstring resolved = assetguid::ResolvePath(guidRef);
            if (!resolved.empty()) {
                cur = WideToUtf8(fs::path(resolved).filename().wstring());
            } else {
                char hex[24];
                std::snprintf(hex, sizeof(hex), "%016llx",
                              static_cast<unsigned long long>(guidRef));
                cur = std::string("(missing ") + hex + ")";
            }
        }
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
        if (ImGui::Button(cur.c_str(), ImVec2(-1, 0))) {
            ImGui::OpenPopup("##mat_tex_pick");
        }
        if (ImGui::BeginPopup("##mat_tex_pick")) {
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
                guidRef = 0;
            }
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(ctx.assetsRoot, ec)) {
                if (!e.is_regular_file(ec)) {
                    continue;
                }
                const std::wstring p = e.path().wstring();
                if (AssetDatabase::IsMetaPath(p)
                    || AssetDatabase::ClassifyPath(p) != AssetType::Texture) {
                    continue;
                }
                ImGui::PushID(WideToUtf8(p).c_str());
                // サムネイル (非同期。プレースホルダ中は白)
                const AssetID tid = ctx.resources->textures.RequestLoadFileAsync(p);
                if (Texture* tex = ctx.resources->textures.Get(tid); tex && tex->srv) {
                    ImGui::Image(reinterpret_cast<ImTextureID>(tex->srv.Get()),
                                 ImVec2(20, 20));
                    ImGui::SameLine();
                }
                const std::string rel =
                    WideToUtf8(fs::relative(e.path(), ctx.assetsRoot, ec).wstring());
                if (ImGui::Selectable(rel.c_str())) {
                    guidRef = AssetDatabase::EnsureMeta(p); // .meta 不在なら生成 (GUID 確定)
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    };
    texPicker("texture", matEdit_.textureGuid);
    texPicker("normalMap", matEdit_.normalGuid);

    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        std::ofstream out(std::filesystem::path(path), std::ios::binary);
        if (out) {
            out << matJson; // プレビューが描いたのと**同じ本文**
            out.close();
            // 即時反映: 同一 AssetID のまま再ロード → 全ビューの MeshRenderer に反映
            ctx.resources->materials.LoadFromFile(path, ctx.resources->textures,
                                                  ctx.assetsRoot);
            MYE_LOG_INFO("material saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("could not write material: %s", WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadMaterialEdit(ctx, path);
    }
}

void InspectorWindow::LoadSoundEdit(const std::wstring& path)
{
    soundEdit_ = SoundAsset{};
    soundEditValid_ = false;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    soundEditValid_ = SoundLibrary::FromJson(root, soundEdit_);
}

void InspectorWindow::DrawSoundInspector(EngineContext& ctx, const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!soundEditValid_) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_SoundFailed));
        return;
    }

    // ---- 試聴 (先頭バリエーション・揺らぎ無しで固定 = 何を聴いているか分かる) ----
    if (ImGui::Button(Tr(StrId::Insp_Preview), ImVec2(110, 0))) {
        if (ctx.audio) {
            PreviewSound(*ctx.audio, soundEdit_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_StopAll), ImVec2(90, 0))) {
        if (ctx.audio) {
            ctx.audio->StopAll();
            ctx.audio->StopMusic(kMusicStopFadeSeconds); // BGM は別レーン (M45f)
        }
    }
    if (soundEdit_.stream) {
        // BGM は voice プールに載らないので、試聴の挙動が SE と違うことを明示する
        ImGui::TextDisabled("%s", Tr(StrId::Insp_StreamNote));
    }
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PreviewNote));

    // ---- バリエーション ----
    ImGui::SeparatorText(Tr(StrId::Insp_Variations));
    int removeAt = -1;
    for (size_t i = 0; i < soundEdit_.variations.size(); ++i) {
        SoundVariation& v = soundEdit_.variations[i];
        ImGui::PushID(static_cast<int>(i));
        std::string cur = Tr(StrId::Insp_NoneItem);
        if (v.clip != 0) {
            const std::wstring resolved = assetguid::ResolvePath(v.clip);
            if (!resolved.empty()) {
                cur = WideToUtf8(fs::path(resolved).filename().wstring());
            } else if (!v.clipPath.empty()) {
                cur = v.clipPath;
            } else {
                char hex[24];
                std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(v.clip));
                cur = std::string("(missing ") + hex + ")";
            }
        } else if (!v.clipPath.empty()) {
            cur = v.clipPath + " (unresolved)";
        }
        if (ImGui::Button(cur.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
            ImGui::OpenPopup("##clip_pick");
        }
        if (ImGui::BeginPopup("##clip_pick")) {
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
                v.clip = 0;
                v.clipPath.clear();
            }
            // ロード済みクリップ = assets\**\*.wav|*.ogg (RegisterAssetLibraries が起動時に登録)
            if (ctx.audio) {
                for (const AssetEntry& e : ctx.audio->Enumerate()) {
                    if (ImGui::Selectable(e.name.c_str(), e.id.value == v.clip)) {
                        v.clip = e.id.value;
                        v.clipPath.clear();
                    }
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragInt(Tr(StrId::Insp_Weight), &v.weight, 0.1f, 0, 100);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        soundEdit_.variations.erase(soundEdit_.variations.begin() + removeAt);
    }
    if (ImGui::Button(Tr(StrId::Insp_AddVariation))) {
        soundEdit_.variations.push_back(SoundVariation{});
    }

    // ---- 2D 再生パラメータ ----
    ImGui::SeparatorText(Tr(StrId::Insp_Playback));
    ImGui::SliderFloat(Tr(StrId::Insp_Volume), &soundEdit_.volume, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_VolumeRandom), &soundEdit_.volumeRandom, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_Pitch), &soundEdit_.pitch, 1.0f / AudioSystem::kMaxFreqRatio,
                       AudioSystem::kMaxFreqRatio);
    ImGui::SliderFloat(Tr(StrId::Insp_PitchRandom), &soundEdit_.pitchRandom, 0.0f, 1.0f);
    ImGui::Checkbox(Tr(StrId::Insp_Loop), &soundEdit_.loop);
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::Insp_StreamBgm), &soundEdit_.stream);
    // バス候補は**実際に張られているミキサー**から採る (バスはデータ駆動、M45d)。
    // 保存は名前なので、未知のバス名は既定バスへ落ちるだけで値自体は壊さない
    if (ctx.audio != nullptr) {
        const int current = ctx.audio->FindBus(soundEdit_.bus.c_str());
        const char* preview = current >= 0 ? ctx.audio->BusName(current) : soundEdit_.bus.c_str();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(Tr(StrId::Insp_Bus), preview)) {
            for (int i = 0; i < ctx.audio->BusCount(); ++i) {
                if (ImGui::Selectable(ctx.audio->BusName(i), i == current)) {
                    soundEdit_.bus = ctx.audio->BusName(i);
                }
            }
            ImGui::EndCombo();
        }
        if (current < 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unknown bus -> %s)", ctx.audio->BusName(ctx.audio->DefaultBus()));
        }
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_Priority), &soundEdit_.priority, 1.0f, 0, 255);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_MaxInstances), &soundEdit_.maxInstances, 0.1f, 0, 64);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PriorityNote));

    // ---- 3D (M45e: X3DAudio が実際にこの値で定位する) ----
    ImGui::SeparatorText(Tr(StrId::Insp_3D));
    ImGui::SliderFloat(Tr(StrId::Insp_SpatialBlend), &soundEdit_.spatialBlend, 0.0f, 1.0f);
    ImGui::DragFloat(Tr(StrId::Insp_MinDistance), &soundEdit_.minDistance, 0.05f, 0.01f, 1000.0f);
    ImGui::DragFloat(Tr(StrId::Insp_MaxDistance), &soundEdit_.maxDistance, 0.5f, 0.02f, 10000.0f);
    {
        int rolloff = static_cast<int>(soundEdit_.rolloff);
        const char* rolloffNames[] = { "Logarithmic", "Linear", "Inverse" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(Tr(StrId::Insp_Rolloff), &rolloff, rolloffNames, 3)) {
            soundEdit_.rolloff = static_cast<SoundRolloff>(rolloff);
        }
    }
    ImGui::SliderFloat(Tr(StrId::Insp_Doppler), &soundEdit_.dopplerScale, 0.0f, 5.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_ReverbSend), &soundEdit_.reverbSend, 0.0f, 1.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_SpatialNote));
    ImGui::TextDisabled("%s", Tr(StrId::Insp_AttenNote));

    // ---- ループ点 (M45f。stream + loop のときだけ意味を持つ) ----
    ImGui::SeparatorText(Tr(StrId::Insp_LoopPoints));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_LoopStart), &soundEdit_.loopStartSample, 8.0f, 0, 1 << 30);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_LoopEnd), &soundEdit_.loopEndSample, 8.0f, 0, 1 << 30);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_LoopEndNote1));
    ImGui::TextDisabled("%s", Tr(StrId::Insp_LoopEndNote2));

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        if (soundEdit_.name.empty()) {
            // "hit.sound.json" → "hit" (stem を 2 回剥がす)
            soundEdit_.name = WideToUtf8(fs::path(path).stem().stem().wstring());
        }
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << SoundLibrary::ToJson(soundEdit_).dump(2);
            out.close();
            // 即時反映: 同一 GUID のまま再ロード (参照側は GUID なので再解決不要)
            if (ctx.sounds) {
                ctx.sounds->LoadFromFile(path);
            }
            MYE_LOG_INFO("sound saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("could not write sound: %s", WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadSoundEdit(path);
    }
}

void InspectorWindow::LoadPhysMatEdit(const std::wstring& path)
{
    physMatEdit_ = PhysMat{};
    physMatEditValid_ = false;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception&) {
        return;
    }
    physMatEditValid_ = PhysMatLibrary::FromJson(root, physMatEdit_); // 読み値は Sanitize 済み
}

void InspectorWindow::DrawPhysMatInspector(const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!physMatEditValid_) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_PhysMatFailed));
        return;
    }

    // ドラッグの min/max は Sanitize と同じ檻 (直接入力のはみ出しは Save 時の Sanitize が
    // 最終防波堤)。ここで編集した値が sim に効き始めるのは M59a2 の Collider 割り当てから
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDensity), &physMatEdit_.density, 10.0f, 0.001f, 1.0e6f,
                     "%.1f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmStaticFriction), &physMatEdit_.staticFriction, 0.01f, 0.0f,
                     100.0f, "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDynamicFriction), &physMatEdit_.dynamicFriction, 0.01f, 0.0f,
                     100.0f, "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRestitution), &physMatEdit_.restitution, 0.005f, 0.0f, 1.0f,
                     "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRollingResistance), &physMatEdit_.rollingResistance, 0.001f,
                     0.0f, 10.0f, "%.4f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDragCoefficient), &physMatEdit_.dragCoefficient, 0.01f, 0.0f,
                     100.0f, "%.3f");
    // M60d: 粘着力 [N]。接触が引っ張る側へ耐えられる上限で、結合則は min (弱いほうが勝つ)
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmAdhesion), &physMatEdit_.adhesion, 0.5f, 0.0f, 1.0e6f,
                     "%.2f");
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PhysMatNote));

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        if (physMatEdit_.name.empty()) {
            // "steel.physmat.json" → "steel" (stem を 2 回剥がす)
            physMatEdit_.name = WideToUtf8(fs::path(path).stem().stem().wstring());
        }
        PhysMatLibrary::Sanitize(physMatEdit_); // 手入力の NaN/範囲外をファイルへ焼かない
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << PhysMatLibrary::ToJson(physMatEdit_).dump(2);
            out.close();
            // 即時反映: 同一 GUID のまま再ロード (参照側は GUID なので再解決不要)
            if (PhysMatLibrary* pm = physmat::Library()) {
                pm->LoadFromFile(path);
            }
            MYE_LOG_INFO("physmat saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("could not write physmat: %s", WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadPhysMatEdit(path);
    }
}

void InspectorWindow::DrawAssetRef(EngineContext& ctx, const FieldDesc& field, void* p,
                                   Selection& selection, UndoStack& undo,
                                   const std::vector<uint64_t>& fids,
                                   const std::vector<void*>& comps, uint32_t fieldOffset)
{
    auto* id = static_cast<AssetID*>(p);
    // フィールド名からライブラリを推定 (mesh / material / texture)。
    // ★**小文字へ畳んでから照合する**。素の名前で探すと "cubemapTexture" /
    //   "lutTexture" / "normalTex" が "tex" に一致せず、どれも総当たり一覧へ落ちる
    std::string fname = field.name;
    std::transform(fname.begin(), fname.end(), fname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<AssetEntry> entries;
    if (fname.find("physmat") != std::string::npos) {
        // M59a1: 物理マテリアル (M59a2 の Collider.physMaterial 等)。
        // ★"material" より**先に**見ること。小文字化した "physmaterial" は "material" を
        //   含むので、順序を誤ると MaterialLibrary と取り違えたまま気付けない
        if (PhysMatLibrary* pm = physmat::Library()) {
            for (const PhysMatEntry& e : pm->Enumerate()) {
                entries.push_back({ AssetID{ e.hash }, e.name });
            }
        }
    } else if (fname.find("model") != std::string::npos) {
        // M18: SkinnedMesh.model。★"model" は "mesh" を含まないので、この分岐が無いと
        //   最後の else (メッシュ + マテリアル + テクスチャの混合) に落ちる。正解の
        //   SkinnedModel が 1 件も候補に出ないうえ、現在値も一覧に無いので表示は常に
        //   "None" になり、そこから選ぶと FBX ローダが入れた参照が壊れる
        for (const SkinnedModelEntry& s : ctx.resources->skinnedModels.Enumerate()) {
            entries.push_back({ AssetID{ s.hash }, s.name });
        }
    } else if (fname.find("mesh") != std::string::npos) {
        entries = ctx.resources->meshes.Enumerate();
    } else if (fname.find("material") != std::string::npos) {
        entries = ctx.resources->materials.Enumerate();
    } else if (fname.find("tex") != std::string::npos) {
        entries = ctx.resources->textures.Enumerate();
    } else if (fname.find("sound") != std::string::npos) {
        // M45c: .sound.json (AudioSource.sound 等)。**"clip"/"anim" より先に見る**
        if (ctx.sounds) {
            for (const SoundEntry& s : ctx.sounds->Enumerate()) {
                entries.push_back({ AssetID{ s.hash }, s.name });
            }
        }
    } else if (fname.find("audio") != std::string::npos) {
        // M45c: 素の音声クリップ (.wav/.ogg) を直接指すフィールド
        if (ctx.audio) {
            entries = ctx.audio->Enumerate();
        }
    } else if (fname.find("clip") != std::string::npos || fname.find("anim") != std::string::npos) {
        // 注: "clip" はアニメーションクリップの既存規約。音のクリップは "audio" を使うこと
        for (const AnimClipEntry& c : ctx.anims->Enumerate()) {
            entries.push_back({ AssetID{ c.hash }, c.name });
        }
    } else if (fname.find("controller") != std::string::npos) {
        // M22: AnimatorController.controller。★"clip" も "anim" も含まないので、この分岐が
        //   無いと SkinnedMesh.model と同じく総当たり一覧へ落ちる — 正解の .controller.json
        //   が 1 件も候補に出ず、選ぶと参照が壊れる
        if (ctx.controllers) {
            for (const ControllerEntry& c : ctx.controllers->Enumerate()) {
                entries.push_back({ AssetID{ c.hash }, c.name });
            }
        }
    } else {
        entries = ctx.resources->meshes.Enumerate();
        const auto mats = ctx.resources->materials.Enumerate();
        const auto texs = ctx.resources->textures.Enumerate();
        entries.insert(entries.end(), mats.begin(), mats.end());
        entries.insert(entries.end(), texs.begin(), texs.end());
    }

    const char* cur = Tr(StrId::Insp_NoneItem);
    for (const AssetEntry& e : entries) {
        if (e.id == *id) {
            cur = e.name.c_str();
        }
    }

    char labelBuf[192];
    ImGui::PushID(MakeFieldLabel(field, labelBuf, sizeof(labelBuf)));
    ImGui::TextUnformatted(FieldLabelText(field));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::Button(cur, ImVec2(-1, 0))) {
        ImGui::OpenPopup("##assetpick");
    }
    if (ImGui::BeginPopup("##assetpick")) {
        auto assign = [&](AssetID v) {
            // マルチ選択は全対象へバッチ適用 (1 Undo エントリ、M40a)
            undo.Record("Assign asset", *ctx.scene, selection, fids,
                        UndoStack::StructuralChanges::None, [&] {
                for (void* c : comps) {
                    *reinterpret_cast<AssetID*>(static_cast<uint8_t*>(c) + fieldOffset) = v;
                }
            });
        };
        if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
            assign(AssetID{});
        }
        for (const AssetEntry& e : entries) {
            if (ImGui::Selectable(e.name.c_str(), e.id == *id)) {
                assign(e.id);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void InspectorWindow::DrawEntityRef(EngineContext& ctx, const FieldDesc& field, void* p,
                                    Selection& selection, UndoStack& undo,
                                    const std::vector<uint64_t>& fids,
                                    const std::vector<void*>& comps, uint32_t fieldOffset)
{
    auto* id = static_cast<EntityID*>(p);
    World& world = ctx.scene->GetWorld();

    // エンティティ一覧を先に収集 (ポップアップ描画中は ForEachArchetype の外で行う。
    // Undo の CaptureBefore が ApplyStructuralChanges を呼ぶため、イテレーション中に記録できない)
    std::vector<std::pair<EntityID, std::string>> ents;
    const ComponentTypeId req[] = { NameComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            ents.emplace_back(e, world.GetName(e));
        }
    });

    const char* cur = Tr(StrId::Insp_NoneItem);
    if (!id->IsNull() && world.IsAlive(*id)) {
        cur = world.GetName(*id);
    }

    char labelBuf[192];
    ImGui::PushID(MakeFieldLabel(field, labelBuf, sizeof(labelBuf)));
    ImGui::TextUnformatted(FieldLabelText(field));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::Button(cur, ImVec2(-1, 0))) {
        ImGui::OpenPopup("##entpick");
    }
    if (ImGui::BeginPopup("##entpick")) {
        auto assign = [&](EntityID v) {
            // マルチ選択は全対象へバッチ適用 (同一エンティティを参照させる、M40a)
            undo.Record("Assign reference", *ctx.scene, selection, fids,
                        UndoStack::StructuralChanges::None, [&] {
                for (void* c : comps) {
                    *reinterpret_cast<EntityID*>(static_cast<uint8_t*>(c) + fieldOffset) = v;
                }
            });
        };
        if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
            assign(kNullEntity);
        }
        for (const auto& [e, name] : ents) {
            ImGui::PushID(static_cast<int>(e.index));
            if (ImGui::Selectable(name.c_str(), e == *id)) {
                assign(e);
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

} // namespace mye
