#include "Editor/Windows/HierarchyWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "Editor/AssetOps.h"
#include "Editor/CreateMenu.h"
#include "Editor/EditorComponentCatalog.h"
#include "Editor/EditorWidgets.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Components.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ImGuiTheme.h" // themeColor::Prefab

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

// 選択サブツリーをアセット化し、その場でインスタンス化タグを付ける (1 Undo エントリ)。
// name はモーダルのユーザー入力 (M50b): 禁止文字だけ落とす緩いサニタイズで日本語名を通し、
// 同名は " (1)" 連番 — 上書きするとパスハッシュ再登録で既存インスタンスが新ベースへ
// 黙って張り替わるため、一意化は安全装置
void CreatePrefabFromEntity(EngineContext& ctx, Selection& selection, UndoStack& undo, EntityID e,
                            const std::string& name, const wchar_t* suffix)
{
    const std::string safe = SanitizeFileName(name, "Prefab");
    const std::wstring dir = ctx.assetsRoot + L"\\prefabs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // プロジェクト起動では未作成のことがある
    const std::wstring path = MakeUniqueAssetPath(dir, Utf8ToWide(safe) + suffix);

    undo.BeginRecord("Create Prefab", selection);
    const uint64_t fid = ctx.scene->EnsureFileId(e);
    undo.CaptureBefore(*ctx.scene, fid);
    const uint64_t hash = Prefab::CreateAsset(*ctx.scene, *ctx.prefabs, path, e);
    ctx.scene->GetWorld().ApplyStructuralChanges();
    undo.CaptureAfter(*ctx.scene, fid);
    undo.EndRecord(selection);
    if (hash == 0) {
        MYE_LOG_ERROR("Create Prefab failed for '%s'", safe.c_str());
    }
}

// 部位の構造ロック (M48f)。ブロックしたときは理由をユーザー向けログに出す —
// 「クリックしても何も起きない」は最悪の UX で、必ず「なぜ駄目か / どうすればよいか」を返す
bool BlockedByPartLock(World& world, EntityID e)
{
    if (!Parts::IsStructureLocked(world, e)) {
        return false;
    }
    MYE_LOG_WARN(Tr(StrId::Log_PartLocked), world.GetName(e));
    return true;
}

} // namespace

void HierarchyWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Hierarchy), &open)) {
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
        GameObject g = ctx.scene->FindByFileId(selection.primary);
        // 部位はロック (M48f)。**記録を開く前**に弾くこと — BeginRecord してから抜けると
        // 開きっぱなしの記録が次の操作を巻き込む
        if (g && !BlockedByPartLock(world, g.Id())) {
            renamingFid_ = selection.primary;
            renameFocus_ = true;
            // 編集開始時点の名前を控える。確定時にこれと一致していれば「変更なし」= 改名しない
            // (ImGui は Esc で元の名前をバッファへ戻すが確定判定は真になるため、M48b)
            renameOriginal_ = world.GetName(g.Id());
            undo.BeginRecord("Rename", selection);
            undo.CaptureBefore(*ctx.scene, renamingFid_);
        }
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
            // 部位をルートへ引き剥がすのも再親化 (M48f)
            if (!BlockedByPartLock(world, src)) {
                undo.BeginRecord("Reparent", selection);
                const uint64_t fid = ctx.scene->EnsureFileId(src);
                undo.CaptureBefore(*ctx.scene, fid);
                world.SetParent(src, kNullEntity);
                world.ApplyStructuralChanges();
                undo.CaptureAfter(*ctx.scene, fid);
                undo.EndRecord(selection);
            }
        }
        // AssetBrowser からのドロップ: .cs はエンティティ行へドロップするよう促す、他はルート配置
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::wstring path = Utf8ToWide(static_cast<const char*>(pa->Data));
            if (AssetDatabase::ClassifyPath(path) == AssetType::Script) {
                MYE_LOG_WARN("drop a script onto an entity row (or the Inspector) to attach it");
            } else {
                InstantiateAssetAtPath(ctx, selection, undo, path, nullptr, 0);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextWindow("##hierarchy_bg",
                                       ImGuiPopupFlags_MouseButtonRight
                                           | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateMenuItems(ctx, selection, undo); // parent 省略 = ルート生成
        ImGui::EndPopup();
    }

    // ---- Create Prefab 命名モーダル (M50b) ----
    if (prefabModalRequest_) {
        ImGui::OpenPopup(Tr(StrId::Popup_CreatePrefab));
        prefabModalRequest_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_CreatePrefab), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText(Tr(StrId::Asset_NameField), prefabNameBuf_,
                                            sizeof(prefabNameBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        // 拡張子 2 択。既定は .actor.json (M48d: 新規作成は actor、.prefab.json は互換用)
        ImGui::TextUnformatted(Tr(StrId::Hier_PrefabFormat));
        ImGui::SameLine();
        if (ImGui::RadioButton(".actor.json", prefabAsActor_)) {
            prefabAsActor_ = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(".prefab.json", !prefabAsActor_)) {
            prefabAsActor_ = false;
        }
        const bool create = ImGui::Button(Tr(StrId::Common_Create), ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0));
        if (create && prefabNameBuf_[0] != '\0') {
            if (GameObject g = ctx.scene->FindByFileId(prefabModalFid_)) {
                CreatePrefabFromEntity(ctx, selection, undo, g.Id(), prefabNameBuf_,
                                       prefabAsActor_ ? PrefabLibrary::kActorSuffix
                                                      : PrefabLibrary::kPrefabSuffix);
            }
            prefabModalFid_ = 0;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            prefabModalFid_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Unpack Prefab 確認モーダル (M50b) ----
    if (unpackModalRequest_) {
        ImGui::OpenPopup(Tr(StrId::Popup_UnpackPrefab));
        unpackModalRequest_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_UnpackPrefab), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        GameObject target = ctx.scene->FindByFileId(unpackModalFid_);
        const char* tname = target ? world.GetName(target.Id()) : "?";
        ImGui::Text(Tr(StrId::Unpack_ConfirmBody), tname);
        ImGui::Spacing();
        if (ImGui::Button(Tr(StrId::Common_Ok), ImVec2(90, 0)) && target) {
            undo.BeginRecord("Unpack Prefab", selection);
            undo.CaptureBefore(*ctx.scene, unpackModalFid_);
            if (Prefab::UnpackInstance(*ctx.scene, unpackModalFid_)) {
                MYE_LOG_INFO(Tr(StrId::Log_Unpacked), tname);
            }
            world.ApplyStructuralChanges();
            undo.CaptureAfter(*ctx.scene, unpackModalFid_);
            undo.EndRecord(selection);
            unpackModalFid_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0))) {
            unpackModalFid_ = 0;
            ImGui::CloseCurrentPopup();
        }
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
                // 確定時に正規化する (M48b)。編集中に毎フレームやってはいけない —
                // "Cube" を打つ途中の "C"/"Cu" が衝突判定されて打ち切れなくなる。
                // ★文字列が変わっていなくても必ず SetEntityName を通すこと: ImGui は
                //   nc->value を直接編集し ImStrncpy が NUL 以降の残骸バイトを消さないため、
                //   名前を短くすると WorldHash に前の名前の残骸が残る (JSON には出ないので
                //   Undo でも直らない)。ここでゼロ埋めし直すのが唯一の修復点
                FinishRename(world, e, nc->value, renameOriginal_);
                undo.CaptureAfter(*ctx.scene, renamingFid_);
                undo.EndRecord(selection);
                renamingFid_ = 0;
            }
        } else {
            undo.CancelRecord(); // NameComponent が消えた: 記録を開きっぱなしにしない
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
        ImGui::PushStyleColor(ImGuiCol_Text, themeColor::Prefab);
    }
    // アイコンはカテゴリ色 — ラベルを空にして DrawItemIconLabel が矩形へ直接描く
    // (プレハブ青は名前側だけ。Pop より前に描くことで DrawItemIconLabel の文字色に乗る)
    const EntityIconInfo icon = EntityIconInfoFor(world, e);
    const bool nodeOpen = ImGui::TreeNodeEx("##node", flags);
    DrawItemIconLabel(icon.icon, ComponentCategoryColor(icon.category), world.GetName(e),
                      /*framed=*/false);
    if (isPrefab) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        ApplyClick(ctx, e, selection);
    }

    if (ImGui::BeginPopupContextItem("##entity_ctx")) {
        selection.SelectOnly(ctx.scene->EnsureFileId(e));
        if (ImGui::BeginMenu(Tr(StrId::Hier_Create))) {
            DrawCreateMenuItems(ctx, selection, undo, e); // e の子として生成
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr(StrId::Hier_CreatePrefab))) {
            // 命名モーダルへ (M50b)。ポップアップはこのコンテキストメニューと同居できない
            // ので、要求フラグを立てて OnImGui 末尾で開く (AssetBrowser の Create と同じ)
            prefabModalFid_ = ctx.scene->EnsureFileId(e);
            prefabModalRequest_ = true;
            std::snprintf(prefabNameBuf_, sizeof(prefabNameBuf_), "%s", world.GetName(e));
        }
        // Unpack Prefab (M50b): インスタンスルートだけに出す。入れ子 (祖先にルートあり) は
        // 外側の Apply でこの枝が新ベースから落ちるため不可 — グレーアウト + 理由表示
        if (world.GetComponent<PrefabInstanceComponent>(e) != nullptr) {
            const EntityID par = world.GetParent(e);
            const bool nested = !par.IsNull() && !Prefab::FindInstanceRoot(world, par).IsNull();
            if (nested) {
                ImGui::BeginDisabled();
            }
            if (ImGui::MenuItem(Tr(StrId::Hier_UnpackPrefab)) && !nested) {
                unpackModalFid_ = ctx.scene->EnsureFileId(e);
                unpackModalRequest_ = true;
            }
            if (nested) {
                ImGui::EndDisabled();
                ImGui::TextDisabled("%s", Tr(StrId::Hier_UnpackNested));
            }
        }
        // 部位は削除もロック (M48f)。グレーアウト + ホバーで理由を出す
        const bool partLocked = Parts::IsStructureLocked(world, e);
        if (partLocked) {
            ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem(Tr(StrId::Hier_Delete)) && !partLocked) {
            undo.BeginRecord("Delete", selection);
            undo.CaptureBefore(*ctx.scene, ctx.scene->EnsureFileId(e));
            world.DestroyEntity(e); // 子孫ごと tick 末に破棄
            world.ApplyStructuralChanges();
            selection.Remove(fid);
            undo.EndRecord(selection); // CaptureAfter 無し → destroyed 扱い
        }
        if (partLocked) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", Tr(StrId::Hier_PartLockedShort));
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

            // 部位は再親化をロック (M48f)。並べ替え (zone != 0) は親が変わらないので許す —
            // FindPart は名前パスで引くため兄弟順には影響されない
            if (payload->IsDelivery() && src != e
                && !(zone == 0 && BlockedByPartLock(world, src))) {
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
        // AssetBrowser からのドロップ: .cs はこのエンティティにコンポーネントとしてアタッチ、
        // .mat.json はこのエンティティの材質に割当、その他 (プレハブ/モデル/画像) は子として配置
        if (const ImGuiPayload* pa = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::wstring path = Utf8ToWide(static_cast<const char*>(pa->Data));
            const AssetType dropType = AssetDatabase::ClassifyPath(path);
            if (dropType == AssetType::Script) {
                AttachScriptToEntity(ctx, selection, undo, path, e);
            } else if (dropType == AssetType::Material) {
                AssignMaterialToEntity(ctx, selection, undo, path, e);
            } else {
                InstantiateAssetAtPath(ctx, selection, undo, path, nullptr,
                                       ctx.scene->EnsureFileId(e));
            }
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
