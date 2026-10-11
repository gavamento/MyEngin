// 自動生成 (tools\gen_project_files.ps1)。手編集しても次の実行で上書きされる。
// assets\shaders\kernels\*.kernel.hlsl を GameLogic.dll の CPU 版として取り込み、カーネル表を作る。
// .kernel.hlsl を編集したら gen_project_files.ps1 を実行して GameLogic を再ビルドすること
// (表のハッシュとディスク上のファイルが食い違うと、エンジンは CPU 版を古いものとして扱う)。
#include "GameLogic/Generated/Kernels.gen.h"
#include "Shared/KernelRangeFn.h"

namespace {

// SelfTestMix
constexpr const char* const kSelfTestMix_argNames[] = { "outv", "probes" };
constexpr auto kSelfTestMix_args = ::mye::kernel::MakeKernelArgs<&::mye::kernel::cpu::SelfTestMix::SelfTestMix_Cpu>(kSelfTestMix_argNames);
const ::mye_script_detail::KernelRegistrar kSelfTestMix_registrar(
    ::mye::kernel::MakeKernelDesc<&::mye::kernel::cpu::SelfTestMix::SelfTestMix_Cpu>("SelfTestMix", 0x86ae184af0d3195dull, kSelfTestMix_args.data()));

// SelfTestSaxpy
constexpr const char* const kSelfTestSaxpy_argNames[] = { "y", "x" };
constexpr auto kSelfTestSaxpy_args = ::mye::kernel::MakeKernelArgs<&::mye::kernel::cpu::SelfTestSaxpy::SelfTestSaxpy_Cpu>(kSelfTestSaxpy_argNames);
const ::mye_script_detail::KernelRegistrar kSelfTestSaxpy_registrar(
    ::mye::kernel::MakeKernelDesc<&::mye::kernel::cpu::SelfTestSaxpy::SelfTestSaxpy_Cpu>("SelfTestSaxpy", 0xef113f8ae00cabf1ull, kSelfTestSaxpy_args.data()));

// SelfTestUnused
constexpr const char* const kSelfTestUnused_argNames[] = { "outv", "a", "skipped", "c" };
constexpr auto kSelfTestUnused_args = ::mye::kernel::MakeKernelArgs<&::mye::kernel::cpu::SelfTestUnused::SelfTestUnused_Cpu>(kSelfTestUnused_argNames);
const ::mye_script_detail::KernelRegistrar kSelfTestUnused_registrar(
    ::mye::kernel::MakeKernelDesc<&::mye::kernel::cpu::SelfTestUnused::SelfTestUnused_Cpu>("SelfTestUnused", 0xb3632114666c08aaull, kSelfTestUnused_args.data()));

} // namespace
