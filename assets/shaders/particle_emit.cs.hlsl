// GPU パーティクル放出 (engine_spec.md 7.3)
// 乱数は GPU では生成しない — CPU (エンジンの決定論 RNG) が生成した初期値バッファを消費する

#include "particle_gpu_common.hlsli"

struct EmitData
{
    float3 pos;
    float  life;
    float3 vel;
    float  size;
};

RWStructuredBuffer<GpuParticle> gPool : register(u0);
ConsumeStructuredBuffer<uint> gDeadList : register(u1);
RWStructuredBuffer<uint> gAliveOut : register(u2); // カウンタ付き
StructuredBuffer<EmitData> gEmitData : register(t0);
Buffer<uint> gCounts : register(t1); // [0] = deadCount (typed — CopyStructureCount の宛先)

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint emitCount = (uint)gParams.x;
    const uint deadCount = gCounts[0];
    if (tid.x >= emitCount || tid.x >= deadCount) {
        return; // dead list の枯渇分は捨てる (アンダーフロー防止)
    }
    const uint slot = gDeadList.Consume();

    const EmitData e = gEmitData[tid.x];
    GpuParticle p;
    p.pos = e.pos;
    p.life = e.life;
    p.vel = e.vel;
    p.invLife = 1.0f / max(e.life, 0.0001f);
    p.size0 = e.size;
    p._pad = float3(0, 0, 0);
    gPool[slot] = p;

    const uint outIndex = gAliveOut.IncrementCounter();
    gAliveOut[outIndex] = slot;
}
