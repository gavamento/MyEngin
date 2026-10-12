//====================================================================================
//                          ComputeInspectorRows.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          スクリプトのインスペクターの Compute 欄の行 (表示専用の対応表から作る)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Engine/Compute/ComputeService.h"

namespace mye::compute {

// 行の状態 (spec 4.1.7)。上にあるものが優先
enum class RowState : uint8_t {
    FailedNotDeclared, // 宣言していないのに呼んだ (U10。実行されていない)
    DeclaredNoKernel,  // 宣言したが .kernel.hlsl が見つからない (D28-f。呼べば失敗)
    LintViolation,     // 検査違反 (または CPU と GPU が別のファイルを読む)
    Ran,               // 宣言あり・実行済み
    NotRun,            // 宣言あり・まだ実行していない
};

enum class RowTarget : uint8_t { None, Cpu, Gpu, SubstitutedCpu, Failed, Skipped };

// CPU 版のハッシュがディスク上のソースと一致するか (呼び出し元の言語の CPU 版で比べる)
enum class RowVersion : uint8_t { Unknown, Match, Mismatch, NoCpuVersion };

struct ComputeInspectorRow {
    std::string kernel;
    bool declared = false;
    RowState state = RowState::NotRun;
    RowTarget target = RowTarget::None;
    FailReason lastFail = FailReason::None;
    RowVersion version = RowVersion::Unknown;
    bool overrideCpu = false; // カーネル単位。このカーネルを使うすべてのスクリプトに効く (U8)
    uint32_t elements = 0;
    uint64_t uploadBytes = 0;
    uint64_t readbackBytes = 0;
    double cpuMs = 0.0;
    uint64_t calls = 0;
    uint64_t failures = 0;
    uint64_t gpuDispatches = 0;
    std::string violation; // LintViolation のとき、規則と行 ("file(12): error KL005 [swizzle]: ...")
};

// 行 = scriptType が lane で宣言したカーネル ∪ 実行時に観測したカーネル (名前の昇順)。
// 観測の対応表はスナップショットにもハッシュにも入らない表示専用のデータ
std::vector<ComputeInspectorRow> BuildComputeInspectorRows(const ComputeService& service, CallerKind lane,
                                                           std::string_view scriptType);

} // namespace mye::compute
