#include "Engine/Engine/HotReload/ReloadHub.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
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
                     PrefabLibrary* prefabs, const std::wstring& assetsRoot)
{
    shaders_ = shaders;
    resources_ = resources;
    scene_ = scene;
    prefabs_ = prefabs;
    std::error_code ec;
    if (!std::filesystem::is_directory(assetsRoot, ec)) {
        MYE_LOG_WARN("[reload] assets root not found, hot reload disabled");
        return false;
    }
    return watcher_.Start(assetsRoot);
}

void ReloadHub::Shutdown()
{
    watcher_.Stop();
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

    if (ext == L".png" || ext == L".tga" || ext == L".jpg" || ext == L".jpeg") {
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

    if (ext == L".json") {
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
