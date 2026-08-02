#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mye::jobs {

// 決定論的タスク並列 (engine_spec.md 11 章、M25)。
//
// 決定論の担保:
//   - ParallelRanges は [0,total) を **連続 index レンジ**へ分割し、各レンジを独立に処理する。
//     どのワーカーがどのレンジを取るかは結果に影響しない (取得順非依存)。
//   - レンジ間に依存が無いこと (縮約は呼出側が index 順で結合) を呼出側が保証する。
//   - 戻り時点で全レンジ完了 = バリア。ハッシュ前に必ず join される。
//   - 無効化 / total 小 / ワーカー 0 のときは呼出スレッドで直列実行 (挙動不変)。
//
// 用途: TransformSystem (深度レベル毎)、フラスタムカリング等の「埋め込み並列」системの広幅化。
// sim 状態を書くレンジは互いに素な出力 (別エンティティ) であること。ネスト呼び出しは非対応。
class JobSystem {
public:
    void Init();     // ワーカー起動: min(16, cores-2)。0 以下なら直列専用
    void Shutdown(); // ワーカー停止 + join
    int WorkerCount() const { return static_cast<int>(workers_.size()); }

    void SetEnabled(bool e) { enabled_.store(e, std::memory_order_relaxed); }
    bool Enabled() const
    {
        return enabled_.load(std::memory_order_relaxed) && !workers_.empty();
    }

    // [0,total) を連続レンジに分割し fn(begin,end) で並列処理する。戻り時 = 全完了。
    void ParallelRanges(size_t total, size_t grain, const std::function<void(size_t, size_t)>& fn);
    // fn(i) を [0,total) で並列処理する (ParallelRanges の要素版ラッパ)。
    void ParallelFor(size_t total, size_t grain, const std::function<void(size_t)>& fn);

private:
    // 1 バッチぶんの不変な記述。ワーカーは mutex_ 下でこれをコピーしてから drain するので、
    // drain 中に呼出スレッドが次バッチを書いても掴んでいる値は動かない。
    struct Batch {
        const std::function<void(size_t, size_t)>* fn = nullptr;
        size_t total = 0;
        size_t chunkSize = 0;
        size_t chunkCount = 0;
        uint64_t base = 0; // cursor_ 上でこのバッチが占有する区間の先頭
    };

    void WorkerLoop();
    void DrainChunks(const Batch& b); // バッチ b の chunk を CAS で消化

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cvWake_; // ワーカー起床 (新バッチ)
    std::condition_variable cvDone_; // 完了通知 (呼出スレッドへ)

    // 現バッチ (ParallelRanges は同期呼び出しなので常に高々 1 バッチ)。mutex_ 保護
    Batch batch_;
    // chunk 取得カーソル。**バッチ間でリセットしない単調増加**で、各バッチは
    // [base, base+chunkCount) を排他占有する。前バッチに取り残されたワーカーが
    // 次バッチの chunk を掴む (= 破棄済みの fn を呼ぶ) 事故を構造的に防ぐため
    std::atomic<uint64_t> cursor_{ 0 };
    std::atomic<size_t> doneChunks_{ 0 };
    uint64_t batchGen_ = 0; // バッチ世代 (mutex_ 保護)
    bool stop_ = false;     // mutex_ 保護
    std::atomic<bool> enabled_{ true };
};

// プロセス全体で共有する既定インスタンス (prof:: と同じ運用)。
// EngineLoop が Init/Shutdown/SetEnabled を呼ぶ。
JobSystem& System();

} // namespace mye::jobs
