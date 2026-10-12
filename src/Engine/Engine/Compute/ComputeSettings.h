//====================================================================================
//                          ComputeSettings.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          project_settings.json の "compute" 節 (負荷枠と実行ポリシー)
//====================================================================================
#pragma once
#include <set>
#include <string>

#include "Engine/Engine/Compute/ComputeService.h"

// assets\project_settings.json の "compute" 節:
//   "compute": { "gpuMinElements": 4096, "cpuMaxChunks": 0, "fxUploadBytesPerTick": 8388608,
//                "fxDispatchPerTick": 64, "stateTotalBytes": 16777216, "kernelOverrides": { "Name": "cpu" } }
// キーが無ければ既定値、型や範囲が不正な値はそのキーだけ既定値に倒して WARN を出す。
// stateTotalBytes は State の確保可否 = sim の結果を変えるので、起動時に 1 回だけ読む。
// project_settings.json は assets 配下なので contentHash に入り、ネットの出自照合で食い違いを弾ける。
namespace mye::compute {

struct ComputeProjectSettings {
    ComputeConfig config;                  // forceCpu / alwaysWaitGpu はここでは読まない (CLI / テスト用)
    std::set<std::string> kernelOverridesCpu; // CPU 固定にするカーネル名
};

// 各キーの許容範囲。外れた値は既定値へ倒す
inline constexpr uint64_t kGpuMinElementsMax = 1ull << 28;
inline constexpr uint64_t kCpuMaxChunksMax = 65536;
inline constexpr uint64_t kFxUploadBytesMax = 1ull << 32;
inline constexpr uint64_t kFxDispatchMax = 65536;
inline constexpr uint64_t kStateTotalBytesMax = 1ull << 32;

// JSON 文字列から "compute" 節を読む (ファイル I/O 抜きの本体 = selftest の口)。
// 節が無い・壊れている場合は既定値。戻り値は「有効な compute 節があったか」
bool ParseComputeSettings(const std::string& jsonText, ComputeProjectSettings& out);

// assetsRoot\project_settings.json を読む。assetsRoot が空 / ファイルが無い / 壊れていれば既定値
ComputeProjectSettings LoadComputeSettings(const std::wstring& assetsRoot);

// 読んだ設定をサービスへ適用する (forceCpu / alwaysWaitGpu は今の値を保つ)。起動時の Bind の直後に呼ぶ
void ApplyComputeSettings(ComputeService& service, const ComputeProjectSettings& settings);

// "compute.kernelOverrides" だけ書き換えるマージ保存 (他のキーは消さない)。
// ★project_settings.json へ上書きを書き戻すのはこの関数だけ (呼び出しは Project Settings 窓)
bool SaveComputeKernelOverrides(const std::wstring& assetsRoot, const std::set<std::string>& cpuOnlyKernels);

} // namespace mye::compute
