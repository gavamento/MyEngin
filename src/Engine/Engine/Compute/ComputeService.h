//====================================================================================
//                          ComputeService.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach のエンジン核 (ストア・実行先の判断・実行・対応表)
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Engine/Compute/ComputeBufferStore.h"
#include "Engine/Engine/Compute/ComputeDecision.h"
#include "Engine/Engine/Compute/ComputeTypes.h"
#include "Engine/Engine/Compute/KernelLint.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Shared/EngineAPI.h" // MyeKernelDesc

namespace mye {
class ByteWriter;
class ByteReader;
class World;
class GraphicsDevice;
class ShaderManager;
namespace jobs {
class JobSystem;
}
} // namespace mye

namespace mye::compute {

class FxGpuBackend;

// 負荷枠と実行ポリシー。読み込みは sub-07 (project_settings.json の "compute")
struct ComputeConfig {
    uint32_t gpuMinElements = 4096;
    uint32_t cpuMaxChunks = 0;                 // 0 = 制限なし
    uint64_t fxUploadBytesPerTick = 8388608;
    uint32_t fxDispatchPerTick = 64;
    uint64_t stateTotalBytes = 8388608;
    bool forceCpu = false;                     // --compute-force-cpu
    bool alwaysWaitGpu = false;                // 決定的撮影・selftest: GPU 実行の直後に結果を待って反映する
};

struct KernelArgDesc {
    std::string name;      // HLSL の変数名 (GPU のバインドはリフレクションの名前で引く)
    bool writable = false; // RW
    uint32_t stride = 0;
};

struct CpuImpl {
    kernel::KernelRangeFn range = nullptr;
    uint64_t sourceHash = 0; // この CPU 版を作ったときの HashKernelSource
};

// カーネル 1 本の登録。args は宣言順で、[0] は RW 必須。C++ 版と C# 版は独立に持てる (D25)
struct ComputeKernelDesc {
    std::string name;
    uint32_t paramsSize = 0;
    std::vector<KernelArgDesc> args;
    CpuImpl cpp;
    CpuImpl csharp;
};

// ABI の表の 1 行 (GameLogic.dll の Kernels.gen.cpp / C# の登録) を ComputeKernelDesc にする。
// lane の CPU 版として入り、文字列は全部コピーする (DLL を解放しても残る)
ComputeKernelDesc KernelDescFromAbi(const MyeKernelDesc& desc, CallerKind lane);

// ディスク上のソースの検査結果とハッシュ (UpdateKernelSource が作る)
struct KernelSourceInfo {
    LintState lint = LintState::Unknown;
    uint64_t hash = 0;
    std::wstring path;      // 読んだファイル (NormalizePathKey 済み)。空 = 手で登録した情報
    std::string violation;  // 最初の検査違反 ("file(12): error KL005 [swizzle]: ...")。違反が無ければ空
    std::string conflict;   // GPU が読む .kernel.hlsl と別のファイルだったときの説明 (lint も Violation にする)
};

struct ComputeCaller {
    CallerKind kind = CallerKind::Cpp;
    std::string_view scriptType; // 空 = スクリプトではない (BT タスクなど)
};

struct ForEachArgs {
    ComputeCaller caller;
    std::string_view kernel;
    Execution execution = Execution::Auto;
    const void* params = nullptr;
    uint32_t paramsSize = 0;
    const uint64_t* buffers = nullptr;
    uint32_t bufferCount = 0;
};

// 対応表の 1 行 (スクリプト型 x カーネル名)。表示専用で、スナップショットにもハッシュにも入れない
struct KernelObservation {
    DecisionKind lastKind = DecisionKind::Failed;
    FailReason lastFail = FailReason::None;
    DecisionNote lastNote = DecisionNote::None;
    JobTarget lastTarget = JobTarget::None;
    uint32_t lastElements = 0;
    uint64_t lastUploadBytes = 0;
    uint64_t lastReadbackBytes = 0;
    double lastCpuMs = 0.0;
    uint64_t calls = 0;
    uint64_t failures = 0;
    uint64_t gpuDispatches = 0;
    uint64_t waitCount = 0;
};

struct ObservationKey {
    std::string scriptType;
    std::string kernel;
    bool operator<(const ObservationKey& o) const
    {
        return scriptType != o.scriptType ? scriptType < o.scriptType : kernel < o.kernel;
    }
};

class ComputeService {
public:
    ComputeService();
    ~ComputeService();
    ComputeService(const ComputeService&) = delete;
    ComputeService& operator=(const ComputeService&) = delete;

    // ---- 配線 ----
    // js: 省略時はプロセス共有の JobSystem。device / shaders が null ならデバイス無し (Fx は CPU 版で代替)
    void Bind(GraphicsDevice* device, ShaderManager* shaders, jobs::JobSystem* js = nullptr);
    void SetConfig(const ComputeConfig& config);
    const ComputeConfig& Config() const { return config_; }
    // tick の頭で呼ぶ。GPU 枠のカウンタを戻し、resim を受け取る
    void BeginTick(bool resim);

    // ---- カーネル表・宣言・ソース情報 ----
    bool RegisterKernel(const ComputeKernelDesc& desc);
    void ClearKernels();
    // lane の CPU 版の表を丸ごと差し替える (GameLogic.dll の読込・ホットリロード / C# の再コンパイル)。
    // もう一方の言語の CPU 版は残す。差し替えで CPU 版が無くなったカーネルは表から消える。
    // 引数表が既存と食い違うカーネルは、もう一方の言語の CPU 版を捨てる (古い版を新しい引数表で走らせない)
    void ReplaceKernels(CallerKind lane, const std::vector<ComputeKernelDesc>& descs);
    // スクリプト型名 → 宣言したカーネル名の集合。言語 (lane) ごとに丸ごと差し替える (モジュールの取り込み)
    void ReplaceDeclarations(CallerKind lane, std::map<std::string, std::set<std::string>> declarations);
    void ReplaceDeclarations(std::map<std::string, std::set<std::string>> declarations)
    {
        ReplaceDeclarations(CallerKind::Cpp, std::move(declarations));
    }
    // dir 直下の *.kernel.hlsl を全部読み直して検査・ハッシュを覚える (前の内容は捨てる)。読めたファイル数を返す
    uint32_t ScanKernelSources(const std::wstring& dir);
    // fileName は "Foo.kernel.hlsl"。検査してハッシュと一緒に覚える。path は読んだファイル (表示と GPU 側との照合用)
    void UpdateKernelSource(const std::string& fileName, std::string_view text, const std::wstring& path = {});
    // FileWatcher が拾った kernelDir 直下の .kernel.hlsl 1 本を読み直す (大文字小文字はディスクの綴りを採る)。
    // ファイルが消えていれば表から外す。内容 (ハッシュ) が変わった・現れた・消えたら true
    bool ReloadKernelSource(const std::wstring& normPath);
    // 覚えている kernelDir (ScanKernelSources が渡された dir)
    const std::wstring& KernelDir() const { return kernelDir_; }
    // 各カーネルについて、CPU 側が覚えたファイルと GPU 側 (ShaderManager) が読むファイルが同じか確かめる。
    // 食い違うカーネルは conflict を立てて検査違反扱い (ForEach は失敗) にし、ログに 1 回出す。食い違った数を返す
    uint32_t VerifyKernelSourceAgreement();
    void SetKernelSourceInfo(const std::string& kernel, const KernelSourceInfo& info);
    // ディスク上の .kernel.hlsl を覚えているか / カーネル表に載っているか (宣言したのに無い、の検出用)
    bool HasKernelSource(std::string_view kernel) const;
    bool HasKernel(std::string_view kernel) const;
    const ComputeKernelDesc* FindKernel(std::string_view kernel) const;
    const KernelSourceInfo* FindKernelSource(std::string_view kernel) const;
    void SetKernelOverrideCpu(const std::string& kernel, bool cpuOnly);
    bool IsKernelOverrideCpu(std::string_view kernel) const;
    const std::set<std::string>& KernelOverridesCpu() const { return overrideCpu_; }

    // ---- バッファ (caller の言語でストアが決まる: C++ = Game、C# = Script) ----
    // owner: 作成したスクリプトのエンティティ。null でなければ、そのエンティティが破棄された tick 末に自動解放する
    uint64_t CreateBuffer(CallerKind lane, uint32_t count, uint32_t stride, BufferKind kind,
                          EntityID owner = EntityID{});
    bool ReleaseBuffer(CallerKind lane, uint64_t handle);
    bool WriteBuffer(CallerKind lane, uint64_t handle, const void* src, uint32_t count);
    // Fx は最新の完了済み結果を返す (待たない)。wait=true で明示ストール
    bool ReadBuffer(CallerKind lane, uint64_t handle, void* dst, uint32_t count, bool wait = false);
    ComputeBufferStore& Store(CallerKind lane) { return lane == CallerKind::Cpp ? gameStore_ : scriptStore_; }
    const ComputeBufferStore& Store(CallerKind lane) const
    {
        return lane == CallerKind::Cpp ? gameStore_ : scriptStore_;
    }

    // ---- 寿命 (D8) ----
    // 持ち主のエンティティがもう居ないバッファを、両方のストアから昇順で解放する。tick 末の構造変更の適用後に呼ぶ
    uint32_t ReleaseDeadOwners(const World& world);
    // 両方のストアを初期状態へ戻す (Play 停止・シーン読込)。採番も戻るので、次のセッションは新規起動と同じ handle から始まる
    void ReleaseAllBuffers();

    // ---- スナップショット (C++ 用ストアの State だけ。Fx と C# 用ストアは入れない) ----
    void WriteGameSnapshot(ByteWriter& w) const;
    bool ReadGameSnapshot(ByteReader& r, std::vector<ComputeBufferStore::Slot>& out) const;
    void ApplyGameSnapshot(std::vector<ComputeBufferStore::Slot>&& slots);

    // ---- 実行とジョブ ----
    // 失敗は kJobFailed (0)。State は同期で完了する。Fx の GPU は非同期 (Pending のジョブを返す)
    uint64_t ForEach(const ForEachArgs& args);
    JobStatus Status(uint64_t job);
    JobTarget Target(uint64_t job) const;
    // GPU ジョブの完了まで待つ (明示ストール)。待てたら true
    bool Wait(uint64_t job);

    // ---- デバイス消失 (M88) ----
    // GPU 側だけ手放す。ストアの中身 (CPU ミラー) は残り、復旧後の最初の GPU 実行で作り直して upload する
    void ReleaseGpu();
    void RecreateGpu();

    // ---- 観測 ----
    const KernelObservation* FindObservation(std::string_view scriptType, std::string_view kernel) const;
    const std::map<ObservationKey, KernelObservation>& Observations() const { return observations_; }
    void ClearObservations();
    // 宣言表を引く。lane を渡すとその言語の宣言だけ、渡さなければどちらかにあれば真 (インスペクター用)
    bool IsDeclared(CallerKind lane, std::string_view scriptType, std::string_view kernel) const;
    bool IsDeclared(std::string_view scriptType, std::string_view kernel) const;
    // scriptType が lane で宣言したカーネル名 (名前順)
    std::vector<std::string> DeclaredKernels(CallerKind lane, std::string_view scriptType) const;
    // 今 tick の使用量 (selftest 用)
    uint32_t TickDispatches() const { return tickDispatches_; }
    uint64_t TickUploadBytes() const { return tickUploadBytes_; }

private:
    struct Resolved;
    bool Resolve(const ForEachArgs& args, const ComputeKernelDesc* desc, Resolved& out, FailReason& why);
    uint64_t RunCpu(const ForEachArgs& args, const ComputeKernelDesc& desc, Resolved& r,
                    KernelObservation& obs, FailReason& why);
    uint64_t RunGpu(const ForEachArgs& args, const ComputeKernelDesc& desc, Resolved& r,
                    KernelObservation& obs, FailReason& why);
    bool SyncFxMirror(Resolved& r, KernelObservation& obs);
    static bool ValidateKernelDesc(const ComputeKernelDesc& desc);
    DecisionInput MakeDecisionInput(const ForEachArgs& args, const ComputeKernelDesc* desc, const Resolved* resolved) const;
    void LogOnce(const ForEachArgs& args, const char* level, FailReason reason, const char* detail);

    ComputeConfig config_;
    ComputeBufferStore gameStore_;
    ComputeBufferStore scriptStore_;
    jobs::JobSystem* jobs_ = nullptr;
    std::unique_ptr<FxGpuBackend> gpu_;
    bool resim_ = false;
    uint32_t tickDispatches_ = 0;
    uint64_t tickUploadBytes_ = 0;

    std::map<std::string, ComputeKernelDesc> kernels_;
    using DeclarationMap = std::map<std::string, std::set<std::string>, std::less<>>;
    DeclarationMap declarations_[2]; // [CallerKind]
    std::map<std::string, KernelSourceInfo> sources_;
    std::wstring kernelDir_;
    ShaderManager* shaders_ = nullptr;
    std::set<std::string> overrideCpu_;
    kernel::KernelLintCache lintCache_;
    std::map<ObservationKey, KernelObservation> observations_;
    std::set<std::string> logged_;
};

} // namespace mye::compute
