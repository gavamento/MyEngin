#include "Editor/Windows/AssetBrowserWindow.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/AssetOps.h"
#include "Editor/AssetPreviewCache.h"
#include "Editor/Undo/UndoStack.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/EntityNaming.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/TextureCook.h"

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

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
    kCreateMaterial,
    kCreateSound,
    kCreateMixer,
    kCreateActor,
    kCreatePhysMat // M59a1
};

// 型フィルタのコンボ内容 (M51i)。先頭 = フィルタなし (Unknown を「すべて」に転用)
struct TypeFilterEntry {
    AssetType type;
    StrId label;
};
constexpr TypeFilterEntry kTypeFilters[] = {
    { AssetType::Unknown, StrId::Asset_FilterAll },
    { AssetType::Texture, StrId::Type_Texture },
    { AssetType::Model, StrId::Type_Model },
    { AssetType::Material, StrId::Asset_MaterialItem },
    { AssetType::Prefab, StrId::Type_Prefab },
    { AssetType::Actor, StrId::Asset_Actor },
    { AssetType::Anim, StrId::Asset_AnimationClip },
    { AssetType::Controller, StrId::Type_Controller },
    { AssetType::Scene, StrId::Asset_Scene },
    { AssetType::Audio, StrId::Type_Audio },
    { AssetType::Sound, StrId::Asset_Sound },
    { AssetType::Mixer, StrId::Asset_Mixer },
    { AssetType::Script, StrId::Type_Script },
    { AssetType::Shader, StrId::Type_Shader },
    { AssetType::Schema, StrId::Type_Schema },
    { AssetType::Terrain, StrId::Terrain_AssetType },
    { AssetType::PhysMat, StrId::Asset_PhysMat }, // M59a1
};

const char* IconFor(const std::wstring& ext)
{
    if (ext == L".hlsl" || ext == L".hlsli") {
        return "shader";
    }
    if (ext == L".wav" || ext == L".ogg") {
        return "audio";
    }
    if (ext == L".glb" || ext == L".gltf" || ext == L".fbx") {
        return "model";
    }
    if (ext == L".json") {
        return "json";
    }
    return "file";
}

// 構成アセット (.actor.json / .prefab.json) をシーンのルートへ配置する。
// M48k でダブルクリックが「編集モードで開く」に変わったため、右クリックメニューの
// 「シーンに配置」から呼ぶ経路として切り出した (挙動は M48b 以降と同一)
void InstantiateComposeAsset(EngineContext& ctx, Selection& selection, UndoStack& undo,
                             const std::wstring& path)
{
    undo.BeginRecord("Instantiate Prefab", selection); // 生成を 1 Undo エントリに
    const uint64_t hash = ctx.prefabs->LoadFromFile(path); // 未登録なら登録 (冪等)
    const uint64_t rootFid =
        (hash != 0) ? Prefab::Instantiate(*ctx.scene, *ctx.prefabs, hash, 0) : 0;
    ctx.scene->GetWorld().ApplyStructuralChanges();
    if (rootFid == 0) {
        undo.CancelRecord();
        return;
    }
    // 兄弟名の一意化 (M48b)。InstantiateAssetAtPath を通らない独立経路なのでここにも要る。
    // ルート配置だが exclude = 自分自身が必須
    if (GameObject r = ctx.scene->FindByFileId(rootFid)) {
        World& w = ctx.scene->GetWorld();
        const EntityID re = r.Id();
        if (auto* nc = w.GetComponent<NameComponent>(re)) {
            SetEntityName(w, re,
                          MakeUniqueSiblingName(w, w.GetParent(re), nc->value, /*exclude=*/re));
        }
    }
    selection.SelectOnly(rootFid);
    undo.CaptureAfter(*ctx.scene, rootFid);
    undo.EndRecord(selection);
}

// サムネイル未生成 (または対象外) のタイルに出す文字ラベル
const char* TileLabel(const std::wstring& ext, const std::wstring& path, bool isCompose,
                      bool isActor, bool isAnim, bool isMat, bool isSound, bool isMixer,
                      bool isSchema, bool isPhysMat)
{
    if (IsImageExt(ext)) {
        return "img"; // デコード完了までのプレースホルダ
    }
    if (isSchema) {
        return "schema"; // .component.schema.json (M48j)
    }
    if (isCompose) {
        return isActor ? "actor" : "prefab";
    }
    if (isAnim) {
        return "anim";
    }
    if (isPhysMat) {
        return "physmat"; // .physmat.json (M59a1)。isMat より先 (どちらも mat.json を含む)
    }
    if (isMat) {
        return "mat";
    }
    if (isSound) {
        return "sound";
    }
    if (isMixer) {
        return "mixer";
    }
    if (AssetPreviewCache::IsPreviewable(path)) {
        return "model"; // 立体サムネイル生成待ち
    }
    return IconFor(ext);
}

} // namespace

void AssetBrowserWindow::BeginRename(const std::wstring& path)
{
    std::error_code ec;
    pendingRenamePath_ = path;
    std::string pre;
    if (fs::is_directory(path, ec)) {
        pre = WideToUtf8(fs::path(path).filename().wstring());
    } else {
        // 拡張子/複合サフィックスは編集させない (RenameAsset 側で維持される)
        std::wstring stem;
        std::wstring suffix;
        SplitAssetName(fs::path(path).filename().wstring(), stem, suffix);
        pre = WideToUtf8(stem);
    }
    strncpy_s(createName_, sizeof(createName_), pre.c_str(), _TRUNCATE);
    requestRenameModal_ = true;
}

void AssetBrowserWindow::DrawDirTree(EngineContext& ctx, const std::wstring& dir)
{
    (void)ctx;
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
        // D&D 移動の受け皿 (M30b、Unity 同様ツリーにも落とせる)。実行はフレーム末 (iterator 保護)
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                pendingMoveSrc_ = Utf8ToWide(static_cast<const char*>(pl->Data));
                pendingMoveDstDir_ = entry.path().wstring();
            }
            ImGui::EndDragDropTarget();
        }
        // 右クリック → Rename (M30d)。実行はモーダル確定時 (iterator 保護)
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(Tr(StrId::Asset_RenameItem))) {
                BeginRename(entry.path().wstring());
            }
            ImGui::EndPopup();
        }
        if (nodeOpen) {
            DrawDirTree(ctx, entry.path().wstring());
            ImGui::TreePop();
        }
    }
}

void AssetBrowserWindow::OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                 const std::string& externalEditorCmd,
                                 AssetPreviewCache& preview)
{
    panelRectValid_ = false; // Begin に成功したフレームだけ矩形が有効
    if (!open) {
        return;
    }
    if (!ImGui::Begin(Tr(StrId::Win_Assets), &open)) {
        ImGui::End();
        return;
    }
    {
        // エクスプローラー D&D の受理判定用にパネル矩形を記録 (クライアント座標)
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        panelMin_[0] = p.x;
        panelMin_[1] = p.y;
        panelMax_[0] = p.x + s.x;
        panelMax_[1] = p.y + s.y;
        panelRectValid_ = true;
    }
    if (!init_) {
        current_ = ctx.assetsRoot;
        init_ = true;
    }

    // ---- キーボード (M51i): 選択中アセットへ Delete / Ctrl+D ----
    // パネル (子ウィンドウ含む) がフォーカスされている間だけ。エンティティ側の
    // Delete / Ctrl+D (EditorApp::HandleShortcuts) とは選択の排他 (M40c) で二重発火しない。
    // チョードは ShortcutHub の Delete / Duplicate と同じ割り当て
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && selection.HasAsset()) {
        std::error_code kec;
        if (fs::exists(selection.assetPath, kec)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                pendingDeletePath_ = selection.assetPath;
                requestDeleteModal_ = true;
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
                pendingDuplicatePath_ = selection.assetPath;
            }
        }
    }

    // ---- 左: フォルダツリー ----
    ImGui::BeginChild("##dirtree", ImVec2(170, 0), ImGuiChildFlags_Borders);
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ctx.assetsRoot == current_) {
        rootFlags |= ImGuiTreeNodeFlags_Selected;
    }
    const bool rootOpen = ImGui::TreeNodeEx("assets", rootFlags);
    if (ImGui::IsItemClicked()) {
        current_ = ctx.assetsRoot;
    }
    // ルートへの D&D 移動 (M30b): assets 直下へ戻す
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            pendingMoveSrc_ = Utf8ToWide(static_cast<const char*>(pl->Data));
            pendingMoveDstDir_ = ctx.assetsRoot;
        }
        ImGui::EndDragDropTarget();
    }
    if (rootOpen) {
        DrawDirTree(ctx, ctx.assetsRoot);
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- 右: ファイルグリッド ----
    ImGui::BeginChild("##filegrid", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::Button(Tr(StrId::Asset_RebuildScripts))) {
        // ★ここで直接 ShellExecuteW しない (M66e)。fire-and-forget で起動すると
        //   プロセスハンドルが誰の手にも残らず、**走っている間ゲートが閉じない** =
        //   ビルドが bin\ と cache\ を書いている最中に checkout / pull が通る。
        //   EditorApp がハンドルを持って毎フレーム見る形へ寄せる
        rebuildScriptsRequest_ = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Asset_TipRebuild));
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Asset_CompileCs))) {
        CompileCSharpScripts(ctx); // assets\scripts\*.cs をエンジン内 Roslyn でコンパイル
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Asset_TipCompileCs));
    }
    ImGui::SameLine();
    // ---- パンくずナビ (M30a): assets > sub > ... をクリックで移動 ----
    {
        std::error_code bec;
        if (!fs::is_directory(current_, bec)) {
            current_ = ctx.assetsRoot; // Explorer 側で削除された場合のフォールバック
        }
        std::wstring navTo;
        const std::wstring rootKey = NormalizePathKey(ctx.assetsRoot);
        const std::wstring curKey = NormalizePathKey(current_);
        const bool underRoot = curKey.size() >= rootKey.size()
            && curKey.compare(0, rootKey.size(), rootKey) == 0
            && (curKey.size() == rootKey.size() || curKey[rootKey.size()] == L'\\');
        if (underRoot) {
            if (ImGui::SmallButton("assets")) {
                navTo = ctx.assetsRoot;
            }
            const fs::path rel = fs::relative(current_, ctx.assetsRoot, bec);
            if (!bec && rel != L".") {
                std::wstring accum = ctx.assetsRoot;
                int seg = 0;
                for (const auto& part : rel) {
                    accum += L"\\" + part.wstring();
                    ImGui::SameLine(0, 2);
                    ImGui::TextDisabled(">");
                    ImGui::SameLine(0, 2);
                    ImGui::PushID(seg++);
                    if (ImGui::SmallButton(WideToUtf8(part.wstring()).c_str())) {
                        navTo = accum;
                    }
                    ImGui::PopID();
                }
            }
        } else {
            ImGui::TextDisabled("%s", WideToUtf8(current_).c_str()); // 異常系フォールバック
        }
        if (!navTo.empty()) {
            current_ = navTo;
        }
    }
    // ---- 検索 + 型フィルタ (M51i)。どちらかが有効な間は再帰検索モード ----
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##assetsearch", Tr(StrId::Asset_SearchHint), searchBuf_,
                             sizeof(searchBuf_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##assettype", Tr(kTypeFilters[typeFilterIdx_].label))) {
        for (int t = 0; t < static_cast<int>(IM_ARRAYSIZE(kTypeFilters)); ++t) {
            if (ImGui::Selectable(Tr(kTypeFilters[t].label), t == typeFilterIdx_)) {
                typeFilterIdx_ = t;
            }
        }
        ImGui::EndCombo();
    }
    if (searchBuf_[0] != '\0' || typeFilterIdx_ != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(StrId::Asset_ClearFilter))) {
            searchBuf_[0] = '\0';
            typeFilterIdx_ = 0;
        }
    }
    ImGui::Separator();

    constexpr float kCell = 88.0f;
    const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (kCell + 12.0f)));

    // ---- エントリ収集 (M30a): フォルダ → ファイルの順、それぞれ名前昇順で安定表示 ----
    // M51i: 検索/型フィルタ中は current_ 以下の再帰列挙 (ファイルのみ) に切り替わる
    std::error_code ec;
    std::vector<fs::path> dirEntries;
    std::vector<fs::path> fileEntries;
    const bool filtering = searchBuf_[0] != '\0' || typeFilterIdx_ != 0;
    if (!filtering) {
        for (const auto& entry : fs::directory_iterator(current_, ec)) {
            if (entry.is_directory()) {
                dirEntries.push_back(entry.path());
            } else if (entry.path().extension().wstring() != L".meta") {
                fileEntries.push_back(entry.path()); // M23: GUID サイドカーは表示しない
            }
        }
    } else {
        std::wstring query = Utf8ToWide(searchBuf_);
        std::transform(query.begin(), query.end(), query.begin(), ::towlower);
        for (const auto& entry : fs::recursive_directory_iterator(current_, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const fs::path& p = entry.path();
            if (p.extension().wstring() == L".meta") {
                continue;
            }
            if (typeFilterIdx_ != 0
                && AssetDatabase::ClassifyPath(p.wstring())
                       != kTypeFilters[typeFilterIdx_].type) {
                continue;
            }
            if (!query.empty()) {
                std::wstring name = p.filename().wstring();
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (name.find(query) == std::wstring::npos) {
                    continue;
                }
            }
            fileEntries.push_back(p);
        }
    }
    auto nameLess = [](const fs::path& a, const fs::path& b) {
        return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
    };
    std::sort(dirEntries.begin(), dirEntries.end(), nameLess);
    std::sort(fileEntries.begin(), fileEntries.end(), nameLess);

    // ---- フォルダタイル (M30a): ダブルクリックで移動。ループ後に適用 (描画中の変更回避) ----
    std::wstring pendingNav;
    int i = 0;
    for (const fs::path& dirPath : dirEntries) {
        if (i % cols != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        ImGui::BeginGroup();
        // 透明ボタン + 大きな FA フォルダグリフ (Unity 風)。ボタンがアイテム本体なので
        // 直後のドラッグ元/受け皿/コンテキストメニュー/ダブルクリックはそのまま機能する
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
        ImGui::Button("##folder", ImVec2(kCell, kCell));
        ImGui::PopStyleColor(3);
        {
            const bool folderHovered = ImGui::IsItemHovered();
            const ImVec2 mn = ImGui::GetItemRectMin();
            ImGui::PushFont(nullptr, kCell * 0.6f); // imgui 1.92 動的アトラス: 任意サイズ描画可
            const ImVec2 ts = ImGui::CalcTextSize(ICON_FA_FOLDER);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(mn.x + (kCell - ts.x) * 0.5f, mn.y + (kCell - ts.y) * 0.5f),
                // 配色ルール (ImGuiTheme.h) の帯に収めた金 — 原色寄りの黄 (232,196,80) は目に刺さる
                folderHovered ? IM_COL32(219, 194, 134, 255) : IM_COL32(199, 173, 112, 255),
                ICON_FA_FOLDER);
            ImGui::PopFont();
        }
        const std::string dirNameU = WideToUtf8(dirPath.filename().wstring());
        // フォルダ自身も移動のドラッグ元になれる (フォルダ→フォルダ移動、M30b)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string u = WideToUtf8(dirPath.wstring());
            ImGui::SetDragDropPayload(kAssetDragPayload, u.c_str(), u.size() + 1);
            ImGui::TextUnformatted(dirNameU.c_str());
            ImGui::EndDragDropSource();
        }
        // ドロップでこのフォルダへ移動 (Unity 風)。実行はフレーム末
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                pendingMoveSrc_ = Utf8ToWide(static_cast<const char*>(pl->Data));
                pendingMoveDstDir_ = dirPath.wstring();
            }
            ImGui::EndDragDropTarget();
        }
        // 右クリック → Rename (M30d) / Duplicate / Delete (M51i)
        if (ImGui::BeginPopupContextItem("##folderctx")) {
            if (ImGui::MenuItem(Tr(StrId::Asset_RenameItem))) {
                BeginRename(dirPath.wstring());
            }
            if (ImGui::MenuItem(Tr(StrId::Asset_DuplicateItem), "Ctrl+D")) {
                pendingDuplicatePath_ = dirPath.wstring();
            }
            if (ImGui::MenuItem(Tr(StrId::Common_Delete), "Del")) {
                pendingDeletePath_ = dirPath.wstring();
                requestDeleteModal_ = true;
            }
            if (ImGui::MenuItem(Tr(StrId::Asset_ShowInExplorer))) {
                ShellExecuteW(nullptr, L"open", dirPath.wstring().c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            pendingNav = dirPath.wstring();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kCell);
        ImGui::TextWrapped("%s", dirNameU.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
        ++i;
    }

    for (const fs::path& filePath : fileEntries) {
        const std::wstring path = filePath.wstring();
        // 拡張子は小文字に正規化してから判定する (DCC 由来の ".FBX" / ".PNG" を取りこぼさない)
        std::wstring ext = filePath.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        const std::string nameU = WideToUtf8(filePath.filename().wstring());
        // .actor.json / .prefab.json はどちらもプレハブライブラリが扱う構成アセット (M48d)。
        // 判定は PrefabLibrary::IsComposePath 一本 (suffix の書き足し漏れを作らない)
        const bool isCompose = PrefabLibrary::IsComposePath(path);
        const bool isActor = isCompose && nameU.size() >= 11
            && nameU.compare(nameU.size() - 11, 11, ".actor.json") == 0;
        const bool isAnim = !isCompose && nameU.size() >= 10
            && nameU.compare(nameU.size() - 10, 10, ".anim.json") == 0;
        const bool isMat = !isCompose && !isAnim && nameU.size() >= 9
            && nameU.compare(nameU.size() - 9, 9, ".mat.json") == 0;
        const bool isScene = !isCompose && !isAnim && !isMat && nameU.size() >= 11
            && nameU.compare(nameU.size() - 11, 11, ".scene.json") == 0;
        const bool isSound = !isCompose && !isAnim && !isMat && !isScene && nameU.size() >= 11
            && nameU.compare(nameU.size() - 11, 11, ".sound.json") == 0;
        const bool isMixer = !isCompose && !isAnim && !isMat && !isScene && !isSound
            && nameU.size() >= 11 && nameU.compare(nameU.size() - 11, 11, ".mixer.json") == 0;
        // M59a1: 物理マテリアル。".physmat.json" の末尾 9 文字は "smat.json" なので isMat には
        // 落ちない (PhysMatSelfTest が固定) — ここは独立判定でよい
        const bool isPhysMat = !isCompose
            && nameU.size() >= 13 && nameU.compare(nameU.size() - 13, 13, ".physmat.json") == 0;
        // M48j: .component.schema.json は起動時に読まれる動的コンポーネント定義。
        // 他の複合サフィックスより長いので単独判定でよい (.json の一般判定より先に効く)
        const bool isSchema = nameU.size() >= 22
            && nameU.compare(nameU.size() - 22, 22, ".component.schema.json") == 0;
        const bool isClip = ext == L".wav" || ext == L".ogg"; // 素の音声ファイル

        if (i % cols != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        ImGui::BeginGroup();
        ID3D11ShaderResourceView* thumb = nullptr;
        if (IsImageExt(ext)) {
            // M23: 非同期ロード。初回は白プレースホルダ → デコード完了後に実体へ差し替わる
            const AssetID id = ctx.resources->textures.RequestLoadFileAsync(path);
            if (Texture* tex = ctx.resources->textures.Get(id)) {
                thumb = tex->srv.Get();
            }
        } else if (AssetPreviewCache::IsPreviewable(path)) {
            // M27d: メッシュ/プレハブの立体サムネイル (OnRenderViews で非同期生成)
            thumb = preview.GetOrRequest(ctx, path);
        }
        // タイル本体は **必ず ID を持つアイテム (Button)** にする。ImGui::Image は ID を持たず、
        // 直後の BeginDragDropSource が「ID なしアイテム」経路 (IM_ASSERT → false) に落ちるため、
        // サムネイルが生成されたアセット (fbx/glb/prefab/画像) だけドラッグで掴めなくなっていた。
        // フォルダタイルと同じ流儀: 透明ボタンをアイテムにして絵は drawlist で重ねる
        ImGui::PushStyleColor(ImGuiCol_Button, thumb ? ImVec4(0, 0, 0, 0)
                                                     : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::Button(thumb ? "##tile"
                            : TileLabel(ext, path, isCompose, isActor, isAnim, isMat, isSound,
                                        isMixer, isSchema, isPhysMat),
                      ImVec2(kCell, kCell));
        ImGui::PopStyleColor();
        if (thumb) {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddImage(reinterpret_cast<ImTextureID>(thumb), mn, mx);
            if (ImGui::IsItemHovered()) {
                dl->AddRect(mn, mx, IM_COL32(255, 255, 255, 96)); // 透明ボタンの代わりのホバー表示
            }
        }
        // 再帰検索モードではどのサブフォルダの一致か分かるよう相対パスを添える (M51i)
        if (filtering && ImGui::IsItemHovered()) {
            std::error_code rec;
            const fs::path rel = fs::relative(filePath, current_, rec);
            ImGui::SetTooltip("%s", WideToUtf8((rec ? filePath : rel).wstring()).c_str());
        }
        // ドラッグソース: パスをペイロードに (Hierarchy / SceneView が受け取り配置)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string u = WideToUtf8(path);
            ImGui::SetDragDropPayload(kAssetDragPayload, u.c_str(), u.size() + 1);
            ImGui::TextUnformatted(nameU.c_str());
            ImGui::EndDragDropSource();
        }
        // 右クリックメニュー: Rename (M30d) + Duplicate/Delete (M51i)
        // + 画像なら Import Settings (M39b) / DDS 圧縮 (M24)
        if (ImGui::BeginPopupContextItem("##filectx")) {
            if (ImGui::MenuItem(Tr(StrId::Asset_RenameItem))) {
                BeginRename(path);
            }
            if (ImGui::MenuItem(Tr(StrId::Asset_DuplicateItem), "Ctrl+D")) {
                pendingDuplicatePath_ = path;
            }
            if (ImGui::MenuItem(Tr(StrId::Common_Delete), "Del")) {
                pendingDeletePath_ = path;
                requestDeleteModal_ = true;
            }
            // M48k でダブルクリックが「編集モードで開く」に変わったので、旧来の
            // 「シーンへ配置」はここに残す (D&D 配置も従来どおり使える)
            if (isCompose) {
                if (ImGui::MenuItem(Tr(StrId::Asset_InstantiateItem))) {
                    InstantiateComposeAsset(ctx, selection, undo, path);
                }
            }
            if (IsImageExt(ext)) {
                if (ImGui::MenuItem(Tr(StrId::Asset_ImportSettings))) {
                    AssetMeta m;
                    AssetDatabase::ReadMeta(path + L".meta", m); // 不在なら既定値のまま
                    importEdit_ = m.tex;
                    pendingImportPath_ = path;
                    requestImportModal_ = true;
                }
            }
            if (IsImageExt(ext) && ext != L".dds") {
                if (ImGui::MenuItem(Tr(StrId::Asset_CompressDds))) {
                    // .meta の Import Settings が mips/圧縮形式を決める (M39b)
                    AssetMeta m;
                    AssetDatabase::ReadMeta(path + L".meta", m);
                    TextureCook::CookOptions co;
                    co.generateMips = m.tex.generateMips != 0;
                    co.compress = m.tex.compress == 0;
                    const std::wstring dds = fs::path(path).replace_extension(L".dds").wstring();
                    if (TextureCook::CookImageToDds(path, dds, co)) {
                        AssetDatabase::EnsureMeta(dds); // 生成した .dds に GUID サイドカーを付与
                    }
                }
            }
            ImGui::EndPopup();
        }
        // タイル単一クリック → アセット選択 (Inspector に情報 + Import Settings 表示、M40c)
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selection.SelectAsset(path);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (isCompose) {
                // M48k: ダブルクリックは **アセットをミニシーンで開く** (Unity の Prefab Mode)。
                // シーンへの配置は D&D と右クリックメニューに残してある
                pendingOpenActor_ = path;
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
            } else if (isSound) {
                // M45e: AudioSource を選択中ならそれに割り当て、そうでなければ試聴する
                // (マテリアル/アニメの「選択中の該当コンポーネントに割り当てる」流儀と同じ)
                if (ctx.sounds && ctx.audio) {
                    const uint64_t hash = ctx.sounds->LoadFromFile(path); // 未登録なら登録 (冪等)
                    GameObject sel = ctx.scene->FindByFileId(selection.primary);
                    AudioSourceComponent* as = nullptr;
                    if (hash != 0 && sel) {
                        as = ctx.scene->GetWorld().GetComponent<AudioSourceComponent>(sel.Id());
                    }
                    if (as != nullptr) {
                        undo.BeginRecord("Assign Sound", selection);
                        undo.CaptureBefore(*ctx.scene, selection.primary);
                        as->sound = AssetID{ hash };
                        undo.CaptureAfter(*ctx.scene, selection.primary);
                        undo.EndRecord(selection);
                    } else if (const SoundAsset* s = ctx.sounds->Get(hash)) {
                        PreviewSound(*ctx.audio, *s); // 先頭バリエーション・揺らぎ無し
                    }
                }
            } else if (isMixer) {
                // .mixer.json をアクティブにして Audio Mixer 窓を開く (M45d)。
                // 適用は AudioSystem::Update = フレーム境界まで遅延される
                if (ctx.mixers && ctx.audio) {
                    const uint64_t hash = ctx.mixers->LoadFromFile(path); // 未登録なら登録 (冪等)
                    if (const MixerAsset* m = ctx.mixers->Get(hash)) {
                        ctx.mixers->SetActive(hash);
                        ctx.audio->ApplyMixer(*m);
                        openMixerRequest_ = true;
                    } else {
                        MYE_LOG_WARN("could not load mixer: %s", WideToUtf8(path).c_str());
                    }
                }
            } else if (isClip) {
                // .wav / .ogg をその場で試聴 (OS の既定プレイヤーは開かない)
                if (ctx.audio) {
                    const AssetID id = ctx.audio->LoadClipFile(path);
                    if (!id.IsNull()) {
                        PlayDesc d;
                        d.clip = id;
                        ctx.audio->Play(d);
                    } else {
                        MYE_LOG_WARN("could not preview audio clip: %s", WideToUtf8(path).c_str());
                    }
                }
            } else if (isScene) {
                // シーンを開く。ロード自体は EditorApp が未保存変更ガードを通して行う
                pendingOpenScene_ = path;
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
    if (!pendingNav.empty()) {
        current_ = pendingNav; // フォルダダブルクリックの適用 (次フレームから新フォルダ表示)
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
        if (ImGui::BeginMenu(Tr(StrId::Asset_Create))) {
            if (ImGui::MenuItem(Tr(StrId::Asset_Folder))) { beginCreate(kCreateFolder, "New Folder"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_CppScript))) { beginCreate(kCreateScript, "NewScript"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_CsScript))) { beginCreate(kCreateCSharp, "NewScript"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_Scene))) { beginCreate(kCreateScene, "New Scene"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_AnimationClip))) { beginCreate(kCreateAnim, "New Clip"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_MaterialItem))) { beginCreate(kCreateMaterial, "New Material"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_Actor))) { beginCreate(kCreateActor, "New Actor"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_Sound))) { beginCreate(kCreateSound, "New Sound"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_Mixer))) { beginCreate(kCreateMixer, "New Mixer"); }
            if (ImGui::MenuItem(Tr(StrId::Asset_PhysMat))) { beginCreate(kCreatePhysMat, "New PhysMat"); }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(Tr(StrId::Asset_ShowInExplorer))) {
            ShellExecuteW(nullptr, L"open", current_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // ---- D&D 移動の実行 (M30b)。描画終了後にまとめて行う (directory_iterator 保護) ----
    if (!pendingMoveSrc_.empty()) {
        MoveAssetToFolder(ctx, pendingMoveSrc_, pendingMoveDstDir_, &undo); // M51i: Undo 記録
        pendingMoveSrc_.clear();
        pendingMoveDstDir_.clear();
    }

    // ---- 複製の実行 (M51i)。fs 変更はフレーム末にまとめる (D&D 移動と同じ理由) ----
    if (!pendingDuplicatePath_.empty()) {
        const std::wstring d = DuplicateAsset(ctx, pendingDuplicatePath_, &undo);
        if (!d.empty()) {
            selection.SelectAsset(d);
        }
        pendingDuplicatePath_.clear();
    }

    // ---- リネームモーダル (M30d) ----
    if (requestRenameModal_) {
        ImGui::OpenPopup(Tr(StrId::Popup_RenameAsset));
        requestRenameModal_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_RenameAsset), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", WideToUtf8(pendingRenamePath_).c_str());
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText(Tr(StrId::Asset_NameField), createName_, sizeof(createName_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool doRename = ImGui::Button(Tr(StrId::Common_Rename), ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancelRename = ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0));
        if (doRename && createName_[0] != '\0') {
            const std::wstring newPath =
                RenameAsset(ctx, pendingRenamePath_, createName_, &undo); // M51i: Undo 記録
            if (!newPath.empty()) {
                // 表示中フォルダがリネーム対象 (またはその配下) なら追従する
                const std::wstring oldKey = NormalizePathKey(pendingRenamePath_);
                const std::wstring curKey = NormalizePathKey(current_);
                if (curKey == oldKey) {
                    current_ = newPath;
                } else if (curKey.size() > oldKey.size()
                           && curKey.compare(0, oldKey.size(), oldKey) == 0
                           && curKey[oldKey.size()] == L'\\') {
                    current_ = newPath + curKey.substr(oldKey.size());
                }
            }
            pendingRenamePath_.clear();
            ImGui::CloseCurrentPopup();
        } else if (cancelRename) {
            pendingRenamePath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- 削除確認モーダル (M51i): ごみ箱送り。UndoStack には積まない (復元手段はごみ箱) ----
    if (requestDeleteModal_) {
        ImGui::OpenPopup(Tr(StrId::Popup_DeleteAsset));
        requestDeleteModal_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_DeleteAsset), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(Tr(StrId::Asset_DeleteMsg),
                    WideToUtf8(fs::path(pendingDeletePath_).filename().wstring()).c_str());
        ImGui::TextDisabled("%s", WideToUtf8(pendingDeletePath_).c_str());
        ImGui::TextUnformatted(Tr(StrId::Asset_TrashNote));
        if (ImGui::Button(Tr(StrId::Common_Delete), ImVec2(90, 0))) {
            if (DeleteAssetToRecycleBin(ctx, pendingDeletePath_)) {
                // 消した本人 (またはその配下) を選択していたら解除する。
                // 表示中フォルダが消えた場合はパンくず側のフォールバックが assets へ戻す
                const std::wstring delKey = NormalizePathKey(pendingDeletePath_);
                const std::wstring selKey = NormalizePathKey(selection.assetPath);
                if (selKey == delKey
                    || (selKey.size() > delKey.size()
                        && selKey.compare(0, delKey.size(), delKey) == 0
                        && selKey[delKey.size()] == L'\\')) {
                    selection.Clear();
                }
            }
            pendingDeletePath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0))) {
            pendingDeletePath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Import Settings モーダル (M39b): .meta v2 のテクスチャインポート設定 ----
    if (requestImportModal_) {
        ImGui::OpenPopup(Tr(StrId::Popup_ImportSettings));
        requestImportModal_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_ImportSettings), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", WideToUtf8(pendingImportPath_).c_str());
        const char* srgbLabels[] = { "Auto (usage hint)", "sRGB (albedo)", "Linear (data)" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo(Tr(StrId::Insp_Srgb), &importEdit_.srgb, srgbLabels, 3);
        bool mips = importEdit_.generateMips != 0;
        if (ImGui::Checkbox(Tr(StrId::Insp_GenerateMips), &mips)) {
            importEdit_.generateMips = mips ? 1 : 0;
        }
        const char* compLabels[] = { "Auto (BC1/BC3)", "None (RGBA8)" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo(Tr(StrId::Insp_CookCompress), &importEdit_.compress, compLabels, 2);
        if (ImGui::Button(Tr(StrId::Common_Apply), ImVec2(90, 0))) {
            const std::wstring metaPath = pendingImportPath_ + L".meta";
            AssetDatabase::EnsureMeta(pendingImportPath_); // 不在なら生成 (GUID 確定)
            AssetMeta m;
            if (AssetDatabase::ReadMeta(metaPath, m)) {
                m.type = AssetType::Texture; // 旧 .meta の型欠落でも tex を書けるように
                m.tex = importEdit_;
                m.version = 2;
                AssetDatabase::WriteMeta(metaPath, m);
                // ロード済みならその場で再ロード (AssetID 不変 = 参照側の再解決不要)
                ctx.resources->textures.ReplaceFromFile(
                    TextureLibrary::IdForFile(pendingImportPath_), pendingImportPath_);
            }
            pendingImportPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0))) {
            pendingImportPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- 命名モーダル (Create の確定) ----
    if (requestModal_) {
        ImGui::OpenPopup(Tr(StrId::Popup_CreateAsset));
        requestModal_ = false;
    }
    if (ImGui::BeginPopupModal(Tr(StrId::Popup_CreateAsset), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText(Tr(StrId::Asset_NameField), createName_, sizeof(createName_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool create = ImGui::Button(Tr(StrId::Common_Create), ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button(Tr(StrId::Common_Cancel), ImVec2(90, 0));
        if (create && createName_[0] != '\0') {
            DoCreate(ctx, undo, externalEditorCmd);
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

void AssetBrowserWindow::DoCreate(EngineContext& ctx, UndoStack& undo,
                                  const std::string& externalEditorCmd)
{
    // アセット系 8 種は Create Undo を記録する (M51i: undo = ごみ箱へ / redo = 書き戻し)
    const std::string name = createName_;
    switch (pendingCreate_) {
    case kCreateFolder:
        RecordAssetCreated(undo, CreateFolderAsset(current_, name));
        break;
    case kCreateScene:
        RecordAssetCreated(undo, CreateSceneAsset(current_, name));
        break;
    case kCreateAnim:
        RecordAssetCreated(undo, CreateAnimationAsset(ctx, current_, name));
        break;
    case kCreateMaterial:
        RecordAssetCreated(undo, CreateMaterialAsset(ctx, current_, name));
        break;
    case kCreateActor:
        RecordAssetCreated(undo, CreateActorAsset(ctx, current_, name));
        break;
    case kCreateSound:
        RecordAssetCreated(undo, CreateSoundAsset(ctx, current_, name));
        break;
    case kCreateMixer:
        RecordAssetCreated(undo, CreateMixerAsset(ctx, current_, name));
        break;
    case kCreatePhysMat: // M59a1
        RecordAssetCreated(undo, CreatePhysMatAsset(ctx, current_, name));
        break;
    case kCreateScript: {
        // スクリプトは Create Undo 対象外 — 既存ファイルなら「開くだけ」で返る経路と
        // 区別できず、Undo が既存ソースをごみ箱送りにしてしまう
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

bool AssetBrowserWindow::IsClientPosInPanel(float x, float y) const
{
    return panelRectValid_ && x >= panelMin_[0] && x < panelMax_[0] && y >= panelMin_[1] &&
           y < panelMax_[1];
}

} // namespace mye
