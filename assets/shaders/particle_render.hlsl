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
    uint     gUseTexture;  // 0=procedural 円 / 1=フリップブックテクスチャ
    int      gFlipTilesX;
    int      gFlipTilesY;
    float    gFlipCycles;  // 寿命あたりのフリップブック周回数
    int      gBlendAdditive; // 1=additive (fog=減光) / 0=alpha (fog=色 lerp)
    int      gFogMode;       // -1=off / 0=linear 1=exp 2=exp2
    float    _p2;
    float3   gCameraPos;
    float    gFogDensity;
    float3   gFogColor;
    float    gFogStart;
    float    gFogEnd;
    float3   _p3;
};

struct ParticleInstance
{
    float3 pos;
    float  size;
    float4 color;
    float  age;   // [0,1] 寿命係数
    float3 _pad;
};
StructuredBuffer<ParticleInstance> gParticles : register(t0);

Texture2D    gTex  : register(t1);
SamplerState gSamp : register(s0);

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  age   : TEXCOORD1;
    float  dist  : TEXCOORD2; // カメラからのワールド距離 (フォグ用)
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
    o.age = p.age;
    o.dist = length(world - gCameraPos);
    return o;
}

// 距離フォグ係数 (common.hlsli::ApplyFog と同一。粒子は register 衝突回避のため独立定義)
float ParticleFogFactor(float dist)
{
    if (gFogMode < 0) { return 0.0f; }
    if (gFogMode == 0) { return saturate((dist - gFogStart) / max(gFogEnd - gFogStart, 0.001f)); }
    if (gFogMode == 1) { return 1.0f - exp(-gFogDensity * dist); }
    const float e = gFogDensity * dist;
    return 1.0f - exp(-e * e);
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 col;
    if (gUseTexture != 0)
    {
        // フリップブック: age を進めてタイルを選ぶ (tilesX*tilesY コマ、flipCycles 周)
        const uint tx = (uint)max(1, gFlipTilesX);
        const uint ty = (uint)max(1, gFlipTilesY);
        const uint tiles = tx * ty;
        uint frame = (uint)max(0, (int)floor(i.age * gFlipCycles * (float)tiles));
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

    // フォグ (M32c): additive は減光、alpha はフォグ色へ補間
    const float f = ParticleFogFactor(i.dist);
    if (gBlendAdditive != 0) {
        col.rgb *= (1.0f - f);
    } else {
        col.rgb = lerp(col.rgb, gFogColor, f);
    }
    return col;
}
