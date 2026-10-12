//====================================================================================
//                          ComputeInspectorRows.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute 欄の行を宣言表・観測の対応表・ソース表から組み立てる
//====================================================================================
#include "Engine/Engine/Compute/ComputeInspectorRows.h"

#include <set>

namespace mye::compute {
namespace {

RowTarget TargetOf(const KernelObservation& obs)
{
    if (obs.calls == 0) {
        return RowTarget::None;
    }
    switch (obs.lastKind) {
    case DecisionKind::Failed: return RowTarget::Failed;
    case DecisionKind::Skipped: return RowTarget::Skipped;
    case DecisionKind::Gpu: return RowTarget::Gpu;
    case DecisionKind::Cpu: return obs.lastNote == DecisionNote::SubstitutedByCpu ? RowTarget::SubstitutedCpu : RowTarget::Cpu;
    }
    return RowTarget::None;
}

RowVersion VersionOf(const ComputeService& service, CallerKind lane, const std::string& kernel)
{
    const KernelSourceInfo* source = service.FindKernelSource(kernel);
    if (source == nullptr) {
        return RowVersion::Unknown;
    }
    const ComputeKernelDesc* desc = service.FindKernel(kernel);
    const CpuImpl* impl = desc == nullptr ? nullptr : (lane == CallerKind::Cpp ? &desc->cpp : &desc->csharp);
    if (impl == nullptr || impl->range == nullptr) {
        return RowVersion::NoCpuVersion;
    }
    return impl->sourceHash == source->hash ? RowVersion::Match : RowVersion::Mismatch;
}

} // namespace

std::vector<ComputeInspectorRow> BuildComputeInspectorRows(const ComputeService& service, CallerKind lane,
                                                           std::string_view scriptType)
{
    std::set<std::string> names;
    for (const std::string& name : service.DeclaredKernels(lane, scriptType)) {
        names.insert(name);
    }
    // 観測は (スクリプト型, カーネル名) の昇順に並んでいるので、型が一致する範囲だけ拾う
    for (const auto& kv : service.Observations()) {
        if (kv.first.scriptType == scriptType) {
            names.insert(kv.first.kernel);
        }
    }

    std::vector<ComputeInspectorRow> rows;
    rows.reserve(names.size());
    for (const std::string& name : names) {
        ComputeInspectorRow row;
        row.kernel = name;
        row.declared = service.IsDeclared(lane, scriptType, name);
        row.overrideCpu = service.IsKernelOverrideCpu(name);
        row.version = VersionOf(service, lane, name);
        const KernelObservation* obs = service.FindObservation(scriptType, name);
        if (obs != nullptr) {
            row.target = TargetOf(*obs);
            row.lastFail = obs->lastFail;
            row.elements = obs->lastElements;
            row.uploadBytes = obs->lastUploadBytes;
            row.readbackBytes = obs->lastReadbackBytes;
            row.cpuMs = obs->lastCpuMs;
            row.calls = obs->calls;
            row.failures = obs->failures;
            row.gpuDispatches = obs->gpuDispatches;
        }
        const KernelSourceInfo* source = service.FindKernelSource(name);
        if (!row.declared) {
            row.state = RowState::FailedNotDeclared;
        } else if (source == nullptr) {
            row.state = RowState::DeclaredNoKernel;
        } else if (source->lint == LintState::Violation) {
            row.state = RowState::LintViolation;
            row.violation = source->conflict.empty() ? source->violation : source->conflict;
        } else {
            row.state = row.calls > 0 ? RowState::Ran : RowState::NotRun;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace mye::compute
