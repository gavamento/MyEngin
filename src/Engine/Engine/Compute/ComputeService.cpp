//====================================================================================
//                          ComputeService.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach のエンジン核の実装
//====================================================================================
#include "Engine/Engine/Compute/ComputeService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Compute/KernelRunner.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Compute/FxGpuBackend.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

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

ComputeKernelDesc KernelDescFromAbi(const MyeKernelDesc& desc, CallerKind lane)
{
    ComputeKernelDesc out;
    out.name = desc.name != nullptr ? desc.name : "";
    out.paramsSize = desc.paramsSize;
    for (uint32_t i = 0; i < desc.argCount && desc.args != nullptr; ++i) {
        KernelArgDesc arg;
        arg.name = desc.args[i].name != nullptr ? desc.args[i].name : "";
        arg.writable = desc.args[i].writable != 0;
        arg.stride = desc.args[i].stride;
        out.args.push_back(std::move(arg));
    }
    CpuImpl& impl = lane == CallerKind::Cpp ? out.cpp : out.csharp;
    impl.range = desc.range;
    impl.sourceHash = desc.sourceHash;
    return out;
}

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
    shaders_ = shaders;
    VerifyKernelSourceAgreement(); // ソースの走査は Bind より前に済んでいることがある (SimInit)
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

bool ComputeService::ValidateKernelDesc(const ComputeKernelDesc& desc)
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
    }
    return ok;
}

bool ComputeService::RegisterKernel(const ComputeKernelDesc& desc)
{
    if (!ValidateKernelDesc(desc)) {
        return false;
    }
    kernels_[desc.name] = desc;
    return true;
}

void ComputeService::ClearKernels()
{
    kernels_.clear();
}

void ComputeService::ReplaceKernels(CallerKind lane, const std::vector<ComputeKernelDesc>& descs)
{
    const auto implOf = [lane](ComputeKernelDesc& d) -> CpuImpl& { return lane == CallerKind::Cpp ? d.cpp : d.csharp; };
    const auto otherOf = [lane](ComputeKernelDesc& d) -> CpuImpl& { return lane == CallerKind::Cpp ? d.csharp : d.cpp; };
    const auto sameSignature = [](const ComputeKernelDesc& a, const ComputeKernelDesc& b) {
        if (a.paramsSize != b.paramsSize || a.args.size() != b.args.size()) {
            return false;
        }
        for (size_t i = 0; i < a.args.size(); ++i) {
            if (a.args[i].name != b.args[i].name || a.args[i].writable != b.args[i].writable
                || a.args[i].stride != b.args[i].stride) {
                return false;
            }
        }
        return true;
    };

    // この言語の CPU 版を全部外す。外した結果どちらの CPU 版も無くなるカーネルだけ、後で表から消す
    std::set<std::string> hadImpl;
    for (auto& kv : kernels_) {
        CpuImpl& impl = implOf(kv.second);
        if (impl.range != nullptr) {
            hadImpl.insert(kv.first);
        }
        impl = CpuImpl{};
    }
    for (const ComputeKernelDesc& desc : descs) {
        if (!ValidateKernelDesc(desc)) {
            continue;
        }
        ComputeKernelDesc incoming = desc;
        const CpuImpl mine = implOf(incoming);
        const auto it = kernels_.find(desc.name);
        if (it == kernels_.end()) {
            kernels_[desc.name] = std::move(incoming);
            continue;
        }
        ComputeKernelDesc& existing = it->second;
        if (!sameSignature(existing, desc)) {
            otherOf(existing) = CpuImpl{}; // 引数表が変わった。もう一方の古い CPU 版は新しい表では走らせられない
            existing.paramsSize = desc.paramsSize;
            existing.args = desc.args;
        }
        implOf(existing) = mine;
    }
    for (auto it = kernels_.begin(); it != kernels_.end();) {
        const bool orphan = it->second.cpp.range == nullptr && it->second.csharp.range == nullptr
            && hadImpl.find(it->first) != hadImpl.end();
        it = orphan ? kernels_.erase(it) : std::next(it);
    }
}

void ComputeService::ReplaceDeclarations(CallerKind lane, std::map<std::string, std::set<std::string>> declarations)
{
    DeclarationMap& dst = declarations_[static_cast<size_t>(lane)];
    dst.clear();
    for (auto& kv : declarations) {
        dst.emplace(kv.first, std::move(kv.second));
    }
}

bool ComputeService::IsDeclared(CallerKind lane, std::string_view scriptType, std::string_view kernel) const
{
    const DeclarationMap& map = declarations_[static_cast<size_t>(lane)];
    const auto it = map.find(scriptType);
    return it != map.end() && it->second.find(std::string(kernel)) != it->second.end();
}

bool ComputeService::IsDeclared(std::string_view scriptType, std::string_view kernel) const
{
    return IsDeclared(CallerKind::Cpp, scriptType, kernel) || IsDeclared(CallerKind::CSharp, scriptType, kernel);
}

std::vector<std::string> ComputeService::DeclaredKernels(CallerKind lane, std::string_view scriptType) const
{
    const DeclarationMap& map = declarations_[static_cast<size_t>(lane)];
    const auto it = map.find(scriptType);
    return it != map.end() ? std::vector<std::string>(it->second.begin(), it->second.end()) : std::vector<std::string>{};
}

uint32_t ComputeService::ScanKernelSources(const std::wstring& dir)
{
    namespace fs = std::filesystem;
    sources_.clear();
    kernelDir_ = dir;
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec)) {
        return 0;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(dir), ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (entry.is_regular_file(ec) && name.size() > 12 && name.compare(name.size() - 12, 12, L".kernel.hlsl") == 0) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end()); // 検査ログの順序を決定的にする
    uint32_t read = 0;
    for (const fs::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            MYE_LOG_WARN("[compute] cannot read kernel source: %s", WideToUtf8(path.wstring()).c_str());
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        UpdateKernelSource(WideToUtf8(path.filename().wstring()), text, NormalizePathKey(path.wstring()));
        ++read;
    }
    return read;
}

void ComputeService::UpdateKernelSource(const std::string& fileName, std::string_view text, const std::wstring& path)
{
    const std::shared_ptr<const kernel::KernelLintResult> result = lintCache_.Get(fileName, text);
    KernelSourceInfo info;
    info.lint = result->Ok() ? LintState::Ok : LintState::Violation;
    info.hash = result->sourceHash;
    info.path = path;
    for (const kernel::KernelDiagnostic& d : result->diagnostics) {
        MYE_LOG_ERROR("%s", kernel::FormatKernelDiagnostic(d).c_str());
    }
    if (!result->diagnostics.empty()) {
        info.violation = kernel::FormatKernelDiagnostic(result->diagnostics.front());
    }
    sources_[result->kernelName] = info;
}

bool ComputeService::ReloadKernelSource(const std::wstring& normPath)
{
    namespace fs = std::filesystem;
    if (kernelDir_.empty()) {
        return false;
    }
    // FileWatcher のパスは小文字化済みなので、カーネル名の綴りはディスクのファイル名から取る
    std::error_code ec;
    fs::path real;
    for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(kernelDir_), ec)) {
        if (entry.is_regular_file(ec) && NormalizePathKey(entry.path().wstring()) == normPath) {
            real = entry.path();
            break;
        }
    }
    if (real.empty()) {
        // 消えた (または kernelDir 直下ではない)。覚えていたカーネルだけ外す
        for (auto it = sources_.begin(); it != sources_.end(); ++it) {
            if (it->second.path == normPath) {
                sources_.erase(it);
                return true;
            }
        }
        return false;
    }
    std::ifstream in(real, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string fileName = WideToUtf8(real.filename().wstring());
    const std::string name = fileName.substr(0, fileName.size() - 12); // ".kernel.hlsl"
    const auto before = sources_.find(name);
    const bool existed = before != sources_.end();
    const uint64_t oldHash = existed ? before->second.hash : 0;
    UpdateKernelSource(fileName, text, normPath);
    return !existed || sources_[name].hash != oldHash;
}

uint32_t ComputeService::VerifyKernelSourceAgreement()
{
    if (shaders_ == nullptr) {
        return 0;
    }
    uint32_t conflicts = 0;
    for (auto& kv : sources_) {
        KernelSourceInfo& info = kv.second;
        if (info.path.empty()) {
            continue;
        }
        info.conflict.clear();
        const std::wstring gpuPath = NormalizePathKey(shaders_->ResolveShaderPath(kv.first + ".kernel"));
        if (gpuPath == info.path) {
            continue;
        }
        info.conflict = "GPU reads " + WideToUtf8(gpuPath) + " but CPU reads " + WideToUtf8(info.path);
        info.lint = LintState::Violation;
        ++conflicts;
        MYE_LOG_ERROR("[compute] kernel '%s' resolves to different files for CPU and GPU (the kernel is disabled): %s",
                      kv.first.c_str(), info.conflict.c_str());
    }
    return conflicts;
}

void ComputeService::SetKernelSourceInfo(const std::string& kernel, const KernelSourceInfo& info)
{
    sources_[kernel] = info;
}

bool ComputeService::HasKernelSource(std::string_view kernel) const
{
    return sources_.find(std::string(kernel)) != sources_.end();
}

bool ComputeService::HasKernel(std::string_view kernel) const
{
    return kernels_.find(std::string(kernel)) != kernels_.end();
}

const ComputeKernelDesc* ComputeService::FindKernel(std::string_view kernel) const
{
    const auto it = kernels_.find(std::string(kernel));
    return it != kernels_.end() ? &it->second : nullptr;
}

const KernelSourceInfo* ComputeService::FindKernelSource(std::string_view kernel) const
{
    const auto it = sources_.find(std::string(kernel));
    return it != sources_.end() ? &it->second : nullptr;
}

void ComputeService::SetKernelOverrideCpu(const std::string& kernel, bool cpuOnly)
{
    if (cpuOnly) {
        overrideCpu_.insert(kernel);
    } else {
        overrideCpu_.erase(kernel);
    }
}

bool ComputeService::IsKernelOverrideCpu(std::string_view kernel) const
{
    return overrideCpu_.find(std::string(kernel)) != overrideCpu_.end();
}

// ---------------------------------------------------------------------------
// バッファ
// ---------------------------------------------------------------------------

uint64_t ComputeService::CreateBuffer(CallerKind lane, uint32_t count, uint32_t stride, BufferKind kind,
                                      EntityID owner)
{
    return Store(lane).Create(count, stride, kind, owner);
}

uint32_t ComputeService::ReleaseDeadOwners(const World& world)
{
    uint32_t released = 0;
    for (const CallerKind lane : { CallerKind::Cpp, CallerKind::CSharp }) {
        ComputeBufferStore& store = Store(lane);
        for (const BufferKind kind : { BufferKind::State, BufferKind::Fx }) {
            const uint32_t slotCount = static_cast<uint32_t>(store.Slots(kind).size());
            for (uint32_t i = 0; i < slotCount; ++i) { // 表ごとにスロット昇順 = 決定的
                const ComputeBufferStore::Slot& s = store.Slots(kind)[i];
                if (s.live && !s.owner.IsNull() && !world.IsAlive(s.owner)) {
                    const uint64_t handle = store.HandleOf(kind, i);
                    ReleaseBuffer(lane, handle);
                    ++released;
                }
            }
        }
    }
    return released;
}

void ComputeService::ReleaseAllBuffers()
{
    for (const CallerKind lane : { CallerKind::Cpp, CallerKind::CSharp }) {
        ComputeBufferStore& store = Store(lane);
        const uint32_t slotCount = static_cast<uint32_t>(store.Slots(BufferKind::Fx).size());
        for (uint32_t i = 0; i < slotCount; ++i) {
            if (store.Slots(BufferKind::Fx)[i].live) {
                gpu_->Forget(store.HandleOf(BufferKind::Fx, i));
            }
        }
        store.ReleaseAll();
    }
}

void ComputeService::WriteGameSnapshot(ByteWriter& w) const
{
    gameStore_.WriteSnapshot(w);
}

bool ComputeService::ReadGameSnapshot(ByteReader& r, std::vector<ComputeBufferStore::Slot>& out) const
{
    return gameStore_.ReadSnapshot(r, out);
}

void ComputeService::ApplyGameSnapshot(std::vector<ComputeBufferStore::Slot>&& slots)
{
    // State の表だけを差し替える。Fx は表示用で、手元のハンドルも GPU 側の対応物もそのまま有効 (D29)
    gameStore_.ApplySnapshot(std::move(slots));
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

// 決定表 (ComputeDecision) への入力。宣言・検査・ハッシュ・設定・デバイス・負荷枠・引数の解決結果を集める。
// resolved が null = 引数の検証に失敗した (バッファに関する入力は空)
DecisionInput ComputeService::MakeDecisionInput(const ForEachArgs& args, const ComputeKernelDesc* desc,
                                                const Resolved* resolved) const
{
    const auto sit = sources_.find(std::string(args.kernel));
    DecisionInput in;
    in.callerIsScript = !args.caller.scriptType.empty();
    in.declared = in.callerIsScript && IsDeclared(args.caller.kind, args.caller.scriptType, args.kernel);
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
    in.hasState = resolved != nullptr && resolved->hasState;
    in.writesStateReadsFx = resolved != nullptr && resolved->writesStateReadsFx;
    in.elementCount = resolved != nullptr ? resolved->elementCount : 0;
    const uint64_t pendingUpload = resolved != nullptr ? resolved->pendingUploadBytes : 0;
    in.gpuBudgetOk = tickDispatches_ < config_.fxDispatchPerTick
        && tickUploadBytes_ + pendingUpload <= config_.fxUploadBytesPerTick;
    return in;
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

    Resolved r;
    FailReason resolveWhy = FailReason::None;
    const bool resolved = Resolve(args, desc, r, resolveWhy);
    const DecisionInput in = MakeDecisionInput(args, desc, resolved ? &r : nullptr);

    // 引数の誤りは resim かどうかに関係なく失敗。Skipped の判定より前に出す (元の実行と resim で Ok() が割れない)
    if (!resolved) {
        const Decision pre = DecideExecution(in);
        obs.lastKind = pre.kind;
        obs.lastNote = pre.note;
        if (pre.kind == DecisionKind::Failed) {
            return fail(pre.reason);
        }
        return fail(resolveWhy);
    }

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
        std::vector<ComputeBufferStore::Slot>& slots = store->MutableSlots(BufferKind::Fx);
        for (uint32_t i = 0; i < slots.size(); ++i) {
            ComputeBufferStore::Slot& s = slots[i];
            if (s.live) {
                const uint64_t handle = store->HandleOf(BufferKind::Fx, i);
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
