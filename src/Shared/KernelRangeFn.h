//====================================================================================
//                          KernelRangeFn.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネルの CPU 版を型を消したレンジ関数にし、カーネル表 (ABI) の 1 行を作る (GameLogic 側)
//====================================================================================
#pragma once
// Kernels.gen.cpp (tools\gen_project_files.ps1 の生成物) が、カーネルの <Name>_Cpu を ABI のレンジ関数と
// MyeKernelDesc に変えるために使う。JobSystem などエンジンの型には依存しない (GameLogic.dll にも入る)。
// 実行器 (レンジの分割と並列実行) はエンジン側: Engine/Compute/KernelRange.h。
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

#include "Shared/KernelCompat.h"
#include "Shared/ScriptTypes.h"

namespace mye::kernel {

// ABI に出る POD と同じもの (data / count / stride)
using KernelBufferRef = ::MyeKernelBuffer;
// [begin, end) の id を処理する。params は P の先頭、bufs は宣言順 (RW / RO 混在)
using KernelRangeFn = ::MyeKernelRangeFn;

namespace detail {

template <class V> struct ViewOf;
template <class T> struct ViewOf<RWView<T>> {
    static constexpr uint32_t kStride = static_cast<uint32_t>(sizeof(T));
    static constexpr uint32_t kWritable = 1;
    static RWView<T> Make(const KernelBufferRef& b) { return RWView<T>(static_cast<T*>(b.data), b.count); }
};
template <class T> struct ViewOf<ROView<T>> {
    static constexpr uint32_t kStride = static_cast<uint32_t>(sizeof(T));
    static constexpr uint32_t kWritable = 0;
    static ROView<T> Make(const KernelBufferRef& b) { return ROView<T>(static_cast<const T*>(b.data), b.count); }
};

template <class Fn> struct RangeTraits;
template <class P, class... Views> struct RangeTraits<void (*)(KernelId, const P&, Views...)> {
    static constexpr size_t kArgCount = sizeof...(Views);
    static constexpr uint32_t kParamsSize = static_cast<uint32_t>(sizeof(P));
    static constexpr uint32_t kStrides[] = { ViewOf<Views>::kStride... };
    static constexpr uint32_t kWritable[] = { ViewOf<Views>::kWritable... };

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

template <class Traits, size_t N, size_t... I>
constexpr std::array<MyeKernelArg, N> MakeArgs(const char* const (&names)[N], std::index_sequence<I...>)
{
    return { MyeKernelArg{ names[I], Traits::kStrides[I], Traits::kWritable[I] }... };
}

} // namespace detail

// 生成された <Name>_Cpu を型を消したレンジ関数にする。使い方: MakeRangeFn<&ns::Foo_Cpu>()
template <auto Fn> constexpr KernelRangeFn MakeRangeFn()
{
    return &detail::RangeTraits<decltype(Fn)>::template Thunk<Fn>;
}

// <Name>_Cpu の RW / RO の並びと stride から引数表を作る。names は HLSL の変数名 (宣言順、数は RW / RO の数と一致)
template <auto Fn, size_t N> constexpr std::array<MyeKernelArg, N> MakeKernelArgs(const char* const (&names)[N])
{
    using Traits = detail::RangeTraits<decltype(Fn)>;
    static_assert(N == Traits::kArgCount, "引数名の数が、カーネルの RW / RO の数と合わない");
    return detail::MakeArgs<Traits>(names, std::make_index_sequence<N>{});
}

// カーネル表の 1 行。args は MakeKernelArgs の結果 (静的な寿命で保持すること)
template <auto Fn> constexpr MyeKernelDesc MakeKernelDesc(const char* name, uint64_t sourceHash, const MyeKernelArg* args)
{
    using Traits = detail::RangeTraits<decltype(Fn)>;
    return MyeKernelDesc{ name, sourceHash, Traits::kParamsSize, static_cast<uint32_t>(Traits::kArgCount), args,
                          MakeRangeFn<Fn>() };
}

} // namespace mye::kernel
