#include "Editor/Windows/InspectorWindow.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Editor/AssetOps.h"
#include "Editor/EditorComponentCatalog.h"
#include "Editor/PhysicsLayerNames.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

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
// ドラッグ全体が 1 エントリになる (transient マージ)
void HandleEditUndo(EngineContext& ctx, Selection& sel, UndoStack& undo, uint64_t fid,
                    const char* label)
{
    if (ImGui::IsItemActivated() && !undo.IsRecording()) {
        undo.BeginRecord(label, sel);
        undo.CaptureBefore(*ctx.scene, fid);
    }
    if (undo.IsRecording()) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo.CaptureAfter(*ctx.scene, fid);
            undo.EndRecord(sel);
        } else if (ImGui::IsItemDeactivated()) {
            undo.CancelRecord(); // 値を変えずに離した
        }
    }
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
struct EnumFieldLabels {
    const char* component;
    const char* field;
    const char* const* labels;
    int count;
};
constexpr const char* kColliderShapeLabels[] = { "Sphere", "Box", "Capsule" };
constexpr const char* kLightTypeLabels[] = { "Directional", "Point", "Spot" };
constexpr const char* kEmitterShapeLabels[] = { "Point", "Sphere", "Cone", "Box" };
constexpr const char* kBlendModeLabels[] = { "Additive", "Alpha" };
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
constexpr EnumFieldLabels kEnumFields[] = {
    { "Collider", "shape", kColliderShapeLabels, 3 },
    { "Light", "type", kLightTypeLabels, 3 },
    { "ParticleEmitter", "shape", kEmitterShapeLabels, 4 },
    { "ParticleEmitter", "blendMode", kBlendModeLabels, 2 },
    { "UIElement", "kind", kUIKindLabels, 3 },
    { "UIElement", "anchor", kUIAnchorLabels, 9 },
    { "ConstantForce", "relative", kForceSpaceLabels, 2 },
    { "SpriteRenderer", "billboardMode", kBillboardLabels, 3 },
    { "TextMesh", "billboardMode", kBillboardLabels, 3 },
    { "Skybox", "mode", kSkyboxModeLabels, 2 },
    { "Fog", "mode", kFogModeLabels, 3 },
    { "CameraPostFx", "tonemapMode", kTonemapLabels, 3 },
    { "CameraPostFx", "bloomOn", kOffOnLabels, 2 },
    { "CameraPostFx", "fxaaOn", kOffOnLabels, 2 },
};

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
        ImGui::TextDisabled("(no managed instance — click 'Compile C# Scripts')");
        return;
    }
    const auto* fields = mh->FieldsForComponent(t);
    if (!fields || fields->empty()) {
        ImGui::TextDisabled("(no public fields)");
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

} // namespace

void InspectorWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Inspector", &open)) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();
    // 選択は fileId 保持 — 現フレームの EntityID に解決する
    const uint64_t fid = selection.primary;
    GameObject go = ctx.scene->FindByFileId(fid);
    const EntityID e = go ? go.Id() : kNullEntity;
    if (fid == 0 || !world.IsAlive(e)) {
        ImGui::TextDisabled("(no selection)");
        ImGui::End();
        return;
    }

    // ---- プレハブ所属判定 (青文字 / オーバーライド表示 / Revert・Apply に使う) ----
    const EntityID prefabRoot = Prefab::FindInstanceRoot(world, e);
    const bool isPrefabMember = !prefabRoot.IsNull();
    const ImVec4 kPrefabBlue(0.45f, 0.68f, 1.0f, 1.0f);

    // ---- 名前 ----
    if (auto* nc = world.GetComponent<NameComponent>(e)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", nc->value, sizeof(nc->value));
        HandleEditUndo(ctx, selection, undo, fid, "Rename");
        if (isPrefabMember && Prefab::IsNameOverridden(*ctx.scene, *ctx.prefabs, e)) {
            ImGui::SameLine();
            ImGui::TextColored(kPrefabBlue, "*");
        }
    }
    ImGui::TextDisabled("Entity %u:%u  (fileId %llu)", e.index, e.generation,
                        static_cast<unsigned long long>(fid));

    // ---- プレハブバー (Revert All / Apply All) ----
    if (isPrefabMember) {
        auto* inst = world.GetComponent<PrefabInstanceComponent>(prefabRoot);
        const PrefabAsset* asset = inst ? ctx.prefabs->Get(inst->prefabHash) : nullptr;
        ImGui::TextColored(kPrefabBlue, "Prefab: %s", asset ? asset->name.c_str() : "(missing)");
        const uint64_t rootFid = ctx.scene->EnsureFileId(prefabRoot);
        if (ImGui::SmallButton("Revert All")) {
            undo.BeginRecord("Revert Prefab", selection);
            undo.CaptureBefore(*ctx.scene, rootFid);
            Prefab::RevertInstance(*ctx.scene, *ctx.prefabs, rootFid);
            ctx.scene->GetWorld().ApplyStructuralChanges();
            undo.CaptureAfter(*ctx.scene, rootFid);
            undo.EndRecord(selection);
        }
        ImGui::SameLine();
        // Apply は他インスタンス・アセットファイルも更新するため Undo 対象外 (Unity 同様)
        if (ImGui::SmallButton("Apply All")) {
            Prefab::ApplyInstance(*ctx.scene, *ctx.prefabs, rootFid);
            ctx.scene->GetWorld().ApplyStructuralChanges();
        }
    }
    ImGui::Separator();

    // ---- コンポーネント一覧 (アーキタイプの型リスト = TypeId 昇順) ----
    const ComponentRegistry& reg = ComponentRegistry::Get();
    const Archetype* arch = world.GetArchetype(e);
    if (!arch) {
        ImGui::End();
        return;
    }
    // 型リストをコピー (描画中の RemoveComponent でアーキタイプが変わっても安全に)
    std::vector<ComponentTypeId> types(arch->Types().begin(), arch->Types().end());
    for (ComponentTypeId t : types) {
        const ComponentDesc& desc = reg.Desc(t);
        if ((desc.flags & kComponentHidden) && t != LocalTransform::sTypeId) {
            continue;
        }
        if (t == NameComponent::sTypeId) {
            continue; // 上部で表示済み
        }
        ImGui::PushID(static_cast<int>(t));
        const std::string headerLabel =
            std::string(ComponentUiFor(desc.name).icon) + " " + desc.name;
        const bool openHeader =
            ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem("##comp_ctx")) {
            if (ImGui::MenuItem("Remove Component")) {
                undo.BeginRecord("Remove Component", selection);
                undo.CaptureBefore(*ctx.scene, fid);
                world.RemoveComponentRaw(e, t); // 基本コンポーネントは World 側で拒否
                world.ApplyStructuralChanges();
                undo.CaptureAfter(*ctx.scene, fid);
                undo.EndRecord(selection);
            }
            ImGui::EndPopup();
        }
        if (openHeader) {
            void* comp = world.GetComponentRaw(e, t);
            // C# スクリプトコンポーネント: フィールドは managed 側が保持 → 専用描画パス
            if (comp && ctx.managedHost && ctx.managedHost->IsManagedComponent(t)) {
                DrawManagedComponentFields(ctx, t, comp, e);
                ImGui::PopID();
                continue;
            }
            if (comp) {
                for (const FieldDesc& f : desc.fields) {
                    if (f.flags & kFieldHidden) {
                        continue;
                    }
                    DrawField(ctx, desc.name, comp, f, e, selection, undo, fid);
                    // 参照ピッカー / 衝突マスク (M36a) はポップアップ内で自前 Undo を記録するので除外
                    const bool ownUndo = f.type == FieldType::AssetRef
                        || f.type == FieldType::EntityRef
                        || (std::strcmp(desc.name, "Collider") == 0
                            && std::strcmp(f.name, "mask") == 0);
                    if (!ownUndo) {
                        HandleEditUndo(ctx, selection, undo, fid, "Modify");
                    }
                    // ---- プレハブオーバーライド: 右クリック Revert/Apply + マーカー ----
                    if (isPrefabMember) {
                        if (f.type != FieldType::AssetRef && f.type != FieldType::EntityRef
                            && ImGui::BeginPopupContextItem()) {
                            const bool ov = Prefab::IsFieldOverridden(*ctx.scene, *ctx.prefabs, e,
                                                                      desc.name, f);
                            if (ImGui::MenuItem("Revert to Prefab", nullptr, false, ov)) {
                                undo.BeginRecord("Revert Field", selection);
                                undo.CaptureBefore(*ctx.scene, fid);
                                Prefab::RevertField(*ctx.scene, *ctx.prefabs, e, desc.name, f);
                                undo.CaptureAfter(*ctx.scene, fid);
                                undo.EndRecord(selection);
                            }
                            if (ImGui::MenuItem("Apply to Prefab")) {
                                Prefab::ApplyInstance(*ctx.scene, *ctx.prefabs,
                                                      ctx.scene->EnsureFileId(prefabRoot));
                                ctx.scene->GetWorld().ApplyStructuralChanges();
                            }
                            ImGui::EndPopup();
                        }
                        if (Prefab::IsFieldOverridden(*ctx.scene, *ctx.prefabs, e, desc.name, f)) {
                            ImGui::SameLine();
                            ImGui::TextColored(kPrefabBlue, "*");
                        }
                    }
                }
            }
        }
        ImGui::PopID();
    }

    // ---- Add Component ----
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) {
        addComponentFilter_[0] = '\0';
        ImGui::OpenPopup("##add_component");
    }
    if (ImGui::BeginPopup("##add_component")) {
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
                if (world.HasComponent(e, t)) {
                    continue;
                }
                const ComponentUiInfo& info = ComponentUiFor(desc.name);
                if (std::strcmp(info.category, cat) != 0) {
                    continue;
                }
                if (!ContainsIgnoreCase(desc.name, addComponentFilter_)) {
                    continue;
                }
                if (!headerShown) {
                    ImGui::SeparatorText(cat);
                    headerShown = true;
                }
                const std::string label = std::string(info.icon) + " " + desc.name;
                if (ImGui::MenuItem(label.c_str())) {
                    undo.BeginRecord("Add Component", selection);
                    undo.CaptureBefore(*ctx.scene, fid);
                    world.AddComponentRaw(e, t);
                    world.ApplyStructuralChanges();
                    undo.CaptureAfter(*ctx.scene, fid);
                    undo.EndRecord(selection);
                }
            }
        }
        ImGui::EndPopup();
    }

    // ---- スクリプト D&D 受け皿 (M31): パネル残余をターゲット化して .cs をアタッチ ----
    // AssetBrowser/SceneView の .cs をここへドロップすると表示中エンティティに付与される。
    const ImVec2 dropAvail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(dropAvail.x, dropAvail.y > 48.0f ? dropAvail.y : 48.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::wstring path = Utf8ToWide(static_cast<const char*>(pa->Data));
            if (AssetDatabase::ClassifyPath(path) == AssetType::Script) {
                AttachScriptToEntity(ctx, selection, undo, path, e);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::End();
}

bool InspectorWindow::DrawField(EngineContext& ctx, const char* componentName, void* comp,
                                const FieldDesc& field, EntityID entity, Selection& selection,
                                UndoStack& undo, uint64_t fid)
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

    bool changed = false;
    switch (field.type) {
    case FieldType::Float:
        changed = ImGui::DragFloat(field.name, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Int32:
        // M36a: 物理レイヤーは project_settings.json の動的名前で Combo (sim は index のみ)
        if (componentName && std::strcmp(componentName, "Collider") == 0
            && std::strcmp(field.name, "layer") == 0) {
            PhysicsLayerNames& ln = PhysicsLayerNames::Get();
            ln.Load(ctx.assetsRoot);
            const char* labels[PhysicsLayerNames::kCount];
            ln.BuildComboLabels(labels);
            int v = *static_cast<int*>(p);
            if (v < 0 || v >= PhysicsLayerNames::kCount) {
                v = -1;
            }
            changed = ImGui::Combo(field.name, &v, labels, PhysicsLayerNames::kCount);
            if (changed) {
                *static_cast<int*>(p) = v;
            }
        } else if (const EnumFieldLabels* ef = FindEnumLabels(componentName, field.name)) {
            int v = *static_cast<int*>(p);
            if (v < 0 || v >= ef->count) {
                v = -1; // 範囲外は "(invalid)" 表示 (値は選択されるまで保持)
            }
            changed = ImGui::Combo(field.name, &v, ef->labels, ef->count);
            if (changed) {
                *static_cast<int*>(p) = v;
            }
        } else {
            changed = ImGui::DragInt(field.name, static_cast<int*>(p), 1.0f, static_cast<int>(lo),
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
                std::snprintf(summary, sizeof(summary), "Everything##mask");
            } else if (m == 0u) {
                std::snprintf(summary, sizeof(summary), "Nothing##mask");
            } else {
                std::snprintf(summary, sizeof(summary), "Mixed (0x%08X)##mask", m);
            }
            if (ImGui::Button(summary)) {
                ImGui::OpenPopup("##collider_mask");
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(field.name);
            if (ImGui::BeginPopup("##collider_mask")) {
                auto applyMask = [&](uint32_t next) {
                    undo.BeginRecord("Modify Mask", selection);
                    undo.CaptureBefore(*ctx.scene, fid);
                    m = next;
                    undo.CaptureAfter(*ctx.scene, fid);
                    undo.EndRecord(selection);
                    changed = true;
                };
                if (ImGui::SmallButton("Everything")) {
                    applyMask(0xFFFFFFFFu);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Nothing")) {
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
        } else {
            changed = ImGui::InputScalar(field.name, ImGuiDataType_U32, p);
        }
        break;
    case FieldType::UInt64:
        changed = ImGui::InputScalar(field.name, ImGuiDataType_U64, p);
        break;
    case FieldType::Bool: {
        bool b = *static_cast<uint8_t*>(p) != 0;
        if (ImGui::Checkbox(field.name, &b)) {
            *static_cast<uint8_t*>(p) = b ? 1 : 0;
            changed = true;
        }
        break;
    }
    case FieldType::Float2:
        changed = ImGui::DragFloat2(field.name, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Float3:
        changed = ImGui::DragFloat3(field.name, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Float4:
        changed = ImGui::DragFloat4(field.name, static_cast<float*>(p), speed, lo, hi);
        break;
    case FieldType::Quat: {
        // オイラー角 (度) で編集。編集中はキャッシュを使い往復変換ドリフトを防ぐ
        auto* q = static_cast<XMFLOAT4*>(p);
        XMFLOAT3 euler;
        const bool usingCache = eulerEditing_ && eulerCacheEntity_ == entity && eulerCacheField_ == p;
        euler = usingCache ? eulerCache_ : QuatToEulerDeg(*q);
        if (ImGui::DragFloat3(field.name, &euler.x, 0.5f)) {
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
        changed = ImGui::ColorEdit4(field.name, static_cast<float*>(p));
        break;
    case FieldType::EntityRef:
        DrawEntityRef(ctx, field, p, selection, undo, fid);
        break;
    case FieldType::AssetRef:
        DrawAssetRef(ctx, field, p, selection, undo, fid);
        break;
    case FieldType::String64:
        changed = ImGui::InputText(field.name, static_cast<char*>(p), 64);
        break;
    case FieldType::String256:
        changed = ImGui::InputText(field.name, static_cast<char*>(p), 256);
        break;
    case FieldType::Float4x4: {
        const float* m = static_cast<const float*>(p);
        ImGui::Text("%s:", field.name);
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
                                   Selection& selection, UndoStack& undo, uint64_t fid)
{
    auto* id = static_cast<AssetID*>(p);
    // フィールド名からライブラリを推定 (mesh / material / texture)
    const std::string fname = field.name;
    std::vector<AssetEntry> entries;
    if (fname.find("mesh") != std::string::npos) {
        entries = ctx.resources->meshes.Enumerate();
    } else if (fname.find("material") != std::string::npos) {
        entries = ctx.resources->materials.Enumerate();
    } else if (fname.find("tex") != std::string::npos) {
        entries = ctx.resources->textures.Enumerate();
    } else if (fname.find("clip") != std::string::npos || fname.find("anim") != std::string::npos) {
        for (const AnimClipEntry& c : ctx.anims->Enumerate()) {
            entries.push_back({ AssetID{ c.hash }, c.name });
        }
    } else {
        entries = ctx.resources->meshes.Enumerate();
        const auto mats = ctx.resources->materials.Enumerate();
        const auto texs = ctx.resources->textures.Enumerate();
        entries.insert(entries.end(), mats.begin(), mats.end());
        entries.insert(entries.end(), texs.begin(), texs.end());
    }

    const char* cur = "(none)";
    for (const AssetEntry& e : entries) {
        if (e.id == *id) {
            cur = e.name.c_str();
        }
    }

    ImGui::PushID(field.name);
    ImGui::TextUnformatted(field.name);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::Button(cur, ImVec2(-1, 0))) {
        ImGui::OpenPopup("##assetpick");
    }
    if (ImGui::BeginPopup("##assetpick")) {
        auto assign = [&](AssetID v) {
            undo.BeginRecord("Assign asset", selection);
            undo.CaptureBefore(*ctx.scene, fid);
            *id = v;
            undo.CaptureAfter(*ctx.scene, fid);
            undo.EndRecord(selection);
        };
        if (ImGui::Selectable("(none)")) {
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
                                    Selection& selection, UndoStack& undo, uint64_t fid)
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

    const char* cur = "(none)";
    if (!id->IsNull() && world.IsAlive(*id)) {
        cur = world.GetName(*id);
    }

    ImGui::PushID(field.name);
    ImGui::TextUnformatted(field.name);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    if (ImGui::Button(cur, ImVec2(-1, 0))) {
        ImGui::OpenPopup("##entpick");
    }
    if (ImGui::BeginPopup("##entpick")) {
        auto assign = [&](EntityID v) {
            undo.BeginRecord("Assign reference", selection);
            undo.CaptureBefore(*ctx.scene, fid);
            *id = v;
            undo.CaptureAfter(*ctx.scene, fid);
            undo.EndRecord(selection);
        };
        if (ImGui::Selectable("(none)")) {
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
