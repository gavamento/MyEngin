// GPU パーティクル描画: alive list + pool から直接ビルボード生成
// DrawInstancedIndirect (InstanceCount は CopyStructureCount で GPU 上のみで確定)

#include "particle_gpu_common.hlsli"

cbuffer GpuRenderCB : register(b1)
{
    float4x4 gViewProj;
    float3   gCamRight;
    float    _q0;
    float3   gCamUp;
    float    _q1;
    float    gOffsetX; // 比較モードの横オフセット
    float3   _q2;
};

StructuredBuffer<GpuParticle> gPoolSRV : register(t0);
StructuredBuffer<uint> gAliveList : register(t1);

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const GpuParticle p = gPoolSRV[gAliveList[iid]];
    const float age = saturate(1.0f - p.life * p.invLife);
    const float size = p.size0 * (1.0f + (gParams.z - 1.0f) * age);
    const float4 color = lerp(gColorBegin, gColorEnd, age);

    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    const float3 world = p.pos + float3(gOffsetX, 0, 0)
        + (gCamRight * corner.x + gCamUp * corner.y) * size;

    VSOut o;
    o.pos = mul(float4(world, 1.0f), gViewProj);
    o.uv = corner * 0.5f + 0.5f;
    o.color = color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float2 d = i.uv * 2.0f - 1.0f;
    float m = saturate(1.0f - dot(d, d));
    m *= m;
    return float4(i.color.rgb * m, i.color.a * m);
}
