//====================================================================================
//                          ComputeDemoSwarm.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          --compute-demo の群れ (State の ForEach を毎 tick 回して見える体へ写す)
//====================================================================================
// 512 体の群れを DemoSwarmStep で 1 tick ずつ進める。前の状態と次の状態の 2 本の State バッファを毎 tick 入れ替える。
// State は CPU で決まる決定的な計算なので、結果を LocalTransform に書いてよい (リプレイの照合に入る)。
// 見える体 (DemoBoid_000..) はシーンの組み立て側 (DemoContent.cpp の BuildComputeShowcaseScene) が作る。
#include <cstdio>

#include "GameLogic/Generated/Kernels.gen.h"
#include "Shared/ScriptAPI.h"

namespace {

constexpr uint32_t kBoidCount = 512; // 見える体の数と同じ
constexpr uint32_t kColumns = 32;
constexpr float kSpacing = 0.5f;
constexpr MyeVec3 kCenter = { 0.0f, 3.0f, 0.0f };
constexpr float kRadius = 4.2f; // 輪の大きさ (半径)

using Boid = MyeKernels::DemoSwarmStep::DemoBoid;
using Params = MyeKernels::DemoSwarmStep::DemoSwarmStepParams;
using Vec = decltype(Boid::pos); // カーネル側の float3 (MyeVec3 とは別の型)

} // namespace

struct ComputeDemoSwarm : Script<ComputeDemoSwarm> {
    uint64_t bufferA = 0; // 偶数 tick の「前の状態」
    uint64_t bufferB = 0; // 奇数 tick の「前の状態」
    uint32_t flip = 0;    // 0 なら A が前の状態
    uint32_t steps = 0;

    void Start(MyeUpdateContext& ctx)
    {
        auto a = Compute::CreateBuffer<Boid>(ctx, kBoidCount, BufferKind::State);
        auto b = Compute::CreateBuffer<Boid>(ctx, kBoidCount, BufferKind::State);
        bufferA = a.Handle();
        bufferB = b.Handle();

        // 格子状に並べる。整数から作るだけで、三角関数も乱数も使わない
        std::vector<Boid> init(kBoidCount);
        for (uint32_t i = 0; i < kBoidCount; ++i) {
            const float col = static_cast<float>(i % kColumns) - 0.5f * static_cast<float>(kColumns - 1);
            const float row = static_cast<float>(i / kColumns);
            init[i].pos = Vec(kCenter.x + col * kSpacing * 0.5f, kCenter.y - 1.0f + row * 0.15f, kCenter.z + row * 0.1f);
            init[i].vel = Vec(0.0f, 0.0f, 0.0f);
        }
        a.Write(ctx, init);
    }

    void Update(MyeUpdateContext& ctx)
    {
        const auto prev = Compute::Buffer<Boid>::FromHandle(flip == 0 ? bufferA : bufferB, kBoidCount);
        const auto next = Compute::Buffer<Boid>::FromHandle(flip == 0 ? bufferB : bufferA, kBoidCount);
        const Params params = { ctx.dt,
                                static_cast<float>(ctx.tickIndex) * ctx.dt,
                                Vec(kCenter.x, kCenter.y, kCenter.z),
                                kRadius,
                                kBoidCount,
                                static_cast<uint32_t>(ctx.tickIndex) };
        if (!Compute::ForEach(ctx, "DemoSwarmStep", Execution::Auto, params, next, prev).Ok()) {
            return;
        }
        flip ^= 1u;
        ++steps;

        std::vector<Boid> boids(kBoidCount);
        if (!next.Read(ctx, boids)) {
            return;
        }
        char name[32];
        for (uint32_t i = 0; i < kBoidCount; ++i) {
            std::snprintf(name, sizeof(name), "DemoBoid_%03u", i);
            const MyeEntityId e = ctx.api->FindByName(ctx.api->engine, name);
            if (!MyeEntityIdIsNull(e)) {
                ctx.api->SetLocalPosition(ctx.api->engine, e, MyeVec3{ boids[i].pos.x, boids[i].pos.y, boids[i].pos.z });
            }
        }
    }
};
REGISTER_SCRIPT(ComputeDemoSwarm, FIELDS(bufferA, bufferB, flip, steps));
COMPUTE_KERNELS(ComputeDemoSwarm, "DemoSwarmStep");
