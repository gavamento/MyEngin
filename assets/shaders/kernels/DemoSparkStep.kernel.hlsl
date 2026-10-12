// Compute.ForEach のデモ (--compute-demo) の火花。1 粒ずつ重力で落ち、寿命が尽きたら噴出口から撃ち直す (Fx、GPU 優先)。
// 乱数は id と tick からのハッシュ。結果は描画にだけ使い、シミュレーションの状態には入れない。
#include "kernel_common.hlsli"

struct DemoSpark
{
    float3 pos;
    float3 vel;
    float life;
    float span;
};

KERNEL_PARAMS(DemoSparkStepParams)
{
    float dt;
    float3 origin;
    uint tick;
};

KERNEL(DemoSparkStep, DemoSparkStepParams, RW(DemoSpark, sparks))
{
    float3 pos = sparks[id].pos;
    float3 vel = sparks[id].vel;
    float life = sparks[id].life - params.dt;
    float span = sparks[id].span;

    if (life <= 0.0f)
    {
        const uint h = HashU32(id * 2654435761u + params.tick * 40503u);
        const float a = HashToUnitFloat(h);
        const float b = HashToUnitFloat(HashU32(h));
        const float c = HashToUnitFloat(HashU32(h + 1u));
        pos = params.origin;
        vel = float3((a - 0.5f) * 6.0f, 6.0f + b * 5.0f, (c - 0.5f) * 6.0f);
        span = 0.8f + b * 1.2f;
        life = span;
    }
    else
    {
        vel = vel + float3(0.0f, -9.8f * params.dt, 0.0f);
        pos = pos + vel * params.dt;
        if (pos.y < 0.0f)
        {
            pos = float3(pos.x, 0.0f, pos.z);
            vel = float3(vel.x * 0.6f, -vel.y * 0.5f, vel.z * 0.6f);
        }
    }

    sparks[id].pos = pos;
    sparks[id].vel = vel;
    sparks[id].life = life;
    sparks[id].span = span;
}
