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
#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/AssetDatabase.h"
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
    kCreateMixer
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

// サムネイル未生成 (または対象外) のタイルに出す文字ラベル
const char* TileLabel(const std::wstring& ext, const std::wstring& path, bool isPrefab, bool isAnim,
                      bool isMat, bool isSound, bool isMixer)
{
    if (IsImageExt(ext)) {
        return "img"; // デコード完了までのプレースホルダ
    }
    if (isPrefab) {
        return "prefab";
    }
    if (isAnim) {
        return "anim";
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
            if (ImGui::MenuItem("Rename")) {
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
    if (!ImGui::Begin("Assets", &open)) {
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
    ImGui::Separator();

    constexpr float kCell = 88.0f;
    const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (kCell + 12.0f)));

    // ---- エントリ収集 (M30a): フォルダ → ファイルの順、それぞれ名前昇順で安定表示 ----
    std::error_code ec;
    std::vector<fs::path> dirEntries;
    std::vector<fs::path> fileEntries;
    for (const auto& entry : fs::directory_iterator(current_, ec)) {
        if (entry.is_directory()) {
            dirEntries.push_back(entry.path());
        } else if (entry.path().extension().wstring() != L".meta") {
            fileEntries.push_back(entry.path()); // M23: GUID サイドカーは表示しない
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
                folderHovered ? IM_COL32(255, 214, 90, 255) : IM_COL32(232, 196, 80, 255),
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
        // 右クリック → Rename (M30d)
        if (ImGui::BeginPopupContextItem("##folderctx")) {
            if (ImGui::MenuItem("Rename")) {
                BeginRename(dirPath.wstring());
            }
            if (ImGui::MenuItem("Show in Explorer")) {
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
        const bool isPrefab = nameU.size() >= 12
            && nameU.compare(nameU.size() - 12, 12, ".prefab.json") == 0;
        const bool isAnim = !isPrefab && nameU.size() >= 10
            && nameU.compare(nameU.size() - 10, 10, ".anim.json") == 0;
        const bool isMat = !isPrefab && !isAnim && nameU.size() >= 9
            && nameU.compare(nameU.size() - 9, 9, ".mat.json") == 0;
        const bool isScene = !isPrefab && !isAnim && !isMat && nameU.size() >= 11
            && nameU.compare(nameU.size() - 11, 11, ".scene.json") == 0;
        const bool isSound = !isPrefab && !isAnim && !isMat && !isScene && nameU.size() >= 11
            && nameU.compare(nameU.size() - 11, 11, ".sound.json") == 0;
        const bool isMixer = !isPrefab && !isAnim && !isMat && !isScene && !isSound
            && nameU.size() >= 11 && nameU.compare(nameU.size() - 11, 11, ".mixer.json") == 0;
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
                            : TileLabel(ext, path, isPrefab, isAnim, isMat, isSound, isMixer),
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
        // ドラッグソース: パスをペイロードに (Hierarchy / SceneView が受け取り配置)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string u = WideToUtf8(path);
            ImGui::SetDragDropPayload(kAssetDragPayload, u.c_str(), u.size() + 1);
            ImGui::TextUnformatted(nameU.c_str());
            ImGui::EndDragDropSource();
        }
        // 右クリックメニュー: Rename (M30d) + 画像なら Import Settings (M39b) / DDS 圧縮 (M24)
        if (ImGui::BeginPopupContextItem("##filectx")) {
            if (ImGui::MenuItem("Rename")) {
                BeginRename(path);
            }
            if (IsImageExt(ext)) {
                if (ImGui::MenuItem("Import Settings...")) {
                    AssetMeta m;
                    AssetDatabase::ReadMeta(path + L".meta", m); // 不在なら既定値のまま
                    importEdit_ = m.tex;
                    pendingImportPath_ = path;
                    requestImportModal_ = true;
                }
            }
            if (IsImageExt(ext) && ext != L".dds") {
                if (ImGui::MenuItem("Compress to DDS")) {
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
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Folder")) { beginCreate(kCreateFolder, "New Folder"); }
            if (ImGui::MenuItem("C++ Script")) { beginCreate(kCreateScript, "NewScript"); }
            if (ImGui::MenuItem("C# Script")) { beginCreate(kCreateCSharp, "NewScript"); }
            if (ImGui::MenuItem("Scene")) { beginCreate(kCreateScene, "New Scene"); }
            if (ImGui::MenuItem("Animation Clip")) { beginCreate(kCreateAnim, "New Clip"); }
            if (ImGui::MenuItem("Material")) { beginCreate(kCreateMaterial, "New Material"); }
            if (ImGui::MenuItem("Sound")) { beginCreate(kCreateSound, "New Sound"); }
            if (ImGui::MenuItem("Mixer")) { beginCreate(kCreateMixer, "New Mixer"); }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) {
            ShellExecuteW(nullptr, L"open", current_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // ---- D&D 移動の実行 (M30b)。描画終了後にまとめて行う (directory_iterator 保護) ----
    if (!pendingMoveSrc_.empty()) {
        MoveAssetToFolder(ctx, pendingMoveSrc_, pendingMoveDstDir_);
        pendingMoveSrc_.clear();
        pendingMoveDstDir_.clear();
    }

    // ---- リネームモーダル (M30d) ----
    if (requestRenameModal_) {
        ImGui::OpenPopup("Rename Asset");
        requestRenameModal_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", WideToUtf8(pendingRenamePath_).c_str());
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("Name", createName_, sizeof(createName_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool doRename = ImGui::Button("Rename", ImVec2(90, 0)) || enter;
        ImGui::SameLine();
        const bool cancelRename = ImGui::Button("Cancel", ImVec2(90, 0));
        if (doRename && createName_[0] != '\0') {
            const std::wstring newPath = RenameAsset(ctx, pendingRenamePath_, createName_);
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

    // ---- Import Settings モーダル (M39b): .meta v2 のテクスチャインポート設定 ----
    if (requestImportModal_) {
        ImGui::OpenPopup("Import Settings");
        requestImportModal_ = false;
    }
    if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", WideToUtf8(pendingImportPath_).c_str());
        const char* srgbLabels[] = { "Auto (usage hint)", "sRGB (albedo)", "Linear (data)" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("sRGB", &importEdit_.srgb, srgbLabels, 3);
        bool mips = importEdit_.generateMips != 0;
        if (ImGui::Checkbox("Generate Mips", &mips)) {
            importEdit_.generateMips = mips ? 1 : 0;
        }
        const char* compLabels[] = { "Auto (BC1/BC3)", "None (RGBA8)" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("Cook Compress", &importEdit_.compress, compLabels, 2);
        if (ImGui::Button("Apply", ImVec2(90, 0))) {
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
        if (ImGui::Button("Cancel", ImVec2(90, 0))) {
            pendingImportPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

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
    case kCreateSound:
        CreateSoundAsset(ctx, current_, name);
        break;
    case kCreateMixer:
        CreateMixerAsset(ctx, current_, name);
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

bool AssetBrowserWindow::IsClientPosInPanel(float x, float y) const
{
    return panelRectValid_ && x >= panelMin_[0] && x < panelMax_[0] && y >= panelMin_[1] &&
           y < panelMax_[1];
}

} // namespace mye
