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
Texture2D gDepth : register(t2); // M42b: シーン深度 (PS のみ。read-only DSV とセット)
Texture2D gTex   : register(t3); // M42c: フリップブックテクスチャ (未使用時は白)
SamplerState gSamp : register(s0);

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  viewZ : TEXCOORD1; // M42b: ビュー空間深度 (= clip.w)
    float  age   : TEXCOORD2; // M42c: [0,1] 寿命係数 (フリップブック用)
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
    o.viewZ = o.pos.w; // 透視投影では clip.w = ビュー空間 z
    o.age = age;       // M42c: フリップブック用
    return o;
}

// M42b: 非線形深度 [0,1] -> ビュー空間 z (particle_render.hlsl と同一式)
float LinearizeSceneDepth(float d)
{
    const float n = gParams2.y, f = gParams2.z;
    return n * f / max(f - d * (f - n), 1e-4f);
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 col;
    if (gParams3.x != 0.0f)
    {
        // フリップブック (M42c): particle_render.hlsl PSMain のフリップブック分岐を移植 (同一式)
        const uint tx = (uint)max(1.0f, gParams3.y);
        const uint ty = (uint)max(1.0f, gParams3.z);
        const uint tiles = tx * ty;
        uint frame = (uint)max(0, (int)floor(i.age * gParams3.w * (float)tiles));
        frame = frame % tiles;
        const uint cx = frame % tx;
        const uint cy = frame / tx;
        const float2 uv = (i.uv + float2(cx, cy)) / float2(tx, ty);
        const float4 tex = gTex.Sample(gSamp, uv);
        col = float4(i.color.rgb * tex.rgb, i.color.a * tex.a);
    }
    else
    {
        // procedural ソフト円形 (テクスチャ未指定時)
        const float2 d = i.uv * 2.0f - 1.0f;
        float m = saturate(1.0f - dot(d, d));
        m *= m;
        col = float4(i.color.rgb * m, i.color.a * m);
    }

    // ソフトパーティクル (M42b): 0=off (従来とビット同一)。CPU 版 particle_render.hlsl と同一式
    if (gParams2.x > 0.0f) {
        const float sceneZ = LinearizeSceneDepth(gDepth.Load(int3(int2(i.pos.xy), 0)).r);
        col *= saturate((sceneZ - i.viewZ) / max(gParams2.x, 1e-4f));
    }
    return col;
}
