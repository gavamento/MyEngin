//====================================================================================
//                          KernelRunner.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          C++ の CPU 版カーネルの実行器 (JobSystem でレンジ分割)
//====================================================================================
#pragma once
#include <algorithm>
#include <cstddef>

#include "Engine/Core/Jobs/JobSystem.h"
#include "Shared/KernelCompat.h"

namespace mye::kernel {

// カーネル 1 回あたりの要素数の上限 (65535 グループ x 64 スレッドから 1 グループぶん引いた値)
inline constexpr size_t kMaxKernelElements = 4194240;
// レンジ分割の単位。要素番号 8 の倍数の境界でしか割らない (将来 SIMD 化しても分割点が動かない)
inline constexpr size_t kKernelLaneBlock = 8;

enum class RunStatus {
    Ok,
    InJob,            // ジョブの中から呼ばれた (ネスト並列は非対応)
    TooManyElements,  // 第 1 引数の RW が kMaxKernelElements を超える
    BadBuffer,        // 2 本目以降の RW が第 1 引数より短い (範囲外書き込みになる)
};

struct RunOptions {
    size_t maxChunks = 0;          // 0 = 制限なし。cpuMaxChunks (負荷枠) に相当
    size_t minBlocksPerChunk = 16; // 1 chunk の最小サイズ (8 要素 x 16 = 128 要素)
};

// KernelId を作れる唯一の場所
struct KernelLauncher {
    static KernelId Make(uint v) { return KernelId(v); }
};

namespace detail {

template <class T> bool WriteViewFits(const RWView<T>& v, size_t count) { return v.Count() >= count; }
template <class T> bool WriteViewFits(const ROView<T>&, size_t) { return true; }

} // namespace detail

// C++ の CPU 版を [0, 第 1 引数の要素数) の全 id に対して実行する。同期で、戻り時に全要素が完了している。
//
// 決定論: レンジは 8 要素境界で割り、要素ごとの結果は chunk 数・ワーカー数・jobs の有無に依らない
// (カーネルが「自分の要素だけを書く」約束を守る限り)。ジョブの中から呼ぶと何もせず InJob を返す。
template <class P, class First, class... Rest>
RunStatus RunKernelCpu(jobs::JobSystem& js, void (*fn)(KernelId, const P&, RWView<First>, Rest...),
                       const P& params, const RunOptions& options, RWView<First> first, Rest... rest)
{
    if (jobs::JobSystem::InJob()) {
        return RunStatus::InJob;
    }
    const size_t count = first.Count();
    if (count > kMaxKernelElements) {
        return RunStatus::TooManyElements;
    }
    if (!(detail::WriteViewFits(rest, count) && ...)) {
        return RunStatus::BadBuffer;
    }
    if (count == 0) {
        return RunStatus::Ok;
    }

    const size_t blocks = (count + kKernelLaneBlock - 1) / kKernelLaneBlock;
    size_t grain = std::max<size_t>(options.minBlocksPerChunk, 1);
    if (options.maxChunks > 0) {
        grain = std::max(grain, (blocks + options.maxChunks - 1) / options.maxChunks);
    }
    js.ParallelRanges(blocks, grain, [&](size_t blockBegin, size_t blockEnd) {
        const size_t begin = blockBegin * kKernelLaneBlock;
        const size_t end = std::min(blockEnd * kKernelLaneBlock, count);
        for (size_t i = begin; i < end; ++i) {
            fn(KernelLauncher::Make(static_cast<uint>(i)), params, first, rest...);
        }
    });
    return RunStatus::Ok;
}

} // namespace mye::kernel
