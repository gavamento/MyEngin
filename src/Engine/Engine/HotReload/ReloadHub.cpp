#include "Engine/Engine/HotReload/ReloadHub.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Animation.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/ImpactSoundAsset.h"
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

// path は正規化済み (NormalizePathKey = 小文字) なので大文字小文字は見ない
bool HasSuffix(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = std::char_traits<wchar_t>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ホットリロードが扱う資産の種類。**上から順に**末尾一致で引き、最初に当たった行が勝つ。
// ★.json の細分 (.mat.json など) は、最後の汎用 .json 行より上に置くこと。
// rank = 一括適用の順位 (小さいほど先)。参照する側が後に来るように並べてある:
//   シェーダ → テクスチャ/音 → マテリアル → モデル → クリップ類 → アクター → シーン
// ★spec §4.1 の「texture → mat → model → actor → scene」を部分列として含む。
//   間に挟んだものは「誰も参照していない」か「テクスチャと同格」のどちらかで、
//   相対順が結果を変えない位置に置いてある
struct AssetKindRow {
    const wchar_t* suffix;
    ReloadKind kind;
    int rank;
};
constexpr int kRankUnknown = 9;
constexpr AssetKindRow kAssetKinds[] = {
    { L".hlsl", ReloadKind::Shader, 0 },
    { L".hlsli", ReloadKind::Shader, 0 },
    { L".png", ReloadKind::Texture, 1 },
    { L".tga", ReloadKind::Texture, 1 },
    { L".jpg", ReloadKind::Texture, 1 },
    { L".jpeg", ReloadKind::Texture, 1 },
    { L".dds", ReloadKind::Texture, 1 },
    { L".wav", ReloadKind::AudioClip, 2 },
    { L".ogg", ReloadKind::AudioClip, 2 },
    { L".glb", ReloadKind::Gltf, 4 },
    { L".gltf", ReloadKind::Gltf, 4 },
    { L".fbx", ReloadKind::Fbx, 4 },
    { L".mat.json", ReloadKind::Material, 3 },
    { L".anim.json", ReloadKind::Anim, 5 },
    { L".sound.json", ReloadKind::Sound, 6 },
    { L".impact.json", ReloadKind::ImpactSound, 6 }, // ImpactSynth。.sound.json と同格 (誰も参照していない)
    { L".mixer.json", ReloadKind::Mixer, 6 },
    { L".physmat.json", ReloadKind::PhysMat, 6 },
    { PrefabLibrary::kActorSuffix, ReloadKind::Compose, 7 },
    { PrefabLibrary::kPrefabSuffix, ReloadKind::Compose, 7 },
    { L".scene.json", ReloadKind::Scene, 8 },
    // 未知の .json。開いているシーンなら拡張子を問わず差分適用する (ReloadActiveScene)。順位は最後
    { L".json", ReloadKind::Scene, kRankUnknown },
};

const AssetKindRow* FindAssetKind(const std::wstring& normPath)
{
    for (const AssetKindRow& row : kAssetKinds) {
        if (HasSuffix(normPath, row.suffix)) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

ReloadKind ReloadKindOf(const std::wstring& normPath)
{
    const AssetKindRow* row = FindAssetKind(normPath);
    return row ? row->kind : ReloadKind::None;
}

int ReloadRank(const std::wstring& normPath)
{
    const AssetKindRow* row = FindAssetKind(normPath);
    return row ? row->rank : kRankUnknown;
}

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
            HandleChange(r.path, r.attempts + 1); // 失敗すれば HandleChange が回数を 1 増やして積み直す
        }
    }
}

void ReloadHub::HandleChange(const std::wstring& normPath, int attempt)
{
    const AssetKindRow* row = FindAssetKind(normPath);
    if (row == nullptr) {
        return;
    }
    ReloadResult result = ReloadResult::Skipped;
    switch (row->kind) {
    case ReloadKind::Shader:
        result = ReloadShader(normPath);
        break;
    case ReloadKind::Texture:
        result = ReloadTexture(normPath);
        break;
    case ReloadKind::AudioClip:
        result = ReloadAudioClip(normPath);
        break;
    case ReloadKind::Gltf:
        result = ReloadGltf(normPath);
        break;
    case ReloadKind::Fbx:
        result = ReloadFbx(normPath);
        break;
    case ReloadKind::Material:
        result = ReloadMaterial(normPath);
        break;
    case ReloadKind::Anim:
        result = ReloadAnim(normPath);
        break;
    case ReloadKind::Sound:
        result = ReloadSound(normPath);
        break;
    case ReloadKind::ImpactSound:
        result = ReloadImpactSound(normPath);
        break;
    case ReloadKind::Mixer:
        result = ReloadMixer(normPath);
        break;
    case ReloadKind::PhysMat:
        result = ReloadPhysMat(normPath);
        break;
    case ReloadKind::Compose:
        result = ReloadCompose(normPath);
        break;
    case ReloadKind::Scene:
        result = ReloadActiveScene(normPath);
        break;
    case ReloadKind::None:
        break;
    }
    if (result == ReloadResult::Reloaded) {
        ++reloadCount_;
    } else if (result == ReloadResult::Retry) {
        QueueRetry(normPath, attempt);
    }
}

void ReloadHub::QueueRetry(const std::wstring& path, int attempts)
{
    for (const Retry& r : retries_) {
        if (r.path == path) {
            return;
        }
    }
    retries_.push_back({ path, attempts });
}

// ★リトライしない — 再コンパイルの要求を積むだけで、コンパイルは Update 冒頭の PollAsyncCompiles が進める
ReloadHub::ReloadResult ReloadHub::ReloadShader(const std::wstring& path)
{
    shaders_->RequestRecompileForFile(path);
    return ReloadResult::Reloaded;
}

ReloadHub::ReloadResult ReloadHub::ReloadTexture(const std::wstring& path)
{
    const AssetID id = TextureLibrary::IdForFile(path);
    if (resources_->textures.Get(id) == nullptr) {
        return ReloadResult::Skipped;
    }
    if (!resources_->textures.ReplaceFromFile(id, path)) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] texture replaced: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// ★再生中の XAUDIO2_BUFFER はクリップのバイト列を直接指しているので、
//   差し替えは必ず「参照している voice を止めてから」行う
//   (ReloadClipFile → RegisterClip → StopVoicesUsingClip の順で保証される)
ReloadHub::ReloadResult ReloadHub::ReloadAudioClip(const std::wstring& path)
{
    if (audio_ == nullptr || !audio_->HasClip(AudioSystem::IdForFile(path))) {
        return ReloadResult::Skipped;
    }
    if (!audio_->ReloadClipFile(path)) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] audio clip reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

ReloadHub::ReloadResult ReloadHub::ReloadGltf(const std::wstring& path)
{
    return ModelLoader::ReloadMeshes(*resources_, *shaders_, path) ? ReloadResult::Reloaded : ReloadResult::Retry;
}

ReloadHub::ReloadResult ReloadHub::ReloadFbx(const std::wstring& path)
{
    return FbxLoader::ReloadMeshes(*resources_, *shaders_, path) ? ReloadResult::Reloaded : ReloadResult::Retry;
}

// 登録済みマテリアルなら読み直す (MeshRenderer は AssetID 参照なので自動で反映される)
ReloadHub::ReloadResult ReloadHub::ReloadMaterial(const std::wstring& path)
{
    const AssetID id = MaterialLibrary::HashForPath(path);
    if (resources_->materials.Get(id) == nullptr) {
        return ReloadResult::Skipped;
    }
    if (resources_->materials.LoadFromFile(path, resources_->textures, assetsRoot_).IsNull()) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] material reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// 登録済みクリップなら読み直す (animator は hash 参照なので自動で反映される)
ReloadHub::ReloadResult ReloadHub::ReloadAnim(const std::wstring& path)
{
    if (anims_ == nullptr || !anims_->Contains(AnimationLibrary::HashForPath(path))) {
        return ReloadResult::Skipped;
    }
    if (anims_->LoadFromFile(path) == 0) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] anim reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// 登録済みサウンドなら読み直す (参照側は GUID なので自動で反映される、M45c)
ReloadHub::ReloadResult ReloadHub::ReloadSound(const std::wstring& path)
{
    if (sounds_ == nullptr || !sounds_->Contains(SoundLibrary::HashForPath(path))) {
        return ReloadResult::Skipped;
    }
    if (sounds_->LoadFromFile(path) == 0) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] sound reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// ImpactSynth: 生成し直して差し替える。RegisterClip が参照中の voice を止めてから PCM を入れ替えるので、
// 耳で詰めながら保存 → 即反映が成立する。
// ★未登録のファイル (起動後に足した) も登録する = .sound.json より緩いが、
//   参照する側が名前キーなので「登録した瞬間から鳴る」で困らない
ReloadHub::ReloadResult ReloadHub::ReloadImpactSound(const std::wstring& path)
{
    if (sounds_ == nullptr || audio_ == nullptr) {
        return ReloadResult::Skipped;
    }
    if (LoadImpactSoundFile(*audio_, *sounds_, path) == 0) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] impact sound regenerated: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// 登録済みミキサーなら読み直す。**アクティブなら即バスグラフへ再適用する**
// (適用自体は AudioSystem::Update = フレーム境界まで遅延される、M45d)
ReloadHub::ReloadResult ReloadHub::ReloadMixer(const std::wstring& path)
{
    if (mixers_ == nullptr) {
        return ReloadResult::Skipped;
    }
    const uint64_t hash = MixerLibrary::HashForPath(path);
    if (!mixers_->Contains(hash)) {
        return ReloadResult::Skipped;
    }
    if (mixers_->LoadFromFile(path) == 0) {
        return ReloadResult::Retry;
    }
    if (audio_ != nullptr && mixers_->ActiveHash() == hash) {
        if (const MixerAsset* m = mixers_->Get(hash)) {
            audio_->ApplyMixer(*m);
        }
    }
    MYE_LOG_INFO("[reload] mixer reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// 登録済みなら読み直す (M59a1)。所有は EngineLoop = physmat:: 経由で引く。
// ★ソルバが材料を読む (M59a2) ので「ホットリロードが sim を変える既存資産クラス
//   (メッシュコライダーと同類)」に属する — record/verify 中の挙動もそちらの規約に従う
ReloadHub::ReloadResult ReloadHub::ReloadPhysMat(const std::wstring& path)
{
    PhysMatLibrary* pm = physmat::Library();
    if (pm == nullptr) {
        return ReloadResult::Skipped;
    }
    if (!pm->Contains(PhysMatLibrary::HashForPath(path))) {
        return ReloadResult::Skipped;
    }
    if (pm->LoadFromFile(path) == 0) {
        return ReloadResult::Retry;
    }
    MYE_LOG_INFO("[reload] physmat reloaded: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// .actor.json / .prefab.json: 登録済みなら読み直し → 全インスタンスの非オーバーライドへ伝播
ReloadHub::ReloadResult ReloadHub::ReloadCompose(const std::wstring& path)
{
    if (prefabs_ == nullptr || scene_ == nullptr) {
        return ReloadResult::Skipped;
    }
    const uint64_t hash = PrefabLibrary::HashForPath(path);
    if (!prefabs_->Contains(hash)) {
        return ReloadResult::Skipped;
    }
    const PrefabAsset* before = prefabs_->Get(hash);
    const nlohmann::json oldBase = before ? before->entities : nlohmann::json::array();
    const uint64_t rh = prefabs_->LoadFromFile(path);
    if (rh == 0) {
        return ReloadResult::Retry; // 書き込み途中 / パースエラー
    }
    const PrefabAsset* after = prefabs_->Get(rh);
    if (after == nullptr) {
        return ReloadResult::Skipped;
    }
    Prefab::PropagateBaseChange(*scene_, oldBase, after->entities, rh);
    MYE_LOG_INFO("[reload] compose asset recomposited: %s", WideToUtf8(path).c_str());
    return ReloadResult::Reloaded;
}

// 開いているシーンの外部編集だけを差分適用する (拡張子は問わない — 表の最後の .json 行もここへ来る)
ReloadHub::ReloadResult ReloadHub::ReloadActiveScene(const std::wstring& path)
{
    if (activeSceneNorm_.empty() || path != activeSceneNorm_) {
        return ReloadResult::Skipped;
    }
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return ReloadResult::Retry;
    }
    nlohmann::json root;
    try {
        f >> root;
    } catch (const nlohmann::json::exception& ex) {
        // 手編集途中の不正 JSON — エンジンは止めない (spec 8.1 と同じ精神)。
        // ★リトライ列にも積まない (直して保存し直せば watcher がまた運んでくる)
        MYE_LOG_WARN("[reload] scene json parse error (keeping current scene): %s", ex.what());
        return ReloadResult::Skipped;
    }
    SceneSerializer::ApplyDiff(*scene_, root);
    return ReloadResult::Reloaded;
}

} // namespace mye
