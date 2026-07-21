#include "Engine/Core/JobSystem.h"

#include <algorithm>

namespace mye::jobs {

void JobSystem::Init()
{
    if (!workers_.empty()) {
        return;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    int n = static_cast<int>(hw) - 2; // 2 コアはメイン + OS 用に残す
    n = std::min(n, 16);
    if (n < 0) {
        n = 0;
    }
    stop_ = false;
    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

void JobSystem::Shutdown()
{
    if (workers_.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
        ++batchGen_;
    }
    cvWake_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

// 現バッチの chunk を atomic カーソルで奪い合いながら処理する。
void JobSystem::DrainChunks()
{
    for (;;) {
        const size_t c = nextChunk_.fetch_add(1, std::memory_order_relaxed);
        if (c >= chunkCount_) {
            break;
        }
        const size_t begin = c * chunkSize_;
        const size_t end = std::min(begin + chunkSize_, total_);
        (*fn_)(begin, end);
        doneChunks_.fetch_add(1, std::memory_order_release);
    }
}

void JobSystem::WorkerLoop()
{
    uint64_t localGen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cvWake_.wait(lk, [this, localGen] { return batchGen_ != localGen; });
            localGen = batchGen_;
            if (stop_) {
                return;
            }
        }
        DrainChunks();
        // 完了を呼出スレッドへ通知 (mutex を一度取ることで lost-wakeup を防ぐ)
        {
            std::lock_guard<std::mutex> lk(mutex_);
        }
        cvDone_.notify_all();
    }
}

void JobSystem::ParallelRanges(size_t total, size_t grain,
                               const std::function<void(size_t, size_t)>& fn)
{
    if (total == 0) {
        return;
    }
    if (grain == 0) {
        grain = 1;
    }
    const size_t maxChunks = (total + grain - 1) / grain;
    size_t chunks = std::min<size_t>(workers_.size() + 1, maxChunks);
    if (!enabled_.load(std::memory_order_relaxed) || workers_.empty() || chunks <= 1) {
        fn(0, total); // 直列 (挙動は並列と同一)
        return;
    }

    // バッチ状態を確定 (batchGen_ をインクリメントする前に全て書く)
    fn_ = &fn;
    total_ = total;
    chunkCount_ = chunks;
    chunkSize_ = (total + chunks - 1) / chunks;
    nextChunk_.store(0, std::memory_order_relaxed);
    doneChunks_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++batchGen_; // ワーカーを起こす境界
    }
    cvWake_.notify_all();

    DrainChunks(); // 呼出スレッドも chunk を消化

    // 全 chunk 完了を待つ (バリア)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cvDone_.wait(lk, [this] {
            return doneChunks_.load(std::memory_order_acquire) >= chunkCount_;
        });
    }
    fn_ = nullptr;
}

void JobSystem::ParallelFor(size_t total, size_t grain, const std::function<void(size_t)>& fn)
{
    ParallelRanges(total, grain, [&fn](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            fn(i);
        }
    });
}

JobSystem& System()
{
    static JobSystem s;
    return s;
}

} // namespace mye::jobs
