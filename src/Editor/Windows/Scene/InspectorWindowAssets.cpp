// InspectorWindow のアセット選択時の描画 (M40c〜)。テクスチャ / モデルの Import Settings、.mat.json、.sound.json、
// .physmat.json、ナビのエリアのフィルタ、Properties スキーマ、*.fxstack.json のスキーマ固定編集。
// アセット編集は UndoStack の対象外 (既存規約)。エンティティ側の描画は InspectorWindow.cpp、ナビのベイクは InspectorWindowNav.cpp
#include "Editor/Windows/Scene/InspectorWindow.h"

#include <algorithm>
#include <cfloat> // M79 sub-06: DragFloat の上限に FLT_MAX を渡す
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Engine/Asset/FbxLoader.h"   // M90e: LOD 設定の適用でモデルのメッシュを登録し直す
#include "Engine/Engine/Asset/ModelLoader.h" // 同上
#include "Editor/Project/NavAreaNames.h"
#include "Editor/Scene/Selection.h"
#include "Engine/Core/Util/Hash.h" // マテリアルプレビューの同一性キー (M53)
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Scene/Prefab.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/PostFx/FxStackAsset.h"  // M78c: fxstack アセット
#include "Engine/Renderer/Shader/ProjectShaderProperties.h"  // M78c r2: スキーマ駆動 Inspector
#include "Engine/Renderer/Shader/ShaderManager.h"             // M78c r2: ShaderDirs() 呼び出し
#include "Engine/Renderer/RayTracing/RtTypes.h" // kRtReflClassCount (M67)
#include "Engine/Renderer/Device/ReflectionClassJson.h" // reflectionClass の受理規則 (M67h)
#include "Engine/Renderer/Pipeline/RenderTypes.h" // kEmissiveMaxIntensity (M46i)

#include "imgui.h"

#include "fontawesome/IconsFontAwesome6.h"

namespace mye {

void InspectorWindow::DrawAssetInspector(EngineContext& ctx, Selection& selection,
                                         AssetPreviewCache& preview)
{
    namespace fs = std::filesystem;
    const std::wstring path = selection.assetPath;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_AssetGone));
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
        assetLodEdit_ = meta.lod;
        if (type == AssetType::Material) {
            LoadMaterialEdit(ctx, path); // M40d
        }
        if (type == AssetType::Sound) {
            LoadSoundEdit(path); // M45c
        }
        if (type == AssetType::PhysMat) {
            LoadPhysMatEdit(path); // M59a1
        }
        if (type == AssetType::NavFilter) {
            LoadNavFilterEdit(path); // M84c
        }
        if (type == AssetType::FxStack) {
            LoadFxStackEdit(path); // M78c
        }
        if ((type == AssetType::Actor || type == AssetType::Prefab) && ctx.prefabs) {
            // 選択が変わったときだけ登録を試す (エディタ起動後に外から置かれたファイルを拾う)。
            // 毎フレーム LoadFromFile すると不正ファイルで警告ログを撒き続ける
            const uint64_t h = PrefabLibrary::HashForPath(path);
            if (!ctx.prefabs->Contains(h)) {
                ctx.prefabs->LoadFromFile(path);
            }
        }
    }

    if (type == AssetType::Actor || type == AssetType::Prefab) {
        // 構成アセット (M48d): 読み取り専用の要約。中身の編集はミニシーン編集モード (M48k) で
        const PrefabAsset* a =
            ctx.prefabs ? ctx.prefabs->Get(PrefabLibrary::HashForPath(path)) : nullptr;
        if (a) {
            int roots = 0;
            for (const nlohmann::json& it : a->entities) {
                if (!it.contains("parent")) {
                    ++roots;
                }
            }
            ImGui::Text(Tr(StrId::Insp_ComposeSummary), static_cast<int>(a->entities.size()), roots);
            if (roots >= 2) {
                ImGui::TextDisabled("%s", Tr(StrId::Insp_ComposeMultiRoot));
            }
        } else {
            ImGui::TextDisabled("%s", Tr(StrId::Insp_ComposeInvalid));
        }
        return;
    }

    if (type == AssetType::Material) {
        DrawMaterialInspector(ctx, path, preview); // M40d + M53 プレビュー
        return;
    }

    if (type == AssetType::Sound) {
        DrawSoundInspector(ctx, path); // M45c
        return;
    }

    if (type == AssetType::PhysMat) {
        DrawPhysMatInspector(path); // M59a1
        return;
    }

    if (type == AssetType::NavFilter) {
        DrawNavFilterInspector(ctx, path); // M84c
        return;
    }

    if (type == AssetType::FxStack) {
            DrawFxStackInspector(ctx, path); // M78c
        return;
    }

    if (type == AssetType::Audio) {
        // 素のクリップ: 情報表示 + 試聴。編集対象は .sound.json 側 (ここは読み取り専用)
        if (ctx.audio) {
            const AssetID id = ctx.audio->LoadClipFile(path); // 冪等 (--no-audio なら null)
            if (ImGui::Button(Tr(StrId::Insp_Preview), ImVec2(110, 0))) {
                if (!id.IsNull()) {
                    PlayDesc d;
                    d.clip = id;
                    ctx.audio->Play(d);
                } else {
                    MYE_LOG_WARN("audio device is not available (or clip failed to decode)");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr(StrId::Insp_StopAll), ImVec2(90, 0))) {
                ctx.audio->StopAll();
                ctx.audio->StopMusic(kMusicStopFadeSeconds); // BGM は別レーン (M45f)
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
        ImGui::SeparatorText(Tr(StrId::Insp_ImportSettings));
        const char* srgbLabels[] = { "Auto (usage hint)", "sRGB (albedo)", "Linear (data)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo(Tr(StrId::Insp_Srgb), &assetImportEdit_.srgb, srgbLabels, 3);
        bool mips = assetImportEdit_.generateMips != 0;
        if (ImGui::Checkbox(Tr(StrId::Insp_GenerateMips), &mips)) {
            assetImportEdit_.generateMips = mips ? 1 : 0;
        }
        const char* compLabels[] = { "Auto (BC1/BC3)", "None (RGBA8)" };
        ImGui::SetNextItemWidth(200.0f);
        ImGui::Combo(Tr(StrId::Insp_CookCompress), &assetImportEdit_.compress, compLabels, 2);
        if (ImGui::Button(Tr(StrId::Common_Apply), ImVec2(90, 0))) {
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
        if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
            AssetMeta meta;
            AssetDatabase::ReadMeta(path + L".meta", meta);
            assetImportEdit_ = meta.tex;
        }
    }

    if (type == AssetType::Model) {
        // ---- メッシュ LOD (M90e): .meta の "lod"。段数 0 = 段なし (既定) ----
        ImGui::SeparatorText(Tr(StrId::Insp_Lod));
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderInt(Tr(StrId::Insp_LodLevels), &assetLodEdit_.levels, 0, importmeta::ModelLodSettings::kMaxExtraLevels);
        for (int i = 0; i < assetLodEdit_.levels; ++i) {
            const std::string index = std::to_string(i + 1);
            const std::string ratioLabel = std::string(Tr(StrId::Insp_LodRatio)) + " " + index + "##lodRatio" + index;
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat(ratioLabel.c_str(), &assetLodEdit_.ratio[i], 0.01f, 0.95f, "%.2f");
            const std::string sizeLabel = std::string(Tr(StrId::Insp_LodScreenSize)) + " " + index + "##lodSize" + index;
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat(sizeLabel.c_str(), &assetLodEdit_.screenSize[i], 0.0f, 1.0f, "%.2f");
        }
        ImGui::TextDisabled("%s", Tr(StrId::Insp_LodNote));
        if (ImGui::Button(Tr(StrId::Common_Apply), ImVec2(90, 0))) {
            const std::wstring metaPath = path + L".meta";
            AssetDatabase::EnsureMeta(path); // 不在なら生成 (GUID 確定)
            AssetMeta meta;
            if (AssetDatabase::ReadMeta(metaPath, meta)) {
                meta.type = AssetType::Model;
                assetLodEdit_.Normalize();
                meta.lod = assetLodEdit_;
                AssetDatabase::WriteMeta(metaPath, meta);
                // 登録済みのメッシュを段つきで登録し直す (AssetID 不変 = 参照側の再解決不要)。
                // .meta はモデル本体の更新時刻に出ないのでファイル監視は反応しない
                std::wstring ext = fs::path(path).extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (ctx.resources != nullptr && ctx.shaders != nullptr) {
                    if (ext == L".fbx") {
                        FbxLoader::ReloadMeshes(*ctx.resources, *ctx.shaders, path);
                    } else {
                        ModelLoader::ReloadMeshes(*ctx.resources, *ctx.shaders, path);
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
            AssetMeta meta;
            AssetDatabase::ReadMeta(path + L".meta", meta);
            assetLodEdit_ = meta.lod;
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
    matEdit_.emissive = root.value("emissive", 0.0f); // M46i (欠損 = 発光なし)
    // M67: 反射クラス。ParseMaterialJson と**同じ関数**を呼ぶ (規則は 1 本、M67h)
    // — ここで拾い方がずれると「Inspector に出る値」と「描画に効く値」が食い違う
    matEdit_.reflectionClass = ParseReflectionClassJson(root);
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
    // M79 sub-06: 視錐台余白 / 両面描画。負値は 0 に丸める (MaterialLibrary::ParseMaterialJson と
    // 同じ規則。ここで丸めないと Inspector に負値が残ったまま保存できてしまう)
    matEdit_.boundsPadding = (std::max)(0.0f, root.value("boundsPadding", 0.0f));
    matEdit_.doubleSided = root.value("doubleSided", false);
    // M79 sub-04: shader が "*.surface" のときの Properties。型ごとの復号にスキーマが要るので
    // 先に matEdit_.shader (直前で読んだ) のスキーマを引く (spec §4.2、Tex2D の数値 GUID)
    {
        const PropertyParseResult& schema =
            GetOrFetchPropertySchema(ctx, matEdit_.shader, matSchemaCache_);
        const std::string propsText = (root.contains("properties") && root["properties"].is_object())
            ? root["properties"].dump()
            : std::string("{}");
        DecodeMaterialProperties(propsText, &schema, matEdit_.properties);
    }
    matEdit_.valid = true;
}

std::string InspectorWindow::MaterialEditToJson(const std::wstring& path) const
{
    namespace fs = std::filesystem;
    nlohmann::json root;
    root["engine"] = "MyEngine";
    root["material"] = 1;
    root["name"] = matEdit_.name.empty() ? WideToUtf8(fs::path(path).stem().stem().wstring())
                                         : matEdit_.name;
    root["shader"] = matEdit_.shader;
    root["baseColor"] = { matEdit_.baseColor[0], matEdit_.baseColor[1], matEdit_.baseColor[2],
                          matEdit_.baseColor[3] };
    root["metallic"] = matEdit_.metallic;
    root["roughness"] = matEdit_.roughness;
    root["emissive"] = matEdit_.emissive;                  // M46i
    root["reflectionClass"] = matEdit_.reflectionClass;    // M67
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
    // M79 sub-06: サーフェスでなくても書く (シェーダを戻したときに値を失わない、spec §4.2)。
    // 既定値 (0 / false) のときも明示して書く — 従来の .mat.json に無かったキーが増えるが、
    // 「欠損 = 既定」の読み側規則があるので既存互換は壊れない
    root["boundsPadding"] = matEdit_.boundsPadding;
    root["doubleSided"] = matEdit_.doubleSided;
    // M79 sub-04: shader が "*.surface" のときの Properties。既知キー・未知キーとも
    // matEdit_.properties に入っている値をそのまま書き戻す (§4.1 「スキーマに無いキーも保持」)。
    // 空なら書かない (forward_lit 等の既存 .mat.json へ空の "properties":{} を増やさないため)
    if (!matEdit_.properties.empty()) {
        try {
            root["properties"] = nlohmann::json::parse(EncodeMaterialProperties(matEdit_.properties));
        } catch (const nlohmann::json::exception&) {
        }
    }
    return root.dump(2);
}

void InspectorWindow::DrawMaterialInspector(EngineContext& ctx, const std::wstring& path,
                                            AssetPreviewCache& preview)
{
    namespace fs = std::filesystem;
    if (!matEdit_.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_MaterialFailed));
        return;
    }

    // ---- ライブプレビュー (M53) ----
    // 保存する JSON をそのまま Material に組み直して描く = 「保存したらこう見える」が保証される。
    // JSON 本文のハッシュを同一性キーにしているので、フィールドを足しても取りこぼしが出ない
    const std::string matJson = MaterialEditToJson(path);
    const uint64_t jsonHash = HashStr(matJson);
    if (jsonHash != matPreviewHash_) {
        Material built;
        if (MaterialLibrary::MaterialFromJsonText(matJson, ctx.resources->textures, ctx.assetsRoot,
                                                  built)) {
            // M79 sub-04: プレビューは RegisterAnonymous 経由で登録され MaterialLibrary の
            // サーフェス横テーブル (surfaceSources_、LoadFromFile 専用) には乗らない。
            // このまま渡すと ForwardPath が「サーフェスでない」と判定して mat->shader の
            // 通常プログラム解決に落ち、"*.surface" は Load() されていないため無描画になる。
            // spec §4.3 の許容どおり forward_lit 表示へ固定する (マゼンタにはしない)
            if (matEdit_.shader != "forward_lit") {
                built.shader = AssetID{ HashStr("forward_lit") };
            }
            matPreviewMat_ = built;
            matPreviewHash_ = jsonHash;
        }
    }
    if (matPreviewHash_ != 0) {
        const auto shape = static_cast<PreviewShape>(matPreviewShape_);
        ID3D11ShaderResourceView* srv =
            preview.GetOrRequestMaterial(ctx, matPreviewMat_, shape, matPreviewHash_);
        const float side = static_cast<float>(AssetPreviewCache::kPreviewSize);
        if (srv) {
            ImGui::Image(reinterpret_cast<ImTextureID>(srv), ImVec2(side, side));
        } else {
            ImGui::Dummy(ImVec2(side, side)); // 生成待ち (次フレームに絵が入る)
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        const char* shapeItems[] = { Tr(StrId::Insp_ShapeSphere), Tr(StrId::Insp_ShapeCube),
                                     Tr(StrId::Insp_ShapePlane) };
        static_assert(static_cast<int>(PreviewShape::Count) == 3,
                      "shapeItems は PreviewShape と同数にすること");
        ImGui::SetNextItemWidth(110.0f);
        ImGui::Combo(Tr(StrId::Insp_PreviewShape), &matPreviewShape_, shapeItems, 3);
        ImGui::EndGroup();
        // 注記は画像の**下**に全幅で折り返す。右に置くと Inspector の幅で右端が切れる
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Tr(StrId::Insp_PreviewNote));
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText(Tr(StrId::Insp_Material));

    // ---- シェーダ選択 (M79 sub-04): forward_lit + 索引済み *.surface 短名 (昇順) ----
    {
        std::vector<std::string> items = { "forward_lit" };
        if (ctx.shaders) {
            const std::vector<std::string> surfaces = ctx.shaders->ProjectShaderNames(".surface");
            items.insert(items.end(), surfaces.begin(), surfaces.end());
        }
        int curIndex = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i] == matEdit_.shader) {
                curIndex = static_cast<int>(i);
                break;
            }
        }
        // 索引に無い値 (削除済み / リネーム済みシェーダを指す古い .mat.json) は
        // 先頭に読み取り専用のまま差し込む — 黙って別の値へ挿げ替えない
        if (curIndex < 0 && !matEdit_.shader.empty()) {
            items.insert(items.begin(), matEdit_.shader);
            curIndex = 0;
        }
        std::vector<const char*> itemPtrs;
        itemPtrs.reserve(items.size());
        for (const std::string& s : items) {
            itemPtrs.push_back(s.c_str());
        }
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo(Tr(StrId::Insp_MatShader), &curIndex, itemPtrs.data(),
                        static_cast<int>(itemPtrs.size()))
            && curIndex >= 0 && curIndex < static_cast<int>(items.size())
            && items[curIndex] != matEdit_.shader) {
            // シェーダを切り替えても Properties は保持する (spec §4.1「シェーダを戻したとき
            // 値が残る」契約)。型が合わないキーは DrawPropertiesEditor の std::get_if が
            // 表示時に既定値へ安全に逃がす (round 1 の properties.clear() は仕様違反として却下)
            ApplyMaterialShaderSelection(matEdit_.shader, items[curIndex]);
        }

        // ---- 失敗バナー (sub-01 の ShaderManager::LoadSurface/GetSurface から取得) ----
        if (ctx.shaders && matEdit_.shader != "forward_lit") {
            const AssetID progId = ctx.shaders->LoadSurface(matEdit_.shader);
            const SurfaceProgram* prog = ctx.shaders->GetSurface(progId);
            if (!prog || !prog->valid) {
                const std::string errFirstLine =
                    prog && !prog->errorMessage.empty()
                        ? prog->errorMessage.substr(0, prog->errorMessage.find('\n'))
                        : std::string("shader not found");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped(Tr(StrId::Insp_MatShaderFailed), matEdit_.shader.c_str(),
                                   errFirstLine.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::ColorEdit4("baseColor", matEdit_.baseColor);
    ImGui::SliderFloat(Tr(StrId::Mat_Metallic), &matEdit_.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Mat_Roughness), &matEdit_.roughness, 0.0f, 1.0f);
    // M46i: 自己発光。放射輝度 = baseColor * emissive。Deferred は G-Buffer へ
    // kEmissiveMaxIntensity 正規化で詰めるので、それを超える値は頭打ちになる
    ImGui::SliderFloat(Tr(StrId::Mat_Emissive), &matEdit_.emissive, 0.0f,
                       static_cast<float>(kEmissiveMaxIntensity));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Insp_TipEmissive));
    }
    // M67: 反射に映るときの品質クラス。**このマテリアルの面が反射像に写るときの扱い**で、
    // このマテリアルが「何を映すか」ではない (Inspector で最も誤解されやすい点なので
    // ツールチップで明示する)
    {
        const char* classItems[] = { Tr(StrId::ReflClass_Hero), Tr(StrId::ReflClass_Character),
                                     Tr(StrId::ReflClass_Vehicle), Tr(StrId::ReflClass_Prop),
                                     Tr(StrId::ReflClass_Default) };
        static_assert(sizeof(classItems) / sizeof(classItems[0]) == kRtReflClassCount,
                      "classItems は kRtReflClassCount と同数にすること");
        ImGui::Combo(Tr(StrId::Mat_ReflClass), &matEdit_.reflectionClass, classItems,
                     kRtReflClassCount);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_TipReflClass));
        }
    }
    ImGui::Checkbox(Tr(StrId::Insp_Transparent), &matEdit_.transparent);

    // テクスチャピッカー (GUID 参照、M39a)。assets 配下の画像をサムネ付きで列挙
    auto texPicker = [&](const char* label, uint64_t& guidRef) {
        ImGui::PushID(label);
        std::string cur = Tr(StrId::Insp_NoneItem);
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
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
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

    // ---- 境界余白 / 両面描画 (M79 sub-06): サーフェスシェーダ選択時のみ (spec §4.3) ----
    if (matEdit_.shader != "forward_lit") {
        ImGui::DragFloat(Tr(StrId::Insp_MatBoundsPadding), &matEdit_.boundsPadding, 0.05f, 0.0f,
                         FLT_MAX, "%.2f");
        if (matEdit_.boundsPadding < 0.0f) {
            matEdit_.boundsPadding = 0.0f;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Tr(StrId::Insp_TipBoundsPadding));
        }
        ImGui::Checkbox(Tr(StrId::Insp_MatDoubleSided), &matEdit_.doubleSided);
    }

    // ---- Properties (M79 sub-04): サーフェスシェーダ選択時のみ ----
    if (matEdit_.shader != "forward_lit") {
        ImGui::Spacing();
        ImGui::SeparatorText(Tr(StrId::Insp_MatProperties));
        const PropertyParseResult& schema =
            GetOrFetchPropertySchema(ctx, matEdit_.shader, matSchemaCache_);
        // マテリアルの Tex2D は Material.texture/normalMap と同じ 10 進 GUID 文字列 (spec §4.2)
        DrawPropertiesEditor(ctx, schema, matEdit_.properties, "mat", [](uint64_t guid) {
            return std::to_string(guid);
        });
    }

    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        std::ofstream out(std::filesystem::path(path), std::ios::binary);
        if (out) {
            out << matJson; // プレビューが描いたのと**同じ本文**
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
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
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
        ImGui::TextDisabled("%s", Tr(StrId::Insp_SoundFailed));
        return;
    }

    // ---- 試聴 (先頭バリエーション・揺らぎ無しで固定 = 何を聴いているか分かる) ----
    if (ImGui::Button(Tr(StrId::Insp_Preview), ImVec2(110, 0))) {
        if (ctx.audio) {
            PreviewSound(*ctx.audio, soundEdit_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_StopAll), ImVec2(90, 0))) {
        if (ctx.audio) {
            ctx.audio->StopAll();
            ctx.audio->StopMusic(kMusicStopFadeSeconds); // BGM は別レーン (M45f)
        }
    }
    if (soundEdit_.stream) {
        // BGM は voice プールに載らないので、試聴の挙動が SE と違うことを明示する
        ImGui::TextDisabled("%s", Tr(StrId::Insp_StreamNote));
    }
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PreviewNote));

    // ---- バリエーション ----
    ImGui::SeparatorText(Tr(StrId::Insp_Variations));
    int removeAt = -1;
    for (size_t i = 0; i < soundEdit_.variations.size(); ++i) {
        SoundVariation& v = soundEdit_.variations[i];
        ImGui::PushID(static_cast<int>(i));
        std::string cur = Tr(StrId::Insp_NoneItem);
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
            if (ImGui::Selectable(Tr(StrId::Insp_NoneItem))) {
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
        ImGui::DragInt(Tr(StrId::Insp_Weight), &v.weight, 0.1f, 0, 100);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            removeAt = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        soundEdit_.variations.erase(soundEdit_.variations.begin() + removeAt);
    }
    if (ImGui::Button(Tr(StrId::Insp_AddVariation))) {
        soundEdit_.variations.push_back(SoundVariation{});
    }

    // ---- 2D 再生パラメータ ----
    ImGui::SeparatorText(Tr(StrId::Insp_Playback));
    ImGui::SliderFloat(Tr(StrId::Insp_Volume), &soundEdit_.volume, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_VolumeRandom), &soundEdit_.volumeRandom, 0.0f, 1.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_Pitch), &soundEdit_.pitch, 1.0f / AudioSystem::kMaxFreqRatio,
                       AudioSystem::kMaxFreqRatio);
    ImGui::SliderFloat(Tr(StrId::Insp_PitchRandom), &soundEdit_.pitchRandom, 0.0f, 1.0f);
    ImGui::Checkbox(Tr(StrId::Insp_Loop), &soundEdit_.loop);
    ImGui::SameLine();
    ImGui::Checkbox(Tr(StrId::Insp_StreamBgm), &soundEdit_.stream);
    // バス候補は**実際に張られているミキサー**から採る (バスはデータ駆動、M45d)。
    // 保存は名前なので、未知のバス名は既定バスへ落ちるだけで値自体は壊さない
    if (ctx.audio != nullptr) {
        const int current = ctx.audio->FindBus(soundEdit_.bus.c_str());
        const char* preview = current >= 0 ? ctx.audio->BusName(current) : soundEdit_.bus.c_str();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(Tr(StrId::Insp_Bus), preview)) {
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
    ImGui::DragInt(Tr(StrId::Insp_Priority), &soundEdit_.priority, 1.0f, 0, 255);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_MaxInstances), &soundEdit_.maxInstances, 0.1f, 0, 64);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PriorityNote));

    // ---- 3D (M45e: X3DAudio が実際にこの値で定位する) ----
    ImGui::SeparatorText(Tr(StrId::Insp_3D));
    ImGui::SliderFloat(Tr(StrId::Insp_SpatialBlend), &soundEdit_.spatialBlend, 0.0f, 1.0f);
    ImGui::DragFloat(Tr(StrId::Insp_MinDistance), &soundEdit_.minDistance, 0.05f, 0.01f, 1000.0f);
    ImGui::DragFloat(Tr(StrId::Insp_MaxDistance), &soundEdit_.maxDistance, 0.5f, 0.02f, 10000.0f);
    {
        int rolloff = static_cast<int>(soundEdit_.rolloff);
        const char* rolloffNames[] = { "Logarithmic", "Linear", "Inverse" };
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo(Tr(StrId::Insp_Rolloff), &rolloff, rolloffNames, 3)) {
            soundEdit_.rolloff = static_cast<SoundRolloff>(rolloff);
        }
    }
    ImGui::SliderFloat(Tr(StrId::Insp_Doppler), &soundEdit_.dopplerScale, 0.0f, 5.0f);
    ImGui::SliderFloat(Tr(StrId::Insp_ReverbSend), &soundEdit_.reverbSend, 0.0f, 1.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_SpatialNote));
    ImGui::TextDisabled("%s", Tr(StrId::Insp_AttenNote));

    // ---- ループ点 (M45f。stream + loop のときだけ意味を持つ) ----
    ImGui::SeparatorText(Tr(StrId::Insp_LoopPoints));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_LoopStart), &soundEdit_.loopStartSample, 8.0f, 0, 1 << 30);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt(Tr(StrId::Insp_LoopEnd), &soundEdit_.loopEndSample, 8.0f, 0, 1 << 30);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_LoopEndNote1));
    ImGui::TextDisabled("%s", Tr(StrId::Insp_LoopEndNote2));

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
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
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadSoundEdit(path);
    }
}

void InspectorWindow::LoadPhysMatEdit(const std::wstring& path)
{
    physMatEdit_ = PhysMat{};
    physMatEditValid_ = false;
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
    physMatEditValid_ = PhysMatLibrary::FromJson(root, physMatEdit_); // 読み値は Sanitize 済み
}

void InspectorWindow::DrawPhysMatInspector(const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!physMatEditValid_) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_PhysMatFailed));
        return;
    }

    // ドラッグの min/max は Sanitize と同じ檻 (直接入力のはみ出しは Save 時の Sanitize が
    // 最終防波堤)。ここで編集した値が sim に効き始めるのは M59a2 の Collider 割り当てから
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDensity), &physMatEdit_.density, 10.0f, 0.001f, 1.0e6f,
                     "%.1f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmStaticFriction), &physMatEdit_.staticFriction, 0.01f, 0.0f,
                     100.0f, "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDynamicFriction), &physMatEdit_.dynamicFriction, 0.01f, 0.0f,
                     100.0f, "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRestitution), &physMatEdit_.restitution, 0.005f, 0.0f, 1.0f,
                     "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRollingResistance), &physMatEdit_.rollingResistance, 0.001f,
                     0.0f, 10.0f, "%.4f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmDragCoefficient), &physMatEdit_.dragCoefficient, 0.01f, 0.0f,
                     100.0f, "%.3f");
    // M60d: 粘着力 [N]。接触が引っ張る側へ耐えられる上限で、結合則は min (弱いほうが勝つ)
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmAdhesion), &physMatEdit_.adhesion, 0.5f, 0.0f, 1.0e6f,
                     "%.2f");
    // M76g: Deep-Modal の音響材質 4 フィールド (spec §4.2)。Sanitize と同じ檻
    // (E [0,1e13] / ν [0,0.49] / α [0,1e4] / β [0,1e-2])。E=0 は「参照材質のまま」(σ1=1) の意味を持つ
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmYoungsModulus), &physMatEdit_.youngsModulus, 1.0e8f, 0.0f,
                     1.0e13f, "%.3e");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmPoissonRatio), &physMatEdit_.poissonRatio, 0.005f, 0.0f,
                     0.49f, "%.3f");
    // ★ν は BuildModes が読まない (spec §2 #4)。編集自体は許すが、効かないことを
    //   その場で伝えないと「変えたのに音が変わらない」で不具合報告になる
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Insp_PmPoissonRatioTip));
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRayleighAlpha), &physMatEdit_.rayleighAlpha, 0.05f, 0.0f,
                     1.0e4f, "%.3f");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(Tr(StrId::Insp_PmRayleighBeta), &physMatEdit_.rayleighBeta, 1.0e-5f, 0.0f,
                     1.0e-2f, "%.3e");
    ImGui::TextDisabled("%s", Tr(StrId::Insp_PhysMatNote));

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        if (physMatEdit_.name.empty()) {
            // "steel.physmat.json" → "steel" (stem を 2 回剥がす)
            physMatEdit_.name = WideToUtf8(fs::path(path).stem().stem().wstring());
        }
        PhysMatLibrary::Sanitize(physMatEdit_); // 手入力の NaN/範囲外をファイルへ焼かない
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << PhysMatLibrary::ToJson(physMatEdit_).dump(2);
            out.close();
            // 即時反映: 同一 GUID のまま再ロード (参照側は GUID なので再解決不要)
            if (PhysMatLibrary* pm = physmat::Library()) {
                pm->LoadFromFile(path);
            }
            MYE_LOG_INFO("physmat saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("could not write physmat: %s", WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadPhysMatEdit(path);
    }
}

void InspectorWindow::LoadNavFilterEdit(const std::wstring& path)
{
    navFilterEdit_ = NavAreaFilter{};
    navFilterEditValid_ = false;
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
    navFilterEditValid_ = NavFilterLibrary::FromJson(root, navFilterEdit_); // 読み値は Sanitize 済み
}

// M84c: エリアごとに「上書きするか・コスト・通らないか」の表。エリア 1 (歩行不可) はもともと通れないので出さない
void InspectorWindow::DrawNavFilterInspector(EngineContext& ctx, const std::wstring& path)
{
    namespace fs = std::filesystem;
    if (!navFilterEditValid_) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_NavFilterFailed));
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr(StrId::Insp_NavFilterHint));
    ImGui::PopTextWrapPos();
    NavAreaNames& areaNames = NavAreaNames::Get();
    areaNames.Load(ctx.assetsRoot);
    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##navFilterAreas", 4, kTableFlags)) {
        ImGui::TableSetupColumn(Tr(StrId::Insp_NavFilterColArea));
        ImGui::TableSetupColumn(Tr(StrId::Insp_NavFilterColOverride), ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn(Tr(StrId::Insp_NavFilterColCost));
        ImGui::TableSetupColumn(Tr(StrId::Insp_NavFilterColExclude), ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NavAreaFilter::kAreaCount; ++i) {
            if (i == kNavAreaNotWalkable) {
                continue;
            }
            float& cost = navFilterEdit_.areaCosts[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(areaNames.Name(i));
            ImGui::TableNextColumn();
            bool overridden = cost > 0.0f;
            if (ImGui::Checkbox("##override", &overridden)) {
                cost = overridden ? 1.0f : 0.0f; // 0 = Surface のコストのまま
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!overridden);
            ImGui::SetNextItemWidth(-FLT_MIN);
            float shown = overridden ? cost : 1.0f;
            if (ImGui::DragFloat("##cost", &shown, 0.1f, 1.0f, 1000.0f, "%.1f") && overridden) {
                cost = (std::max)(1.0f, shown);
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            bool excluded = ((navFilterEdit_.excludedAreas >> i) & 1u) != 0u;
            if (ImGui::Checkbox("##exclude", &excluded)) {
                navFilterEdit_.excludedAreas ^= 1u << i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Common_Save), ImVec2(90, 0))) {
        if (navFilterEdit_.name.empty()) {
            navFilterEdit_.name = WideToUtf8(fs::path(path).stem().stem().wstring()); // "x.navfilter.json" -> "x"
        }
        NavFilterLibrary::Sanitize(navFilterEdit_);
        std::ofstream out(fs::path(path), std::ios::binary);
        if (out) {
            out << NavFilterLibrary::ToJson(navFilterEdit_).dump(2);
            out.close();
            // 同じ GUID のまま読み直す (参照側は GUID なので次の tick から効く)
            if (NavFilterLibrary* nf = navfilter::Library()) {
                nf->LoadFromFile(path);
            }
            MYE_LOG_INFO("navfilter saved: %s", WideToUtf8(path).c_str());
        } else {
            MYE_LOG_ERROR("could not write navfilter: %s", WideToUtf8(path).c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadNavFilterEdit(path);
    }
}

// ---------------------------------------------------------------------------
// M79 sub-04: Properties スキーマ取得・ウィジェット描画の共通化 (fxstack / マテリアル)
// ---------------------------------------------------------------------------

const PropertyParseResult& InspectorWindow::GetOrFetchPropertySchema(
    EngineContext& ctx, const std::string& shaderName, PropertySchemaCache& cache)
{
    // shaders 未設定 / シェーダ未選択はキャッシュせず毎回空スキーマ (元々パースを伴わない)
    static const PropertyParseResult kEmpty = ParseProperties(std::string_view{});
    if (!ctx.shaders || shaderName.empty()) {
        return kEmpty;
    }
    // M79d-fix (review-1 #5): shaderName だけでなく解決先ファイルの更新時刻もキャッシュ鍵にする。
    // ホットリロードで HLSL の Properties ブロックが変わっても、エディタ再起動まで
    // 古いスキーマを返し続けていた (PropertySchemaCache 側で無効化を担う)
    return cache.GetOrFetch(shaderName, ctx.shaders->ResolveShaderPath(shaderName),
                            [&]() { return ctx.shaders->FetchPropertySchema(shaderName); });
}

void InspectorWindow::DrawPropertiesEditor(
    EngineContext& ctx, const PropertyParseResult& schema,
    std::unordered_map<std::string, PropValue>& values, const char* idScope,
    const std::function<std::string(uint64_t)>& texAssetEncode)
{
    namespace fs = std::filesystem;
    ImGui::PushID(idScope);
    const bool hasSchema = schema.ok && !schema.properties.empty();

    if (hasSchema) {
        // スキーマ順にウィジェット描画
        for (const auto& prop : schema.properties) {
            // [HideInInspector] はスキップ
            if (prop.attr.hideInInspector) {
                continue;
            }
            // [Header(name)] 行: name.empty() && cbOffset == -1
            if (prop.name.empty() && prop.cbOffset == -1) {
                if (!prop.attr.header.empty()) {
                    ImGui::SeparatorText(prop.attr.header.c_str());
                }
                continue;
            }

            ImGui::PushID(prop.name.c_str());
            const char* label =
                prop.displayName.empty() ? prop.name.c_str() : prop.displayName.c_str();

            switch (prop.type) {
            case PropType::Float: {
                float v = prop.defaultFloat;
                auto it = values.find(prop.name);
                if (it != values.end()) {
                    if (const float* fp = std::get_if<float>(&it->second)) {
                        v = *fp;
                    }
                }
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragFloat(label, &v, 0.01f)) {
                    values[prop.name] = v;
                }
                break;
            }
            case PropType::Range: {
                float v = prop.defaultFloat;
                auto it = values.find(prop.name);
                if (it != values.end()) {
                    if (const float* fp = std::get_if<float>(&it->second)) {
                        v = *fp;
                    }
                }
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat(label, &v, prop.attr.rangeMin, prop.attr.rangeMax)) {
                    values[prop.name] = v;
                }
                break;
            }
            case PropType::Color: {
                using Vec4 = std::array<float, 4>;
                Vec4 v = prop.defaultVec4;
                auto it = values.find(prop.name);
                if (it != values.end()) {
                    if (const Vec4* vp = std::get_if<Vec4>(&it->second)) {
                        v = *vp;
                    }
                }
                ImGuiColorEditFlags flags =
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB;
                if (prop.attr.isHDR) {
                    flags |= ImGuiColorEditFlags_HDR;
                }
                if (ImGui::ColorEdit4(label, v.data(), flags)) {
                    values[prop.name] = v;
                }
                break;
            }
            case PropType::Vector: {
                using Vec4 = std::array<float, 4>;
                Vec4 v = prop.defaultVec4;
                auto it = values.find(prop.name);
                if (it != values.end()) {
                    if (const Vec4* vp = std::get_if<Vec4>(&it->second)) {
                        v = *vp;
                    }
                }
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::DragFloat4(label, v.data(), 0.01f)) {
                    values[prop.name] = v;
                }
                break;
            }
            case PropType::Tex2D: {
                // 現在値 (string: ビルトイン名 / GUID の文字列表現)
                std::string cur = prop.defaultTex.empty() ? "white" : prop.defaultTex;
                auto it = values.find(prop.name);
                if (it != values.end()) {
                    if (const std::string* sp = std::get_if<std::string>(&it->second)) {
                        cur = *sp;
                    }
                }

                // ラベル + ピッカーボタン (mat の texPicker 流儀)
                ImGui::TextUnformatted(label);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
                const std::string popId = std::string("##tex_") + prop.name;
                if (ImGui::Button(cur.c_str(), ImVec2(-1, 0))) {
                    ImGui::OpenPopup(popId.c_str());
                }
                if (ImGui::BeginPopup(popId.c_str())) {
                    // ビルトイン名
                    for (const char* bn : { "white", "black", "gray", "bump" }) {
                        if (ImGui::Selectable(bn)) {
                            values[prop.name] = std::string(bn);
                        }
                    }
                    ImGui::Separator();
                    // プロジェクト assets からテクスチャを列挙
                    if (!ctx.assetsRoot.empty()) {
                        std::error_code ec2;
                        for (const auto& e :
                             fs::recursive_directory_iterator(ctx.assetsRoot, ec2)) {
                            if (!e.is_regular_file(ec2)) {
                                continue;
                            }
                            const std::wstring wp = e.path().wstring();
                            if (AssetDatabase::IsMetaPath(wp)
                                || AssetDatabase::ClassifyPath(wp) != AssetType::Texture) {
                                continue;
                            }
                            ImGui::PushID(WideToUtf8(wp).c_str());
                            const std::string rel =
                                WideToUtf8(fs::relative(e.path(), ctx.assetsRoot, ec2).wstring());
                            if (ImGui::Selectable(rel.c_str())) {
                                const uint64_t guid = AssetDatabase::EnsureMeta(wp);
                                values[prop.name] = texAssetEncode(guid);
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndPopup();
                }
                break;
            }
            } // switch

            ImGui::PopID();
        }
    } else if (!values.empty()) {
        // スキーマ未取得の場合はフォールバック: JSON キー列挙 (シェーダ不在・パース失敗)
        std::vector<std::string> keys;
        keys.reserve(values.size());
        for (const auto& kv : values) {
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        for (const std::string& key : keys) {
            PropValue& val = values[key];
            ImGui::PushID(key.c_str());
            if (std::holds_alternative<float>(val)) {
                float v = std::get<float>(val);
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragFloat(key.c_str(), &v, 0.01f)) {
                    val = v;
                }
            } else if (std::holds_alternative<std::array<float, 4>>(val)) {
                auto& arr = std::get<std::array<float, 4>>(val);
                ImGui::SetNextItemWidth(200.0f);
                ImGui::ColorEdit4(key.c_str(), arr.data(),
                                 ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
            } else if (std::holds_alternative<std::string>(val)) {
                std::string sv = std::get<std::string>(val);
                ImGui::TextUnformatted(key.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("[tex] %s", sv.c_str());
            }
            ImGui::PopID();
        }
    } else {
        ImGui::TextDisabled("(no properties)");
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// M78c: fxstack アセットインスペクタ
// ---------------------------------------------------------------------------

void InspectorWindow::LoadFxStackEdit(const std::wstring& path)
{
    fxstackEdit_       = FxStackAsset{};
    fxstackEditState_  = FxStackEditState{};
    std::string err;
    fxstackEditState_.valid = LoadFxStack(path, fxstackEdit_, &err);
    if (!fxstackEditState_.valid) {
        MYE_LOG_WARN("InspectorWindow: fxstack ロード失敗 (%s): %s",
                      WideToUtf8(path).c_str(), err.c_str());
    }
}

void InspectorWindow::DrawFxStackInspector(EngineContext& ctx, const std::wstring& path)
{
    namespace fs = std::filesystem;

    if (!fxstackEditState_.valid) {
        ImGui::TextDisabled("%s", Tr(StrId::Insp_FxStackFailed));
        ImGui::Separator();
        if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
            LoadFxStackEdit(path);
        }
        return;
    }

    // ---- パス一覧 ----
    ImGui::SeparatorText(Tr(StrId::Insp_FxStackPasses));

    const int n = static_cast<int>(fxstackEdit_.passes.size());

    // 選択インデックスの範囲クランプ
    if (fxstackEditState_.selectedPass >= n) {
        fxstackEditState_.selectedPass = n > 0 ? n - 1 : 0;
    }

    if (n == 0) {
        ImGui::TextDisabled("(no passes)");
    } else {
        for (int i = 0; i < n; ++i) {
            FxStackEntry& entry = fxstackEdit_.passes[i];

            ImGui::PushID(i);
            // 選択ハイライト
            const bool selected = (i == fxstackEditState_.selectedPass);
            if (ImGui::Selectable("##sel", selected,
                                  ImGuiSelectableFlags_SpanAllColumns,
                                  ImVec2(0, 0))) {
                fxstackEditState_.selectedPass = i;
            }
            ImGui::SameLine();

            // enabled チェックボックス
            bool en = entry.enabled;
            if (ImGui::Checkbox("##en", &en)) {
                entry.enabled = en;
            }
            ImGui::SameLine();

            // 種別ラベル
            const char* kindLabel = (entry.kind == FxStackKind::Compute)
                ? Tr(StrId::Insp_FxStackKindCs)
                : Tr(StrId::Insp_FxStackKindPost);
            ImGui::TextDisabled("[%s]", kindLabel);
            ImGui::SameLine();

            // シェーダ名
            ImGui::TextUnformatted(entry.shader.c_str());
            ImGui::SameLine();

            // 挿入点 / dispatchPoint
            if (entry.kind == FxStackKind::Post) {
                ImGui::TextDisabled("%s  p=%d",
                    InsertionToString(entry.insertion), entry.priority);
            } else {
                ImGui::TextDisabled("%s  p=%d",
                    entry.dispatchPoint.c_str(), entry.priority);
            }

            ImGui::PopID();
        }
    }

    // ---- 選択パスの Properties 編集 (スキーマ駆動 M78c round 2、共通化は M79 sub-04) ----
    if (n > 0 && fxstackEditState_.selectedPass < n) {
        ImGui::Spacing();
        ImGui::SeparatorText(Tr(StrId::Insp_FxStackProperties));

        FxStackEntry& sel = fxstackEdit_.passes[fxstackEditState_.selectedPass];
        const PropertyParseResult& schema =
            GetOrFetchPropertySchema(ctx, sel.shader, fxstackEditState_.schemaCache);
        // fxstack の Tex2D は 16 進 GUID 文字列で書く (旧来の規約。マテリアルの 10 進とは別)
        DrawPropertiesEditor(ctx, schema, sel.properties, "fx", [](uint64_t guid) {
            char hex[20];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(guid));
            return std::string(hex);
        });
    }

    // ---- 保存ステータス表示 ----
    if (!fxstackEditState_.saveStatus.empty()) {
        ImGui::TextDisabled("%s", fxstackEditState_.saveStatus.c_str());
    }

    // ---- 保存 / 再読み込みボタン ----
    ImGui::Separator();
    if (ImGui::Button(Tr(StrId::Insp_FxStackSave), ImVec2(90, 0))) {
        std::string errSave;
        if (SaveFxStack(path, fxstackEdit_, &errSave)) {
            fxstackEditState_.saveStatus = Tr(StrId::Insp_FxStackSaveOk);
            MYE_LOG_INFO("fxstack saved: %s", WideToUtf8(path).c_str());
        } else {
            fxstackEditState_.saveStatus = std::string(Tr(StrId::Insp_FxStackSaveFail))
                                           + " " + errSave;
            MYE_LOG_ERROR("fxstack save failed: %s - %s",
                           WideToUtf8(path).c_str(), errSave.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Insp_Revert), ImVec2(90, 0))) {
        LoadFxStackEdit(path);
        fxstackEditState_.saveStatus.clear();
    }
}

} // namespace mye
