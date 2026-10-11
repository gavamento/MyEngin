// 自己テスト用カーネル: sin/cos・for・while・三項・自由関数・ベクトル関数・RO の範囲外読みを 1 本に詰める。
// 出力は要素ごとに独立 (他の要素を読まない)。
#include "kernel_common.hlsli"

struct SelfTestMixElem
{
    float3 pos;
    float val;
    uint tag;
};

struct SelfTestMixProbe
{
    float3 dir;
    float weight;
};

KERNEL_PARAMS(SelfTestMixParams)
{
    float time;
    float3 axis;
    uint seed;
};

float SelfTestWave(float t)
{
    return sin(t) * 0.5f + 0.5f;
}

KERNEL(SelfTestMix, SelfTestMixParams, RW(SelfTestMixElem, outv) RO(SelfTestMixProbe, probes))
{
    const float jitter = HashToUnitFloat(HashU32(params.seed ^ id));
    const float t = params.time + jitter * 3.0f;
    const float3 d = float3(sin(t * 3.0f), cos(t * 0.5f), t + 1.0f);

    // probes の末尾を超えて読むと 0 初期化の値になる (id + 3 まで読むので、末尾付近の要素が範囲外を踏む)
    float acc = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const SelfTestMixProbe p = probes[id + (uint)i];
        acc = acc + dot(p.dir, d) * p.weight;
    }

    // while / break / continue
    int steps = 0;
    float walk = 0.0f;
    while (steps < 16)
    {
        steps = steps + 1;
        if (steps % 2 == 0)
        {
            continue;
        }
        walk = walk + SelfTestWave(t + (float)steps);
        if (walk > 3.0f)
        {
            break;
        }
    }

    const float3 n = normalize(d);
    const float3 c = cross(n, params.axis);
    SelfTestMixElem e;
    e.pos = lerp(n, c, smoothstep(0.0f, 1.0f, saturate(jitter))) * clamp(length(d), 1.0f, 4.0f);
    e.val = (acc > 1.0f ? acc : -acc) + abs(walk) + (float)sign(c.x) + min(distance(n, c), 2.0f);
    e.tag = HashU32(id) >> 8u;
    outv[id] = e;
}
