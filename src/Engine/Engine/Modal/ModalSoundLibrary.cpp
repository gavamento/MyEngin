//====================================================================================
//                          ModalSoundLibrary.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          非同期焼きワーカーと .msfm キャッシュの実装
//====================================================================================
#include "Engine/Engine/Modal/ModalSoundLibrary.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h"

namespace fs = std::filesystem;

namespace mye {
namespace {
constexpr const wchar_t* kMsfmExt = L".msfm";
} // namespace

ModalSoundLibrary::~ModalSoundLibrary()
{
    Shutdown();
}

void ModalSoundLibrary::SetBackend(std::unique_ptr<ModalInferenceBackend> backend)
{
    backend_ = std::move(backend);
}

bool ModalSoundLibrary::SetBackendByName(const std::string& name)
{
    if (name == "cpu") {
        SetBackend(std::make_unique<CpuModalBackend>());
        return true;
    }
    if (name == "d3d11cs") {
        // M76 時点では GPU 実装 (D3d11ModalBackend) は差し込み口のみ (spec §3「後回し」)。
        // 未実装名を静かに cpu へ丸めると「GPU を選んだつもり」のまま気付かれないので WARN する
        MYE_LOG_WARN("[modal] backend 'd3d11cs' is not implemented yet, falling back to cpu");
        SetBackend(std::make_unique<CpuModalBackend>());
        return true;
    }
    return false; // 綴り違い。呼び出し側 (CLI) が exit 1 に使う
}

bool ModalSoundLibrary::LoadModel(const std::wstring& path)
{
    modelPath_ = path;
    if (path.empty()) {
        net_.reset();
        return false;
    }
    auto net = std::make_shared<DmNet>();
    std::string err;
    if (!LoadDmNet(path, *net, &err)) {
        MYE_LOG_WARN("[modal] failed to load .dmnet: %s (%s)", WideToUtf8(path).c_str(),
                     err.c_str());
        net_.reset();
        return false;
    }
    if (!backend_) {
        MYE_LOG_WARN("[modal] no inference backend installed, cannot load .dmnet");
        net_.reset();
        return false;
    }
    if (!backend_->Prepare(*net, &err)) {
        MYE_LOG_WARN("[modal] backend Prepare() failed: %s (%s)", err.c_str(),
                     WideToUtf8(path).c_str());
        net_.reset();
        return false;
    }
    net_ = std::move(net);
    MYE_LOG_INFO("[modal] loaded .dmnet: %s (paramCount=%u, backend=%s)", WideToUtf8(path).c_str(),
                 net_->header.paramCount, BackendName());
    return true;
}

bool ModalSoundLibrary::ReloadModel()
{
    if (modelPath_.empty()) {
        return false;
    }
    // Prepare() と Infer() の同時実行を避けるため、ワーカーの手持ちジョブが尽きるのを待つ
    // (.dmnet のホットリロードはまれなので、数百 ms 級の主スレッド待ちは許容する)
    if (workerStarted_) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return jobQueue_.empty() && !busy_; });
    }
    return LoadModel(modelPath_);
}

void ModalSoundLibrary::EnsureWorker()
{
    if (workerStarted_) {
        return;
    }
    workerStarted_ = true;
    worker_ = std::thread([this] { WorkerLoop(); });
}

void ModalSoundLibrary::WorkerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return workerStop_ || !jobQueue_.empty(); });
            if (workerStop_ && jobQueue_.empty()) {
                return;
            }
            job = std::move(jobQueue_.front());
            jobQueue_.pop_front();
            busy_ = true;
        }

        JobResult r;
        r.meshId = job.meshId;
        r.meshName = job.meshName;
        r.net = job.net;

        modal::VoxelGrid grid;
        const bool voxOk = modal::VoxelizeMesh(job.positions.data(), job.positions.size(),
                                               job.indices.data(), job.indices.size(), grid);
        if (!voxOk) {
            r.ok = false;
            r.err = "voxelize failed";
        } else if (backend_ && backend_->RunsOnWorkerThread()) {
            r.ok = BuildFeatureMap(*backend_, *job.net, grid, r.map, &r.err);
        } else {
            r.needsMainInfer = true;
            r.grid = std::move(grid);
            r.ok = true; // Pump 側で確定させる (voxelize までは成功)
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            doneQueue_.push_back(std::move(r));
            busy_ = false;
        }
        cv_.notify_all(); // ReloadModel のドレイン待ちを起こす
    }
}

bool ModalSoundLibrary::GatherGeometry(const ModalMeshRef& ref,
                                       std::vector<DirectX::XMFLOAT3>& positions,
                                       std::vector<uint32_t>& indices, std::string& nameOut) const
{
    positions.clear();
    indices.clear();
    nameOut.clear();
    if (resources_ == nullptr || ref.parts.empty()) {
        return false;
    }
    // 先に全パーツの揃いを確かめる — 途中まで連結してから欠けに気付くと、欠けた形で
    // 焼いた結果を表へ残しかねない (後から登録されたときに焼き直されない)
    size_t vertTotal = 0;
    size_t idxTotal = 0;
    for (const ModalMeshPart& part : ref.parts) {
        Mesh* m = resources_->meshes.Get(part.mesh);
        if (!m || m->positions.empty()) {
            return false; // 未登録 / CPU 頂点なし。キャッシュしない (後から登録され得る)
        }
        vertTotal += m->positions.size();
        idxTotal += m->indices.size();
    }
    const std::string* firstName = resources_->meshes.NameOf(ref.parts[0].mesh);
    if (!ref.IsComposite()) {
        nameOut = firstName ? *firstName : std::string{};
    } else {
        // 合成は先頭パーツ (整列済み) のモデルの .msfm 表へ相乗りさせる: "guid://<16hex>" の接頭辞を
        // 流用すれば SourcePathForSubAssetKey がそのモデルファイルへ引ける (表の形式は不変)。
        // GUID を持たない (builtin 等) なら別スキーム = ディスクに持たない (builtin 単体と同じ扱い)
        char suffix[40];
        std::snprintf(suffix, sizeof(suffix), "%016llx",
                      static_cast<unsigned long long>(ref.id.value));
        uint64_t guid = 0;
        if (firstName && assetkey::ParseSubAssetKey(*firstName, guid)) {
            nameOut = firstName->substr(0, firstName->find('#')) + "#modal#" + suffix;
        } else {
            nameOut = std::string("modal-composite://") + suffix;
        }
    }

    positions.reserve(vertTotal);
    indices.reserve(idxTotal);
    for (const ModalMeshPart& part : ref.parts) {
        const Mesh* m = resources_->meshes.Get(part.mesh);
        const uint32_t base = static_cast<uint32_t>(positions.size());
        if (part.identity) {
            positions.insert(positions.end(), m->positions.begin(), m->positions.end());
        } else {
            const DirectX::XMMATRIX xf = DirectX::XMLoadFloat4x4(&part.toSource);
            for (const DirectX::XMFLOAT3& p : m->positions) {
                DirectX::XMFLOAT3 q;
                DirectX::XMStoreFloat3(&q,
                                       DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&p), xf));
                positions.push_back(q);
            }
        }
        for (const uint32_t i : m->indices) {
            indices.push_back(base + i);
        }
    }
    return true;
}

bool ModalSoundLibrary::TryLoadFromTable(uint64_t id, const std::string& name)
{
    const std::wstring srcPath =
        name.empty() ? std::wstring{} : assetkey::SourcePathForSubAssetKey(name);
    if (srcPath.empty()) {
        return false; // builtin / 合成でも GUID が無い = ディスクに持たない
    }
    CookTable& table = LoadTable(srcPath);
    const auto hit = std::lower_bound(
        table.begin(), table.end(), name,
        [](const std::pair<std::string, ModalFeatureMap>& e, const std::string& k) {
            return e.first < k;
        });
    if (hit != table.end() && hit->first == name
        && hit->second.modelHash == net_->header.weightsHash) {
        cache_[id] = { ModalState::Ready, hit->second };
        return true;
    }
    return false;
}

ModalState ModalSoundLibrary::Request(const ModalMeshRef& ref)
{
    if (ref.IsNull()) {
        return ModalState::Missing;
    }
    if (!net_ || !backend_) {
        return ModalState::NoModel;
    }
    const auto it = cache_.find(ref.id.value);
    if (it != cache_.end()) {
        return it->second.state;
    }

    Job job;
    if (!GatherGeometry(ref, job.positions, job.indices, job.meshName)) {
        return ModalState::Missing;
    }
    if (TryLoadFromTable(ref.id.value, job.meshName)) {
        return ModalState::Ready;
    }

    EnsureWorker();
    job.meshId = ref.id.value;
    job.net = net_;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        jobQueue_.push_back(std::move(job));
    }
    cv_.notify_one();
    cache_[ref.id.value] = { ModalState::Baking, {} };
    return ModalState::Baking;
}

const ModalFeatureMap* ModalSoundLibrary::Get(AssetID mesh) const
{
    const auto it = cache_.find(mesh.value);
    if (it == cache_.end() || it->second.state != ModalState::Ready) {
        return nullptr;
    }
    return &it->second.map;
}

void ModalSoundLibrary::FinishResult(JobResult&& r)
{
    if (!r.ok) {
        cache_[r.meshId] = { ModalState::Failed, {} };
        if (warnedFailed_.insert(r.meshId).second) {
            MYE_LOG_WARN("[modal] bake failed for %s: %s", r.meshName.c_str(), r.err.c_str());
        }
        return;
    }
    ModalFeatureMap map = std::move(r.map);
    map.modelHash = r.net ? r.net->header.weightsHash : map.modelHash;
    cache_[r.meshId] = { ModalState::Ready, map };

    const std::wstring srcPath =
        r.meshName.empty() ? std::wstring{} : assetkey::SourcePathForSubAssetKey(r.meshName);
    if (!srcPath.empty()) {
        UpdateTableEntry(srcPath, r.meshName, map);
    }
}

void ModalSoundLibrary::Pump()
{
    // GPU バックエンド (RunsOnWorkerThread()==false) 用: ワーカーが渡してきたボクセルグリッドへ
    // 1 フレーム 1 件だけ Infer をかける (immediate context はメインスレッド専用という前提)
    std::deque<JobResult> done;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        done.swap(doneQueue_);
    }
    bool inferredOne = false;
    for (JobResult& r : done) {
        if (r.needsMainInfer && !inferredOne) {
            inferredOne = true;
            if (backend_ && r.net) {
                r.ok = BuildFeatureMap(*backend_, *r.net, r.grid, r.map, &r.err);
            } else {
                r.ok = false;
                r.err = "no backend/net at main-thread infer time";
            }
            r.needsMainInfer = false;
        } else if (r.needsMainInfer) {
            // このフレームでは処理しない。次の Pump へ回す
            std::lock_guard<std::mutex> lk(mutex_);
            doneQueue_.push_back(std::move(r));
            continue;
        }
        FinishResult(std::move(r));
    }
}

void ModalSoundLibrary::Register(AssetID id, ModalFeatureMap map)
{
    if (id.IsNull()) {
        return;
    }
    cache_[id.value] = { ModalState::Ready, std::move(map) };
}

void ModalSoundLibrary::Clear()
{
    FlushDirtyTables(); // dirty なまま tables_ を捨てると焼いた結果が静かに消える
    cache_.clear();
    warnedFailed_.clear();
    tables_.clear();
}

void ModalSoundLibrary::Shutdown()
{
    FlushDirtyTables(); // ライブラリ破棄時の明示 flush (reviewer round 1 指摘 3、spec sub-10)
    if (workerStarted_) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            workerStop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        workerStarted_ = false;
        workerStop_ = false;
    }
}

bool ModalSoundLibrary::BakeSync(const ModalMeshRef& ref)
{
    if (ref.IsNull() || !net_ || !backend_ || resources_ == nullptr) {
        return false;
    }
    const uint64_t id = ref.id.value;
    // 既に Ready ならそのまま (プロセス内キャッシュ命中。2 回目の Request/BakeSync が
    // 無駄に焼き直さないようにする)
    const auto cached = cache_.find(id);
    if (cached != cache_.end() && cached->second.state == ModalState::Ready) {
        return true;
    }
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    std::string name;
    if (!GatherGeometry(ref, positions, indices, name)) {
        return false;
    }
    // `.msfm` の表 (ディスクの cook キャッシュ) 命中を先に見る — Request() の非同期経路と
    // 同じ TryLoadFromTable を通す (2 本目を書くと必ずずれる)。
    // ★これが無いと `--modal-bake` の「2 回目は全部 cached」が成立しない
    //   (焼き直しの時間が毎回かかる = 実測で発覚した欠陥、コミット前に確認)
    if (TryLoadFromTable(id, name)) {
        return true;
    }
    modal::VoxelGrid grid;
    if (!modal::VoxelizeMesh(positions.data(), positions.size(), indices.data(), indices.size(),
                             grid)) {
        cache_[id] = { ModalState::Failed, {} };
        return false;
    }
    ModalFeatureMap map;
    std::string err;
    if (!BuildFeatureMap(*backend_, *net_, grid, map, &err)) {
        cache_[id] = { ModalState::Failed, {} };
        if (warnedFailed_.insert(id).second) {
            MYE_LOG_WARN("[modal] BakeSync failed: %s", err.c_str());
        }
        return false;
    }
    cache_[id] = { ModalState::Ready, map };

    const std::wstring srcPath =
        name.empty() ? std::wstring{} : assetkey::SourcePathForSubAssetKey(name);
    if (!srcPath.empty()) {
        UpdateTableEntry(srcPath, name, map);
    }
    return true;
}

ModalSoundLibrary::CookTable& ModalSoundLibrary::LoadTable(const std::wstring& srcPath)
{
    auto it = tables_.find(srcPath);
    if (it != tables_.end()) {
        return it->second;
    }
    CookTable table;
    std::vector<uint8_t> payload;
    if (CookedCache::Enabled() && CookedCache::ReadValidated(srcPath, kMsfmExt, payload)) {
        if (!DeserializeModalTable(payload, table)) {
            MYE_LOG_WARN("[cook] .msfm is corrupt - rebuilding feature maps (%s)",
                         WideToUtf8(srcPath).c_str());
            table.clear();
        }
    }
    return tables_.emplace(srcPath, std::move(table)).first->second;
}

void ModalSoundLibrary::SaveTable(const std::wstring& srcPath, const CookTable& table)
{
    if (!CookedCache::Enabled() || CookedCache::Sealed() || table.empty()) {
        return;
    }
    std::vector<uint8_t> payload;
    SerializeModalTable(table, payload);
    CookedCache::Write(srcPath, kMsfmExt, payload.data(), payload.size());
}

void ModalSoundLibrary::UpdateTableEntry(const std::wstring& srcPath, const std::string& meshName,
                                         const ModalFeatureMap& map)
{
    CookTable& table = LoadTable(srcPath);
    const auto hit = std::lower_bound(
        table.begin(), table.end(), meshName,
        [](const std::pair<std::string, ModalFeatureMap>& e, const std::string& k) {
            return e.first < k;
        });
    if (hit != table.end() && hit->first == meshName) {
        hit->second = map;
    } else {
        table.emplace(hit, meshName, map);
    }
    // 毎回は保存しない (reviewer round 1 指摘 3)。1 モデルが N 個のサブメッシュを持つとき
    // 焼くたびに SaveTable すると表全体 (N 枚ぶん) を N 回書き直す = O(N^2) のディスク I/O
    // になる (実測: 最終 15.1 MB の表に対し累積 約 1.09 GB)。dirty だけ立てて、まとめて書く。
    dirtyTables_.insert(srcPath);
    // reviewer round 2 指摘 4 + planner round 2 追補の規則 2「flush は損失の窓を区切る」:
    // Shutdown()/Clear()/--modal-bake の 3 箇所だけが flush 点だと、長時間の Editor
    // セッション中の異常終了で溜まった焼き結果が全部消える。かといって「N 件たまったら
    // 全ての dirty テーブルを flush」にすると、複数モデルを並行して焼いているときに
    // 無関係なモデルまで巻き込んで書き直す — 損失の単位を**モデル境界 (srcPath)** に揃え、
    // この srcPath 自身の更新が kFlushEveryNUpdates 件たまったらこの表**だけ**を flush する
    if (++dirtyUpdateCounts_[srcPath] >= kFlushEveryNUpdates) {
        FlushOneTable(srcPath);
    }
}

void ModalSoundLibrary::FlushOneTable(const std::wstring& srcPath)
{
    const auto it = tables_.find(srcPath);
    if (it != tables_.end()) {
        SaveTable(srcPath, it->second);
    }
    dirtyTables_.erase(srcPath);
    dirtyUpdateCounts_.erase(srcPath);
}

void ModalSoundLibrary::FlushDirtyTables()
{
    for (const std::wstring& srcPath : dirtyTables_) {
        const auto it = tables_.find(srcPath);
        if (it != tables_.end()) {
            SaveTable(srcPath, it->second);
        }
    }
    dirtyTables_.clear();
    dirtyUpdateCounts_.clear();
}

namespace modalsound {
namespace {
ModalSoundLibrary* g_lib = nullptr;
} // namespace

void Install(ModalSoundLibrary* lib)
{
    g_lib = lib;
}

ModalSoundLibrary* Library()
{
    return g_lib;
}

bool IsReady(AssetID mesh)
{
    return g_lib && g_lib->Get(mesh) != nullptr;
}

const DmNetHeader* Header()
{
    return g_lib ? g_lib->Header() : nullptr;
}

} // namespace modalsound

std::wstring ResolveDeepModalPath(const std::wstring& assetsRoot)
{
    std::error_code ec;
    if (!assetsRoot.empty()) {
        const fs::path projectPath = fs::path(assetsRoot) / L"deepmodal" / L"deepmodal.dmnet";
        if (fs::exists(projectPath, ec)) {
            return projectPath.wstring();
        }
    }
    const std::wstring engineDir = FindEngineDeepModalDir();
    if (!engineDir.empty()) {
        const fs::path enginePath = fs::path(engineDir) / L"deepmodal.dmnet";
        if (fs::exists(enginePath, ec)) {
            return enginePath.wstring();
        }
    }
    return {};
}

} // namespace mye
