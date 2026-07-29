#include "Engine/Engine/HotReload/ReloadHub.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Prefab.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/SceneSerializer.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

std::wstring ExtensionLower(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    return (dot == std::wstring::npos) ? L"" : path.substr(dot); // path は正規化済み (小文字)
}

} // namespace

bool ReloadHub::Init(ShaderManager* shaders, RenderResources* resources, Scene* scene,
                     PrefabLibrary* prefabs, AnimationLibrary* anims, const std::wstring& assetsRoot)
{
    shaders_ = shaders;
    resources_ = resources;
    scene_ = scene;
    prefabs_ = prefabs;
    anims_ = anims;
    assetsRoot_ = assetsRoot;
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsRoot, ec)) {
        MYE_LOG_WARN("[reload] assets root not found, hot reload disabled");
        return false;
    }
    // エンジン組込みシェーダも監視する (2 ルート化でプロジェクト assets\ の外にあるため)。
    // レガシー起動では assets\shaders が assetsRoot 配下 = watcher_ が既に拾うので張らない
    const std::wstring engineShaders = FindEngineShaderDir();
    if (!engineShaders.empty()) {
        const std::wstring engKey = NormalizePathKey(engineShaders);
        const std::wstring rootKey = NormalizePathKey(assetsRoot);
        const bool underAssets = engKey.size() > rootKey.size()
            && engKey.compare(0, rootKey.size(), rootKey) == 0 && engKey[rootKey.size()] == L'\\';
        if (!underAssets && engineShaderWatcher_.Start(engineShaders)) {
            MYE_LOG_INFO("[reload] watching engine shaders: %s",
                         WideToUtf8(engineShaders).c_str());
        }
    }
    return watcher_.Start(assetsRoot);
}

void ReloadHub::Shutdown()
{
    watcher_.Stop();
    engineShaderWatcher_.Stop();
}

void ReloadHub::SetActiveScenePath(const std::wstring& path)
{
    activeSceneNorm_ = NormalizePathKey(path);
}

void ReloadHub::Update()
{
    // フェーズ 2 = セーフポイント (spec 5.3)。ここ以外でリロードを適用しない
    shaders_->PollAsyncCompiles();

    for (const std::wstring& path : watcher_.DrainChanges()) {
        HandleChange(path);
    }
    for (const std::wstring& path : engineShaderWatcher_.DrainChanges()) {
        HandleChange(path); // .hlsl/.hlsli 以外は HandleChange 側の拡張子分岐で無視される
    }

    if (!retries_.empty()) {
        std::vector<Retry> current;
        current.swap(retries_);
        for (Retry& r : current) {
            HandleChange(r.path); // 失敗すれば HandleChange が再登録する
        }
    }
}

void ReloadHub::HandleChange(const std::wstring& normPath)
{
    const std::wstring ext = ExtensionLower(normPath);

    // 共有違反 (エディタがまだ書き込み中) はリトライ
    auto retryLater = [this, &normPath] {
        for (Retry& r : retries_) {
            if (r.path == normPath) {
                return;
            }
        }
        retries_.push_back({ normPath, 0 });
    };

    if (ext == L".hlsl" || ext == L".hlsli") {
        shaders_->RequestRecompileForFile(normPath);
        ++reloadCount_;
        return;
    }

    if (ext == L".png" || ext == L".tga" || ext == L".jpg" || ext == L".jpeg" || ext == L".dds") {
        const AssetID id = TextureLibrary::IdForFile(normPath);
        if (resources_->textures.Get(id) != nullptr) {
            if (resources_->textures.ReplaceFromFile(id, normPath)) {
                MYE_LOG_INFO("[reload] texture replaced: %s", WideToUtf8(normPath).c_str());
                ++reloadCount_;
            } else {
                retryLater();
            }
        }
        return;
    }

    if (ext == L".glb" || ext == L".gltf") {
        if (ModelLoader::ReloadMeshes(*resources_, *shaders_, normPath)) {
            ++reloadCount_;
        } else {
            retryLater();
        }
        return;
    }

    if (ext == L".fbx") {
        if (FbxLoader::ReloadMeshes(*resources_, *shaders_, normPath)) {
            ++reloadCount_;
        } else {
            retryLater();
        }
        return;
    }

    if (ext == L".json") {
        // .mat.json: 登録済みマテリアルなら再読込 (MeshRenderer は AssetID 参照なので自動反映)
        const bool isMat = normPath.size() >= 9
            && normPath.compare(normPath.size() - 9, 9, L".mat.json") == 0;
        if (isMat) {
            const AssetID id = MaterialLibrary::HashForPath(normPath);
            if (resources_->materials.Get(id) != nullptr) {
                if (!resources_->materials.LoadFromFile(normPath, resources_->textures, assetsRoot_)
                         .IsNull()) {
                    MYE_LOG_INFO("[reload] material reloaded: %s", WideToUtf8(normPath).c_str());
                    ++reloadCount_;
                } else {
                    retryLater();
                }
            }
            return;
        }
        // .anim.json: 登録済みクリップなら再読込 (animator は hash 参照なので自動反映)
        const bool isAnim = normPath.size() >= 10
            && normPath.compare(normPath.size() - 10, 10, L".anim.json") == 0;
        if (isAnim) {
            if (anims_) {
                const uint64_t hash = AnimationLibrary::HashForPath(normPath);
                if (anims_->Contains(hash)) {
                    if (anims_->LoadFromFile(normPath) != 0) {
                        MYE_LOG_INFO("[reload] anim reloaded: %s", WideToUtf8(normPath).c_str());
                        ++reloadCount_;
                    } else {
                        retryLater();
                    }
                }
            }
            return;
        }
        // .prefab.json: 登録済みプレハブなら再読込 → 全インスタンスの非オーバーライドへ伝播
        const bool isPrefab = normPath.size() >= 12
            && normPath.compare(normPath.size() - 12, 12, L".prefab.json") == 0;
        if (isPrefab) {
            if (prefabs_ && scene_) {
                const uint64_t hash = PrefabLibrary::HashForPath(normPath);
                if (prefabs_->Contains(hash)) {
                    const PrefabAsset* before = prefabs_->Get(hash);
                    const nlohmann::json oldBase = before ? before->entities : nlohmann::json::array();
                    const uint64_t rh = prefabs_->LoadFromFile(normPath);
                    if (rh == 0) {
                        retryLater(); // 書き込み途中 / パースエラー
                        return;
                    }
                    if (const PrefabAsset* after = prefabs_->Get(rh)) {
                        Prefab::PropagateBaseChange(*scene_, oldBase, after->entities, rh);
                        MYE_LOG_INFO("[reload] prefab recomposited: %s",
                                     WideToUtf8(normPath).c_str());
                        ++reloadCount_;
                    }
                }
            }
            return;
        }
        if (!activeSceneNorm_.empty() && normPath == activeSceneNorm_) {
            std::ifstream f(std::filesystem::path(normPath), std::ios::binary);
            if (!f) {
                retryLater();
                return;
            }
            nlohmann::json root;
            try {
                f >> root;
            } catch (const nlohmann::json::exception& ex) {
                // 手編集途中の不正 JSON — エンジンは止めない (spec 8.1 と同じ精神)
                MYE_LOG_WARN("[reload] scene json parse error (keeping current scene): %s", ex.what());
                return;
            }
            SceneSerializer::ApplyDiff(*scene_, root);
            ++reloadCount_;
        }
        return;
    }
}

} // namespace mye
