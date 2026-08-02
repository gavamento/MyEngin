#pragma once
#include <string>
#include <vector>

#include "Engine/Core/FileWatcher.h"

namespace mye {

class ShaderManager;
class Scene;
class PrefabLibrary;
class AnimationLibrary;
class SoundLibrary;
class MixerLibrary;
class AudioSystem;
struct RenderResources;

// ホットリロードの司令塔 (engine_spec.md 8 章)。
// assets\ を 1 本の FileWatcher で再帰監視し、拡張子で各リロード先へ振り分ける。
// 適用は必ずメインループのフェーズ 2 (Update) で行う
class ReloadHub {
public:
    bool Init(ShaderManager* shaders, RenderResources* resources, Scene* scene,
              PrefabLibrary* prefabs, AnimationLibrary* anims, SoundLibrary* sounds,
              MixerLibrary* mixers, AudioSystem* audio, const std::wstring& assetsRoot);
    void Shutdown();

    // 現在編集中のシーンファイル (このファイルの外部編集だけ差分適用する)
    void SetActiveScenePath(const std::wstring& path);

    void Update(); // フェーズ 2 で毎フレーム呼ぶ

    uint64_t ReloadCount() const { return reloadCount_; } // AssetBrowser 表示用

private:
    void HandleChange(const std::wstring& normPath);

    FileWatcher watcher_;
    // エンジン組込みシェーダ (<engineRepo>\assets\shaders) の監視。
    // assets\ の外にあるので watcher_ では拾えず、別ルートとして張る。
    // レガシー起動 (assets = エンジンの assets) では重複するので起動しない
    FileWatcher engineShaderWatcher_;
    ShaderManager* shaders_ = nullptr;
    RenderResources* resources_ = nullptr;
    Scene* scene_ = nullptr;
    PrefabLibrary* prefabs_ = nullptr;
    AnimationLibrary* anims_ = nullptr;
    SoundLibrary* sounds_ = nullptr; // .sound.json (M45c)
    MixerLibrary* mixers_ = nullptr; // .mixer.json (M45d)。アクティブなら再適用まで行う
    AudioSystem* audio_ = nullptr;   // .wav/.ogg の差し替え (再生中 voice は先に停止される)
    std::wstring assetsRoot_; // .mat.json のテクスチャ相対パス解決に使う (M17)
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
