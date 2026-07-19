#pragma once
#include <string>
#include <vector>

#include "Engine/Core/FileWatcher.h"

namespace mye {

class ShaderManager;
class Scene;
class PrefabLibrary;
struct RenderResources;

// ホットリロードの司令塔 (engine_spec.md 8 章)。
// assets\ を 1 本の FileWatcher で再帰監視し、拡張子で各リロード先へ振り分ける。
// 適用は必ずメインループのフェーズ 2 (Update) で行う
class ReloadHub {
public:
    bool Init(ShaderManager* shaders, RenderResources* resources, Scene* scene,
              PrefabLibrary* prefabs, const std::wstring& assetsRoot);
    void Shutdown();

    // 現在編集中のシーンファイル (このファイルの外部編集だけ差分適用する)
    void SetActiveScenePath(const std::wstring& path);

    void Update(); // フェーズ 2 で毎フレーム呼ぶ

    uint64_t ReloadCount() const { return reloadCount_; } // AssetBrowser 表示用

private:
    void HandleChange(const std::wstring& normPath);

    FileWatcher watcher_;
    ShaderManager* shaders_ = nullptr;
    RenderResources* resources_ = nullptr;
    Scene* scene_ = nullptr;
    PrefabLibrary* prefabs_ = nullptr;
    std::wstring activeSceneNorm_;
    uint64_t reloadCount_ = 0;

    // 書き込み途中 (共有違反) だったファイルのリトライ
    struct Retry {
        std::wstring path;
        int attempts = 0;
    };
    std::vector<Retry> retries_;
};

} // namespace mye
