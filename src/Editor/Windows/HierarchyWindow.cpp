#include "Editor/Windows/HierarchyWindow.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "Editor/AssetOps.h"
#include "Editor/CreateMenu.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

#include "imgui.h"

namespace mye {
namespace {

constexpr const char* kDragPayload = "MYE_ENTITY";

// エンティティの fileId (無ければ 0)。選択/Undo の同一性キー
uint64_t FidOf(World& world, EntityID e)
{
    auto* f = world.GetComponent<FileIdComponent>(e);
    return f ? f->value : 0;
}

// 選択サブツリーを .prefab.json 化し、その場でインスタンス化タグを付ける (1 Undo エントリ)
void CreatePrefabFromEntity(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID e)
{
    // 名前をファイル名向けにサニタイズ
    std::string base = ctx.scene->GetWorld().GetName(e);
    std::string safe;
    for (char c : base) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        safe += ok ? c : '_';
    }
    if (safe.empty()) {
        safe = "Prefab";
    }
    const std::wstring path = ctx.assetsRoot + L"\\prefabs\\" + Utf8ToWide(safe) + L".prefab.json";

    undo.BeginRecord("Create Prefab", selection);
    const uint64_t fid = ctx.scene->EnsureFileId(e);
    undo.CaptureBefore(*ctx.scene, fid);
    const uint64_t hash = Prefab::CreateAsset(*ctx.scene, *ctx.prefabs, path, e);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    if (hash == 0) {
        MYE_LOG_ERROR("Create Prefab failed for '%s'", base.c_str());
    }
}

} // namespace

void HierarchyWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Hierarchy", &open)) {
        ImGui::End();
        return;
    }
    World& world = ctx.scene->GetWorld();

    // 検索ボックス
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search...", searchBuf_, sizeof(searchBuf_));
    ImGui::Separator();

    // F2: 選択エンティティのインラインリネーム開始
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_F2, false) && selection.primary != 0 && renamingFid_ == 0) {
        renamingFid_ = selection.primary;
        renameFocus_ = true;
        undo.BeginRecord("Rename", selection);
        undo.CaptureBefore(*ctx.scene, renamingFid_);
    }

    // Shift 範囲選択用の表示順を毎フレーム再構築 (クリック判定は前フレームの並びを使う)
    visibleOrderPrev_ = std::move(visibleOrder_);
    visibleOrder_.clear();

    if (searchBuf_[0] != '\0') {
        DrawFiltered(ctx, world, selection);
    } else {
        // ルート (parent 無し) を firstRoot リンク順で描画 (= 兄弟順 = 保存順)
        EntityID r = world.FirstRoot();
        while (!r.IsNull()) {
            auto* rh = world.GetComponent<HierarchyComponent>(r);
            const EntityID next = rh ? rh->nextSibling : kNullEntity;
            DrawEntityNode(ctx, world, r, selection, undo);
            r = next;
        }
    }

    // 空き領域: 背景コンテキストメニュー + ルート化ドロップ
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragPayload)) {
            const EntityID src = *static_cast<const EntityID*>(payload->Data);
            undo.BeginRecord("Reparent", selection);
            const uint64_t fid = ctx.scene->EnsureFileId(src);
            undo.CaptureBefore(*ctx.scene, fid);
            world.SetParent(src, kNullEntity);
            world.ApplyStructuralChanges();
            undo.CaptureAfter(*ctx.scene, fid);
            undo.EndRecord(selection);
        }
        // AssetBrowser からのドロップ: ルートに配置
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            InstantiateAssetAtPath(ctx, selection, undo,
                                   Utf8ToWide(static_cast<const char*>(pa->Data)), nullptr, 0);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextWindow("##hierarchy_bg",
                                       ImGuiPopupFlags_MouseButtonRight
                                           | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateMenuItems(ctx, selection, undo); // parent 省略 = ルート生成
        ImGui::EndPopup();
    }
    ImGui::End();
}

void HierarchyWindow::ApplyClick(EngineContext& ctx, EntityID e, Selection& selection)
{
    const uint64_t fid = ctx.scene->EnsureFileId(e);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        selection.Toggle(fid);
        anchorFid_ = fid;
    } else if (io.KeyShift) {
        // Shift: 表示順 (前フレーム) で anchor〜クリックの範囲を選択 (Unity 風、M27d)
        const auto& order = visibleOrderPrev_;
        const auto ia = std::find(order.begin(), order.end(), anchorFid_);
        const auto ib = std::find(order.begin(), order.end(), fid);
        if (anchorFid_ != 0 && ia != order.end() && ib != order.end()) {
            const size_t lo = std::min(ia, ib) - order.begin();
            const size_t hi = std::max(ia, ib) - order.begin();
            selection.Set(std::vector<uint64_t>(order.begin() + lo, order.begin() + hi + 1), fid);
        } else {
            selection.Add(fid); // anchor が非表示 (折り畳み/検索) なら追加選択に留める
        }
    } else {
        selection.SelectOnly(fid);
        anchorFid_ = fid;
    }
}

void HierarchyWindow::DrawFiltered(EngineContext& ctx, World& world, Selection& selection)
{
    // 検索中は名前部分一致 (大文字小文字無視) のフラット表示
    std::string needle = searchBuf_;
    for (char& c : needle) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const ComponentTypeId req[] = { NameComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            std::string name = world.GetName(e);
            std::string lower = name;
            for (char& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower.find(needle) == std::string::npos) {
                continue;
            }
            const uint64_t fid = FidOf(world, e);
            if (fid != 0) {
                visibleOrder_.push_back(fid);
            }
            ImGui::PushID(static_cast<int>(e.index));
            const bool sel = (fid != 0 && selection.Contains(fid));
            if (ImGui::Selectable(name.c_str(), sel)) {
                ApplyClick(ctx, e, selection);
            }
            ImGui::PopID();
        }
    });
}

void HierarchyWindow::DrawEntityNode(EngineContext& ctx, World& world, EntityID e,
                                     Selection& selection, UndoStack& undo)
{
    if (!world.IsAlive(e)) {
        return;
    }
    const uint64_t fid = FidOf(world, e);
    if (fid != 0) {
        visibleOrder_.push_back(fid); // Shift 範囲選択用の表示順 (M27d)
    }

    ImGui::PushID(static_cast<int>(e.index));

    // 有効/無効チェックボックス。トグルで ActiveComponent 追加 = アーキタイプ変更が起きるため、
    // HierarchyComponent ポインタ (h) を取得する前に処理する (取得後だと use-after-move)
    bool active = IsEntityActive(world, e);
    if (ImGui::Checkbox("##active", &active)) {
        undo.BeginRecord("Toggle Active", selection);
        undo.CaptureBefore(*ctx.scene, ctx.scene->EnsureFileId(e));
        auto* a = world.GetComponent<ActiveComponent>(e);
        if (!a) {
            a = static_cast<ActiveComponent*>(world.AddComponentRaw(e, ActiveComponent::sTypeId));
        }
        if (a) {
            a->enabled = active ? 1 : 0;
        }
        world.ApplyStructuralChanges();
        undo.CaptureAfter(*ctx.scene, ctx.scene->EnsureFileId(e));
        undo.EndRecord(selection);
    }
    ImGui::SameLine();

    // インラインリネーム中はこのノードを InputText に置き換える
    if (fid != 0 && fid == renamingFid_) {
        if (auto* nc = world.GetComponent<NameComponent>(e)) {
            if (renameFocus_) {
                ImGui::SetKeyboardFocusHere();
                renameFocus_ = false;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##rename", nc->value, sizeof(nc->value),
                             ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemDeactivated()) {
                undo.CaptureAfter(*ctx.scene, renamingFid_);
                undo.EndRecord(selection);
                renamingFid_ = 0;
            }
        } else {
            renamingFid_ = 0;
        }
        ImGui::PopID();
        return;
    }

    // h はチェックボックスのトグル (アーキタイプ変更) 後に取得する
    auto* h = world.GetComponent<HierarchyComponent>(e);
    const bool leaf = (h == nullptr) || h->firstChild.IsNull();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
        | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (fid != 0 && selection.Contains(fid)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // プレハブインスタンスは青文字 (Unity 風)。メンバは全て PrefabLink を持つ
    const bool isPrefab = world.GetComponent<PrefabLinkComponent>(e) != nullptr;
    if (isPrefab) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.68f, 1.0f, 1.0f));
    }
    const bool nodeOpen = ImGui::TreeNodeEx("##node", flags, "%s", world.GetName(e));
    if (isPrefab) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        ApplyClick(ctx, e, selection);
    }

    if (ImGui::BeginPopupContextItem("##entity_ctx")) {
        selection.SelectOnly(ctx.scene->EnsureFileId(e));
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenuItems(ctx, selection, undo, e); // e の子として生成
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Create Prefab")) {
            CreatePrefabFromEntity(ctx, selection, undo, e);
        }
        if (ImGui::MenuItem("Delete")) {
            undo.BeginRecord("Delete", selection);
            undo.CaptureBefore(*ctx.scene, ctx.scene->EnsureFileId(e));
            world.DestroyEntity(e); // 子孫ごと tick 末に破棄
            world.ApplyStructuralChanges();
            selection.Remove(fid);
            undo.EndRecord(selection); // CaptureAfter 無し → destroyed 扱い
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kDragPayload, &e, sizeof(e));
        ImGui::TextUnformatted(world.GetName(e));
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        // 3 ゾーンドロップ (M27d): 上 25% = 前に挿入 / 下 25% = 後ろに挿入 / 中央 = 子にする
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                kDragPayload,
                ImGuiDragDropFlags_AcceptBeforeDelivery
                    | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const EntityID src = *static_cast<const EntityID*>(payload->Data);
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            const float t = (ImGui::GetMousePos().y - rmin.y)
                            / std::max(1.0f, rmax.y - rmin.y);
            const int zone = (t < 0.25f) ? -1 : (t > 0.75f) ? 1 : 0;

            // ドロップ先インジケータ (中央 = 枠、上下 = 挿入ライン)
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            if (zone == 0) {
                dl->AddRect(rmin, rmax, col, 0.0f, 2.0f);
            } else {
                const float y = (zone < 0) ? rmin.y : rmax.y;
                dl->AddLine(ImVec2(rmin.x, y), ImVec2(rmax.x, y), col, 2.0f);
            }

            if (payload->IsDelivery() && src != e) {
                if (zone == 0) {
                    undo.BeginRecord("Reparent", selection);
                    const uint64_t sfid = ctx.scene->EnsureFileId(src);
                    undo.CaptureBefore(*ctx.scene, sfid);
                    world.SetParent(src, e); // 循環は World 側で拒否される
                    world.ApplyStructuralChanges();
                    undo.CaptureAfter(*ctx.scene, sfid);
                    undo.EndRecord(selection);
                } else {
                    ReorderAsSibling(ctx, world, src, e, zone > 0, selection, undo);
                }
            }
        }
        // AssetBrowser からのドロップ: このエンティティの子に配置
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            InstantiateAssetAtPath(ctx, selection, undo,
                                   Utf8ToWide(static_cast<const char*>(pa->Data)), nullptr,
                                   ctx.scene->EnsureFileId(e));
        }
        ImGui::EndDragDropTarget();
    }

    if (nodeOpen) {
        if (h) {
            EntityID child = h->firstChild;
            while (!child.IsNull()) {
                auto* ch = world.GetComponent<HierarchyComponent>(child);
                const EntityID next = ch ? ch->nextSibling : kNullEntity;
                DrawEntityNode(ctx, world, child, selection, undo);
                child = next;
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void HierarchyWindow::ReorderAsSibling(EngineContext& ctx, World& world, EntityID src,
                                       EntityID target, bool after, Selection& selection,
                                       UndoStack& undo)
{
    // target が src の子孫なら不可 (再ペアレントすると循環になる)
    for (EntityID p = world.GetParent(target); !p.IsNull(); p = world.GetParent(p)) {
        if (p == src) {
            return;
        }
    }
    const EntityID parent = world.GetParent(target);

    // 挿入位置: 親の子リスト (ルートは firstRoot リスト) を src を除いて数え、
    // target の位置 (+ after) を求める — SetSiblingIndex は「src を外した後のリスト」が基準
    uint32_t insertIdx = 0;
    {
        EntityID cur;
        if (parent.IsNull()) {
            cur = world.FirstRoot();
        } else {
            auto* ph = world.GetComponent<HierarchyComponent>(parent);
            cur = ph ? ph->firstChild : kNullEntity;
        }
        uint32_t count = 0;
        bool found = false;
        while (!cur.IsNull()) {
            if (cur == target) {
                insertIdx = count + (after ? 1u : 0u);
                found = true;
                break;
            }
            if (cur != src) {
                ++count;
            }
            auto* ch = world.GetComponent<HierarchyComponent>(cur);
            cur = ch ? ch->nextSibling : kNullEntity;
        }
        if (!found) {
            return;
        }
    }

    // 兄弟順は WorldHash 非対象・シーン JSON に childIndex で保存され、
    // Undo は SubtreeToJson の childIndex を ApplyPartial が復元することで成立する
    undo.BeginRecord("Reorder", selection);
    const uint64_t sfid = ctx.scene->EnsureFileId(src);
    undo.CaptureBefore(*ctx.scene, sfid);
    if (world.GetParent(src) != parent) {
        world.SetParent(src, parent);
    }
    world.SetSiblingIndex(src, insertIdx);
    world.ApplyStructuralChanges();
    undo.CaptureAfter(*ctx.scene, sfid);
    undo.EndRecord(selection);
}

} // namespace mye
