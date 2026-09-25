//====================================================================================
//                          FractureBakeService.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片焼き非同期ワーカーの実装 (M80i)
//====================================================================================
#include "Editor/FractureBakeService.h"

#include <utility>

namespace mye {

void FractureBakeService::OnBakeProgress(FractureBakeStage stage, void* userData)
{
    auto* self = static_cast<FractureBakeService*>(userData);
    self->currentStage_.store(static_cast<int32_t>(stage), std::memory_order_relaxed);
}

FractureBakeService::~FractureBakeService()
{
    Shutdown();
}

void FractureBakeService::Request(uint64_t id, FractureBakeRequest request)
{
    Entry& entry = entries_[id];
    if (entry.state == FractureBakeJobState::Baking) {
        return; // 二重投入しない (ボタンは Baking 中 disabled にする想定だが念のため)
    }
    entry.state = FractureBakeJobState::Baking;
    entry.request = request;
    entry.result = FractureBakeResult{};

    EnsureWorker();
    Job job;
    job.id = id;
    job.request = std::move(request);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        jobQueue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

FractureBakeJobState FractureBakeService::GetState(uint64_t id) const
{
    const auto it = entries_.find(id);
    return it != entries_.end() ? it->second.state : FractureBakeJobState::None;
}

FractureBakeStage FractureBakeService::GetStage(uint64_t id) const
{
    if (hasCurrentJob_.load(std::memory_order_relaxed)
        && currentJobId_.load(std::memory_order_relaxed) == id) {
        return static_cast<FractureBakeStage>(currentStage_.load(std::memory_order_relaxed));
    }
    return FractureBakeStage::ClosedCheck; // キュー待ち中の既定表示
}

bool FractureBakeService::TakeResult(uint64_t id, FractureBakeRequest& requestOut,
                                     FractureBakeResult& resultOut)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state != FractureBakeJobState::Ready) {
        return false;
    }
    requestOut = std::move(it->second.request);
    resultOut = std::move(it->second.result);
    entries_.erase(it);
    return true;
}

void FractureBakeService::EnsureWorker()
{
    if (workerStarted_) {
        return;
    }
    workerStarted_ = true;
    worker_ = std::thread([this] { WorkerLoop(); });
}

void FractureBakeService::WorkerLoop()
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
        }

        currentJobId_.store(job.id, std::memory_order_relaxed);
        currentStage_.store(static_cast<int32_t>(FractureBakeStage::ClosedCheck),
                            std::memory_order_relaxed);
        hasCurrentJob_.store(true, std::memory_order_relaxed);

        FractureBakeInput in;
        in.sourceMesh = job.request.sourceMesh;
        in.seed = job.request.seed;
        in.pieceCount = job.request.pieceCount;
        in.openMeshMode = job.request.openMeshMode;
        in.voxelResolution = job.request.voxelResolution;
        in.progress = &FractureBakeService::OnBakeProgress;
        in.progressUserData = this;

        JobResult r;
        r.id = job.id;
        r.request = job.request;
        BakeFracture(in, r.result); // 戻り値は result.success と同じ意味なので見なくてよい

        hasCurrentJob_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            doneQueue_.push_back(std::move(r));
        }
    }
}

void FractureBakeService::Pump()
{
    std::deque<JobResult> done;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        done.swap(doneQueue_);
    }
    for (JobResult& r : done) {
        Entry& entry = entries_[r.id];
        // Baking 中でない (Request 後に別の要求で上書きされた等) は起こらない想定だが、
        // 念のため id が生きている間だけ結果を反映する
        entry.state = FractureBakeJobState::Ready;
        entry.request = std::move(r.request);
        entry.result = std::move(r.result);
    }
}

void FractureBakeService::Shutdown()
{
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

} // namespace mye
