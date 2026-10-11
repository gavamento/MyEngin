// 自動生成 (tools\gen_project_files.ps1)。手編集しても次の実行で上書きされる。
// assets\shaders\kernels\*.kernel.hlsl を GameLogic.dll の CPU 版として取り込み、カーネル表を作る。
// .kernel.hlsl を編集したら gen_project_files.ps1 を実行して GameLogic を再ビルドすること
// (表のハッシュとディスク上のファイルが食い違うと、エンジンは CPU 版を古いものとして扱う)。
#pragma once
#include "Shared/KernelCompat.h"

#pragma warning(push)
#pragma warning(disable : 4100 4505)
namespace mye::kernel::cpu::SelfTestMix {
namespace {
#include "../../../assets/shaders/kernels/SelfTestMix.kernel.hlsl"
} // namespace
}
#pragma warning(pop)
namespace MyeKernels::SelfTestMix { using namespace ::mye::kernel::cpu::SelfTestMix; }

#pragma warning(push)
#pragma warning(disable : 4100 4505)
namespace mye::kernel::cpu::SelfTestSaxpy {
namespace {
#include "../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
} // namespace
}
#pragma warning(pop)
namespace MyeKernels::SelfTestSaxpy { using namespace ::mye::kernel::cpu::SelfTestSaxpy; }

#pragma warning(push)
#pragma warning(disable : 4100 4505)
namespace mye::kernel::cpu::SelfTestUnused {
namespace {
#include "../../../assets/shaders/kernels/SelfTestUnused.kernel.hlsl"
} // namespace
}
#pragma warning(pop)
namespace MyeKernels::SelfTestUnused { using namespace ::mye::kernel::cpu::SelfTestUnused; }
