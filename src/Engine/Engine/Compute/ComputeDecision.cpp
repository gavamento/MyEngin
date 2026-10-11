//====================================================================================
//                          ComputeDecision.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach の実行先を決める純粋関数の実装
//====================================================================================
#include "Engine/Engine/Compute/ComputeDecision.h"

namespace mye::compute {
namespace {

Decision Fail(FailReason reason)
{
    Decision d;
    d.kind = DecisionKind::Failed;
    d.reason = reason;
    return d;
}

Decision Run(DecisionKind kind, DecisionNote note = DecisionNote::None)
{
    Decision d;
    d.kind = kind;
    d.note = note;
    return d;
}

} // namespace

Decision DecideExecution(const DecisionInput& in)
{
    // 1. 宣言。他のすべての判定より先 (U10)
    if (!in.callerIsScript || !in.declared) {
        return Fail(FailReason::NotDeclared);
    }
    // 2. カーネル自身の健全性。違反したカーネルはどの実行先でも使わない
    if (in.lint == LintState::Unknown) {
        return Fail(FailReason::UnknownKernel);
    }
    if (in.lint == LintState::Violation) {
        return Fail(FailReason::LintViolation);
    }
    if (in.writesStateReadsFx) {
        return Fail(FailReason::MixedKinds);
    }
    const bool cpuUsable = in.cpuVersion && in.hashMatch;
    const FailReason cpuFail = !in.cpuVersion ? FailReason::NoCpuVersion : FailReason::HashMismatch;

    // 3. State を含む: 常に CPU (resim でも走る)
    if (in.hasState) {
        if (in.execution == Execution::Gpu) {
            return Fail(FailReason::StateGpuRejected);
        }
        return cpuUsable ? Run(DecisionKind::Cpu) : Fail(cpuFail);
    }

    // 4. 全部 Fx
    if (in.resim) {
        return Run(DecisionKind::Skipped);
    }
    Execution ex = in.execution;
    if (in.forceCpu) {
        ex = Execution::Cpu;
    } else if (ex == Execution::Auto && in.overrideCpu) {
        ex = Execution::Cpu;
    }
    switch (ex) {
    case Execution::Cpu:
        return cpuUsable ? Run(DecisionKind::Cpu) : Fail(cpuFail);
    case Execution::Gpu:
        if (in.deviceAvailable) {
            return Run(DecisionKind::Gpu);
        }
        return cpuUsable ? Run(DecisionKind::Cpu, DecisionNote::SubstitutedByCpu) : Fail(FailReason::NoDevice);
    case Execution::Auto:
        break;
    }
    // Auto: 下限と枠に収まるなら GPU。CPU 版が古いときは GPU を選ばず、CPU に落ちる。
    // CPU 版が無いカーネルは選べる先が GPU しか無いので、下限と枠は見ない (GPU か CPU かを選ぶための値だから)
    const bool gpuOk = in.deviceAvailable
        && (!in.cpuVersion || (in.hashMatch && in.elementCount >= in.gpuMinElements && in.gpuBudgetOk));
    if (gpuOk) {
        return Run(DecisionKind::Gpu);
    }
    if (cpuUsable) {
        return Run(DecisionKind::Cpu);
    }
    if (in.deviceAvailable && in.cpuVersion && !in.hashMatch) {
        return Run(DecisionKind::Gpu, DecisionNote::HashMismatchGpu);
    }
    return Fail(in.cpuVersion ? FailReason::HashMismatch : FailReason::NoCpuVersion);
}

const char* FailReasonName(FailReason r)
{
    switch (r) {
    case FailReason::None: return "none";
    case FailReason::NotDeclared: return "not-declared";
    case FailReason::UnknownKernel: return "unknown-kernel";
    case FailReason::LintViolation: return "lint-violation";
    case FailReason::MixedKinds: return "state-reads-fx";
    case FailReason::StateGpuRejected: return "state-gpu-rejected";
    case FailReason::NoCpuVersion: return "no-cpu-version";
    case FailReason::HashMismatch: return "hash-mismatch";
    case FailReason::NoDevice: return "no-device";
    case FailReason::BadArgs: return "bad-args";
    case FailReason::TooManyElements: return "too-many-elements";
    case FailReason::InJob: return "in-job";
    case FailReason::GpuDispatchFailed: return "gpu-dispatch-failed";
    case FailReason::CpuRunFailed: return "cpu-run-failed";
    }
    return "?";
}

} // namespace mye::compute
