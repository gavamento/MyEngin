//====================================================================================
//                          KernelRange.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          型を消したレンジ関数 (C++ 版・C# 版共通) と、その実行器
//====================================================================================
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Engine/Engine/Compute/KernelRunner.h"

namespace mye::kernel {

// カーネルに渡す 1 本のバッファ。ABI に出す形 (sub-05 / sub-06) と同じ POD
struct KernelBufferRef {
    void* data = nullptr;
    uint32_t count = 0;
    uint32_t stride = 0;
};

// [begin, end) の id を処理する。params は P の先頭、bufs は宣言順 (RW / RO 混在)
using KernelRangeFn = void (*)(const void* params, const KernelBufferRef* bufs, uint32_t bufCount,
                               uint32_t begin, uint32_t end);

namespace detail {

template <class V> struct ViewOf;
template <class T> struct ViewOf<RWView<T>> {
    static RWView<T> Make(const KernelBufferRef& b) { return RWView<T>(static_cast<T*>(b.data), b.count); }
};
template <class T> struct ViewOf<ROView<T>> {
    static ROView<T> Make(const KernelBufferRef& b) { return ROView<T>(static_cast<const T*>(b.data), b.count); }
};

template <class Fn> struct RangeTraits;
template <class P, class... Views> struct RangeTraits<void (*)(KernelId, const P&, Views...)> {
    static constexpr size_t kArgCount = sizeof...(Views);

    template <void (*Fn)(KernelId, const P&, Views...), size_t... I>
    static void RunIndexed(const void* params, const KernelBufferRef* bufs, uint32_t begin, uint32_t end,
                           std::index_sequence<I...>)
    {
        const P& p = *static_cast<const P*>(params);
        const std::tuple<Views...> views{ ViewOf<Views>::Make(bufs[I])... };
        for (uint32_t i = begin; i < end; ++i) {
            Fn(KernelLauncher::Make(i), p, std::get<I>(views)...);
        }
    }

    template <void (*Fn)(KernelId, const P&, Views...)>
    static void Thunk(const void* params, const KernelBufferRef* bufs, uint32_t bufCount, uint32_t begin,
                      uint32_t end)
    {
        if (bufCount != kArgCount) {
            return;
        }
        RunIndexed<Fn>(params, bufs, begin, end, std::make_index_sequence<kArgCount>{});
    }
};

} // namespace detail

// 生成された <Name>_Cpu を型を消したレンジ関数にする。使い方: MakeRangeFn<&ns::Foo_Cpu>()
template <auto Fn> constexpr KernelRangeFn MakeRangeFn()
{
    return &detail::RangeTraits<decltype(Fn)>::template Thunk<Fn>;
}

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
