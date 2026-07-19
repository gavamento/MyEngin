// CPU パーティクル描画 (ビルボード展開を VS で行う)
// DrawInstanced(4, count) + TRIANGLESTRIP。頂点入力なし (SV_VertexID / SV_InstanceID)

cbuffer ParticleCB : register(b0)
{
    float4x4 gViewProj;
    float3   gCamRight;
    float    _p0;
    float3   gCamUp;
    float    _p1;
    uint     gBaseIndex;
    float3   _p2;
};

struct ParticleInstance
{
    float3 pos;
    float  size;
    float4 color;
};
StructuredBuffer<ParticleInstance> gParticles : register(t0);

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const ParticleInstance p = gParticles[gBaseIndex + iid];
    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    const float3 world = p.pos + (gCamRight * corner.x + gCamUp * corner.y) * p.size;

    VSOut o;
    o.pos = mul(float4(world, 1.0f), gViewProj);
    o.uv = corner * 0.5f + 0.5f;
    o.color = p.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float2 d = i.uv * 2.0f - 1.0f;
    float m = saturate(1.0f - dot(d, d));
    m *= m; // ソフトな円形フォールオフ
    return float4(i.color.rgb * m, i.color.a * m);
}
