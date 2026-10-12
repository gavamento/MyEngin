// Compute.ForEach のデモ (--compute-demo) の群れ。前の状態 prev を読んで次の状態 next を書く (State、CPU)。
// 各体は輪の上を回る自分の目標へ引かれ、隣の 2 体にも引き寄せられる。自分の next[id] だけを書き、prev は読むだけ。
#include "kernel_common.hlsli"

struct DemoBoid
{
    float3 pos;
    float3 vel;
};

KERNEL_PARAMS(DemoSwarmStepParams)
{
    float dt;
    float time;
    float3 center;
    float radius;
    uint count;
    uint tick;
};

KERNEL(DemoSwarmStep, DemoSwarmStepParams, RW(DemoBoid, next) RO(DemoBoid, prev))
{
    const DemoBoid me = prev[id];

    // 輪になった隣 (id が 0 と count - 1 でつながる)
    uint after = id + 1u;
    if (after >= params.count)
    {
        after = 0u;
    }
    uint before = params.count - 1u;
    if (id > 0u)
    {
        before = id - 1u;
    }
    const float3 mid = (prev[after].pos + prev[before].pos) * 0.5f;

    // 輪 (横に長い楕円で、上下にうねる) の上で id ごとに少しずれた点が目標
    const float angle = (float)id * (6.2831853f / (float)params.count) + params.time * 0.6f;
    const float3 target = params.center
        + float3(sin(angle) * params.radius, sin(angle * 3.0f + params.time) * params.radius * 0.25f, cos(angle) * params.radius * 0.6f);
    const float jitter = HashToUnitFloat(HashU32(id + params.tick * 7919u)) - 0.5f;

    const float3 acc = (target - me.pos) * 9.0f + (mid - me.pos) * 3.0f + float3(jitter, jitter * 0.5f, -jitter) * 4.0f;
    float3 vel = (me.vel + acc * params.dt) * 0.96f;
    const float speed = length(vel);
    if (speed > 12.0f)
    {
        vel = vel * (12.0f / speed);
    }

    next[id].pos = me.pos + vel * params.dt;
    next[id].vel = vel;
}
