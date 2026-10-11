//====================================================================================
//                          ComputeService.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach のエンジン核の実装
//====================================================================================
#include "Engine/Engine/Compute/ComputeService.h"

#include <chrono>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Compute/KernelRunner.h"
#include "Engine/Renderer/Compute/FxGpuBackend.h"

namespace mye::compute {

struct ComputeService::Resolved {
    ComputeBufferStore* store = nullptr;
    std::vector<ComputeBufferStore::Slot*> slots; // 引数の並び
    std::vector<uint64_t> handles;
    uint32_t elementCount = 0;
    bool hasState = false;
    bool writesStateReadsFx = false;
    uint64_t pendingUploadBytes = 0; // GPU で走らせるときに写す量 (枠の判定用)
};

namespace {

double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

ComputeService::ComputeService()
    : gameStore_(StoreLane::Game)
    , scriptStore_(StoreLane::Script)
    , gpu_(std::make_unique<FxGpuBackend>())
{
    jobs_ = &jobs::System();
}

ComputeService::~ComputeService() = default;

void ComputeService::Bind(GraphicsDevice* device, ShaderManager* shaders, jobs::JobSystem* js)
{
    gpu_->Bind(device, shaders);
    jobs_ = js != nullptr ? js : &jobs::System();
}

void ComputeService::SetConfig(const ComputeConfig& config)
{
    config_ = config;
    gameStore_.SetStateTotalBytes(config.stateTotalBytes);
    scriptStore_.SetStateTotalBytes(config.stateTotalBytes);
}

void ComputeService::BeginTick(bool resim)
{
    resim_ = resim;
    tickDispatches_ = 0;
    tickUploadBytes_ = 0;
}

bool ComputeService::RegisterKernel(const ComputeKernelDesc& desc)
{
    uint32_t rw = 0;
    uint32_t ro = 0;
    bool ok = !desc.name.empty() && desc.paramsSize > 0 && (desc.paramsSize & 3u) == 0 && !desc.args.empty()
        && desc.args[0].writable;
    for (const KernelArgDesc& a : desc.args) {
        ok = ok && !a.name.empty() && a.stride > 0 && (a.stride & 3u) == 0;
        (a.writable ? rw : ro) += 1;
    }
    ok = ok && rw <= FxGpuBackend::kMaxReadWrite && ro <= FxGpuBackend::kMaxReadOnly;
    if (!ok) {
        MYE_LOG_ERROR("[compute] kernel '%s' has an invalid descriptor and was not registered", desc.name.c_str());
        return false;
    }
    kernels_[desc.name] = desc;
    return true;
}

void ComputeService::ClearKernels()
{
    kernels_.clear();
}

void ComputeService::ReplaceDeclarations(std::map<std::string, std::set<std::string>> declarations)
{
    declarations_.clear();
    for (auto& kv : declarations) {
        declarations_.emplace(kv.first, std::move(kv.second));
    }
}

bool ComputeService::IsDeclared(std::string_view scriptType, std::string_view kernel) const
{
    const auto it = declarations_.find(scriptType);
    return it != declarations_.end() && it->second.find(std::string(kernel)) != it->second.end();
}

void ComputeService::UpdateKernelSource(const std::string& fileName, std::string_view text)
{
    const std::shared_ptr<const kernel::KernelLintResult> result = lintCache_.Get(fileName, text);
    KernelSourceInfo info;
    info.lint = result->Ok() ? LintState::Ok : LintState::Violation;
    info.hash = result->sourceHash;
    for (const kernel::KernelDiagnostic& d : result->diagnostics) {
        MYE_LOG_ERROR("%s", kernel::FormatKernelDiagnostic(d).c_str());
    }
    sources_[result->kernelName] = info;
}

void ComputeService::SetKernelSourceInfo(const std::string& kernel, const KernelSourceInfo& info)
{
    sources_[kernel] = info;
}

void ComputeService::SetKernelOverrideCpu(const std::string& kernel, bool cpuOnly)
{
    if (cpuOnly) {
        overrideCpu_.insert(kernel);
    } else {
        overrideCpu_.erase(kernel);
    }
}

// ---------------------------------------------------------------------------
// バッファ
// ---------------------------------------------------------------------------

uint64_t ComputeService::CreateBuffer(CallerKind lane, uint32_t count, uint32_t stride, BufferKind kind)
{
    return Store(lane).Create(count, stride, kind);
}

bool ComputeService::ReleaseBuffer(CallerKind lane, uint64_t handle)
{
    if (!Store(lane).Release(handle)) {
        return false;
    }
    gpu_->Forget(handle);
    return true;
}

bool ComputeService::WriteBuffer(CallerKind lane, uint64_t handle, const void* src, uint32_t count)
{
    ComputeBufferStore& store = Store(lane);
    if (!store.Write(handle, src, count)) {
        return false;
    }
    // CPU が書いたので、GPU から戻ってくる古い結果で上書きされないようにする
    gpu_->Discard(handle);
    return true;
}

bool ComputeService::ReadBuffer(CallerKind lane, uint64_t handle, void* dst, uint32_t count, bool wait)
{
    ComputeBufferStore& store = Store(lane);
    ComputeBufferStore::Slot* s = store.Find(handle);
    if (s == nullptr) {
        return false;
    }
    if (s->kind == BufferKind::Fx && gpu_->HasUnapplied(handle)) {
        gpu_->Poll(handle, s->data.data(), s->count, s->stride, wait);
    }
    return store.Read(handle, dst, count);
}

// ---------------------------------------------------------------------------
// ForEach
// ---------------------------------------------------------------------------

bool ComputeService::Resolve(const ForEachArgs& args, const ComputeKernelDesc* desc, Resolved& out,
                             FailReason& why)
{
    why = FailReason::BadArgs;
    if (desc == nullptr || args.bufferCount != desc->args.size() || args.buffers == nullptr
        || args.paramsSize != desc->paramsSize || args.params == nullptr) {
        return false;
    }
    out.store = &Store(args.caller.kind);
    out.slots.assign(args.bufferCount, nullptr);
    out.handles.assign(args.buffers, args.buffers + args.bufferCount);
    bool anyStateWrite = false;
    bool anyFx = false;
    for (uint32_t i = 0; i < args.bufferCount; ++i) {
        ComputeBufferStore::Slot* s = out.store->Find(args.buffers[i]);
        if (s == nullptr || s->stride != desc->args[i].stride) {
            return false;
        }
        out.slots[i] = s;
        out.hasState = out.hasState || s->kind == BufferKind::State;
        anyFx = anyFx || s->kind == BufferKind::Fx;
        anyStateWrite = anyStateWrite || (desc->args[i].writable && s->kind == BufferKind::State);
        if (s->kind == BufferKind::Fx && s->fxCpuDirty) {
            out.pendingUploadBytes += static_cast<uint64_t>(s->count) * s->stride;
        }
    }
    out.elementCount = out.slots[0]->count;
    if (out.elementCount > kernel::kMaxKernelElements) {
        why = FailReason::TooManyElements;
        return false;
    }
    for (uint32_t i = 0; i < args.bufferCount; ++i) {
        // 2 本目以降の RW は第 1 引数以上の要素数が要る (範囲外書き込みになる)
        if (desc->args[i].writable && out.slots[i]->count < out.elementCount) {
            return false;
        }
        // RW が絡む同一バッファの重複は、他要素の今回の書き込みを読む競合になる
        for (uint32_t j = i + 1; j < args.bufferCount; ++j) {
            if (args.buffers[i] == args.buffers[j] && (desc->args[i].writable || desc->args[j].writable)) {
                return false;
            }
        }
    }
    out.writesStateReadsFx = anyStateWrite && anyFx;
    why = FailReason::None;
    return true;
}

void ComputeService::LogOnce(const ForEachArgs& args, const char* level, FailReason reason, const char* detail)
{
    const std::string key = std::string(args.caller.scriptType) + "|" + std::string(args.kernel) + "|"
        + level + "|" + FailReasonName(reason) + "|" + detail;
    if (!logged_.insert(key).second) {
        return;
    }
    if (level[0] == 'E') {
        MYE_LOG_ERROR("[compute] ForEach '%.*s' from '%.*s' failed: %s %s", static_cast<int>(args.kernel.size()),
                      args.kernel.data(), static_cast<int>(args.caller.scriptType.size()),
                      args.caller.scriptType.data(), FailReasonName(reason), detail);
    } else {
        MYE_LOG_WARN("[compute] ForEach '%.*s' from '%.*s': %s %s", static_cast<int>(args.kernel.size()),
                     args.kernel.data(), static_cast<int>(args.caller.scriptType.size()),
                     args.caller.scriptType.data(), FailReasonName(reason), detail);
    }
}

// Fx バッファの CPU ミラーを、GPU の未反映の結果と揃える (CPU が読む・書く前に明示ストール)
bool ComputeService::SyncFxMirror(Resolved& r, KernelObservation& obs)
{
    for (size_t i = 0; i < r.slots.size(); ++i) {
        ComputeBufferStore::Slot* s = r.slots[i];
        if (s->kind != BufferKind::Fx || !gpu_->HasUnapplied(r.handles[i])) {
            continue;
        }
        obs.waitCount += 1;
        if (!gpu_->Poll(r.handles[i], s->data.data(), s->count, s->stride, true)) {
            return false;
        }
    }
    return true;
}

uint64_t ComputeService::RunCpu(const ForEachArgs& args, const ComputeKernelDesc& desc, Resolved& r,
                                KernelObservation& obs, FailReason& why)
{
    if (!SyncFxMirror(r, obs)) {
        why = FailReason::CpuRunFailed;
        return kJobFailed;
    }
    std::vector<kernel::KernelBufferRef> refs(r.slots.size());
    for (size_t i = 0; i < r.slots.size(); ++i) {
        refs[i].data = r.slots[i]->data.data();
        refs[i].count = r.slots[i]->count;
        refs[i].stride = r.slots[i]->stride;
    }
    const CpuImpl& impl = args.caller.kind == CallerKind::Cpp ? desc.cpp : desc.csharp;
    kernel::RunOptions opt;
    opt.maxChunks = config_.cpuMaxChunks;
    const auto t0 = std::chrono::steady_clock::now();
    const kernel::RunStatus st = kernel::RunKernelRange(*jobs_, impl.range, args.params, refs.data(),
                                                        static_cast<uint32_t>(refs.size()), r.elementCount, opt);
    obs.lastCpuMs = MsSince(t0);
    if (st != kernel::RunStatus::Ok) {
        why = st == kernel::RunStatus::InJob ? FailReason::InJob
            : st == kernel::RunStatus::TooManyElements ? FailReason::TooManyElements
                                                       : FailReason::CpuRunFailed;
        return kJobFailed;
    }
    // Fx は GPU 側が古くなる。次の GPU 実行の前に upload し、戻ってくる古い結果は捨てる
    for (size_t i = 0; i < r.slots.size(); ++i) {
        if (r.slots[i]->kind == BufferKind::Fx && desc.args[i].writable) {
            r.slots[i]->fxCpuDirty = true;
            gpu_->Discard(r.handles[i]);
        }
    }
    obs.lastTarget = JobTarget::Cpu;
    return kJobDoneCpu;
}

uint64_t ComputeService::RunGpu(const ForEachArgs& args, const ComputeKernelDesc& desc, Resolved& r,
                                KernelObservation& obs, FailReason& why)
{
    FxGpuBackend::Request req;
    req.kernel = args.kernel;
    req.params = args.params;
    req.paramsSize = args.paramsSize;
    req.elementCount = r.elementCount;
    for (size_t i = 0; i < r.slots.size(); ++i) {
        FxGpuBackend::BufferIo io;
        io.name = desc.args[i].name;
        io.key = r.handles[i];
        io.cpu = r.slots[i]->data.data();
        io.count = r.slots[i]->count;
        io.stride = r.slots[i]->stride;
        io.upload = r.slots[i]->fxCpuDirty;
        (desc.args[i].writable ? req.readWrite : req.readOnly).push_back(io);
    }
    uint64_t uploaded = 0;
    const uint64_t serial = gpu_->Dispatch(req, &uploaded);
    if (serial == 0) {
        why = FailReason::GpuDispatchFailed;
        return kJobFailed;
    }
    uint64_t readback = 0;
    for (size_t i = 0; i < r.slots.size(); ++i) {
        r.slots[i]->fxCpuDirty = false;
        if (desc.args[i].writable) {
            readback += static_cast<uint64_t>(r.slots[i]->count) * r.slots[i]->stride;
        }
    }
    tickDispatches_ += 1;
    tickUploadBytes_ += uploaded;
    obs.lastUploadBytes = uploaded;
    obs.lastReadbackBytes = readback;
    obs.gpuDispatches += 1;
    obs.lastTarget = JobTarget::Gpu;
    if (config_.alwaysWaitGpu) {
        // 決定的撮影・selftest: 結果が CPU ミラーに載るまで待つ
        obs.waitCount += 1;
        gpu_->WaitSerial(serial);
        for (size_t i = 0; i < r.slots.size(); ++i) {
            if (desc.args[i].writable) {
                gpu_->Poll(r.handles[i], r.slots[i]->data.data(), r.slots[i]->count, r.slots[i]->stride, true);
            }
        }
    }
    return kJobGpuBase + serial;
}

uint64_t ComputeService::ForEach(const ForEachArgs& args)
{
    ObservationKey key{ std::string(args.caller.scriptType), std::string(args.kernel) };
    KernelObservation& obs = observations_[key];
    obs.calls += 1;
    obs.lastTarget = JobTarget::None;
    obs.lastNote = DecisionNote::None;
    obs.lastUploadBytes = 0;
    obs.lastReadbackBytes = 0;
    obs.lastCpuMs = 0.0;

    const auto fail = [&](FailReason why, const char* detail = "") -> uint64_t {
        obs.failures += 1;
        obs.lastKind = DecisionKind::Failed;
        obs.lastFail = why;
        obs.lastTarget = JobTarget::None;
        LogOnce(args, "E", why, detail);
        return kJobFailed;
    };

    // ジョブの中からの呼び出しは入れ子の並列になる (JobSystem は非対応)。何にも触らず失敗にする
    if (jobs::JobSystem::InJob()) {
        return fail(FailReason::InJob);
    }

    const auto kit = kernels_.find(std::string(args.kernel));
    const ComputeKernelDesc* desc = kit != kernels_.end() ? &kit->second : nullptr;
    const auto sit = sources_.find(std::string(args.kernel));

    DecisionInput in;
    in.callerIsScript = !args.caller.scriptType.empty();
    in.declared = in.callerIsScript && IsDeclared(args.caller.scriptType, args.kernel);
    in.lint = (desc != nullptr && sit != sources_.end()) ? sit->second.lint : LintState::Unknown;
    if (desc != nullptr) {
        const CpuImpl& impl = args.caller.kind == CallerKind::Cpp ? desc->cpp : desc->csharp;
        in.cpuVersion = impl.range != nullptr;
        in.hashMatch = sit != sources_.end() && impl.sourceHash == sit->second.hash;
    }
    in.execution = args.execution;
    in.forceCpu = config_.forceCpu;
    in.overrideCpu = overrideCpu_.find(std::string(args.kernel)) != overrideCpu_.end();
    in.resim = resim_;
    in.deviceAvailable = gpu_->Available();
    in.gpuMinElements = config_.gpuMinElements;

    Resolved r;
    FailReason resolveWhy = FailReason::None;
    const bool resolved = Resolve(args, desc, r, resolveWhy);
    in.hasState = resolved && r.hasState;
    in.writesStateReadsFx = resolved && r.writesStateReadsFx;
    in.elementCount = resolved ? r.elementCount : 0;
    in.gpuBudgetOk = tickDispatches_ < config_.fxDispatchPerTick
        && tickUploadBytes_ + r.pendingUploadBytes <= config_.fxUploadBytesPerTick;

    const Decision d = DecideExecution(in);
    obs.lastKind = d.kind;
    obs.lastNote = d.note;
    if (d.kind == DecisionKind::Failed) {
        return fail(d.reason);
    }
    if (d.kind == DecisionKind::Skipped) {
        obs.lastFail = FailReason::None;
        return kJobSkipped;
    }
    if (!resolved) {
        return fail(resolveWhy);
    }

    obs.lastElements = r.elementCount;
    FailReason why = FailReason::None;
    uint64_t job = kJobFailed;
    if (d.kind == DecisionKind::Gpu) {
        if (d.note == DecisionNote::HashMismatchGpu) {
            LogOnce(args, "W", FailReason::HashMismatch, "CPU version is stale; running on GPU");
        }
        job = RunGpu(args, *desc, r, obs, why);
        const bool cpuUsable = in.cpuVersion && in.hashMatch;
        if (job == kJobFailed && cpuUsable) {
            // GPU で走らせられなかった Fx は、同じカーネルの CPU 版で代わりに走らせる
            LogOnce(args, "W", why, "GPU dispatch failed; substituted by CPU");
            obs.lastNote = DecisionNote::SubstitutedByCpu;
            job = RunCpu(args, *desc, r, obs, why);
        }
    } else {
        if (d.note == DecisionNote::SubstitutedByCpu) {
            LogOnce(args, "W", FailReason::NoDevice, "no GPU device; substituted by CPU");
        }
        job = RunCpu(args, *desc, r, obs, why);
    }
    if (job == kJobFailed) {
        return fail(why);
    }
    obs.lastFail = FailReason::None;
    return job;
}

JobStatus ComputeService::Status(uint64_t job)
{
    if (job == kJobFailed) {
        return JobStatus::Failed;
    }
    if (job == kJobDoneCpu) {
        return JobStatus::Done;
    }
    if (job == kJobSkipped) {
        return JobStatus::Skipped;
    }
    if (job < kJobGpuBase) {
        return JobStatus::Failed;
    }
    const uint64_t serial = job - kJobGpuBase;
    if (serial == 0 || serial > gpu_->Submitted()) {
        return JobStatus::Failed;
    }
    return gpu_->Completed() >= serial ? JobStatus::Done : JobStatus::Pending;
}

JobTarget ComputeService::Target(uint64_t job) const
{
    if (job == kJobDoneCpu) {
        return JobTarget::Cpu;
    }
    if (job >= kJobGpuBase && job - kJobGpuBase <= gpu_->Submitted() && job != kJobGpuBase) {
        return JobTarget::Gpu;
    }
    return JobTarget::None;
}

bool ComputeService::Wait(uint64_t job)
{
    if (job < kJobGpuBase) {
        return job == kJobDoneCpu || job == kJobSkipped;
    }
    const uint64_t serial = job - kJobGpuBase;
    if (serial == 0 || serial > gpu_->Submitted()) {
        return false;
    }
    return gpu_->WaitSerial(serial);
}

// ---------------------------------------------------------------------------
// デバイス消失・観測
// ---------------------------------------------------------------------------

void ComputeService::ReleaseGpu()
{
    // 消失前に終わっていた結果は拾えるだけ拾う (消失後は Map が失敗するだけ)
    for (ComputeBufferStore* store : { &gameStore_, &scriptStore_ }) {
        std::vector<ComputeBufferStore::Slot>& slots = store->MutableSlots();
        for (uint32_t i = 0; i < slots.size(); ++i) {
            ComputeBufferStore::Slot& s = slots[i];
            if (s.live && s.kind == BufferKind::Fx) {
                const uint64_t handle = store->HandleOf(i);
                if (gpu_->HasUnapplied(handle)) {
                    gpu_->Poll(handle, s.data.data(), s.count, s.stride, false);
                }
                s.fxCpuDirty = true; // 復旧後は CPU ミラーから作り直して upload する
            }
        }
    }
    gpu_->ReleaseGpu();
}

void ComputeService::RecreateGpu()
{
    gpu_->RecreateGpu();
}

const KernelObservation* ComputeService::FindObservation(std::string_view scriptType,
                                                         std::string_view kernel) const
{
    const auto it = observations_.find(ObservationKey{ std::string(scriptType), std::string(kernel) });
    return it != observations_.end() ? &it->second : nullptr;
}

void ComputeService::ClearObservations()
{
    observations_.clear();
    logged_.clear();
}

} // namespace mye::compute
