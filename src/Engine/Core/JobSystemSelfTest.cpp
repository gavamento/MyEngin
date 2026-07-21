#include "Engine/Core/JobSystemSelfTest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Engine/Core/JobSystem.h"
#include "Engine/Core/Log.h"

namespace mye {

namespace {

// index に対する決定論的なスカラ float 計算 (レンジ非依存)。
float Compute(size_t i)
{
    const float x = static_cast<float>(i) * 0.001f;
    return std::sin(x) * 1.5f + std::cos(x * 0.5f) - static_cast<float>(i % 7);
}

// 与えた total を並列/直列で計算し、結果配列を返す。
std::vector<float> RunOnce(jobs::JobSystem& js, size_t total, bool enabled)
{
    js.SetEnabled(enabled);
    std::vector<float> out(total, 0.0f);
    js.ParallelFor(total, 64, [&](size_t i) { out[i] = Compute(i); });
    return out;
}

} // namespace

bool RunJobSystemSelfTest()
{
    MYE_LOG_INFO("==== JobSystem self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    jobs::JobSystem js;
    js.Init();
    MYE_LOG_INFO("  workers = %d", js.WorkerCount());

    // ---- カバレッジ: 各 index がちょうど 1 回処理される ----
    for (size_t total : { size_t(0), size_t(1), size_t(10), size_t(1000), size_t(100003) }) {
        std::vector<int> touch(total, 0);
        js.SetEnabled(true);
        js.ParallelFor(total, 64, [&](size_t i) { touch[i] += 1; });
        bool allOnce = true;
        for (size_t i = 0; i < total; ++i) {
            if (touch[i] != 1) {
                allOnce = false;
                break;
            }
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "coverage: every index touched once (total=%zu)", total);
        check(allOnce, msg);
    }

    // ---- 決定論: 並列出力 == 直列出力 (ビット単位) ----
    for (size_t total : { size_t(1), size_t(999), size_t(100003) }) {
        const std::vector<float> par = RunOnce(js, total, true);
        const std::vector<float> ser = RunOnce(js, total, false);
        const bool identical =
            par.size() == ser.size()
            && (total == 0 || std::memcmp(par.data(), ser.data(), total * sizeof(float)) == 0);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "parallel == serial bit-identical (total=%zu)", total);
        check(identical, msg);
    }

    // ---- ParallelRanges: レンジが [0,total) を連続被覆する ----
    {
        constexpr size_t total = 50000;
        std::vector<int> touch(total, 0);
        js.SetEnabled(true);
        js.ParallelRanges(total, 128, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                touch[i] += 1;
            }
        });
        bool ok = true;
        for (size_t i = 0; i < total; ++i) {
            if (touch[i] != 1) {
                ok = false;
                break;
            }
        }
        check(ok, "ParallelRanges: contiguous full coverage, no overlap");
    }

    // ---- 反復安定性: 同じ計算を複数回並列実行しても毎回同一 ----
    {
        constexpr size_t total = 20000;
        const std::vector<float> a = RunOnce(js, total, true);
        bool stable = true;
        for (int rep = 0; rep < 8 && stable; ++rep) {
            const std::vector<float> b = RunOnce(js, total, true);
            if (std::memcmp(a.data(), b.data(), total * sizeof(float)) != 0) {
                stable = false;
            }
        }
        check(stable, "repeated parallel runs are identical");
    }

    js.Shutdown();

    if (failCount == 0) {
        MYE_LOG_INFO("==== JobSystem self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== JobSystem self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
