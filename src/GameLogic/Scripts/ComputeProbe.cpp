//====================================================================================
//                          ComputeProbe.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach の ABI を GameLogic から通す恒久 probe
//====================================================================================
// Start と毎 tick の Update で、宣言したカーネル (SelfTestSaxpy) と宣言していないカーネル (SelfTestUnused) を ForEach する。
// どのシーンにも自動では付かない: Compute の selftest (ComputeScriptSelfTest) が名前で引いて付ける。
// State だけを使うので、FIELD に持つハンドルとジョブの値は実行環境に依らず決定的。
#include "GameLogic/Generated/Kernels.gen.h"
#include "Shared/ScriptAPI.h"

namespace {

constexpr uint32_t kElements = 100; // 8 の倍数にしない (端数のレンジも通す)

// 結果の確認用。float のビット列を FNV-1a で畳む (ComputeScriptSelfTest が同じ式で期待値を作る)
uint32_t ChecksumOf(const float* v, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bits = 0;
        memcpy(&bits, &v[i], sizeof(bits));
        h = (h ^ bits) * 16777619u;
    }
    return h;
}

} // namespace

struct ComputeProbe : Script<ComputeProbe> {
    uint64_t yBuf = 0; // SelfTestSaxpy の y (RW) と x (RO)
    uint64_t xBuf = 0;
    uint64_t unusedBuf0 = 0; // SelfTestUnused の outv / a / skipped / c
    uint64_t unusedBuf1 = 0;
    uint64_t unusedBuf2 = 0;
    uint64_t unusedBuf3 = 0;
    uint64_t startDeclaredJob = 0;    // Start での宣言したカーネルの結果 (1 = CPU で完了)
    uint64_t startUndeclaredJob = 0;  // Start での宣言していないカーネルの結果 (0 = 失敗)
    uint64_t updateDeclaredJob = 0;   // 直近の Update での結果
    uint64_t updateUndeclaredJob = 0;
    uint64_t wrongCaseJob = 0;        // 大文字小文字違いの名前 (0 = 失敗)
    uint32_t updates = 0;
    uint32_t checksum = 0;            // 直近の宣言したカーネルの後の y
    uint32_t outvTouched = 0;         // 宣言していないカーネルの出力 (outv) が 0 以外になった要素の数

    void Start(MyeUpdateContext& ctx)
    {
        using Params = MyeKernels::SelfTestSaxpy::SelfTestSaxpyParams;
        auto y = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        auto x = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        yBuf = y.Handle();
        xBuf = x.Handle();
        float initY[kElements];
        float initX[kElements];
        for (uint32_t i = 0; i < kElements; ++i) {
            initY[i] = static_cast<float>(i) * 0.5f;
            initX[i] = 1.0f;
        }
        y.Write(ctx, initY, kElements);
        x.Write(ctx, initX, kElements);
        auto u0 = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        auto u1 = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        auto u2 = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        auto u3 = Compute::CreateBuffer<float>(ctx, kElements, BufferKind::State);
        unusedBuf0 = u0.Handle();
        unusedBuf1 = u1.Handle();
        unusedBuf2 = u2.Handle();
        unusedBuf3 = u3.Handle();
        u0.Write(ctx, initX, kElements); // outv を 1 で埋める (書き換えられたら分かる)
        u1.Write(ctx, initX, kElements);
        u2.Write(ctx, initX, kElements);
        u3.Write(ctx, initX, kElements);
        const Params params = { 2.0f };
        startDeclaredJob = Compute::ForEach(ctx, "SelfTestSaxpy", Execution::Auto, params, y, x).handle;
        startUndeclaredJob = CallUndeclared(ctx);
    }

    void Update(MyeUpdateContext& ctx)
    {
        using Params = MyeKernels::SelfTestSaxpy::SelfTestSaxpyParams;
        const auto y = Compute::Buffer<float>::FromHandle(yBuf, kElements);
        const auto x = Compute::Buffer<float>::FromHandle(xBuf, kElements);
        const Params params = { 2.0f };
        updateDeclaredJob = Compute::ForEach(ctx, "SelfTestSaxpy", Execution::Auto, params, y, x).handle;
        wrongCaseJob = Compute::ForEach(ctx, "selftestsaxpy", Execution::Auto, params, y, x).handle;
        updateUndeclaredJob = CallUndeclared(ctx);

        float values[kElements];
        y.Read(ctx, values, kElements);
        checksum = ChecksumOf(values, kElements);
        const auto outv = Compute::Buffer<float>::FromHandle(unusedBuf0, kElements);
        float outValues[kElements];
        outv.Read(ctx, outValues, kElements);
        outvTouched = 0;
        for (uint32_t i = 0; i < kElements; ++i) {
            outvTouched += outValues[i] != 1.0f ? 1u : 0u;
        }
        ++updates;
    }

private:
    // COMPUTE_KERNELS に書いていない SelfTestUnused を呼ぶ。実行されずに失敗する
    uint64_t CallUndeclared(MyeUpdateContext& ctx) const
    {
        using Params = MyeKernels::SelfTestUnused::SelfTestUnusedParams;
        const Params params = { 0.0f };
        return Compute::ForEach(ctx, "SelfTestUnused", Execution::Auto, params,
                                Compute::Buffer<float>::FromHandle(unusedBuf0, kElements),
                                Compute::Buffer<float>::FromHandle(unusedBuf1, kElements),
                                Compute::Buffer<float>::FromHandle(unusedBuf2, kElements),
                                Compute::Buffer<float>::FromHandle(unusedBuf3, kElements))
            .handle;
    }
};
REGISTER_SCRIPT(ComputeProbe, FIELDS(yBuf, xBuf, unusedBuf0, unusedBuf1, unusedBuf2, unusedBuf3, startDeclaredJob,
                                     startUndeclaredJob, updateDeclaredJob, updateUndeclaredJob, wrongCaseJob, updates,
                                     checksum, outvTouched));
COMPUTE_KERNELS(ComputeProbe, "SelfTestSaxpy");
