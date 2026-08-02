#include "Editor/Windows/InspectorWindow.h"

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
#include "Editor/ComponentClipboard.h"
#include "Engine/Core/AssetGuidResolver.h"
#include "Editor/EditorComponentCatalog.h"
#include "Editor/PhysicsLayerNames.h"
#include "Editor/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
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
struct EnumFieldLabels {
    const char* component;
    const char* field;
    const char* const* labels;
    int count;
};
constexpr const char* kColliderShapeLabels[] = { "Sphere", "Box", "Capsule", "Mesh" };
constexpr const char* kLightTypeLabels[] = { "Directional", "Point", "Spot" };
constexpr const char* kEmitterShapeLabels[] = { "Point", "Sphere", "Cone", "Box" };
constexpr const char* kBlendModeLabels[] = { "Additive", "Alpha", "Distortion" }; // M42d
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
    { "ParticleEmitter", "blendMode", kBlendModeLabels, 3 },
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
    // ---- アセット選択 (M40c): AssetBrowser タイルクリックで Inspector に情報表示 ----
    if (selection.HasAsset()) {
        DrawAssetInspector(ctx, selection);
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

    // ---- マルチ選択の対象集合 (M40a): 生存する選択 fileId 群、primary 先頭 ----
    // 表示値は primary のもの。編集は全対象へバッチ適用 (ギズモ操作は従来どおり primary のみ)
    std::vector<uint64_t> targetFids;
    std::vector<EntityID> targetEnts;
    targetFids.push_back(fid);
    targetEnts.push_back(e);
    for (uint64_t sfid : selection.ids) {
        if (sfid == fid) {
            continue;
        }
        GameObject g = ctx.scene->FindByFileId(sfid);
        if (g && world.IsAlive(g.Id())) {
            targetFids.push_back(sfid);
            targetEnts.push_back(g.Id());
        }
    }
    const bool multi = targetFids.size() > 1;

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
    if (multi) {
        ImGui::TextDisabled("%zu entities selected — edits apply to all (gizmo: primary only)",
                            targetFids.size());
    } else {
        ImGui::TextDisabled("Entity %u:%u  (fileId %llu)", e.index, e.generation,
                            static_cast<unsigned long long>(fid));
    }

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
        // マルチ選択: 全対象が共通に持つコンポーネントだけ表示 (M40a)
        if (multi) {
            bool commonToAll = true;
            for (size_t i = 1; i < targetEnts.size(); ++i) {
                if (!world.HasComponent(targetEnts[i], t)) {
                    commonToAll = false;
                    break;
                }
            }
            if (!commonToAll) {
                continue;
            }
        }
        // この型のバッチ対象 (fileId + コンポーネント実体、[0] = primary)。
        // フィールド編集中に構造変更は起きないためポインタはフレーム内有効
        std::vector<uint64_t> tfids;
        std::vector<void*> tcomps;
        for (size_t i = 0; i < targetEnts.size(); ++i) {
            if (void* c = world.GetComponentRaw(targetEnts[i], t)) {
                tfids.push_back(targetFids[i]);
                tcomps.push_back(c);
            }
        }
        const bool managed = ctx.managedHost && ctx.managedHost->IsManagedComponent(t);

        ImGui::PushID(static_cast<int>(t));
        const std::string headerLabel =
            std::string(ComponentUiFor(desc.name).icon) + " " + desc.name;
        const bool openHeader =
            ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem("##comp_ctx")) {
            // 全対象の before/after を取り 1 Undo エントリにするバッチヘルパ (M40a)
            auto batchOp = [&](const char* label, auto&& mutate) {
                undo.BeginRecord(label, selection);
                for (uint64_t tf : tfids) {
                    undo.CaptureBefore(*ctx.scene, tf);
                }
                mutate();
                world.ApplyStructuralChanges();
                for (uint64_t tf : tfids) {
                    undo.CaptureAfter(*ctx.scene, tf);
                }
                undo.EndRecord(selection);
            };
            // C# コンポーネントはフィールドが managed 側にあるため copy/paste/reset 対象外
            ComponentClipboard& clip = GetComponentClipboard();
            if (ImGui::MenuItem("Copy Component", nullptr, false, !managed && !tcomps.empty())) {
                clip.componentName = desc.name;
                clip.fields = ComponentFieldsToJson(desc, tcomps[0]);
            }
            const bool canPaste = !managed && !clip.Empty() && clip.componentName == desc.name;
            if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPaste)) {
                batchOp("Paste Component", [&] {
                    for (void* c : tcomps) {
                        ComponentFieldsFromJson(desc, c, clip.fields);
                    }
                });
            }
            if (ImGui::MenuItem("Reset Component", nullptr, false, !managed && desc.construct)) {
                batchOp("Reset Component", [&] {
                    for (void* c : tcomps) {
                        desc.construct(c); // 既定値の書き込み (placement new)
                    }
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Component")) {
                batchOp("Remove Component", [&] {
                    for (EntityID te : targetEnts) {
                        world.RemoveComponentRaw(te, t); // 基本コンポーネントは World 側で拒否
                    }
                });
            }
            ImGui::EndPopup();
        }
        if (openHeader) {
            void* comp = tcomps.empty() ? nullptr : tcomps[0];
            // C# スクリプトコンポーネント: フィールドは managed 側が保持 → 専用描画パス
            // (マルチ選択でも primary のみ編集 — managed 状態はエンティティ毎に独立)
            if (comp && managed) {
                DrawManagedComponentFields(ctx, t, comp, e);
                ImGui::PopID();
                continue;
            }
            if (comp) {
                for (const FieldDesc& f : desc.fields) {
                    if (f.flags & kFieldHidden) {
                        continue;
                    }
                    const bool changed =
                        DrawField(ctx, desc.name, comp, f, e, selection, undo, tfids, tcomps);
                    // マルチ選択: primary で編集した値をフィールド単位で他対象へ伝播
                    // (バイトコピー — POD リフレクション型のみなので安全)
                    if (changed && tcomps.size() > 1 && !(f.flags & kFieldReadOnly)
                        && f.type != FieldType::AssetRef && f.type != FieldType::EntityRef) {
                        const uint32_t sz = FieldTypeSize(f.type);
                        for (size_t i = 1; i < tcomps.size(); ++i) {
                            std::memcpy(static_cast<uint8_t*>(tcomps[i]) + f.offset,
                                        static_cast<const uint8_t*>(comp) + f.offset, sz);
                        }
                    }
                    // 参照ピッカー / 衝突マスク (M36a) はポップアップ内で自前 Undo を記録するので除外
                    const bool ownUndo = f.type == FieldType::AssetRef
                        || f.type == FieldType::EntityRef
                        || (std::strcmp(desc.name, "Collider") == 0
                            && std::strcmp(f.name, "mask") == 0);
                    if (!ownUndo) {
                        HandleEditUndoMulti(ctx, selection, undo, tfids, "Modify");
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
                    // マルチ選択: まだ持っていない全対象へ追加 (1 Undo エントリ、M40a)
                    undo.BeginRecord("Add Component", selection);
                    for (size_t i = 0; i < targetEnts.size(); ++i) {
                        if (!world.HasComponent(targetEnts[i], t)) {
                            undo.CaptureBefore(*ctx.scene, targetFids[i]);
                        }
                    }
                    std::vector<uint64_t> addedFids;
                    for (size_t i = 0; i < targetEnts.size(); ++i) {
                        if (!world.HasComponent(targetEnts[i], t)) {
                            world.AddComponentRaw(targetEnts[i], t);
                            addedFids.push_back(targetFids[i]);
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
                    // マルチ選択は全対象へバッチ適用 (1 Undo エントリ、M40a)
                    undo.BeginRecord("Modify Mask", selection);
                    for (uint64_t tf : fids) {
                        undo.CaptureBefore(*ctx.scene, tf);
                    }
                    for (void* c : comps) {
                        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(c) + field.offset) =
                            next;
                    }
                    for (uint64_t tf : fids) {
                        undo.CaptureAfter(*ctx.scene, tf);
                    }
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
        DrawEntityRef(ctx, field, p, selection, undo, fids, comps, field.offset);
        break;
    case FieldType::AssetRef:
        DrawAssetRef(ctx, field, p, selection, undo, fids, comps, field.offset);
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

void InspectorWindow::DrawAssetInspector(EngineContext& ctx, Selection& selection)
{
    namespace fs = std::filesystem;
    const std::wstring path = selection.assetPath;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        ImGui::TextDisabled("(asset no longer exists)");
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
    }

    if (type == AssetType::Material) {
        DrawMaterialInspector(ctx, path); // M40d
        return;
    }

    if (type == AssetType::Sound) {
        DrawSoundInspector(ctx, path); // M45c
        return;
    }

    if (type == AssetType::Audio) {
        // 素のクリップ: 情報表示 + 試聴。編集対象は .sound.json 側 (ここは読み取り専用)
        if (ctx.audio) {
            const AssetID id = ctx.audio->LoadClipFile(path); // 冪等 (--no-audio なら null)
            if (ImGui::Button("Preview", ImVec2(110, 0))) {
                if (!id.IsNull()) {
                    PlayDesc d;
                    d.clip = id;
                    ctx.audio->Play(d);
                } else {
                    MYE_LOG_WARN("audio device is not available (or clip failed to decode)");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop All", ImVec2(90, 0))) {
                ctx.audio->StopAll();
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
        ImGui::SeparatorText("Import Settings");
        const char* srgbLabels[] = { "Auto (usage hint)", "sRGB (albedo)", "Linear (data)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo("sRGB", &assetImportEdit_.srgb, srgbLabels, 3);
        bool mips = assetImportEdit_.generateMips != 0;
        if (ImGui::Checkbox("Generate Mips", &mips)) {
            assetImportEdit_.generateMips = mips ? 1 : 0;
        }
        const char* compLabels[] = { "Auto (BC1/BC3)", "None (RGBA8)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo("Cook Compress", &assetImportEdit_.compress, compLabels, 2);
        if (ImGui::Button("Apply", ImVec2(90, 0))) {
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
        if (ImGui::Button("Revert", ImVec2(90, 0))) {
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

void InspectorWindow::DrawMaterialInspector(EngineContext& ctx, const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!matEdit_.valid) {
        ImGui::TextDisabled("(material parse failed)");
        return;
    }
    ImGui::SeparatorText("Material");
    ImGui::TextDisabled("shader: %s", matEdit_.shader.c_str());
    ImGui::ColorEdit4("baseColor", matEdit_.baseColor);
    ImGui::SliderFloat("metallic", &matEdit_.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("roughness", &matEdit_.roughness, 0.0f, 1.0f);
    ImGui::Checkbox("transparent", &matEdit_.transparent);

    // テクスチャピッカー (GUID 参照、M39a)。assets 配下の画像をサムネ付きで列挙
    auto texPicker = [&](const char* label, uint64_t& guidRef) {
        ImGui::PushID(label);
        std::string cur = "(none)";
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
            if (ImGui::Selectable("(none)")) {
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

    if (ImGui::Button("Save", ImVec2(90, 0))) {
        nlohmann::json root;
        root["engine"] = "MyEngine";
        root["material"] = 1;
        root["name"] = matEdit_.name.empty()
            ? WideToUtf8(fs::path(path).stem().stem().wstring())
            : matEdit_.name;
        root["shader"] = matEdit_.shader;
        root["baseColor"] = { matEdit_.baseColor[0], matEdit_.baseColor[1],
                              matEdit_.baseColor[2], matEdit_.baseColor[3] };
        root["metallic"] = matEdit_.metallic;
        root["roughness"] = matEdit_.roughness;
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
        std::ofstream out(std::filesystem::path(path), std::ios::binary);
        if (out) {
            out << root.dump(2);
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
    if (ImGui::Button("Revert", ImVec2(90, 0))) {
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
        ImGui::TextDisabled("(sound parse failed)");
        return;
    }

    // ---- 試聴 (先頭バリエーション・揺らぎ無しで固定 = 何を聴いているか分かる) ----
    if (ImGui::Button("Preview", ImVec2(110, 0))) {
        if (ctx.audio) {
            PreviewSound(*ctx.audio, soundEdit_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop All", ImVec2(90, 0))) {
        if (ctx.audio) {
            ctx.audio->StopAll();
        }
    }
    ImGui::TextDisabled("(preview uses the saved-in-editor values, not the file on disk)");

    // ---- バリエーション ----
    ImGui::SeparatorText("Variations");
    int removeAt = -1;
    for (size_t i = 0; i < soundEdit_.variations.size(); ++i) {
        SoundVariation& v = soundEdit_.variations[i];
        ImGui::PushID(static_cast<int>(i));
        std::string cur = "(none)";
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
            if (ImGui::Selectable("(none)")) {
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
        ImGui::DragInt("weight", &v.weight, 0.1f, 0, 100);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        soundEdit_.variations.erase(soundEdit_.variations.begin() + removeAt);
    }
    if (ImGui::Button("+ Add Variation")) {
        soundEdit_.variations.push_back(SoundVariation{});
    }

    // ---- 2D 再生パラメータ ----
    ImGui::SeparatorText("Playback");
    ImGui::SliderFloat("volume", &soundEdit_.volume, 0.0f, 1.0f);
    ImGui::SliderFloat("volume random", &soundEdit_.volumeRandom, 0.0f, 1.0f);
    ImGui::SliderFloat("pitch", &soundEdit_.pitch, 1.0f / AudioSystem::kMaxFreqRatio,
                       AudioSystem::kMaxFreqRatio);
    ImGui::SliderFloat("pitch random", &soundEdit_.pitchRandom, 0.0f, 1.0f);
    ImGui::Checkbox("loop", &soundEdit_.loop);
    ImGui::SameLine();
    ImGui::Checkbox("stream (BGM)", &soundEdit_.stream);
    // バス候補は**実際に張られているミキサー**から採る (M45d でバスはデータ駆動になった)。
    // 保存は名前なので、未知のバス名は既定バスへ落ちるだけで値自体は壊さない
    if (ctx.audio != nullptr) {
        const int current = ctx.audio->FindBus(soundEdit_.bus.c_str());
        const char* preview = current >= 0 ? ctx.audio->BusName(current) : soundEdit_.bus.c_str();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("bus", preview)) {
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
    ImGui::DragInt("priority", &soundEdit_.priority, 1.0f, 0, 255);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("max instances", &soundEdit_.maxInstances, 0.1f, 0, 64);
    ImGui::TextDisabled("0 = unlimited. Higher priority wins when voices are stolen.");

    // ---- 3D (M45e で実際に効く) ----
    ImGui::SeparatorText("3D");
    ImGui::SliderFloat("spatial blend", &soundEdit_.spatialBlend, 0.0f, 1.0f);
    ImGui::DragFloat("min distance", &soundEdit_.minDistance, 0.05f, 0.01f, 1000.0f);
    ImGui::DragFloat("max distance", &soundEdit_.maxDistance, 0.5f, 0.02f, 10000.0f);
    {
        int rolloff = static_cast<int>(soundEdit_.rolloff);
        const char* rolloffNames[] = { "Logarithmic", "Linear", "Inverse" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("rolloff", &rolloff, rolloffNames, 3)) {
            soundEdit_.rolloff = static_cast<SoundRolloff>(rolloff);
        }
    }
    ImGui::SliderFloat("doppler", &soundEdit_.dopplerScale, 0.0f, 5.0f);
    ImGui::SliderFloat("reverb send", &soundEdit_.reverbSend, 0.0f, 1.0f);
    ImGui::TextDisabled("3D settings take effect in M45e (X3DAudio).");

    // ---- ループ点 (M45f 予約) ----
    ImGui::SeparatorText("Loop points (frames)");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("loop start", &soundEdit_.loopStartSample, 8.0f, 0, 1 << 30);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("loop end", &soundEdit_.loopEndSample, 8.0f, 0, 1 << 30);
    ImGui::TextDisabled("end <= start means \"to the end\". Used by streaming (M45f).");

    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(90, 0))) {
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
    if (ImGui::Button("Revert", ImVec2(90, 0))) {
        LoadSoundEdit(path);
    }
}

void InspectorWindow::DrawAssetRef(EngineContext& ctx, const FieldDesc& field, void* p,
                                   Selection& selection, UndoStack& undo,
                                   const std::vector<uint64_t>& fids,
                                   const std::vector<void*>& comps, uint32_t fieldOffset)
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
            // マルチ選択は全対象へバッチ適用 (1 Undo エントリ、M40a)
            undo.BeginRecord("Assign asset", selection);
            for (uint64_t tf : fids) {
                undo.CaptureBefore(*ctx.scene, tf);
            }
            for (void* c : comps) {
                *reinterpret_cast<AssetID*>(static_cast<uint8_t*>(c) + fieldOffset) = v;
            }
            for (uint64_t tf : fids) {
                undo.CaptureAfter(*ctx.scene, tf);
            }
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
            // マルチ選択は全対象へバッチ適用 (同一エンティティを参照させる、M40a)
            undo.BeginRecord("Assign reference", selection);
            for (uint64_t tf : fids) {
                undo.CaptureBefore(*ctx.scene, tf);
            }
            for (void* c : comps) {
                *reinterpret_cast<EntityID*>(static_cast<uint8_t*>(c) + fieldOffset) = v;
            }
            for (uint64_t tf : fids) {
                undo.CaptureAfter(*ctx.scene, tf);
            }
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
