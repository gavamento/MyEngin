// GPU パーティクル更新: aliveIn を積分し、生存者を aliveOut へ圧縮、死亡者は dead list へ返す

#include "particle_gpu_common.hlsli"

RWStructuredBuffer<GpuParticle> gPool : register(u0);
AppendStructuredBuffer<uint> gDeadList : register(u1);
RWStructuredBuffer<uint> gAliveOut : register(u2); // カウンタ付き
StructuredBuffer<uint> gAliveIn : register(t0);
Buffer<uint> gCounts : register(t1); // [1] = aliveInCount (typed — CopyStructureCount の宛先)

[numthreads(256, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint aliveCount = gCounts[1];
    if (tid.x >= aliveCount) {
        return;
    }
    const uint slot = gAliveIn[tid.x];
    GpuParticle p = gPool[slot];

    const float dt = gGravityWind.w;
    const float turb = gParams.y;
    // CPU 実装 (CpuParticleBackend::SimulateScalar) と同じ演算列
    float3 accel = gGravityWind.xyz + turb * float3(-p.vel.z, 0.0f, p.vel.x);
    p.vel += accel * dt;
    p.pos += p.vel * dt;
    p.life -= dt;

    if (p.life <= 0.0f) {
        gDeadList.Append(slot);
        return;
    }
    gPool[slot] = p;
    const uint outIndex = gAliveOut.IncrementCounter();
    gAliveOut[outIndex] = slot;
}
