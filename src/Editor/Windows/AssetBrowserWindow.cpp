#include "Editor/Windows/AssetBrowserWindow.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/AssetOps.h"
#include "Editor/AssetPreviewCache.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/TextureCook.h"

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

bool IsImageExt(const std::wstring& ext)
{
    return ext == L".png" || ext == L".tga" || ext == L".jpg" || ext == L".jpeg"
        || ext == L".dds"; // M24: BCn 圧縮テクスチャもサムネイル表示
}

enum CreateKind {
    kCreateNone = 0,
    kCreateFolder,
    kCreateScript,
    kCreateCSharp,
    kCreateScene,
    kCreateAnim,
    kCreateMaterial
};

const char* IconFor(const std::wstring& ext)
{
    if (ext == L".hlsl" || ext == L".hlsli") {
        return "shader";
    }
    if (ext == L".glb" || ext == L".gltf" || ext == L".fbx") {
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
        const bool nodeOpen = ImGui::TreeNodeEx(name.c_str(), flags);
        if (ImGui::IsItemClicked()) {
            current_ = entry.path().wstring();
        }
        if (nodeOpen) {
            DrawDirTree(entry.path().wstring());
            ImGui::TreePop();
        }
    }
}

void AssetBrowserWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                 const std::string& externalEditorCmd,
                                 AssetPreviewCache& preview)
{
    if (!open) {
        return;
    }
    if (!ImGui::Begin("Assets", &open)) {
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
    if (ImGui::Button("Rebuild Scripts")) {
        RebuildGameLogic(ctx); // gen + msbuild GameLogic → ホットリロード
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("C++ スクリプト (GameLogic.dll) を再生成 + ビルドしてホットリロード");
    }
    ImGui::SameLine();
    if (ImGui::Button("Compile C# Scripts")) {
        CompileCSharpScripts(ctx); // assets\scripts\*.cs をエンジン内 Roslyn でコンパイル
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("C# スクリプト (assets\\scripts\\*.cs) をエンジン内でコンパイル (Roslyn)");
    }
    ImGui::SameLine();
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
        if (ext == L".meta") {
            continue; // M23: GUID サイドカーはブラウザに表示しない
        }
        const bool isPrefab = nameU.size() >= 12
            && nameU.compare(nameU.size() - 12, 12, ".prefab.json") == 0;
        const bool isAnim = !isPrefab && nameU.size() >= 10
            && nameU.compare(nameU.size() - 10, 10, ".anim.json") == 0;
        const bool isMat = !isPrefab && !isAnim && nameU.size() >= 9
            && nameU.compare(nameU.size() - 9, 9, ".mat.json") == 0;

        if (i % cols != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        ImGui::BeginGroup();
        if (IsImageExt(ext)) {
            // M23: 非同期ロード。初回は白プレースホルダ → デコード完了後に実体へ差し替わる
            const AssetID id = ctx.resources->textures.RequestLoadFileAsync(path);
            Texture* tex = ctx.resources->textures.Get(id);
            if (tex && tex->srv) {
                ImGui::Image(reinterpret_cast<ImTextureID>(tex->srv.Get()), ImVec2(kCell, kCell));
            } else {
                ImGui::Button("img", ImVec2(kCell, kCell));
            }
        } else if (AssetPreviewCache::IsPreviewable(path)) {
            // M27d: メッシュ/プレハブの立体サムネイル (OnRenderViews で非同期生成)
            if (ID3D11ShaderResourceView* srv = preview.GetOrRequest(ctx, path)) {
                ImGui::Image(reinterpret_cast<ImTextureID>(srv), ImVec2(kCell, kCell));
            } else {
                ImGui::Button(isPrefab ? "prefab" : "model", ImVec2(kCell, kCell));
            }
        } else {
            const char* icon = isPrefab ? "prefab"
                                        : (isAnim ? "anim" : (isMat ? "mat" : IconFor(ext)));
            ImGui::Button(icon, ImVec2(kCell, kCell));
        }
        // ドラッグソース: パスをペイロードに (Hierarchy / SceneView が受け取り配置)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string u = WideToUtf8(path);
            ImGui::SetDragDropPayload(kAssetDragPayload, u.c_str(), u.size() + 1);
            ImGui::TextUnformatted(nameU.c_str());
            ImGui::EndDragDropSource();
        }
        // M24: 画像 (.dds 以外) を右クリック → BCn 圧縮 DDS にクック
        if (IsImageExt(ext) && ext != L".dds") {
            if (ImGui::BeginPopupContextItem("##ddsctx")) {
                if (ImGui::MenuItem("Compress to DDS (BCn)")) {
                    const std::wstring dds = fs::path(path).replace_extension(L".dds").wstring();
                    if (TextureCook::CookImageToDds(path, dds)) {
                        AssetDatabase::EnsureMeta(dds); // 生成した .dds に GUID サイドカーを付与
                    }
                }
                ImGui::EndPopup();
            }
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
            } else if (isMat) {
                // 選択エンティティの MeshRenderer にこのマテリアルを割り当てる (anim と同じ流儀)
                const AssetID id = ctx.resources->materials.LoadFromFile(
                    path, ctx.resources->textures, ctx.assetsRoot);
                GameObject sel = ctx.scene->FindByFileId(selection.primary);
                if (!id.IsNull() && sel) {
                    World& w = ctx.scene->GetWorld();
                    if (auto* mr = w.GetComponent<MeshRendererComponent>(sel.Id())) {
                        undo.BeginRecord("Assign Material", selection);
                        undo.CaptureBefore(*ctx.scene, selection.primary);
                        mr->material = id;
                        undo.CaptureAfter(*ctx.scene, selection.primary);
                        undo.EndRecord(selection);
                    } else {
                        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
                    }
                } else {
                    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

    // 空き領域を右クリック → Create メニュー
    auto beginCreate = [&](int kind, const char* def) {
        pendingCreate_ = kind;
        strncpy_s(createName_, sizeof(createName_), def, _TRUNCATE);
        requestModal_ = true;
    };
    if (ImGui::BeginPopupContextWindow("##assets_ctx",
                                       ImGuiPopupFlags_MouseButtonRight
                                           | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Folder")) { beginCreate(kCreateFolder, "New Folder"); }
            if (ImGui::MenuItem("C++ Script")) { beginCreate(kCreateScript, "NewScript"); }
            if (ImGui::MenuItem("C# Script")) { beginCreate(kCreateCSharp, "NewScript"); }
            if (ImGui::MenuItem("Scene")) { beginCreate(kCreateScene, "New Scene"); }
            if (ImGui::MenuItem("Animation Clip")) { beginCreate(kCreateAnim, "New Clip"); }
            if (ImGui::MenuItem("Material")) { beginCreate(kCreateMaterial, "New Material"); }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) {
            ShellExecuteW(nullptr, L"open", current_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // ---- 命名モーダル (Create の確定) ----
    if (requestModal_) {
        ImGui::OpenPopup("Create Asset");
        requestModal_ = false;
    }
    if (ImGui::BeginPopupModal("Create Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("Name", createName_, sizeof(createName_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool create = ImGui::Button("Create", ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(90, 0));
        if (create && createName_[0] != '\0') {
            DoCreate(ctx, externalEditorCmd);
            pendingCreate_ = 0;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            pendingCreate_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void AssetBrowserWindow::DoCreate(EngineContext& ctx, const std::string& externalEditorCmd)
{
    const std::string name = createName_;
    switch (pendingCreate_) {
    case kCreateFolder:
        CreateFolderAsset(current_, name);
        break;
    case kCreateScene:
        CreateSceneAsset(current_, name);
        break;
    case kCreateAnim:
        CreateAnimationAsset(ctx, current_, name);
        break;
    case kCreateMaterial:
        CreateMaterialAsset(ctx, current_, name);
        break;
    case kCreateScript: {
        const std::wstring p = CreateCppScript(ctx, name);
        if (!p.empty()) {
            OpenInExternalEditor(externalEditorCmd, p);
        }
        break;
    }
    case kCreateCSharp: {
        const std::wstring p = CreateCSharpScript(ctx, name);
        if (!p.empty()) {
            OpenInExternalEditor(externalEditorCmd, p);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace mye
