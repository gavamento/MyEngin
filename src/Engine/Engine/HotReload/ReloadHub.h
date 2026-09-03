#pragma once
#include <cstdint>
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

// 一括適用 1 件分の変更 (M66d)。**Collab を知らない Engine 層の汎用型**で、
// 「外の何かが assets\ をまとめて書き換えた」ことだけを表す。
// git 以外 (将来の一括インポートなど) からも同じ口を使えるようにしてある
struct BatchChange {
    enum class Kind : uint8_t {
        Modified, // 内容が変わった
        Added,    // 新しく現れた
        Deleted,  // 消えた (★ReloadHub は扱わない — OrderBatch が落とす。理由は下)
        Renamed,  // 名前が変わった (path = 新しい名前、oldPath = 旧)
    };
    std::wstring path;
    Kind kind = Kind::Modified;
    std::wstring oldPath;
};

// 一括適用の順番を決める**純関数** (M66d)。
//
// なぜ順番が要るか: マテリアルはテクスチャを、モデルはマテリアルを、アクターはモデルを、
// シーンはアクターを参照する。逆順に適用すると「シーンを読み直した後でアクターが
// 更新される」= 配置済みインスタンスが 1 世代古いまま残る。
//
// 戻り値は `NormalizePathKey` 済みのパス列 (HandleChange が期待する形)。
//   * 並びは **種別順 → 同種は正規化キーの昇順**。同じ変更集合なら必ず同じ順になる
//     (どの機体でも同じ結果になることが再現の前提)
//   * `Deleted` は**出力から落とす**。ReloadHub の HandleChange は「登録済み資産を
//     読み直す」しかできず、消えたファイルを渡すと読めずにリトライ列へ積まれるだけ。
//     消えたものの後始末は呼び手 (段階 B = シーンの開き直し) の仕事 (spec §4.1 S4)
//   * `Renamed` は新しい名前を Modified と同じに扱う (旧名は Deleted と同じ = 落とす)
std::vector<std::wstring> OrderBatch(const std::vector<BatchChange>& changes);

// 共有違反 (書き込み途中) のリトライを諦める回数 (M66d、spec §2 の S4)。
// ★上限が無いと、外部で消されたファイルが**永久に**リトライ列に残って
//   毎フレーム開き直しを試み続ける。60 回 = 1 秒 (60 fps) で諦める
constexpr int kReloadRetryMax = 60;

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

    // ---- 一括適用 (M66d) ----
    // BeginBatch と EndBatch の間、Update() は watcher を drain するだけで
    // **1 件も適用しない**。git が working tree を書き換えている最中に
    // 1 ファイルずつ読み直すと、半分だけ新しいシーン + 古いマテリアル、のような
    // 中間状態を必ず作る (しかも順番は書き込み順 = 非決定)。
    // ★BeginBatch を呼んだら必ず EndBatch を呼ぶこと。呼ばないとホットリロードが
    //   止まったままになる (エラーで抜ける経路でも EndBatch({}) を通す)
    void BeginBatch();
    // 変更集合を OrderBatch の順で適用する。溜まっていた watcher 分は**破棄**
    // (git が書いた分は changes が正本。両方適用すると同じファイルを 2 度読む)
    void EndBatch(const std::vector<BatchChange>& changes);
    bool Batching() const { return batching_; }

    uint64_t ReloadCount() const { return reloadCount_; } // AssetBrowser 表示用

private:
    void HandleChange(const std::wstring& normPath);
    // watcher の溜まりを捨てる (バッチ中と EndBatch 直後)
    void DiscardPendingChanges();

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
    // retryLater が積むときに引き継ぐ試行回数 (今処理している Retry の回数 + 1)。
    // ★HandleChange のラムダから見えるところに置くしかない — 引数で渡すと
    //   HandleChange の呼び出し元 12 箇所すべてを書き換えることになる
    int retryAttempt_ = 0;
    bool batching_ = false; // BeginBatch 〜 EndBatch の間だけ true
};

} // namespace mye
