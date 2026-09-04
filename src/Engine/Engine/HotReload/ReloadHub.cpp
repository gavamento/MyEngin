#include "Engine/Engine/HotReload/ReloadHub.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
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

bool HasSuffix(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = std::char_traits<wchar_t>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// 適用順の階級 (小さいほど先)。参照する側が後に来るように並べてある:
//   シェーダ → テクスチャ/音 → マテリアル → モデル → クリップ類 → アクター → シーン
// ★spec §4.1 の「texture → mat → model → actor → scene」を部分列として含む。
//   間に挟んだものは「誰も参照していない」か「テクスチャと同格」のどちらかで、
//   相対順が結果を変えない位置に置いてある
int ReloadRank(const std::wstring& normPath)
{
    const std::wstring ext = ExtensionLower(normPath);
    if (ext == L".hlsl" || ext == L".hlsli") {
        return 0;
    }
    if (ext == L".png" || ext == L".tga" || ext == L".jpg" || ext == L".jpeg" || ext == L".dds") {
        return 1;
    }
    if (ext == L".wav" || ext == L".ogg") {
        return 2;
    }
    if (ext == L".json") {
        if (HasSuffix(normPath, L".mat.json")) {
            return 3;
        }
        if (HasSuffix(normPath, L".anim.json")) {
            return 5;
        }
        if (HasSuffix(normPath, L".sound.json")) {
            return 6;
        }
        if (HasSuffix(normPath, L".mixer.json")) {
            return 6;
        }
        if (HasSuffix(normPath, L".physmat.json")) {
            return 6;
        }
        if (HasSuffix(normPath, L".actor.json") || HasSuffix(normPath, L".prefab.json")) {
            return 7;
        }
        if (HasSuffix(normPath, L".scene.json")) {
            return 8;
        }
        return 9; // 未知の .json (HandleChange は何もしない)
    }
    if (ext == L".glb" || ext == L".gltf" || ext == L".fbx") {
        return 4;
    }
    return 9;
}

} // namespace

std::vector<std::wstring> OrderBatch(const std::vector<BatchChange>& changes)
{
    std::vector<std::wstring> paths;
    paths.reserve(changes.size());
    for (const BatchChange& c : changes) {
        // ★Deleted は落とす。HandleChange は「登録済みの資産を読み直す」しかできず、
        //   消えたファイルを渡すと開けずにリトライ列へ積まれるだけになる (spec S4)
        if (c.kind == BatchChange::Kind::Deleted || c.path.empty()) {
            continue;
        }
        paths.push_back(NormalizePathKey(c.path));
    }
    // 種別順 → 同種は正規化キーの昇順。**明示的な決定論キー**で並べる
    // (入力の並びに依存させると、同じ checkout が機体によって違う順で適用される)
    std::stable_sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) {
        const int ra = ReloadRank(a);
        const int rb = ReloadRank(b);
        return (ra != rb) ? (ra < rb) : (a < b);
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

bool ReloadHub::Init(ShaderManager* shaders, RenderResources* resources, Scene* scene,
                     PrefabLibrary* prefabs, AnimationLibrary* anims, SoundLibrary* sounds,
                     MixerLibrary* mixers, AudioSystem* audio, const std::wstring& assetsRoot)
{
    shaders_ = shaders;
    resources_ = resources;
    scene_ = scene;
    prefabs_ = prefabs;
    anims_ = anims;
    sounds_ = sounds;
    mixers_ = mixers;
    audio_ = audio;
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

void ReloadHub::DiscardPendingChanges()
{
    // DrainChanges は溜まりを取り出して空にする = 呼んで捨てるのが「破棄」
    watcher_.DrainChanges();
    engineShaderWatcher_.DrainChanges();
}

void ReloadHub::BeginBatch()
{
    batching_ = true;
}

void ReloadHub::EndBatch(const std::vector<BatchChange>& changes)
{
    batching_ = false;
    // ★先に捨ててから適用する。git が書いた分は changes が正本なので、
    //   watcher の溜まりを一緒に流すと同じファイルを 2 度読む (テクスチャなら
    //   無駄なだけだが、シーンは ApplyDiff が 2 回走って編集が 1 世代戻る)。
    // ★ただし「捨てる」は**全部は捨てられない** (review-1 #7)。`DrainChanges` が返すのは
    //   最後のイベントから `kDebounceMs` (150 ms) 経ったパスだけで (FileWatcher.cpp)、
    //   git が書き終えてから EndBatch までは数十 ms しかない = 直前に書かれたパスは
    //   `pending_` に残り、**EndBatch の 150 ms 後に通常経路でもう一度 HandleChange される**。
    //   起きるのは「同じファイルをもう一度読み直す」だけ — ディスクの中身は EndBatch で
    //   適用したものと同じままなので、シーンは同内容の ApplyDiff、prefab は同じ base の
    //   PropagateBaseChange で結果が変わらない (この 150 ms の間に人が編集していれば
    //   ディスク側へ引き戻されるが、それは外部編集を拾う経路本来の挙動と同じ)。
    //   塞ごうとして「EndBatch で適用したパスを 1 デバウンス分だけ無視する」を足すと、
    //   **EndBatch 直後に人が入れた本物の外部編集まで飲み込む** — 取りこぼしの方が
    //   高くつくので、v1 は二度読みを許す
    DiscardPendingChanges();
    const std::vector<std::wstring> ordered = OrderBatch(changes);
    for (const std::wstring& path : ordered) {
        HandleChange(path);
    }
    if (!ordered.empty()) {
        MYE_LOG_INFO("[reload] batch applied: %zu change(s)", ordered.size());
    }
}

void ReloadHub::Update()
{
    // フェーズ 2 = セーフポイント (spec 5.3)。ここ以外でリロードを適用しない
    shaders_->PollAsyncCompiles();

    if (batching_) {
        // 一括適用の最中。**溜まりは捨てるだけ**で 1 件も適用しない (M66d)。
        // 捨てないと、EndBatch までの数フレームぶんが後からまとめて流れ込み、
        // 適用済みのものをもう一度読み直す
        DiscardPendingChanges();
        return;
    }

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
            // ★上限で諦める。付けないと「外部で消されたファイル」が永久に残り、
            //   毎フレーム開き直しを試み続ける (spec §2 の S4)
            if (r.attempts >= kReloadRetryMax) {
                MYE_LOG_WARN("[reload] giving up after %d attempts: %s", r.attempts,
                             WideToUtf8(r.path).c_str());
                continue;
            }
            retryAttempt_ = r.attempts + 1; // retryLater が積み直すときに引き継ぐ
            HandleChange(r.path);           // 失敗すれば HandleChange が再登録する
        }
        retryAttempt_ = 0;
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
        // retryAttempt_ = 「今 Update が処理している Retry の回数 + 1」。
        // watcher 由来の初回は 0 のまま = 1 回目として積まれる
        retries_.push_back({ normPath, retryAttempt_ });
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

    if (ext == L".wav" || ext == L".ogg") {
        // ★再生中の XAUDIO2_BUFFER はクリップのバイト列を直接指しているので、
        //   差し替えは必ず「参照している voice を止めてから」行う
        //   (ReloadClipFile → RegisterClip → StopVoicesUsingClip の順で保証される)。
        //   未ロードのファイルは false が返るだけで何も起きない
        if (audio_ != nullptr && audio_->HasClip(AudioSystem::IdForFile(normPath))) {
            if (audio_->ReloadClipFile(normPath)) {
                MYE_LOG_INFO("[reload] audio clip reloaded: %s", WideToUtf8(normPath).c_str());
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
        // .sound.json: 登録済みサウンドなら再読込 (参照側は GUID なので自動反映、M45c)
        const bool isSound = normPath.size() >= 11
            && normPath.compare(normPath.size() - 11, 11, L".sound.json") == 0;
        if (isSound) {
            if (sounds_ != nullptr) {
                const uint64_t hash = SoundLibrary::HashForPath(normPath);
                if (sounds_->Contains(hash)) {
                    if (sounds_->LoadFromFile(normPath) != 0) {
                        MYE_LOG_INFO("[reload] sound reloaded: %s", WideToUtf8(normPath).c_str());
                        ++reloadCount_;
                    } else {
                        retryLater();
                    }
                }
            }
            return;
        }
        // .mixer.json: 登録済みミキサーなら再読込。**アクティブなら即バスグラフへ再適用する**
        // (適用自体は AudioSystem::Update = フレーム境界まで遅延される、M45d)
        const bool isMixer = normPath.size() >= 11
            && normPath.compare(normPath.size() - 11, 11, L".mixer.json") == 0;
        if (isMixer) {
            if (mixers_ != nullptr) {
                const uint64_t hash = MixerLibrary::HashForPath(normPath);
                if (mixers_->Contains(hash)) {
                    if (mixers_->LoadFromFile(normPath) != 0) {
                        if (audio_ != nullptr && mixers_->ActiveHash() == hash) {
                            if (const MixerAsset* m = mixers_->Get(hash)) {
                                audio_->ApplyMixer(*m);
                            }
                        }
                        MYE_LOG_INFO("[reload] mixer reloaded: %s", WideToUtf8(normPath).c_str());
                        ++reloadCount_;
                    } else {
                        retryLater();
                    }
                }
            }
            return;
        }
        // .physmat.json: 登録済みなら再読込 (M59a1)。所有は EngineLoop = physmat:: 経由で引く。
        // ★M59a2 で sim が消費し始めたら「ホットリロードが sim を変える既存資産クラス
        //   (メッシュコライダーと同類)」に合流する — record/verify 中の挙動もそちらの規約に従う
        const bool isPhysMat = normPath.size() >= 13
            && normPath.compare(normPath.size() - 13, 13, L".physmat.json") == 0;
        if (isPhysMat) {
            if (PhysMatLibrary* pm = physmat::Library()) {
                const uint64_t hash = PhysMatLibrary::HashForPath(normPath);
                if (pm->Contains(hash)) {
                    if (pm->LoadFromFile(normPath) != 0) {
                        MYE_LOG_INFO("[reload] physmat reloaded: %s",
                                     WideToUtf8(normPath).c_str());
                        ++reloadCount_;
                    } else {
                        retryLater();
                    }
                }
            }
            return;
        }
        // .actor.json / .prefab.json: 登録済みなら再読込 → 全インスタンスの非オーバーライドへ伝播
        if (PrefabLibrary::IsComposePath(normPath)) {
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
                        MYE_LOG_INFO("[reload] compose asset recomposited: %s",
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
