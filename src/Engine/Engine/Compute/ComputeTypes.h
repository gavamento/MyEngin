//====================================================================================
//                          ComputeTypes.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach の共通 enum (バッファ種別・実行指定・ジョブ状態)
//====================================================================================
#pragma once
#include <cstdint>

namespace mye::compute {

// State = ゲーム状態 (常に CPU・決定的)。Fx = 演出 (GPU を候補にできる。結果を sim へ戻さない約束)
enum class BufferKind : uint8_t { State = 0, Fx = 1 };

enum class Execution : uint8_t { Auto = 0, Cpu = 1, Gpu = 2 };

// 呼び出し元の言語。CPU 版はこれで選ぶ (C++ は C++ 版、C# は C# 版。D25)
enum class CallerKind : uint8_t { Cpp = 0, CSharp = 1 };

enum class JobStatus : uint8_t { Done = 0, Pending = 1, Failed = 2, Skipped = 3 };
enum class JobTarget : uint8_t { None = 0, Cpu = 1, Gpu = 2 };

// ジョブハンドル。0 = 失敗。同期で完了した CPU 実行と resim の Skipped は定数で、GPU の非同期ジョブだけ連番。
// 定数にする理由: ハンドルをスクリプトの FIELD に保存されても、record / resim / デバイスの有無で値が割れない
inline constexpr uint64_t kJobFailed = 0;
inline constexpr uint64_t kJobDoneCpu = 1;
inline constexpr uint64_t kJobSkipped = 2;
inline constexpr uint64_t kJobGpuBase = 0x100; // この値 + GPU 側の連番

} // namespace mye::compute
