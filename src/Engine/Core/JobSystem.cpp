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

// バッチ b の chunk をカーソル CAS で奪い合いながら処理する。
//
// cursor_ はバッチ間でリセットせず単調増加させ、バッチ b は [b.base, b.base+b.chunkCount)
// だけを消費する。取り残されたワーカー (バリア通過後もまだこのループにいるスレッド) は
// cursor_ が自分の区間を超えているのを見て抜けるだけで、次バッチの chunk は掴めない。
// fetch_add ではなく CAS なのは、掴めない時にカーソルを進めない (= 次バッチの chunk を
// 食い逃げしない) ため。
void JobSystem::DrainChunks(const Batch& b)
{
    if (b.fn == nullptr || b.chunkCount == 0) {
        return;
    }
    const uint64_t last = b.base + b.chunkCount;
    for (;;) {
        uint64_t cur = cursor_.load(std::memory_order_relaxed);
        if (cur >= last) {
            break; // このバッチの chunk は尽きた (or 既に次バッチへ移っている)
        }
        if (!cursor_.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
            continue;
        }
        const size_t c = static_cast<size_t>(cur - b.base);
        const size_t begin = c * b.chunkSize;
        const size_t end = std::min(begin + b.chunkSize, b.total);
        (*b.fn)(begin, end);
        doneChunks_.fetch_add(1, std::memory_order_release);
    }
}

void JobSystem::WorkerLoop()
{
    uint64_t localGen = 0;
    for (;;) {
        Batch b;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cvWake_.wait(lk, [this, localGen] { return batchGen_ != localGen; });
            localGen = batchGen_;
            if (stop_) {
                return;
            }
            b = batch_; // ロック下でスナップショット (以後 batch_ は触らない)
        }
        DrainChunks(b);
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

    // バッチ状態は mutex_ 下で確定する。ワーカーも mutex_ 下でコピーするので、
    // これらのフィールドにデータ競合は無い (以前は素の書き込みで、drain 中のワーカーが
    // 破棄済みの fn_ を読んで落ちていた)
    Batch b;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        batch_.fn = &fn;
        batch_.total = total;
        batch_.chunkCount = chunks;
        batch_.chunkSize = (total + chunks - 1) / chunks;
        // 直前バッチの終端 = 現在のカーソル。以降このバッチが chunkCount ぶんを占有する
        batch_.base = cursor_.load(std::memory_order_relaxed);
        doneChunks_.store(0, std::memory_order_relaxed);
        ++batchGen_; // ワーカーを起こす境界
        b = batch_;
    }
    cvWake_.notify_all();

    DrainChunks(b); // 呼出スレッドも chunk を消化 (最低でも呼出スレッドが全部消化しきる)

    // 全 chunk 完了を待つ (バリア)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cvDone_.wait(lk, [this, &b] {
            return doneChunks_.load(std::memory_order_acquire) >= b.chunkCount;
        });
        batch_.fn = nullptr; // 破棄される fn への参照を残さない
    }
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
