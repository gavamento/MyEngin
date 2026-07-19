#include "Editor/Windows/InspectorWindow.h"

#include <cmath>

#include "Engine/Core/ComponentRegistry.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Scene.h"

#include "imgui.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

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

} // namespace

void InspectorWindow::OnImGui(EngineContext& ctx, Selection& selection)
{
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();
    const EntityID e = selection.entity;
    if (!world.IsAlive(e)) {
        ImGui::TextDisabled("(no selection)");
        ImGui::End();
        return;
    }

    // ---- 名前 ----
    if (auto* nc = world.GetComponent<NameComponent>(e)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", nc->value, sizeof(nc->value));
    }
    ImGui::TextDisabled("Entity %u:%u", e.index, e.generation);
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
        const bool openHeader = ImGui::CollapsingHeader(desc.name, ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem("##comp_ctx")) {
            if (ImGui::MenuItem("Remove Component")) {
                world.RemoveComponentRaw(e, t); // 基本コンポーネントは World 側で拒否
            }
            ImGui::EndPopup();
        }
        if (openHeader) {
            void* comp = world.GetComponentRaw(e, t);
            if (comp) {
                for (const FieldDesc& f : desc.fields) {
                    if (f.flags & kFieldHidden) {
                        continue;
                    }
                    DrawField(comp, f, e);
                }
            }
        }
        ImGui::PopID();
    }

    // ---- Add Component ----
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) {
        ImGui::OpenPopup("##add_component");
    }
    if (ImGui::BeginPopup("##add_component")) {
        for (ComponentTypeId t = 0; t < reg.Count(); ++t) {
            const ComponentDesc& desc = reg.Desc(t);
            if (desc.flags & kComponentHidden) {
                continue;
            }
            if (world.HasComponent(e, t)) {
                continue;
            }
            if (ImGui::MenuItem(desc.name)) {
                world.AddComponentRaw(e, t);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

bool InspectorWindow::DrawField(void* comp, const FieldDesc& field, EntityID entity)
{
    void* p = static_cast<uint8_t*>(comp) + field.offset;
    const bool readOnly = (field.flags & kFieldReadOnly) != 0;
    if (readOnly) {
        ImGui::BeginDisabled();
    }

    bool changed = false;
    switch (field.type) {
    case FieldType::Float:
        changed = ImGui::DragFloat(field.name, static_cast<float*>(p), 0.05f);
        break;
    case FieldType::Int32:
        changed = ImGui::DragInt(field.name, static_cast<int*>(p));
        break;
    case FieldType::UInt32:
        changed = ImGui::InputScalar(field.name, ImGuiDataType_U32, p);
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
        changed = ImGui::DragFloat2(field.name, static_cast<float*>(p), 0.05f);
        break;
    case FieldType::Float3:
        changed = ImGui::DragFloat3(field.name, static_cast<float*>(p), 0.05f);
        break;
    case FieldType::Float4:
        changed = ImGui::DragFloat4(field.name, static_cast<float*>(p), 0.05f);
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
    case FieldType::EntityRef: {
        const auto* id = static_cast<const EntityID*>(p);
        if (id->IsNull()) {
            ImGui::Text("%s: (none)", field.name);
        } else {
            ImGui::Text("%s: %u:%u", field.name, id->index, id->generation);
        }
        break;
    }
    case FieldType::AssetRef: {
        const auto* id = static_cast<const AssetID*>(p);
        ImGui::Text("%s: %016llX", field.name, static_cast<unsigned long long>(id->value));
        break;
    }
    case FieldType::String64:
        changed = ImGui::InputText(field.name, static_cast<char*>(p), 64);
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

    if (readOnly) {
        ImGui::EndDisabled();
    }
    return changed;
}

} // namespace mye
