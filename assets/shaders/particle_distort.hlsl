// スクリーンスペース歪みパーティクル (M42d)。blendMode=2 のエミッタを
// R16G16F の歪みバッファへ加算描画し、postfx_tonemap がシーンサンプル UV に加算する。
// VS/CB レイアウトは particle_render.hlsl と同一 (CPU バックエンドが同じ CB を使う)。
// 深度テストは read-only DSV で有効 = 遮蔽された粒子は歪まない。

cbuffer ParticleCB : register(b0)
{
    float4x4 gViewProj;
    float3   gCamRight;
    float    _p0;
    float3   gCamUp;
    float    _p1;
    uint     gBaseIndex;
    uint     gUseTexture;  // 未使用 (歪みは procedural 放射勾配のみ)
    int      gFlipTilesX;
    int      gFlipTilesY;
    float    gFlipCycles;
    int      gBlendAdditive;
    int      gFogMode;
    float    _p2;
    float3   gCameraPos;
    float    gFogDensity;
    float3   gFogColor;
    float    gFogStart;
    float    gFogEnd;
    float    gSoftFade; // M42b (歪みでは未使用 — 交差の緩和は将来拡張)
    float    gNearZ;
    float    gFarZ;
};

struct ParticleInstance
{
    float3 pos;
    float  size;
    float4 color;
    float  age;
    float3 _pad;
};
StructuredBuffer<ParticleInstance> gParticles : register(t0);

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float  alpha : TEXCOORD1; // 歪み強度 (color.a を流用)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const ParticleInstance p = gParticles[gBaseIndex + iid];
    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    const float3 world = p.pos + (gCamRight * corner.x + gCamUp * corner.y) * p.size;

    VSOut o;
    o.pos = mul(float4(world, 1.0f), gViewProj);
    o.uv = corner * 0.5f + 0.5f;
    o.alpha = p.color.a;
    return o;
}

// 最大歪み量 (UV 単位)。alpha=1 の粒子中心付近でこの割合だけシーンサンプルがずれる
static const float kDistortionScale = 0.05f;

float2 PSMain(VSOut i) : SV_Target
{
    // 中心から外向きの放射勾配。縁で 0 に落として billboard 境界を見せない
    const float2 d = i.uv * 2.0f - 1.0f;
    const float r2 = dot(d, d);
    float m = saturate(1.0f - r2);
    m *= m;
    // 中心 (d=0) では方向が無いので r で減衰した放射ベクトルを使う
    return d * m * i.alpha * kDistortionScale;
}
