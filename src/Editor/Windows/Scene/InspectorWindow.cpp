#include "Editor/Windows/Scene/InspectorWindow.h"

#include <algorithm>
#include <cctype>
#include <cfloat> // M79 sub-06: DragFloat の上限に FLT_MAX を渡す
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Editor/Asset/AssetOps.h"
#include "Editor/Scene/CameraPilot.h"
#include "Editor/Scene/ComponentClipboard.h"
#include "Editor/Scene/ComponentDependencies.h"
#include "Editor/Widgets/EditorWidgets.h"
#include "Editor/Windows/Scene/ComputeInspectorSection.h" // M91h: スクリプトの Compute 欄
#include "Engine/Engine/Script/ScriptHost.h"
#include "Editor/Tools/FractureBakeCommit.h" // M80i: 焼き成功結果の確定 (.mfrac 保存・登録・Undo)
#include "Editor/Tools/NavBakeCommit.h"      // M82b: ベイク結果の確定 (.mnav 保存・参照の設定・Undo)
#include "Editor/Tools/PatrolRouteEdit.h"    // M85g: 巡回ルートの点の追加・削除・入れ替え
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Asset/AssetKeyResolver.h" // M80j: guid:// 登録名 → クック元パス (スキンの骨ウェイト取得)
#include "Engine/Engine/Asset/FbxLoader.h"   // M90e: LOD 設定の適用でモデルのメッシュを登録し直す
#include "Engine/Engine/Asset/ModelLoader.h" // 同上
#include "Engine/Engine/Asset/ModelCook.h" // M80j: .mmdl クックキャッシュからボーンウェイト付き頂点を読む
#include "Engine/Engine/Physics/Fracture/FractureSkinBake.h" // M80j: 骨割り当てへ渡す入力の型
#include "Editor/Widgets/EditorComponentCatalog.h"
#include "Editor/Project/PartTagNames.h"
#include "Editor/Project/PhysicsLayerNames.h"
#include "Editor/Project/NavAreaNames.h"
#include "Editor/Project/NavAgentTypes.h"
#include "Engine/Engine/Navigation/NavSystem.h" // NavCheckLinkPlacement (Link の置き方の検査)
#include "Editor/SourceControl/ScmHint.h" // M66i: タグ名の保存直後に status を取り直させる
#include "Engine/Engine/Scene/TagNames.h" // 汎用タグの名前表 (タグ欄のドロップダウン)
#include "Engine/Engine/Scene/Tags.h" // 汎用タグの読み書き (Tags::OwnMask / SetOwnMask)
#include "Engine/Engine/RayTracing/RtScope.h" // RT の実効値と根拠 (描画と同じ判定)
#include "Engine/Engine/Rendering/RenderSystem.h" // RT のタグ規則 / レーンの on/off
#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Util/Hash.h" // マテリアルプレビューの同一性キー (M53)
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/UI/UILayout.h" // M75a: RectTransform の解決済み矩形の読み取り表示
#include "Engine/Engine/UI/UILayoutGroup.h" // M75e: 自動レイアウトに上書きされている欄の表示
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Audio/Synth/ModalAudio.h" // Deep-Modal 面打ちプレビュー (M76g、sub-06 と同じ関数)
#include "Engine/Engine/Audio/Synth/SynthCore.h"  // WriteWavToFile (Export WAV)
#include "Engine/Engine/Scene/EntityNaming.h"
#include "Engine/Engine/Physics/Fracture/FractureBuilder.h" // M80i: BuildFracturePieces / CountFracturePieceChildren
#include "Engine/Engine/Physics/Fracture/FractureSystem.h" // ResolveFractureAsset / FracturePieceIndicesMatchAsset の共有
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/HotReload/ReloadHub.h" // M80p: ReloadCount() でスキンウェイトキャッシュを無効化
#include "Engine/Engine/Modal/ModalSoundLibrary.h" // 状態 (Missing/Baking/Ready/Failed/NoModel)
#include "Engine/Engine/Animation/Parts.h"
#include "Engine/Engine/Physics/Fracture/FractureLibrary.h" // M80i: .mfrac の登録 (FractureLibrary/fracturelib::)
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/ImGui/ImGuiTheme.h"  // 見出しフォント (テーマ第 3 世代)
#include "Engine/Renderer/PostFx/FxStackAsset.h"  // M78c: fxstack アセット
#include "Engine/Renderer/Shader/ProjectShaderProperties.h"  // M78c r2: スキーマ駆動 Inspector
#include "Engine/Renderer/Shader/ShaderManager.h"             // M78c r2: ShaderDirs() 呼び出し
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClassCount (M67)
#include "Engine/Renderer/Device/ReflectionClassJson.h" // reflectionClass の受理規則 (M67h)
#include "Engine/Renderer/Pipeline/RenderTypes.h" // kEmissiveMaxIntensity (M46i)
#include "Engine/Renderer/Mesh/Skeleton.h"    // SkinnedModel のジョイント名 (M48i)

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

// M76g: Deep-Modal のプレビュー専用 AssetID。sub-06 の回転プール (modal://slot#k) とは
// 別キー — 共有すると衝突再生中のプレビューがプールの奪い合いに巻き込まれる
// (SoundGenWindow::kPreviewClipId と同じ考え方)
constexpr AssetID kModalPreviewClipId{ HashStr("modal://preview") };

const char* ModalStateLabel(ModalState s)
{
    switch (s) {
    case ModalState::Missing:
        return Tr(StrId::Insp_ModalStateMissing);
    case ModalState::Baking:
        return Tr(StrId::Insp_ModalStateBaking);
    case ModalState::Ready:
        return Tr(StrId::Insp_ModalStateReady);
    case ModalState::Failed:
        return Tr(StrId::Insp_ModalStateFailed);
    case ModalState::NoModel:
        return Tr(StrId::Insp_ModalStateNoModel);
    }
    return "?";
}

// sub-10 H (reviewer round 1 指摘 2): 面ボタンの直近の結果を UI に出すためのラベル。
// Played を含む全ケースを網羅する (実際に Inspector 経由で起き得るのは Played/BelowMin
// だけだが、MakeModalShotPlay の契約を先取りして固定しておく — 将来ここへ分岐を足しても
// 黙って "?" にならない)
const char* ModalShotResultLabel(ModalShotResult r)
{
    switch (r) {
    case ModalShotResult::Played:
        return Tr(StrId::Insp_ModalResultPlayed);
    case ModalShotResult::NotReady:
        return Tr(StrId::Insp_ModalResultNotReady);
    case ModalShotResult::NoModel:
        return Tr(StrId::Insp_ModalResultNoModel);
    case ModalShotResult::Cooldown:
        return Tr(StrId::Insp_ModalResultCooldown);
    case ModalShotResult::BelowMin:
        return Tr(StrId::Insp_ModalResultBelowMin);
    case ModalShotResult::PoolFull:
        return Tr(StrId::Insp_ModalResultPoolFull);
    case ModalShotResult::PlayFailed:
        return Tr(StrId::Insp_ModalResultPlayFailed);
    }
    return "?";
}

// 6 面ボタンのラベルとファイル名スラグ (spec §4.3 の並び: +X -X +Y -Y +Z -Z)
struct ModalFaceInfo {
    StrId label;
    const char* slug;
};
const ModalFaceInfo kModalFaces[6] = {
    { StrId::Insp_ModalFacePX, "px" }, { StrId::Insp_ModalFaceNX, "nx" },
    { StrId::Insp_ModalFacePY, "py" }, { StrId::Insp_ModalFaceNY, "ny" },
    { StrId::Insp_ModalFacePZ, "pz" }, { StrId::Insp_ModalFaceNZ, "nz" },
};

// M80i: 破片焼きの段階ラベル (「焼いています: <段階>」の %s に入る)
const char* FractureStageLabel(FractureBakeStage s)
{
    switch (s) {
    case FractureBakeStage::ClosedCheck:
        return Tr(StrId::Insp_FractureStageClosedCheck);
    case FractureBakeStage::Voxelize:
        return Tr(StrId::Insp_FractureStageVoxelize);
    case FractureBakeStage::Split:
        return Tr(StrId::Insp_FractureStageSplit);
    case FractureBakeStage::Hull:
        return Tr(StrId::Insp_FractureStageHull);
    }
    return "?";
}

// resources.meshes の CPU 頂点/インデックスを分割コアの頂点形式へ詰め替える。
// DemoContent.cpp の ToFractureMesh と同じ変換 (ファイルをまたいで公開する価値の無い
// 小さな詰め替えなので、このファイルにも複製する — FractureBake.cpp 冒頭コメントと同じ流儀)
FractureMesh ToFractureMeshForBake(const Mesh& mesh)
{
    FractureMesh out;
    out.verts.resize(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        out.verts[i].position = mesh.positions[i];
        out.verts[i].normal = i < mesh.normals.size() ? mesh.normals[i] : XMFLOAT3{ 0, 1, 0 };
        out.verts[i].uv = i < mesh.uvs.size() ? mesh.uvs[i] : XMFLOAT2{ 0, 0 };
    }
    out.indices.assign(mesh.indices.begin(), mesh.indices.end());
    return out;
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
// lightReceiveShadow / collisionFloor は Bool 登録 (Checkbox) へ移行済み
// (kOffOnLabels へ寄せると「オン/オフ」以上の意味を持つ日が来たとき値域を広げられない)
constexpr const char* kPtclLightModeLabels[] = { "Unlit", "Per Particle", "Per Pixel" };
constexpr const char* kUIKindLabels[] = { "Panel", "Text", "Button" };
constexpr const char* kUIAnchorLabels[] = { "TopLeft",    "TopCenter",    "TopRight",
                                            "MiddleLeft", "Center",       "MiddleRight",
                                            "BottomLeft", "BottomCenter", "BottomRight" };
constexpr const char* kForceSpaceLabels[] = { "World", "Local" };
constexpr const char* kBillboardLabels[] = { "Billboard", "BillboardY", "World" };
constexpr const char* kSkyboxModeLabels[] = { "Gradient", "Cubemap", "Panoramic" };
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
constexpr const char* kSkyboxModeJa[] = { "グラデーション", "キューブマップ", "パノラマ (2D)" };
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
// M75g: ScrollRect の動き方 (uiwidgets::kScroll* と同じ並び)
constexpr const char* kUIScrollMovementLabels[] = { "Unrestricted", "Elastic", "Clamped" };
constexpr const char* kUIScrollMovementJa[] = { "制限なし", "Elastic (端で戻る)", "Clamped (端で止まる)" };
constexpr const char* kUIPresetColJa[] = { "左", "中央", "右", "伸縮" };
constexpr const char* kUIPresetRowJa[] = { "上", "中央", "下", "伸縮" };
// RayTracing.inScene / receiver (kRtScopeInherit / On / Off の並び)
constexpr const char* kRtScopeLabels[] = { "Inherit", "On", "Off" };
constexpr const char* kRtScopeJa[] = { "継承", "ON", "OFF" };
static_assert(kRtScopeInherit == 0 && kRtScopeOn == 1 && kRtScopeOff == 2);
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
    { "UILayoutGroup", "startCorner", kUIGridCornerLabels, 4, kUIGridCornerJa },
    { "UILayoutGroup", "startAxis", kUIGridAxisLabels, 2, kUIGridAxisJa },
    { "UILayoutGroup", "constraint", kUIGridConstraintLabels, 3, kUIGridConstraintJa },
    { "UIContentSizeFitter", "horizontalFit", kUIFitModeLabels, 3, kUIFitModeJa },
    { "UIContentSizeFitter", "verticalFit", kUIFitModeLabels, 3, kUIFitModeJa },
    // M75f: ウィジェット
    { "UISelectable", "transition", kUITransitionLabels, 3, kUITransitionJa },
    { "UISelectable", "navigationMode", kUINavModeLabels, 5, kUINavModeJa },
    { "UISlider", "direction", kUISliderDirLabels, 4, kUISliderDirJa },
    // M75g: Scrollbar の向きは Slider と同じ並び
    { "UIScrollbar", "direction", kUISliderDirLabels, 4, kUISliderDirJa },
    { "UIScrollRect", "movementType", kUIScrollMovementLabels, 3, kUIScrollMovementJa },
    { "UIElement", "wrap", kOffOnLabels, 2, kOffOnJa },
    { "ConstantForce", "relative", kForceSpaceLabels, 2, kForceSpaceJa },
    { "SpriteRenderer", "billboardMode", kBillboardLabels, 3, kBillboardJa },
    { "TextMesh", "billboardMode", kBillboardLabels, 3, kBillboardJa },
    { "Skybox", "mode", kSkyboxModeLabels, 3, kSkyboxModeJa },
    { "Fog", "mode", kFogModeLabels, 3, kFogModeJa },
    { "CameraPostFx", "tonemapMode", kTonemapLabels, 3, kTonemapJa },
    { "PartBounds", "shape", kPartBoundsShapeLabels, 2, kPartBoundsShapeJa },
    { "RayTracing", "inScene", kRtScopeLabels, 3, kRtScopeJa },
    { "RayTracing", "receiver", kRtScopeLabels, 3, kRtScopeJa },
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
                              AssetPreviewCache& preview, bool inPlayMode)
{
    // M80j (sub-10 round 2): Play 中は「生成」を押しても .mfrac は書かれるが Stop で子ごと
    // 消える (Play はシーンをスナップショットに戻す) ので、DrawDestructibleNotes が読む
    inPlayMode_ = inPlayMode;
    // ナビメッシュのベイク結果は選択・ウィンドウの開閉に関係なく確定する (取りこぼすと .mnav も SceneView も古いまま)
    CommitReadyNavBakes(ctx, selection, undo);
    if (!open) {
        return;
    }
    // M80i: 破片焼きの結果取り込み。選択やウィンドウの開閉に関係なく毎フレーム進める —
    // 焼きはボタンを押したエンティティ単位で走るので、選択が外れても止めない
    fractureBakeService_.Pump();
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
    DrawTagRow(ctx, selection, undo, tg);
    DrawTargetInfoRow(tg);
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

// 名前欄 (対象の説明 1 行は DrawTagRow の後 = DrawTargetInfoRow)
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
}

// 対象の説明 1 行 (マルチ選択の件数 / Entity index:generation)。
// ★名前 → タグ → ここ、の順に描く — タグは「その物が何であるか」の印なので名前の直下に
//   置く (Unity の Tag と同じ位置)。デバッグ用の識別子はその下で足りる
void InspectorWindow::DrawTargetInfoRow(const InspectorTargets& tg)
{
    if (tg.multi) {
        ImGui::TextDisabled("%zu entities selected — edits apply to all (gizmo: primary only)",
                            tg.fids.size());
    } else {
        ImGui::TextDisabled("Entity %u:%u  (fileId %llu)", tg.e.index, tg.e.generation,
                            static_cast<unsigned long long>(tg.fid));
    }
}

// 汎用タグの欄 (M76k)。名前の直下に置くのは、タグが「付け外しする機能」= コンポーネントでは
// なく「その物が何であるか」の印 = オブジェクト固有の属性だから (Unity の Tag と同じ位置)。
// TagComponent は Hidden なのでコンポーネント一覧には出ず、**ここが唯一の編集口**。
// ★実体の付け外しは Tags::SetOwnMask の 1 本に通す (mask=0 ⇔ コンポーネント無し)。保存側が
//   非ゼロのときだけ "tagMask" を書くので、付けて外した跡はシーンにもハッシュにも残らない
void InspectorWindow::DrawTagRow(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                 const InspectorTargets& tg)
{
    World& world = ctx.scene->GetWorld();
    TagNames& tn = TagNames::Get();
    tn.Load(ctx.assetsRoot);
    const uint64_t mask = Tags::OwnMask(world, tg.e); // 表示は primary の値 (M40a の流儀)

    // クリック即確定 = 自前 Undo。マルチ選択は全対象へ同じ集合を適用する
    auto applyMask = [&](uint64_t next) {
        undo.Record("Modify Tags", *ctx.scene, selection, tg.fids,
                    UndoStack::StructuralChanges::Apply, [&] {
            for (EntityID te : tg.ents) {
                Tags::SetOwnMask(world, te, next);
            }
        });
    };

    // プレビュー: 付いているタグ名を並べる。名前の無い番号しか無ければ件数で出す
    int count = 0;
    for (int i = 0; i < kMaxTags; ++i) {
        count += ((mask >> i) & 1ull) ? 1 : 0;
    }
    std::string preview;
    int shown = 0;
    for (int i = 0; i < kMaxTags && preview.size() < 48; ++i) {
        if (((mask >> i) & 1ull) == 0 || tn.Name(i)[0] == '\0') {
            continue;
        }
        preview += (shown++ == 0) ? "" : ", ";
        preview += tn.Name(i);
    }
    char tail[32];
    if (mask == 0) {
        preview = Tr(StrId::Insp_TagNone);
    } else if (shown == 0) {
        std::snprintf(tail, sizeof(tail), Tr(StrId::Insp_TagCount), count);
        preview = tail;
    } else if (shown < count) {
        std::snprintf(tail, sizeof(tail), Tr(StrId::Insp_TagMore), count - shown);
        preview += " ";
        preview += tail;
    }

    ImGui::TextUnformatted(Tr(StrId::Insp_TagLabel));
    const bool overridden =
        tg.isPrefabMember && Prefab::IsTagMaskOverridden(*ctx.scene, *ctx.prefabs, tg.e);
    if (overridden) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(themeColor::Prefab, "*"); // 名前欄と同じ「プレハブと違う」印
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##tag", preview.c_str())) {
        // 「なし」= タグを持たない状態。押すと TagComponent 自体が外れる
        if (ImGui::Selectable(Tr(StrId::Insp_TagNone), mask == 0)) {
            applyMask(0ull);
        }
        // ★名前の無い番号は出さない (64 行並べても選べない)。ただし既に立っているビットは
        //   名前が無くても出す — 黙って外せなくなる / 見えないまま残るのを防ぐ
        for (int i = 0; i < kMaxTags; ++i) {
            const bool on = ((mask >> i) & 1ull) != 0;
            if (tn.Name(i)[0] == '\0' && !on) {
                continue;
            }
            ImGui::PushID(i);
            char row[64];
            std::snprintf(row, sizeof(row), "%2d  %s", i, tn.Display(i));
            // 複数持てるので、選んでもポップアップは閉じない (Unity の単一 Tag との違い)
            if (ImGui::Selectable(row, on, ImGuiSelectableFlags_NoAutoClosePopups)) {
                applyMask(mask ^ (1ull << i));
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        // 「タグを追加...」: 名前表 (project_settings.json) へその場で足して即保存する。
        // プロジェクト設定へ往復させないための近道で、足した番号はそのまま付ける
        // (このドロップダウンを開いている = この物に付けたい、という文脈なので)。
        // ★Undo 対象外 — シーンではなくプロジェクト設定の変更 (物理レイヤー名と同じ扱い)
        int freeSlot = -1;
        for (int i = 0; i < kMaxTags; ++i) {
            if (tn.Name(i)[0] == '\0') {
                freeSlot = i;
                break;
            }
        }
        if (freeSlot < 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_TagAddFull));
        } else if (!tagAddOpen_) {
            if (ImGui::Selectable(Tr(StrId::Insp_TagAdd), false,
                                  ImGuiSelectableFlags_NoAutoClosePopups)) {
                tagAddOpen_ = true;
                tagAddBuffer_[0] = '\0';
                ImGui::SetKeyboardFocusHere(); // 次フレームの InputText が焦点を取る
            }
        } else {
            ImGui::SetNextItemWidth(160.0f);
            const bool enter = ImGui::InputText("##tag_add_name", tagAddBuffer_,
                                                sizeof(tagAddBuffer_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::SmallButton(Tr(StrId::Insp_TagAddApply)) || enter)
                && tagAddBuffer_[0] != '\0') {
                std::snprintf(tn.EditBuffer(freeSlot), TagNames::kNameCapacity, "%s", tagAddBuffer_);
                if (tn.Save(ctx.assetsRoot)) {
                    tn.Load(ctx.assetsRoot, true);
                    scmhint::Changed(ctx.assetsRoot + L"\\project_settings.json"); // M66i
                }
                applyMask(mask | (1ull << freeSlot));
                tagAddBuffer_[0] = '\0';
                tagAddOpen_ = false;
            }
        }
        ImGui::TextDisabled("%s", Tr(StrId::Insp_TagHint));
        ImGui::EndCombo();
    }
    // ベース値へ戻す (プレハブメンバのときだけ)。コンポーネント一覧に Tag が出ない以上、
    // 「削除された/追加された」の節では戻せないので、戻し口はこの行に集約する
    if (tg.isPrefabMember && ImGui::BeginPopupContextItem("tag_ctx")) {
        if (ImGui::MenuItem(Tr(StrId::Insp_RevertToPrefab), nullptr, false, overridden)) {
            undo.Record("Revert Tags", *ctx.scene, selection, tg.fid,
                        UndoStack::StructuralChanges::Apply, [&] {
                Prefab::RevertTagMask(*ctx.scene, *ctx.prefabs, tg.e);
            });
        }
        ImGui::EndPopup();
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
            DrawComputeInspectorSection(ctx, desc.name, compute::CallerKind::CSharp); // M91h
            ImGui::PopID();
            return;
        }
        if (comp) {
            if (std::strcmp(desc.name, "Collider") == 0) {
                const auto& collider = *static_cast<const ColliderComponent*>(comp);
                const bool supported = selection.ids.size() == 1
                    && collider.shape >= collidershape::kSphere
                    && collider.shape <= collidershape::kCapsule;
                bool editing = selection.colliderEditFileId == selection.primary;
                ImGui::BeginDisabled(!supported);
                if (ImGui::Checkbox(Tr(StrId::Insp_EditCollider), &editing)) {
                    selection.colliderEditFileId = editing ? selection.primary : 0;
                }
                ImGui::EndDisabled();
                if (!supported) {
                    ImGui::TextWrapped("%s", Tr(StrId::Insp_EditColliderUnsupported));
                }
            }
            DrawComponentFields(ctx, selection, undo, tg, row, comp);
            if (ctx.scriptHost != nullptr && ctx.scriptHost->IsScriptComponent(t)) {
                DrawComputeInspectorSection(ctx, desc.name, compute::CallerKind::Cpp); // M91h
            }
        }
        DrawComponentNotes(ctx, selection, undo, tg, row);
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
        if (std::strcmp(desc.name, "Collider") == 0) {
            const auto& collider = *static_cast<const ColliderComponent*>(comp);
            if ((std::strcmp(f.name, "center") == 0 || std::strcmp(f.name, "rotation") == 0)
                && (collider.shape < collidershape::kSphere || collider.shape > collidershape::kCapsule)) {
                continue;
            }
            if (std::strcmp(f.name, "rotation") == 0 && collider.shape == collidershape::kSphere) {
                continue;
            }
        }
        if (isJoint && !JointFieldApplies(jointType, f.name)) {
            continue;
        }
        const bool changed =
            DrawField(ctx, desc.name, comp, f, e, selection, undo, row.fids, row.comps);
        if (changed && std::strcmp(desc.name, "Collider") == 0
            && (std::strcmp(f.name, "radius") == 0 || std::strcmp(f.name, "height") == 0
                || std::strcmp(f.name, "halfExtents") == 0)) {
            constexpr float kMinSize = 0.0001f;
            for (void* value : row.comps) {
                auto& collider = *static_cast<ColliderComponent*>(value);
                collider.radius = (std::max)(kMinSize, collider.radius);
                collider.halfExtents.x = (std::max)(kMinSize, collider.halfExtents.x);
                collider.halfExtents.y = (std::max)(kMinSize, collider.halfExtents.y);
                collider.halfExtents.z = (std::max)(kMinSize, collider.halfExtents.z);
                if (collider.shape == collidershape::kCapsule) {
                    collider.height = (std::max)(2 * collider.radius, collider.height);
                }
            }
        }
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
        // M84a: Surface の型を選んだら、その型の寸法を写す (伝播の後なので全対象に。CaptureAfter より前なので同じ Undo)
        if (changed && std::strcmp(desc.name, "NavMeshSurface") == 0 && std::strcmp(f.name, "agentTypeId") == 0) {
            if (const NavAgentType* type = NavAgentTypes::Get().Find(static_cast<const NavMeshSurfaceComponent*>(comp)->agentTypeId)) {
                for (void* value : row.comps) {
                    NavApplyAgentType(*static_cast<NavMeshSurfaceComponent*>(value), *type);
                }
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
        if ((driven & uilayout::kDrivenByScrollbar) != 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_UIDrivenScrollbar)); // M75g
        }
        if ((driven & uilayout::kDrivenByScrollRect) != 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_UIDrivenScrollRect)); // M75g
        }
        if ((driven & uilayout::kDrivenByDropdown) != 0) {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_UIDrivenDropdown)); // M75g
        }
    }
}

namespace {

// RT の実効値 1 レーン分を「実効 BVH に入る: ON (タグ Background)」の形で出す
void DrawRtScopeLine(World& world, StrId laneLabel, const RtScopeLane& lane, EntityID self)
{
    char from[96] = {};
    switch (lane.source) {
    case RtScopeSource::Explicit:
        if (lane.from == self) {
            std::snprintf(from, sizeof(from), "%s", Tr(StrId::Insp_RtFromSelf));
        } else {
            const auto* nc = world.GetComponent<NameComponent>(lane.from);
            std::snprintf(from, sizeof(from), "%s %s", Tr(StrId::Insp_RtFromParent),
                          (nc != nullptr && nc->value[0] != '\0') ? nc->value : "?");
        }
        break;
    case RtScopeSource::Tag:
        std::snprintf(from, sizeof(from), "%s %s", Tr(StrId::Insp_RtFromTag),
                      TagNames::Get().Display(lane.tagIndex));
        break;
    case RtScopeSource::Default:
        std::snprintf(from, sizeof(from), "%s", Tr(StrId::Insp_RtFromDefault));
        break;
    }
    ImGui::TextDisabled("%s %s: %s (%s)", Tr(StrId::Insp_RtEffective), Tr(laneLabel),
                        Tr(lane.on ? StrId::Menu_RtRuleOn : StrId::Menu_RtRuleOff), from);
}

// RayTracing / MeshRenderer / Terrain 節の末尾に、描画と同じ判定 (ResolveRtScope) の結果を出す。
// ★子の MeshRenderer にも出すのは、親に付けた設定やタグ規則が「本当に子へ効いているか」を
//   子を選んだだけで確かめられるようにするため (設定を持たない物でも実効値が見える)
void DrawRtScopeNotes(EngineContext& ctx, EntityID e)
{
    if (ctx.renderSystem == nullptr) {
        return;
    }
    const RenderSystem& rs = *ctx.renderSystem;
    TagNames::Get().Load(ctx.assetsRoot);
    World& world = ctx.scene->GetWorld();
    const RtScope scope = ResolveRtScope(world, e, rs.rtTagRules);
    DrawRtScopeLine(world, StrId::Insp_RtInScene, scope.inScene, e);
    DrawRtScopeLine(world, StrId::Insp_RtReceiver, scope.receiver, e);
    if (!rs.enableRtGi && !rs.enableRtShadow && !rs.enableRtRefl) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_RtLanesOff));
    }
}

} // namespace

// 型ごとの付記 (フィールド行の下)。PartBounds の警告と、カメラの操縦ボタン
bool& EditorWaterPreviewOn()
{
    static bool on = false;
    return on;
}

void InspectorWindow::DrawComponentNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                         const InspectorTargets& tg, const InspectorComponentRow& row)
{
    const ComponentDesc& desc = *row.desc;
    World& world = ctx.scene->GetWorld();
    // 水面プレビュー: 水面の時計はシミュレーション tick なので Edit 中は止まる。
    // オンの間だけ描画専用の経過秒を足して水面を動かす (ワールドの状態には触れない)
    if (std::strcmp(desc.name, "WaterWave") == 0) {
        bool& on = EditorWaterPreviewOn();
        if (ToolbarToggle(Tr(on ? StrId::Insp_WaterPreviewStop : StrId::Insp_WaterPreviewStart), on,
                          nullptr, /*mode=*/true)) {
            on = !on;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Insp_WaterPreviewHint));
    }
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
    // M76g: Deep-Modal 面打ちプレビュー。焼き状態は entity ごとに違いうるので
    // マルチ選択では出さない (Camera 操縦と同じ理由)
    if (std::strcmp(desc.name, "ModalSound") == 0 && !tg.multi) {
        DrawModalSoundNotes(ctx, tg, row);
    }
    // M80i: 破壊物の焼きボタン・状態表示。マルチ選択では出さない (ModalSound と同じ理由 —
    // 焼きはメッシュ 1 つ・エンティティ 1 つに対する非同期処理)
    if (std::strcmp(desc.name, "Destructible") == 0 && !tg.multi) {
        DrawDestructibleNotes(ctx, selection, undo, tg, row);
    }
    // M82b: ナビメッシュの Bake / Clear。ベイクは Surface 1 つ・エンティティ 1 つに対する非同期処理なので
    // マルチ選択では出さない
    if (std::strcmp(desc.name, "NavMeshSurface") == 0 && !tg.multi) {
        DrawNavMeshSurfaceNotes(ctx, selection, undo, tg);
    }
    // M82f: Obstacle の切り抜き無効の注意
    if (std::strcmp(desc.name, "NavMeshObstacle") == 0 && !tg.multi) {
        const auto* obstacle = ctx.scene->GetWorld().GetComponent<NavMeshObstacleComponent>(tg.e);
        if (obstacle != nullptr && !obstacle->carve) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavObstacleNoCarve));
            ImGui::PopTextWrapPos();
        }
    }
    // M82c: Agent の実行状態と警告。状態は sim が書く値の読み取り表示なのでマルチ選択では出さない
    if (std::strcmp(desc.name, "NavMeshAgent") == 0 && !tg.multi) {
        DrawNavMeshAgentNotes(ctx, selection, undo, tg);
    }
    // M85: ビヘイビアツリーの設定の注意 (AgentBrain との同居・木が引けない)。マルチ選択では出さない
    if (std::strcmp(desc.name, "BehaviorTree") == 0 && !tg.multi) {
        const auto* bt = world.GetComponent<BehaviorTreeComponent>(tg.e);
        if (bt != nullptr) {
            ImGui::PushTextWrapPos(0.0f);
            if (bt->tree.IsNull()) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_BtNoTree));
            } else if (bt->status == btstatus::kAssetMissing) {
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_BtAssetMissing));
            }
            if (world.GetComponent<AgentBrainComponent>(tg.e) != nullptr) {
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_BtAgentBrain));
            }
            const BehaviorTreeLibrary* trees = behaviortree::Library();
            const BehaviorTreeAsset* tree = (trees != nullptr && !bt->tree.IsNull()) ? trees->Get(bt->tree.value) : nullptr;
            if (tree != nullptr && BtAssetUsesCsTask(*trees, *tree)) {
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_BtCSharp));
            }
            ImGui::TextDisabled("%s", Tr(StrId::Insp_BtEntityKeysHint));
            ImGui::PopTextWrapPos();
        }
    }
    // M85g: 巡回ルートの点の編集。マルチ選択では出さない (点の並びは 1 つのルートに対する編集)
    if (std::strcmp(desc.name, "PatrolRoute") == 0 && !tg.multi) {
        DrawPatrolRouteNotes(ctx, selection, undo, tg);
    }
    // M83: 知覚している相手の一覧 (PerceptionSystem が毎 tick 書く値の読み取り表示) と設定の注意
    if (std::strcmp(desc.name, "AIPerception") == 0 && !tg.multi) {
        const auto* perc = world.GetComponent<AIPerceptionComponent>(tg.e);
        if (perc != nullptr) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            if (perc->hearingEnabled && perc->hearingMode == hearingmode::kAcoustic
                && world.GetComponent<AcousticListenerComponent>(tg.e) == nullptr) {
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_PercNoEar));
            }
            const int count = (std::min)((std::max)(perc->perceivedCount, 0), kMaxPercepts);
            if (count == 0) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_PercNone));
                ImGui::TextDisabled("%s", Tr(StrId::Insp_PercNoStimulus));
            } else {
                ImGui::TextDisabled("%s: %d (%s %d)", Tr(StrId::Insp_PercPerceived), count, Tr(StrId::Insp_PercInSight),
                                    perc->seenCount);
            }
            const struct {
                uint32_t bit;
                StrId label;
            } kSenseNames[] = { { perceptionsense::kSight, StrId::Insp_PercSight },
                                { perceptionsense::kHearing, StrId::Insp_PercHearing },
                                { perceptionsense::kDamage, StrId::Insp_PercDamage },
                                { perceptionsense::kTouch, StrId::Insp_PercTouch } };
            for (int i = 0; i < count; ++i) {
                const AIPercept& q = perc->percepts[i];
                const auto* name = q.target.IsNull() ? nullptr : world.GetComponent<NameComponent>(q.target);
                std::string senses;
                for (const auto& s : kSenseNames) {
                    if ((q.lastSenses & s.bit) != 0) {
                        senses += senses.empty() ? "" : ", ";
                        senses += Tr(s.label);
                    }
                }
                const long long ago = ctx.tickIndex >= q.lastSensedTick
                    ? static_cast<long long>(ctx.tickIndex - q.lastSensedTick) : 0;
                const ImVec4 color = q.currentSenses != 0 ? themeColor::Success : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                ImGui::TextColored(color, "%s [%s] (%.1f, %.1f, %.1f) %lld %s", name != nullptr ? name->value : Tr(StrId::Insp_PercUnknown),
                                   senses.c_str(), q.lastSensedPos.x, q.lastSensedPos.y, q.lastSensedPos.z, ago,
                                   Tr(StrId::Insp_PercTicksAgo));
                if (q.currentSenses == 0) {
                    ImGui::TextDisabled("    %s (%.1f, %.1f, %.1f)", Tr(StrId::Insp_PercPredicted), q.predictedPos.x,
                                        q.predictedPos.y, q.predictedPos.z);
                }
            }
            ImGui::PopTextWrapPos();
        }
    }
    // M82g: Modifier のエリア名 (番号だけでは分からない)。1 は歩行不可 = 範囲が歩けなくなることを添える
    if (std::strcmp(desc.name, "NavMeshModifier") == 0 && !tg.multi) {
        const auto* modifier = ctx.scene->GetWorld().GetComponent<NavMeshModifierComponent>(tg.e);
        if (modifier != nullptr) {
            NavAreaNames& areaNames = NavAreaNames::Get();
            areaNames.Load(ctx.assetsRoot);
            const int area = (std::min)((std::max)(modifier->area, 0), kNavAreaCount - 1);
            ImGui::TextDisabled(Tr(StrId::Insp_NavModifierArea), area, areaNames.Name(area));
            if (area == kNavAreaNotWalkable) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", Tr(StrId::Insp_NavModifierBlocks));
                ImGui::PopTextWrapPos();
            }
        }
    }
    // M82h: Link のエリア名と、使われない / 片方向 / Manual の注意書き
    if (std::strcmp(desc.name, "NavMeshLink") == 0 && !tg.multi) {
        const auto* link = ctx.scene->GetWorld().GetComponent<NavMeshLinkComponent>(tg.e);
        if (link != nullptr) {
            NavAreaNames& areaNames = NavAreaNames::Get();
            areaNames.Load(ctx.assetsRoot);
            const int area = (std::min)((std::max)(link->area, 0), kNavAreaCount - 1);
            ImGui::TextDisabled(Tr(StrId::Insp_NavLinkArea), area, areaNames.Name(area));
            ImGui::PushTextWrapPos(0.0f);
            if (area == kNavAreaNotWalkable) {
                ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavLinkBlocked));
            }
            const auto* linkMatrix = ctx.scene->GetWorld().GetComponent<WorldMatrixComponent>(tg.e);
            NavLinkSpec placed;
            if (linkMatrix != nullptr && NavMakeLinkSpec(*link, linkMatrix->value.m, tg.e, placed)) {
                const NavLinkPlacement placement = NavCheckLinkPlacement(ctx.scene->GetWorld(), placed);
                if (placement == NavLinkPlacement::NoSurface) {
                    ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavLinkNoSurface));
                } else if (placement == NavLinkPlacement::ExitTooFar) {
                    ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_NavLinkExitFar));
                }
            }
            if (!link->bidirectional) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_NavLinkOneWay));
            }
            if (link->traversal == navlinktraversal::kManual) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_NavLinkManual));
            }
            ImGui::PopTextWrapPos();
        }
    }
    // RT の実効値。RayTracing を持つ物はその節に、持たない物は描画される節 (MeshRenderer /
    // Terrain) に 1 回だけ出す。マルチ選択では出さない (物ごとに違いうるため)
    const bool hasRt = world.GetComponent<RayTracingComponent>(tg.e) != nullptr;
    const bool rtRow = hasRt ? std::strcmp(desc.name, "RayTracing") == 0
                             : (std::strcmp(desc.name, "MeshRenderer") == 0
                                || std::strcmp(desc.name, "Terrain") == 0);
    if (rtRow && !tg.multi) {
        DrawRtScopeNotes(ctx, tg.e);
    }
}

// M76g: ModalSound 節の末尾 (状態 / セル数 / 6 面ボタン / Export WAV)。
// 6 面ボタンは **sub-06 と同じ MakeModalShotPlay を呼ぶ** (spec §4.3「2 本目を書かない」)。
// ワールド行列・材質解決も AudioSourceSystem::Update (ランタイム経路) と同じ手順を踏む
void InspectorWindow::DrawModalSoundNotes(EngineContext& ctx, const InspectorTargets& tg,
                                          const InspectorComponentRow& row)
{
    (void)row; // 実体は他の分岐と同じく world 経由で取り直す (row.comps は空になり得るため)
    World& world = ctx.scene->GetWorld();
    const EntityID e = tg.e;
    const auto* comp = world.GetComponent<ModalSoundComponent>(e);
    if (comp == nullptr) {
        return;
    }
    const ModalMeshRef mesh = ResolveModalMesh(world, e, *comp);

    ModalSoundLibrary* lib = modalsound::Library();
    // Request は未着手なら非ブロッキングで焼きジョブを積む — Inspector を開くだけで
    // 裏の焼成が始まる (AudioSourceSystem::Update と同じ入口を共有する、規則は 1 本)
    const ModalState state = (lib != nullptr) ? lib->Request(mesh) : ModalState::NoModel;
    const ModalFeatureMap* fm = (lib != nullptr) ? lib->Get(mesh.id) : nullptr;
    const DmNetHeader* hdr = (lib != nullptr) ? lib->Header() : nullptr;
    const bool ready = (state == ModalState::Ready) && fm != nullptr && hdr != nullptr;

    ImGui::Separator();
    ImGui::Text(Tr(StrId::Insp_ModalState), ModalStateLabel(state),
               lib != nullptr ? lib->BackendName() : "none");
    ImGui::Text(Tr(StrId::Insp_ModalCells), fm != nullptr ? fm->validCount : 0u);
    if (mesh.IsNull()) {
        ImGui::TextUnformatted(Tr(StrId::Insp_ModalMeshNone));
    } else if (mesh.IsComposite()) {
        ImGui::Text(Tr(StrId::Insp_ModalMeshComposite), static_cast<int>(mesh.parts.size()));
    } else {
        ImGui::TextUnformatted(Tr(StrId::Insp_ModalMeshSingle));
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_ModalImpulse), &modalPreview_.impulse, 0.1f, 20.0f, "%.2f");

    ImGui::TextUnformatted(Tr(StrId::Insp_ModalTapHeading));
    // Baking / Missing / Failed / NoModel はどれも焼けた特徴が無いので、面ボタンは
    // まとめて disabled にする (spec §4.3「Baking 中はボタン disabled」)
    ImGui::BeginDisabled(!ready);
    for (int face = 0; face < 6; ++face) {
        if (face != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(face);
        if (ImGui::Button(Tr(kModalFaces[face].label))) {
            FireModalPreviewFace(ctx, tg, *comp, *fm, *hdr, face);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    // sub-10 H (reviewer round 1 指摘 2): 「押しても何も起きない」と「まだ押していない」を
    // 区別できるように、直近の結果を常に出す (Played も含めて明示する。黙って return しない)
    if (modalPreview_.everFired) {
        ImGui::Text(Tr(StrId::Insp_ModalResultHeading), ModalShotResultLabel(modalPreview_.lastResult));
    }

    // Export は「直近のプレビュー clip」を書き出すだけなので、面ボタンの ready 状態とは
    // 無関係 — 別のエンティティを見ていても直近に鳴らした音は書き出せる (spec §4.3)
    ImGui::BeginDisabled(!modalPreview_.valid);
    if (ImGui::Button(Tr(StrId::Insp_ModalExportWav))) {
        ExportModalPreviewWav(ctx);
    }
    ImGui::EndDisabled();
}

// 6 面ボタン 1 個ぶんの本体。localPoint = ローカル AABB 面中心、k = 内向き法線 × スライダ [N・s]。
// CollectModalImpacts (ランタイム) と同じ形の PendingModalImpact を組み立てて
// MakeModalShotPlay へそのまま渡す — テストが見ている規則と実際に鳴る規則を分けない
void InspectorWindow::FireModalPreviewFace(EngineContext& ctx, const InspectorTargets& tg,
                                          const ModalSoundComponent& comp, const ModalFeatureMap& fm,
                                          const DmNetHeader& hdr, int face)
{
    if (ctx.audio == nullptr || !ctx.audio->IsReady()) {
        return;
    }
    World& world = ctx.scene->GetWorld();
    const EntityID e = tg.e;
    const auto* wm = world.GetComponent<WorldMatrixComponent>(e);
    if (wm == nullptr) {
        return; // ワールド行列が無いと局所化できない (通常は起きない)
    }

    const int axis = face / 2;
    const bool positive = (face % 2) == 0; // 偶数 index = +軸側の面 (+X -X +Y -Y +Z -Z の並び)

    PendingModalImpact impact;
    impact.source = e;
    impact.mesh = ResolveModalMesh(world, e, comp).id;
    for (int a = 0; a < 3; ++a) {
        impact.localPoint[a] = (a == axis) ? (positive ? fm.frame.aabbMax[a] : fm.frame.aabbMin[a])
                                            : 0.5f * (fm.frame.aabbMin[a] + fm.frame.aabbMax[a]);
    }
    // 内向き法線 × 圧縮済み力積 (sub-10 A。CollectModalImpacts (ランタイム) と
    // 同じ ModalImpulseCurve を通す — 2 本目の規則を書くと較正の前提が崩れる)。
    // excessImpulse は生の力積のまま (ログ/UI 表示用)
    impact.k[axis] = (positive ? -1.0f : 1.0f) * ModalImpulseCurve(modalPreview_.impulse);
    impact.excessImpulse = modalPreview_.impulse;
    impact.tick = ctx.tickIndex;

    const XMMATRIX world_ = XMLoadFloat4x4(&wm->value);
    const XMVECTOR pLocal =
        XMVectorSet(impact.localPoint[0], impact.localPoint[1], impact.localPoint[2], 1.0f);
    XMFLOAT3 pWorld;
    XMStoreFloat3(&pWorld, XMVector3TransformCoord(pLocal, world_));
    impact.worldPoint[0] = pWorld.x;
    impact.worldPoint[1] = pWorld.y;
    impact.worldPoint[2] = pWorld.z;

    const auto* col = world.GetComponent<ColliderComponent>(e);
    const PhysMat* mat = (col != nullptr) ? physmat::Resolve(col->physMaterial) : nullptr;
    const float scale = ModalWorldScaleOfLongestAxis(fm.frame, wm->value);

    AudioClip clip;
    AudioSpatial spatial;
    const ModalShotResult result =
        MakeModalShotPlay(fm, hdr, impact, comp, mat, scale, clip, spatial, nullptr);
    // sub-10 H: 結果は Played 以外も含めて必ず記録する (DrawModalSoundNotes が表示する)。
    // 「押しても何も起きない」を UI から見えるようにするのが目的なので、ここで黙って
    // return する前に必ず書くこと
    modalPreview_.everFired = true;
    modalPreview_.lastResult = result;
    if (result != ModalShotResult::Played) {
        return; // BelowMin 等。直前の有効なプレビュー clip はそのまま残す (何も鳴らなかった扱い)
    }

    modalPreview_.clip = clip; // Export WAV 用に保持 (SoundGenWindow::Preview と同じ二重利用)
    modalPreview_.valid = true;
    modalPreview_.face = face;
    modalPreview_.entityFid = tg.fid;

    // RegisterClip は差し替え前に同 id の voice を止める (SoundGenWindow::Preview と同型) —
    // 前回のプレビューはここで自動的に切れる
    ctx.audio->RegisterClip(kModalPreviewClipId, std::move(clip), "(deep-modal preview)");
    PlayDesc desc;
    desc.clip = kModalPreviewClipId;
    desc.bus = AudioSystem::kBusUi; // エディタ操作音なので UI バス
    desc.volume = 1.0f;
    desc.priority = 255; // 明示操作の試聴は他の音に負けない
    ctx.audio->Play(desc);
}

// Export WAV: 直近のプレビュー clip を書き出す。重複名は " (N)" 連番
// (SoundGenWindow::Save 204-234 と同じ規約)
void InspectorWindow::ExportModalPreviewWav(EngineContext& ctx)
{
    if (!modalPreview_.valid || modalPreview_.clip.Empty()) {
        return;
    }
    namespace fs = std::filesystem;
    const std::wstring dir = ctx.assetsRoot + L"\\audio";
    std::error_code ec;
    fs::create_directories(fs::path{ dir }, ec);

    const char* faceSlug = (modalPreview_.face >= 0 && modalPreview_.face < 6)
        ? kModalFaces[modalPreview_.face].slug
        : "face";
    const std::string stem =
        "modal_" + std::to_string(modalPreview_.entityFid) + "_" + faceSlug;
    std::wstring path = dir + L"\\" + Utf8ToWide(stem) + L".wav";
    for (int n = 1; n < 1000 && fs::exists(path); ++n) {
        path = dir + L"\\" + Utf8ToWide(stem) + L" (" + std::to_wstring(n) + L").wav";
    }
    if (!WriteWavToFile(modalPreview_.clip, path)) {
        MYE_LOG_ERROR("could not write modal preview wav: %s", WideToUtf8(path).c_str());
        return;
    }
    if (ctx.audio != nullptr) {
        ctx.audio->LoadClipFile(path); // 生成直後に登録 → Asset Browser から再生できる
    }
    MYE_LOG_INFO("modal preview wav saved: %s", WideToUtf8(path).c_str());
}

// M80p: ModelCook::TryLoadCookedMeshVertices はファイル全体の読み込み・検証・
// デシリアライズを伴い、DrawDestructibleNotes から毎フレーム呼ばれると重い。(srcPath, meshKey)
// が前回と同じで、その間に資産のホットリロードが起きていなければ (ReloadCount 不変) 結果を使い回す
bool InspectorWindow::GetCachedSkinWeights(EngineContext& ctx, uint64_t fid, const std::wstring& srcPath,
                                           const std::string& meshKey,
                                           std::vector<MeshVertex>& outVertices)
{
    const uint64_t reloadCount = ctx.reloadHub != nullptr ? ctx.reloadHub->ReloadCount() : 0;
    auto it = fractureSkinWeightCache_.find(fid);
    if (it != fractureSkinWeightCache_.end() && it->second.srcPath == srcPath
        && it->second.meshKey == meshKey && it->second.reloadCountAtCache == reloadCount) {
        outVertices = it->second.vertices;
        return it->second.found;
    }

    FractureSkinWeightCache entry;
    entry.srcPath = srcPath;
    entry.meshKey = meshKey;
    entry.reloadCountAtCache = reloadCount;
    entry.found = ModelCook::TryLoadCookedMeshVertices(srcPath, meshKey, entry.vertices);
    outVertices = entry.vertices;
    const bool found = entry.found;
    fractureSkinWeightCache_[fid] = std::move(entry);
    return found;
}

// M80i: Destructible 節の末尾。焼き中の表示・完了時の確定 (CommitFractureBakeResult)・
// 生成ボタンの可否判定・拒否/失敗理由を出す。マルチ選択では呼ばれない (呼び出し元で除外済み)
void InspectorWindow::DrawDestructibleNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                            const InspectorTargets& tg, const InspectorComponentRow& row)
{
    (void)row; // 実体は world 経由で取り直す (他の分岐と同じ流儀)
    World& world = ctx.scene->GetWorld();
    const EntityID e = tg.e;
    auto* comp = world.GetComponent<DestructibleComponent>(e);
    if (comp == nullptr) {
        return;
    }

    // 前フレームまでに焼きが終わっていれば、ここで初めて確定させる (メインスレッド限定の
    // .mfrac 書き出し・AssetDatabase/FractureLibrary 登録・BuildFracturePieces + Undo)
    if (fractureBakeService_.GetState(tg.fid) == FractureBakeJobState::Ready) {
        CommitFractureBakeResult(ctx, selection, undo, tg);
    }

    ImGui::Separator();
    const FractureBakeJobState state = fractureBakeService_.GetState(tg.fid);
    if (state == FractureBakeJobState::Baking) {
        ImGui::TextDisabled(Tr(StrId::Insp_FractureBaking),
                            FractureStageLabel(fractureBakeService_.GetStage(tg.fid)));
        if (ImGui::Button(Tr(StrId::Insp_FractureCancel))) {
            fractureBakeService_.Cancel(tg.fid);
        }
    } else {
        // 「生成済み」表示は焼き結果のキャッシュではなく今の世界の状態 (Destructible.fractureAsset
        // + 直子の数) から毎フレーム導出する — シーンを読み直しても正しい表示になるように
        if (comp->fractureAsset.IsNull()) {
            ImGui::TextUnformatted(Tr(StrId::Insp_FractureStateNone));
        } else {
            // 解決は FractureSystem と共有 (FindByAssetId だけを直接呼ぶと、GUID→パス経由が
            // 要るファイル資産を一生見つけられない)
            const FractureAssetHandle* handle = ResolveFractureAsset(comp->fractureAsset);
            // 判定も FractureSystem::Update と共有: broken 後は割れた塊がルートの親の下へ
            // 移るため、直子の数だけを見ると誤って「一致していません」になる
            const bool piecesMatch
                = handle != nullptr && DestructiblePiecesMatchAsset(world, e, *handle, comp->broken);
            if (!piecesMatch) {
                ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Insp_FractureMismatch));
            } else {
                ImGui::Text(Tr(StrId::Insp_FractureStateReady), static_cast<int>(handle->pieces.size()),
                           handle->data.droppedNeighborTotal, handle->data.mergedCount);
                // M80k: sub-11 の実測による推奨上限 (64、bench.md)。ハード上限 256 は変えない
                constexpr int kRecommendedMaxPieces = 64;
                if (static_cast<int>(handle->pieces.size()) > kRecommendedMaxPieces) {
                    ImGui::TextColored(themeColor::Warning, Tr(StrId::Insp_FracturePieceCountHigh),
                                       static_cast<int>(handle->pieces.size()));
                }
            }
        }
        // 直近 (このセッション中) の焼きが拒否/失敗/取り消しだったときだけ理由を添える
        const auto outIt = fractureOutcomes_.find(tg.fid);
        if (outIt != fractureOutcomes_.end() && !outIt->second.success) {
            const FractureBakeOutcome& o = outIt->second;
            if (o.cancelled) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_FractureCancelled));
            } else if (o.rejectedOpenMesh) {
                ImGui::TextColored(themeColor::Error, Tr(StrId::Insp_FractureOpenMeshReason),
                                   o.boundaryEdges, o.nonManifoldEdges, o.orientationMismatches);
                ImGui::TextColored(themeColor::Error, "%s", Tr(StrId::Insp_FractureVoxelizeHint));
            } else {
                ImGui::TextColored(themeColor::Error, Tr(StrId::Insp_FractureStateFailed),
                                   o.failReason.c_str());
            }
        }
    }

    // 生成ボタンの可否 (spec §4.3): Play 中 / プレハブインスタンス / ソースメッシュ無し / スキンは
    // ウェイトを取得できるかで判定する (複数選択は呼び出し元で除外済み、DrawComponentNotes 参照)
    const auto* mr = world.GetComponent<MeshRendererComponent>(e);
    Mesh* mesh = (mr != nullptr && !mr->mesh.IsNull() && ctx.resources != nullptr)
        ? ctx.resources->meshes.Get(mr->mesh)
        : nullptr;
    const auto* skinComp = world.GetComponent<SkinnedMeshComponent>(e);
    // スキンの焼きに要る入力 (ボーンウェイト付き頂点・骨のバインド行列)。.mmdl クックキャッシュ
    // から毎フレーム引き直す (「生成済み」表示と同じ流儀 — シーンを読み直しても正しく動くように
    // 都度導出する)。取得できなければ生成ボタンを無効にする
    std::vector<FractureSkinVertex> skinVerts;
    std::vector<FractureSkinJoint> skinJoints;
    bool skinInputReady = false;
    if (skinComp != nullptr && mesh != nullptr && mr != nullptr && ctx.resources != nullptr) {
        const std::string* meshName = ctx.resources->meshes.NameOf(mr->mesh);
        const std::wstring srcPath =
            meshName != nullptr ? assetkey::SourcePathForSubAssetKey(*meshName) : std::wstring{};
        std::vector<MeshVertex> weighted;
        if (meshName != nullptr && !srcPath.empty()
            && GetCachedSkinWeights(ctx, tg.fid, srcPath, *meshName, weighted)) {
            if (const SkinnedModel* model = ctx.resources->skinnedModels.Get(skinComp->model)) {
                skinJoints.reserve(model->joints.size());
                for (const SkeletonJoint& j : model->joints) {
                    skinJoints.push_back({ j.name, j.inverseBind });
                }
                skinVerts.resize(weighted.size());
                for (size_t i = 0; i < weighted.size(); ++i) {
                    skinVerts[i].position = weighted[i].position;
                    std::memcpy(skinVerts[i].boneIndices, weighted[i].boneIndices,
                               sizeof(skinVerts[i].boneIndices));
                    skinVerts[i].boneWeights = weighted[i].boneWeights;
                }
                skinInputReady = !skinJoints.empty();
            }
        }
    }
    const char* disableReason = nullptr;
    if (inPlayMode_) {
        // Play 中の生成は .mfrac 自体は書けるが、Stop でシーンがスナップショットへ巻き戻り
        // 生成した子ごと消える (PlayModeController.h の契約) ので黙って無駄骨にしない
        disableReason = Tr(StrId::Insp_FracturePlayModeDisabled);
    } else if (tg.isPrefabMember) {
        disableReason = Tr(StrId::Insp_FracturePrefabDisabled);
    } else if (mesh == nullptr || mesh->positions.empty() || mesh->indices.empty()) {
        disableReason = Tr(StrId::Insp_FractureNoMesh);
    } else if (skinComp != nullptr && !skinInputReady) {
        disableReason = Tr(StrId::Insp_FractureSkinNoWeights);
    }
    const bool canGenerate = (disableReason == nullptr) && (state != FractureBakeJobState::Baking);

    ImGui::BeginDisabled(!canGenerate);
    if (ImGui::Button(Tr(StrId::Insp_FractureGenerate))) {
        FractureBakeRequest req;
        req.sourceMesh = ToFractureMeshForBake(*mesh);
        const std::string* meshName = ctx.resources->meshes.NameOf(mr->mesh);
        req.sourceMeshHash = meshName ? HashStr(*meshName) : 0;
        req.seed = comp->seed;
        req.pieceCount = comp->pieceCount;
        req.openMeshMode = comp->openMeshMode;
        req.voxelResolution = comp->voxelResolution;
        if (skinInputReady) {
            req.skinVertices = std::move(skinVerts);
            req.skinJoints = std::move(skinJoints);
        }
        fractureBakeService_.Request(tg.fid, std::move(req));
    }
    ImGui::EndDisabled();
    if (disableReason != nullptr) {
        ImGui::TextDisabled("%s", disableReason);
    }
}

// M80i: 焼き結果が Ready になった直後 (メインスレッド) に 1 回呼ぶ。失敗/拒否は
// fractureOutcomes_ に理由を残すだけ (UI 表示は DrawDestructibleNotes 側)。成功した場合の
// 確定処理 (.mfrac 保存・AssetDatabase/FractureLibrary 登録・Destructible.fractureAsset の
// 書き換え・BuildFracturePieces・Undo 記録) は CommitFractureBake (FractureBakeCommit.h) が
// 持つ — ImGui に触れない純粋なロジックなので SelfTest からも直接呼べる
void InspectorWindow::CommitFractureBakeResult(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                               const InspectorTargets& tg)
{
    // ギズモ等の複数フレームの Undo 記録中は確定を持ち越す (結果は Ready のまま残り、記録が閉じたフレームで取り込む)
    if (undo.IsRecording()) {
        return;
    }
    FractureBakeRequest req;
    FractureBakeResult result;
    std::vector<std::string> pieceBoneNames; // M80j: スキンのときだけ非空
    if (!fractureBakeService_.TakeResult(tg.fid, req, result, pieceBoneNames)) {
        return;
    }

    FractureBakeOutcome outcome;
    outcome.success = result.success;
    outcome.rejectedOpenMesh = result.rejectedOpenMesh;
    outcome.boundaryEdges = result.boundaryEdges;
    outcome.nonManifoldEdges = result.nonManifoldEdges;
    outcome.orientationMismatches = result.orientationMismatches;
    outcome.cancelled = result.cancelled;
    outcome.failReason = result.failReason;
    fractureOutcomes_[tg.fid] = outcome;

    if (!result.success) {
        World& world = ctx.scene->GetWorld();
        MYE_LOG_ERROR("[fracture] bake failed for '%s': %s", world.GetName(tg.e),
                     result.failReason.c_str());
        return;
    }
    if (!CommitFractureBake(ctx, selection, undo, tg.e, tg.fid, req, result, pieceBoneNames)) {
        fractureOutcomes_[tg.fid].success = false;
        fractureOutcomes_[tg.fid].failReason = "failed to write or register the .mfrac asset";
    }
}

void InspectorWindow::DrawPatrolRouteNotes(EngineContext& ctx, Selection& selection, UndoStack& undo, const InspectorTargets& tg)
{
    constexpr int32_t kMaxWaitTicks = 216000; // BT のパラメータの tick 数の上限 (kBtMaxTicksParam) と同じ
    World& world = ctx.scene->GetWorld();
    PatrolRouteComponent* route = world.GetComponent<PatrolRouteComponent>(tg.e);
    if (route == nullptr) {
        return;
    }
    ImGui::Separator();
    const int count = (std::min)((std::max)(route->pointCount, 0), kMaxPatrolPoints);
    ImGui::TextDisabled("%s: %d / %d", Tr(StrId::Insp_PatrolPoints), count, kMaxPatrolPoints);
    if (count == 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(themeColor::Warning, "%s", Tr(StrId::Insp_PatrolNoPoints));
        ImGui::PopTextWrapPos();
    }

    int removeIndex = -1;
    int moveIndex = -1;
    int moveDelta = 0;
    for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%d", i);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        ImGui::DragFloat3("##pos", &route->points[i].x, 0.05f);
        HandleEditUndo(ctx, selection, undo, tg.fid, "Edit Patrol Point");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f);
        ImGui::DragInt("##wait", &route->waitTicks[i], 1.0f, 0, kMaxWaitTicks);
        HandleEditUndo(ctx, selection, undo, tg.fid, "Edit Patrol Wait");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_PatrolWait));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton(ICON_FA_ARROW_UP "##up")) {
            moveIndex = i;
            moveDelta = -1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_PatrolUp));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == count - 1);
        if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "##down")) {
            moveIndex = i;
            moveDelta = 1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_PatrolDown));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##remove")) {
            removeIndex = i;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_PatrolRemove));
        }
        ImGui::PopID();
    }

    const auto edit = [&](const char* label, auto&& mutate) {
        undo.Record(label, *ctx.scene, selection, tg.fid, UndoStack::StructuralChanges::None, [&] {
            if (auto* editable = world.GetComponent<PatrolRouteComponent>(tg.e)) {
                mutate(*editable);
            }
        });
    };
    if (removeIndex >= 0) {
        edit("Remove Patrol Point", [&](PatrolRouteComponent& r) { PatrolRouteRemovePoint(r, removeIndex); });
    } else if (moveIndex >= 0) {
        edit("Move Patrol Point", [&](PatrolRouteComponent& r) { PatrolRouteMovePoint(r, moveIndex, moveDelta); });
    }
    ImGui::BeginDisabled(count >= kMaxPatrolPoints);
    if (ImGui::Button(Tr(StrId::Insp_PatrolAdd))) {
        edit("Add Patrol Point", [](PatrolRouteComponent& r) { PatrolRouteAddPoint(r); });
    }
    ImGui::EndDisabled();
    if (count >= kMaxPatrolPoints) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Tr(StrId::Insp_PatrolFull));
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PatrolHint));
    ImGui::PopTextWrapPos();
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
                        AddComponentWithRequirements(world, tg.ents[i], t); // M82c: NavMeshAgent なら CC も同じ Undo で
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
    bool readOnly = (field.flags & kFieldReadOnly) != 0;
    // autoCellSize が true の Surface では cellSize / cellHeight は使われない (M82d): 無効表示にする
    if (std::strcmp(componentName, "NavMeshSurface") == 0
        && (std::strcmp(field.name, "cellSize") == 0 || std::strcmp(field.name, "cellHeight") == 0)
        && static_cast<const NavMeshSurfaceComponent*>(comp)->autoCellSize) {
        readOnly = true;
    }
    // ベイク寸法は Agent Type の写し (M84a)。表にある型なら Project Settings でだけ変える
    if (std::strcmp(componentName, "NavMeshSurface") == 0
        && (std::strcmp(field.name, "agentRadius") == 0 || std::strcmp(field.name, "agentHeight") == 0
            || std::strcmp(field.name, "maxClimb") == 0 || std::strcmp(field.name, "maxSlopeDeg") == 0
            || std::strcmp(field.name, "dropHeight") == 0 || std::strcmp(field.name, "jumpDistance") == 0)) {
        NavAgentTypes& types = NavAgentTypes::Get();
        types.Load(ctx.assetsRoot);
        if (types.Find(static_cast<const NavMeshSurfaceComponent*>(comp)->agentTypeId) != nullptr) {
            readOnly = true;
        }
    }
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
        } else if (componentName
                   && (std::strcmp(componentName, "NavMeshSurface") == 0
                       || std::strcmp(componentName, "NavMeshAgent") == 0)
                   && std::strcmp(field.name, "agentTypeId") == 0) {
            // M84a: Agent Type は Project Settings の表から名前で選ぶ。表に無い id はそのまま保つ
            // (別プロジェクトのシーンを開いたときに黙って 0 へ潰さない) ため、末尾に「未定義の id」の項目を足して選択中にする。
            // Combo を使うのは、終わった後の項目状態が Combo 自身になり、呼び出し側の HandleEditUndoMulti がそのまま効くから
            NavAgentTypes& types = NavAgentTypes::Get();
            types.Load(ctx.assetsRoot);
            int& v = *static_cast<int*>(p);
            const char* labels[NavAgentTypes::kMaxTypes + 1];
            for (int i = 0; i < types.Count(); ++i) {
                labels[i] = types.At(i).name;
            }
            int count = types.Count();
            int index = types.IndexOf(v);
            char undefinedLabel[48];
            if (index < 0) {
                std::snprintf(undefinedLabel, sizeof(undefinedLabel), Tr(StrId::Insp_NavAgentTypeUndefined), v);
                labels[count] = undefinedLabel;
                index = count;
                ++count;
            }
            if (ImGui::Combo(label, &index, labels, count) && index < types.Count() && types.At(index).id != v) {
                v = types.At(index).id;
                changed = true;
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
        } else if (componentName && std::strcmp(componentName, "BehaviorTree") == 0
                   && std::strncmp(field.name, "bbEntityKey", 11) == 0) {
            // M85h: Entity キーの初期値のキー名は、木が使う BB の Entity キーから選ぶ。
            // 木 / BB が引けないときは自由入力にフォールバックする (既に入っている名前を消させない)
            char* key = static_cast<char*>(p);
            const auto* bt = static_cast<const BehaviorTreeComponent*>(comp);
            BehaviorTreeLibrary* trees = behaviortree::Library();
            BlackboardLibrary* boards = blackboard::Library();
            const BehaviorTreeAsset* tree = trees != nullptr ? trees->Get(bt->tree.value) : nullptr;
            const BlackboardAsset* board = (tree != nullptr && boards != nullptr) ? boards->Get(tree->blackboard) : nullptr;
            if (board == nullptr) {
                changed = ImGui::InputText(label, key, 64);
                if (changed) {
                    ZeroStringTail(key, 64);
                }
                if (std::strcmp(field.name, "bbEntityKey0") == 0) {
                    ImGui::TextDisabled("%s", Tr(StrId::Insp_BtEntityKeyNoBoard)); // 4 組とも同じ理由なので先頭の 1 回だけ
                }
            } else {
                char preview[160];
                const int found = board->FindKey(key);
                if (key[0] == '\0') {
                    std::snprintf(preview, sizeof(preview), "%s", Tr(StrId::Insp_BtEntityKeyNone));
                } else if (found >= 0 && board->keys[static_cast<size_t>(found)].type == BbType::Entity) {
                    std::snprintf(preview, sizeof(preview), "%s", key);
                } else {
                    std::snprintf(preview, sizeof(preview), Tr(StrId::Insp_BtEntityKeyMissing), key);
                }
                if (ImGui::BeginCombo(label, preview)) {
                    if (ImGui::Selectable(Tr(StrId::Insp_BtEntityKeyNone), key[0] == '\0')) {
                        std::memset(key, 0, 64); // 生バイトが hash 対象なので末尾までゼロ埋め
                        changed = true;
                    }
                    for (size_t i = 0; i < board->keys.size(); ++i) {
                        if (board->keys[i].type != BbType::Entity) {
                            continue;
                        }
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(board->keys[i].name.c_str(), board->keys[i].name == key)) {
                            std::memset(key, 0, 64);
                            std::snprintf(key, 64, "%s", board->keys[i].name.c_str());
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
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
    // ディスク上のファイルも候補に出す種別 (Unknown = ライブラリの一覧だけ)。
    // ディスクの走査はポップアップを開いている間だけ行う (毎フレーム assets 全体を再帰走査し、
    // .meta の無いファイルへ .meta を書き出していた)
    AssetType diskType = AssetType::Unknown;
    if (fname == "tree") {
        // M85: ビヘイビアツリー (BehaviorTree.tree)。"tex" など他の語には当たらない
        if (BehaviorTreeLibrary* bt = behaviortree::Library()) {
            for (const BehaviorTreeEntry& e : bt->Enumerate()) {
                entries.push_back({ AssetID{ e.hash }, e.name });
            }
        }
    } else if (fname.find("navfilter") != std::string::npos) {
        // M84c: ナビのエリアのフィルタ (NavMeshAgent.navFilter)
        if (NavFilterLibrary* nf = navfilter::Library()) {
            for (const NavFilterEntry& e : nf->Enumerate()) {
                entries.push_back({ AssetID{ e.hash }, e.name });
            }
        }
    } else if (fname.find("physmat") != std::string::npos) {
        // M59a1: 物理マテリアル (M59a2 の Collider.physMaterial 等)。
        // ★"material" より**先に**見ること。小文字化した "physmaterial" は "material" を
        //   含むので、順序を誤ると MaterialLibrary と取り違えたまま気付けない
        if (PhysMatLibrary* pm = physmat::Library()) {
            for (const PhysMatEntry& e : pm->Enumerate()) {
                entries.push_back({ AssetID{ e.hash }, e.name });
            }
        }
    } else if (fname == "fxstack") {
        // M78c: プロジェクトポスト/コンピュートスタック (*.fxstack.json はディスクにしか無い)
        diskType = AssetType::FxStack;
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
        diskType = AssetType::Texture; // 未ロードのテクスチャもポップアップで出す (MaterialInspector と同じ)
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

    std::string resolvedName;
    const char* cur = Tr(StrId::Insp_NoneItem);
    for (const AssetEntry& e : entries) {
        if (e.id == *id) {
            cur = e.name.c_str();
        }
    }
    if (std::strcmp(cur, Tr(StrId::Insp_NoneItem)) == 0 && !id->IsNull()) {
        const std::wstring resPath = assetguid::ResolvePath(id->value);
        if (!resPath.empty()) {
            std::error_code ec;
            resolvedName = (diskType == AssetType::FxStack)
                ? WideToUtf8(std::filesystem::path(resPath).stem().stem().wstring()) // ポップアップの表記と揃える
                : WideToUtf8(std::filesystem::relative(resPath, ctx.assetsRoot, ec).wstring());
            cur = resolvedName.c_str();
        }
    }

    auto assign = [&](AssetID v) {
        if (!v.IsNull() && fname.find("tex") != std::string::npos) {
            if (!ctx.resources->textures.Get(v)) {
                const std::wstring loadPath = assetguid::ResolvePath(v.value);
                if (!loadPath.empty()) {
                    ctx.resources->textures.RequestLoadFileAsync(loadPath);
                }
            }
        }
        // マルチ選択は全対象へバッチ適用 (1 Undo エントリ、M40a)
        undo.Record("Assign asset", *ctx.scene, selection, fids,
                    UndoStack::StructuralChanges::None, [&] {
            for (void* c : comps) {
                *reinterpret_cast<AssetID*>(static_cast<uint8_t*>(c) + fieldOffset) = v;
            }
        });
    };

    char labelBuf[192];
    ImGui::PushID(MakeFieldLabel(field, labelBuf, sizeof(labelBuf)));
    ImGui::TextUnformatted(FieldLabelText(field));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::Button(cur, ImVec2(-1, 0))) {
        ImGui::OpenPopup("##assetpick");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::string utf8 = static_cast<const char*>(pa->Data);
            const std::wstring dropPath = Utf8ToWide(utf8);
            if (fname.find("tex") != std::string::npos && AssetDatabase::ClassifyPath(dropPath) == AssetType::Texture) {
                const AssetID aid = ctx.resources->textures.RequestLoadFileAsync(dropPath);
                assign(aid);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("##assetpick")) {
        if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
            assign(AssetID{});
        }
        for (const AssetEntry& e : entries) {
            if (ImGui::Selectable(e.name.c_str(), e.id == *id)) {
                assign(e.id);
            }
        }
        if (diskType != AssetType::Unknown) {
            for (const DiskAssetCandidate& c : CollectDiskAssetCandidates(ctx.assetsRoot, diskType)) {
                // 読むだけの解決 (.meta を作らない)。ロード済みでライブラリ側に出ている物は重ねない
                const uint64_t known = ctx.assetDb ? ctx.assetDb->GuidForPath(c.path, false) : 0;
                if (known != 0 && std::any_of(entries.begin(), entries.end(),
                                              [&](const AssetEntry& ae) { return ae.id.value == known; })) {
                    continue;
                }
                const std::string label = (diskType == AssetType::FxStack)
                    ? WideToUtf8(std::filesystem::path(c.path).stem().stem().wstring()) // foo.fxstack.json → foo
                    : c.relUtf8;
                ImGui::PushID(c.relUtf8.c_str());
                if (ImGui::Selectable(label.c_str(), known != 0 && known == id->value)) {
                    // 選んだときだけ GUID を確定する (.meta 不在なら作り、表にも登録して ResolvePath で引けるように)
                    const uint64_t guid = ctx.assetDb ? ctx.assetDb->GuidForPath(c.path, true)
                                                      : AssetDatabase::EnsureMeta(c.path);
                    assign(AssetID{ guid });
                }
                ImGui::PopID();
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
