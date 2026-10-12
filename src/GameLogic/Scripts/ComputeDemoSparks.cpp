//====================================================================================
//                          ComputeDemoSparks.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          --compute-demo の火花 (Fx の ForEach を毎 tick 回して DebugDrawLine で描く)
//====================================================================================
// 4096 粒の火花を DemoSparkStep で進める。Fx なので GPU が使えれば GPU、無ければ CPU で走る。
// ★Fx の結果 (Read の中身・ジョブ・バッファのハンドル) は実行先で変わりうる。描画 (DebugDrawLine) にだけ使い、
//   登録フィールドへも ECS へも書かない (そのためフィールドは持たず、ハンドルは登録しないメンバに置く)。
#include <vector>

#include "GameLogic/Generated/Kernels.gen.h"
#include "Shared/ScriptAPI.h"

namespace {

constexpr uint32_t kSparkCount = 4096; // gpuMinElements の既定値と同じ = Auto で GPU に回る下限
constexpr MyeVec3 kOrigin = { -9.0f, 0.1f, 0.0f };
constexpr float kTrail = 0.045f; // 尾の長さ (速度に掛ける秒)

using Spark = MyeKernels::DemoSparkStep::DemoSpark;
using Params = MyeKernels::DemoSparkStep::DemoSparkStepParams;
using Vec = decltype(Spark::pos); // カーネル側の float3 (MyeVec3 とは別の型)

} // namespace

struct ComputeDemoSparks : Script<ComputeDemoSparks> {
    uint64_t buffer = 0; // 登録しない: 実行先で値が変わりうる Fx のハンドル。無効になったら作り直す

    void Update(MyeUpdateContext& ctx)
    {
        auto fx = Compute::Buffer<Spark>::FromHandle(buffer, kSparkCount);
        std::vector<Spark> sparks(kSparkCount);
        if (buffer == 0 || !fx.Read(ctx, sparks)) {
            fx = Compute::CreateBuffer<Spark>(ctx, kSparkCount, BufferKind::Fx);
            buffer = fx.Handle();
            if (!fx.Valid()) {
                return;
            }
            // 寿命をずらして初期化する。全粒が同じ tick に噴き出さないように
            for (uint32_t i = 0; i < kSparkCount; ++i) {
                sparks[i] = Spark{};
                sparks[i].life = -1.5f * static_cast<float>(i % 64u) / 64.0f;
            }
            fx.Write(ctx, sparks);
        }

        const Params params = { ctx.dt, Vec(kOrigin.x, kOrigin.y, kOrigin.z), static_cast<uint32_t>(ctx.tickIndex) };
        Compute::ForEach(ctx, "DemoSparkStep", Execution::Auto, params, fx);

        // 最新の完了済みの結果を描く (GPU のときは数 tick 遅れてよい)
        fx.Read(ctx, sparks);
        for (const Spark& s : sparks) {
            if (s.life <= 0.0f || s.span <= 0.0f) {
                continue;
            }
            const float heat = s.life / s.span;
            const MyeVec3 head = { s.pos.x, s.pos.y, s.pos.z };
            const MyeVec3 tail = { s.pos.x - s.vel.x * kTrail, s.pos.y - s.vel.y * kTrail, s.pos.z - s.vel.z * kTrail };
            ctx.api->DebugDrawLine(ctx.api->engine, head, tail, MyeColor{ 1.0f, 0.35f + 0.6f * heat, 0.1f + 0.5f * heat * heat, 1.0f });
        }
    }
};
REGISTER_SCRIPT_NO_FIELDS(ComputeDemoSparks);
COMPUTE_KERNELS(ComputeDemoSparks, "DemoSparkStep");
