// 自己テスト用カーネル: ホワイトリストの関数とサブセットの構文を一通り使う。C++ の CPU 版と C# の CPU 版の出力が
// バイト一致することを確かめる (D24)。FloatToInt / sin / cos の範囲外の折り返しも通る。出力は要素ごとに独立
#include "kernel_common.hlsli"

struct SelfTestAllFuncsElem
{
    float3 pos;
    float val;
    int2 cell;
    uint tag;
};

KERNEL_PARAMS(SelfTestAllFuncsParams)
{
    float time;
    float3 axis;
    uint seed;
    int count;
};

float SelfTestAllFuncsSum(float a, int n)
{
    float acc = 0.0f;
    for (int k = 0; k < n; ++k)
    {
        acc += a * (float)k;
    }
    return acc;
}

float3 SelfTestAllFuncsNorm(float3 v)
{
    if (dot(v, v) > 0.0f)
    {
        return normalize(v);
    }
    return float3(0.0f, 1.0f, 0.0f);
}

int SelfTestAllFuncsPick(int i)
{
    return i < 0 ? -i : (i > 9 ? 9 : i);
}

KERNEL(SelfTestAllFuncs, SelfTestAllFuncsParams, RW(SelfTestAllFuncsElem, outv) RO(float4, src))
{
    const float4 s = src[id];
    const float2 a2 = float2(s.x, s.y);
    const float3 a3 = float3(a2, s.z);
    const float4 a4 = float4(a3, s.w);
    float f = 0.0f;
    f += min(s.x, s.y) + max(s.y, s.z) + clamp(s.z, -1.0f, 1.0f);
    f += abs(s.x) + saturate(s.y) + lerp(s.x, s.y, 0.25f) + step(0.5f, s.z) + smoothstep(0.0f, 1.0f, s.w);
    f += floor(s.x) + ceil(s.y) + frac(s.z) + sqrt(abs(s.w)) + rsqrt(abs(s.x) + 1.0f) + rcp(abs(s.y) + 1.0f);
    f += dot(a3, a3) + length(a4) + distance(a2, float2(1.0f, 2.0f)) + sin(s.x) + cos(s.y);
    f += (float)sign(s.z) + (float)abs(-3) + (float)sign(-2);
    f += SelfTestAllFuncsSum(f, 3) + (float)SelfTestAllFuncsPick(params.count);

    const float3 c = cross(a3, params.axis);
    float3 v = SelfTestAllFuncsNorm(c + 0.001f);
    v = min(v, 2.0f) + max(v, c) - clamp(v, 0.0f, 1.0f);
    v = lerp(v, c, 0.5f) + lerp(v, c, c) + step(0.5f, v) * smoothstep(0.0f, 1.0f, v);
    v = abs(v) + saturate(v) + floor(v) + ceil(v) + frac(v) + sqrt(abs(v)) + rsqrt(abs(v) + 1.0f) + rcp(abs(v) + 1.0f);
    v = v + sin(v) + cos(v);
    v = -v * 2.0f / 3.0f;
    v.y = v.x;
    f += (float)sign(v).x;
    f += (float)FloatToInt(f * 0.001f);

    int2 q = int2(1, 2) * 3;
    q += int2(1, 1);
    q = q / 2;
    q = q % 3;
    const int2 m = min(q, 5) + max(q, int2(0, 0)) + clamp(q, 0, 9) + abs(q);
    uint3 u3 = uint3(1u, 2u, 3u);
    u3 = (u3 & 255u) | uint3(1u, 1u, 1u);
    u3 = u3 ^ 15u;
    uint h = HashU32(params.seed ^ id);
    h = ((h >> 3) & 0xFFu) | (1u << 2);
    h = ~h;

    int w = 0;
    while (w < 8)
    {
        w += 1;
        if (w == 2)
        {
            continue;
        }
        if (w > 5 || (w < 0 && !(f > 1.0f)))
        {
            break;
        }
    }
    w++;
    --w;
    int sgn = 0;
    if (f > 0.5f)
    {
        sgn = 1;
    }
    else if (f < -0.5f)
    {
        sgn = -1;
    }
    else
    {
        sgn = 0;
    }
    const float tf = f > 0.0f ? f : -f;
    const uint b = FloatToUint(abs(f)) + (uint)w;

    outv[id].pos = v;
    outv[id].val = tf + HashToUnitFloat(h) + (float)m.x + (float)u3.y + (float)sgn + (float)b + a3.x;
    outv[id].cell = q;
    outv[id].tag = h ^ u3.z;
    outv[id].tag = outv[id].tag + 1u;
}
