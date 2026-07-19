#include "Editor/Windows/AssetBrowserWindow.h"

#include <algorithm>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/Undo/UndoStack.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

bool IsImageExt(const std::wstring& ext)
{
    return ext == L".png" || ext == L".tga" || ext == L".jpg" || ext == L".jpeg";
}

const char* IconFor(const std::wstring& ext)
{
    if (ext == L".hlsl" || ext == L".hlsli") {
        return "shader";
    }
    if (ext == L".glb" || ext == L".gltf") {
        return "model";
    }
    if (ext == L".json") {
        return "json";
    }
    return "file";
}

} // namespace

void AssetBrowserWindow::DrawDirTree(const std::wstring& dir)
{
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = WideToUtf8(entry.path().filename().wstring());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.path().wstring() == current_) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool open = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked()) {
            current_ = entry.path().wstring();
        }
        if (open) {
            DrawDirTree(entry.path().wstring());
            ImGui::TreePop();
        }
    }
}

void AssetBrowserWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo)
{
    if (!ImGui::Begin("Assets")) {
        ImGui::End();
        return;
    }
    if (!init_) {
        current_ = ctx.assetsRoot;
        init_ = true;
    }

    // ---- 左: フォルダツリー ----
    ImGui::BeginChild("##dirtree", ImVec2(170, 0), ImGuiChildFlags_Borders);
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ctx.assetsRoot == current_) {
        rootFlags |= ImGuiTreeNodeFlags_Selected;
    }
    if (ImGui::TreeNodeEx("assets", rootFlags)) {
        if (ImGui::IsItemClicked()) {
            current_ = ctx.assetsRoot;
        }
        DrawDirTree(ctx.assetsRoot);
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- 右: ファイルグリッド ----
    ImGui::BeginChild("##filegrid", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", WideToUtf8(current_).c_str());
    ImGui::Separator();

    constexpr float kCell = 88.0f;
    const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (kCell + 12.0f)));

    std::error_code ec;
    int i = 0;
    for (const auto& entry : fs::directory_iterator(current_, ec)) {
        if (entry.is_directory()) {
            continue;
        }
        const std::wstring path = entry.path().wstring();
        const std::wstring ext = entry.path().extension().wstring();
        const std::string nameU = WideToUtf8(entry.path().filename().wstring());
        const bool isPrefab = nameU.size() >= 12
            && nameU.compare(nameU.size() - 12, 12, ".prefab.json") == 0;
        const bool isAnim = !isPrefab && nameU.size() >= 10
            && nameU.compare(nameU.size() - 10, 10, ".anim.json") == 0;

        if (i % cols != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        ImGui::BeginGroup();
        if (IsImageExt(ext)) {
            const AssetID id = ctx.resources->textures.LoadFile(path); // idempotent (キャッシュ)
            Texture* tex = ctx.resources->textures.Get(id);
            if (tex && tex->srv) {
                ImGui::Image(reinterpret_cast<ImTextureID>(tex->srv.Get()), ImVec2(kCell, kCell));
            } else {
                ImGui::Button("img", ImVec2(kCell, kCell));
            }
        } else {
            const char* icon = isPrefab ? "prefab" : (isAnim ? "anim" : IconFor(ext));
            ImGui::Button(icon, ImVec2(kCell, kCell));
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (isPrefab) {
                // シーンへインスタンス化 (ルートに配置)。生成を 1 Undo エントリに
                undo.BeginRecord("Instantiate Prefab", selection);
                const uint64_t hash = ctx.prefabs->LoadFromFile(path); // 未登録なら登録 (冪等)
                const uint64_t rootFid =
                    (hash != 0) ? Prefab::Instantiate(*ctx.scene, *ctx.prefabs, hash, 0) : 0;
                ctx.scene->GetWorld().ApplyStructuralChanges();
                if (rootFid != 0) {
                    selection.SelectOnly(rootFid);
                    undo.CaptureAfter(*ctx.scene, rootFid);
                    undo.EndRecord(selection);
                } else {
                    undo.CancelRecord();
                }
            } else if (isAnim) {
                // 選択エンティティに Animator を付けてこのクリップを割り当てる
                const uint64_t hash = ctx.anims->LoadFromFile(path);
                GameObject sel = ctx.scene->FindByFileId(selection.primary);
                if (hash != 0 && sel) {
                    World& w = ctx.scene->GetWorld();
                    undo.BeginRecord("Assign Clip", selection);
                    undo.CaptureBefore(*ctx.scene, selection.primary);
                    auto* an = w.GetComponent<AnimatorComponent>(sel.Id());
                    if (!an) {
                        an = static_cast<AnimatorComponent*>(
                            w.AddComponentRaw(sel.Id(), AnimatorComponent::sTypeId));
                    }
                    if (an) {
                        an->clip = AssetID{ hash };
                    }
                    w.ApplyStructuralChanges();
                    undo.CaptureAfter(*ctx.scene, selection.primary);
                    undo.EndRecord(selection);
                }
            } else {
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kCell);
        ImGui::TextWrapped("%s", nameU.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
        ++i;
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace mye
