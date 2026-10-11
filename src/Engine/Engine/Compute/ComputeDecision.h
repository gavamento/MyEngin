//====================================================================================
//                          ComputeDecision.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          ForEach の実行先を決める純粋関数 (spec 4.1.4 の決定表)
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Engine/Compute/ComputeTypes.h"

namespace mye::compute {

enum class LintState : uint8_t { Ok, Violation, Unknown };

enum class DecisionKind : uint8_t { Cpu, Gpu, Failed, Skipped };

enum class FailReason : uint8_t {
    None,
    NotDeclared,      // 宣言なし、または呼び出し元がスクリプトではない (D28 / U10)
    UnknownKernel,    // 登録も検査結果も無い
    LintViolation,    // 検査器の違反
    MixedKinds,       // State に書くカーネルが Fx を読む (GPU の結果が sim へ入る穴)
    StateGpuRejected, // State を含むのに Gpu 指定
    NoCpuVersion,
    HashMismatch,
    NoDevice,
    BadArgs,          // ハンドル・stride・params の大きさ・個数・重なりの不正
    TooManyElements,
    InJob,            // ジョブの中から呼ばれた
    GpuDispatchFailed,
    CpuRunFailed,
};

enum class DecisionNote : uint8_t {
    None,
    SubstitutedByCpu, // Gpu 指定だがデバイスが無く CPU で代替した
    HashMismatchGpu,  // CPU 版が古いので GPU 固定にした
};

// 決定表の入力。全部決定的な値で、時間の計測は入れない
struct DecisionInput {
    // 呼び出し元
    bool callerIsScript = false; // スクリプトのコールバック内か (BT タスクなどは false)
    bool declared = false;       // 呼び出し元がこのカーネルを宣言しているか
    // カーネル
    LintState lint = LintState::Unknown;
    bool cpuVersion = false;     // 呼び出し元の言語向けの CPU 版があるか
    bool hashMatch = false;      // CPU 版のハッシュがディスク上のソースと一致するか
    // バッファ
    bool hasState = false;       // 1 つでも State があるか
    bool writesStateReadsFx = false;
    uint32_t elementCount = 0;
    // 指定・設定
    Execution execution = Execution::Auto;
    bool forceCpu = false;       // --compute-force-cpu
    bool overrideCpu = false;    // kernelOverrides でカーネル単位に CPU 固定
    bool resim = false;
    // 実行環境
    bool deviceAvailable = false;
    uint32_t gpuMinElements = 0;
    bool gpuBudgetOk = true;     // tick の GPU 枠 (dispatch 回数・upload バイト) に収まるか
};

struct Decision {
    DecisionKind kind = DecisionKind::Failed;
    FailReason reason = FailReason::None;
    DecisionNote note = DecisionNote::None;
};

// 宣言の判定が他のすべてより先 (U10)。失敗した判定ではバッファに触らない責任は呼び出し側にある
Decision DecideExecution(const DecisionInput& in);

const char* FailReasonName(FailReason r);

} // namespace mye::compute
