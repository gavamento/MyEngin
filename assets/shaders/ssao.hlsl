// SSAO (M38e、Deferred 限定)。ワールド座標 GBuffer + 法線から半球 16 サンプルの遮蔽を
// 半解像度で求める。ランダム回転は 4x4 ノイズテクスチャ (タイル) で分散し、後段の
// ssao_blur (4x4 box) でノイズを均す。出力 R8: 1=遮蔽なし。

cbuffer SsaoCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj) — 他シェーダと同じ行ベクトル規約
    float3   gCameraPos;
    float    gRadius;     // サンプル半径 (ワールド)
    float2   gNoiseScale; // (halfW/4, halfH/4) — 4x4 ノイズをピクセル単位でタイル
    float    gIntensity;  // 遮蔽の効き (0=無効)
    float    gBias;       // 自己遮蔽回避
};

Texture2D gPosition : register(t0); // フル解像度 GBuffer (w=0 はジオメトリ無し)
Texture2D gNormal   : register(t1);
Texture2D gNoise    : register(t2); // 4x4 ランダムベクトル
SamplerState gPointClamp : register(s0);
SamplerState gPointWrap  : register(s1);

// 半球カーネル (z>0、長さを内側に寄せた固定 16 点 — ベイク不要の決定的テーブル)
static const float3 kKernel[16] = {
    float3( 0.53,  0.20, 0.47), float3(-0.26,  0.31, 0.35), float3( 0.10, -0.58, 0.42),
    float3(-0.44, -0.28, 0.28), float3( 0.72, -0.15, 0.23), float3(-0.17,  0.68, 0.30),
    float3( 0.28,  0.44, 0.71), float3(-0.61,  0.34, 0.44), float3( 0.35, -0.30, 0.68),
    float3(-0.23, -0.62, 0.53), float3( 0.09,  0.14, 0.30), float3(-0.14,  0.07, 0.20),
    float3( 0.20, -0.10, 0.16), float3(-0.07, -0.18, 0.24), float3( 0.44,  0.62, 0.38),
    float3(-0.52, -0.47, 0.61),
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float4 center = gPosition.SampleLevel(gPointClamp, i.uv, 0);
    if (center.w < 0.5f) {
        return 1.0f; // ジオメトリ無し (空)
    }
    const float3 posW = center.xyz;
    const float3 n = normalize(gNormal.SampleLevel(gPointClamp, i.uv, 0).xyz * 2.0f - 1.0f);
    const float3 rand =
        normalize(gNoise.SampleLevel(gPointWrap, i.uv * gNoiseScale, 0).xyz * 2.0f - 1.0f);
    // Gram-Schmidt でノイズ回転付き接空間基底
    const float3 tangent = normalize(rand - n * dot(rand, n));
    const float3 bitangent = cross(n, tangent);
    const float3x3 tbn = float3x3(tangent, bitangent, n);

    const float centerDist = length(posW - gCameraPos);
    float occlusion = 0.0f;
    [unroll] for (int s = 0; s < 16; ++s) {
        const float3 samplePos = posW + mul(kKernel[s], tbn) * gRadius;
        float4 clip = mul(float4(samplePos, 1.0f), gViewProj);
        if (clip.w <= 0.0f) {
            continue;
        }
        clip.xyz /= clip.w;
        const float2 uv2 = clip.xy * float2(0.5f, -0.5f) + 0.5f;
        if (uv2.x < 0.0f || uv2.x > 1.0f || uv2.y < 0.0f || uv2.y > 1.0f) {
            continue;
        }
        const float4 scene = gPosition.SampleLevel(gPointClamp, uv2, 0);
        if (scene.w < 0.5f) {
            continue; // 空
        }
        const float sceneDist = length(scene.xyz - gCameraPos);
        const float sampleDist = length(samplePos - gCameraPos);
        // 遮蔽: シーンがサンプル点より手前。範囲チェックで遠景の誤遮蔽を抑制
        const float rangeCheck = saturate(gRadius / max(abs(centerDist - sceneDist), 1e-4f));
        occlusion += (sceneDist < sampleDist - gBias ? 1.0f : 0.0f) * rangeCheck;
    }
    const float ao = 1.0f - gIntensity * (occlusion / 16.0f);
    return saturate(ao);
}
