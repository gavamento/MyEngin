//====================================================================================
//                          NavBakeService.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュ・ベイクの非同期ワーカー実装
//====================================================================================
#include "Editor/Tools/NavBakeService.h"

#include <chrono>

namespace mye {

NavBakeService::~NavBakeService()
{
    Shutdown();
}

void NavBakeService::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!workerStarted_) {
            return;
        }
        workerStop_ = true;
        for (auto& entry : jobs_) {
            entry.second->control.cancel.store(true); // 終了を待たせない
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    workerStarted_ = false;
    workerStop_ = false;
    queue_.clear();
    jobs_.clear();
}

void NavBakeService::EnsureWorker()
{
    // mutex_ を保持して呼ぶ
    if (!workerStarted_) {
        workerStarted_ = true;
        worker_ = std::thread([this] { WorkerLoop(); });
    }
}

void NavBakeService::Request(uint64_t id, NavBakeInputs inputs)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(id);
        if (it != jobs_.end() && it->second->state == NavBakeJobState::Baking) {
            return;
        }
        auto job = std::make_shared<Job>();
        job->inputs = std::move(inputs);
        jobs_[id] = std::move(job);
        queue_.push_back(id);
        EnsureWorker();
    }
    cv_.notify_one();
}

NavBakeJobState NavBakeService::GetState(uint64_t id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? NavBakeJobState::None : it->second->state;
}

bool NavBakeService::GetProgress(uint64_t id, int& done, int& total) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return false;
    }
    done = it->second->control.tilesDone.load();
    total = it->second->control.tilesTotal.load();
    return true;
}

void NavBakeService::Cancel(uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it != jobs_.end() && it->second->state == NavBakeJobState::Baking) {
        it->second->control.cancel.store(true);
    }
}

bool NavBakeService::TakeResult(uint64_t id, NavBakeResult& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second->state != NavBakeJobState::Ready) {
        return false;
    }
    out = std::move(it->second->result);
    jobs_.erase(it);
    return true;
}

void NavBakeService::WorkerLoop()
{
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return workerStop_ || !queue_.empty(); });
            if (workerStop_) {
                return;
            }
            const uint64_t id = queue_.front();
            queue_.pop_front();
            const auto it = jobs_.find(id);
            if (it == jobs_.end()) {
                continue;
            }
            job = it->second;
        }
        // 重い処理は mutex の外。job は shared_ptr なので、途中で jobs_ から外れても生きている
        const auto begin = std::chrono::steady_clock::now();
        job->result.output = NavBakeAsset(job->inputs.config, job->inputs.soup, &job->control);
        job->result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::lock_guard<std::mutex> lock(mutex_);
        job->state = NavBakeJobState::Ready;
    }
}

} // namespace mye
