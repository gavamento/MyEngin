//====================================================================================
//                          KernelRange.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          型を消したレンジ関数 (C++ 版・C# 版共通) の実行器
//====================================================================================
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "Engine/Engine/Compute/KernelRunner.h"
#include "Shared/KernelRangeFn.h" // KernelBufferRef / KernelRangeFn / MakeRangeFn (GameLogic.dll と共有)

namespace mye::kernel {

// レンジ関数を [0, elementCount) の全 id に対して JobSystem で回す。同期で、戻り時に全要素が完了している。
// RunKernelCpu と同じ分割 (8 要素境界) なので、結果は chunk 数・jobs の有無に依らない。
// バッファの個数・stride・長さの検査は呼び出し側 (ComputeService) の責任
inline RunStatus RunKernelRange(jobs::JobSystem& js, KernelRangeFn fn, const void* params,
                                const KernelBufferRef* bufs, uint32_t bufCount, uint32_t elementCount,
                                const RunOptions& options)
{
    if (jobs::JobSystem::InJob()) {
        return RunStatus::InJob;
    }
    if (elementCount > kMaxKernelElements) {
        return RunStatus::TooManyElements;
    }
    if (elementCount == 0) {
        return RunStatus::Ok;
    }
    const size_t blocks = (static_cast<size_t>(elementCount) + kKernelLaneBlock - 1) / kKernelLaneBlock;
    size_t grain = std::max<size_t>(options.minBlocksPerChunk, 1);
    if (options.maxChunks > 0) {
        grain = std::max(grain, (blocks + options.maxChunks - 1) / options.maxChunks);
    }
    js.ParallelRanges(blocks, grain, [&](size_t blockBegin, size_t blockEnd) {
        const size_t begin = blockBegin * kKernelLaneBlock;
        const size_t end = std::min(blockEnd * kKernelLaneBlock, static_cast<size_t>(elementCount));
        fn(params, bufs, bufCount, static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
    });
    return RunStatus::Ok;
}

} // namespace mye::kernel
