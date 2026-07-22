// IBL ベイク共通 (M38c)。Hammersley 列 + GGX importance sampling (Karis split-sum)。
// ベイクシェーダ (ibl_prefilter / ibl_irradiance / brdf_lut) が共有する。

static const float IBL_PI = 3.14159265358979f;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverse_VdC(i));
}

// GGX NDF に従う半ベクトルサンプル (N 周りの接空間へ変換済み)
float3 ImportanceSampleGGX(float2 xi, float3 N, float rough)
{
    const float a = rough * rough;
    const float phi = 2.0f * IBL_PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    const float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    const float3 up = (abs(N.z) < 0.999f) ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 tx = normalize(cross(up, N));
    const float3 ty = cross(N, tx);
    return normalize(tx * h.x + ty * h.y + N * h.z);
}

// ---- キューブ面ベイク共通 (BakeCB + フルスクリーン VS + 環境ソース) ----
// EnvMapBaker (C++) の BakeCB とレイアウト一致
cbuffer BakeCB : register(b0)
{
    float3 gFaceForward; float gRoughness;
    float3 gFaceRight;   int   gSrcMode; // 0=cubemap (t0) / 1=gradient (解析)
    float3 gFaceUp;      float _bakePad0;
    float3 gGradTop;     float _bakePad1;
    float3 gGradHorizon; float _bakePad2;
    float3 gGradBottom;  float _bakePad3;
};

TextureCube  gSrcCube : register(t0);
SamplerState gSampler : register(s0);

// 環境放射輝度 (方向 d)。gradient は skybox.hlsl と同じ補間式 (色はリニア変換済みで届く)。
// 単一 return (早期 return の混在は fxc が X4000 を誤検知する)
float3 SampleEnv(float3 d)
{
    float3 c;
    if (gSrcMode == 0) {
        c = gSrcCube.SampleLevel(gSampler, d, 0).rgb;
    } else {
        const float t = d.y;
        c = (t >= 0.0f) ? lerp(gGradHorizon, gGradTop, saturate(t * 1.4f))
                        : lerp(gGradHorizon, gGradBottom, saturate(-t * 1.4f));
    }
    return c;
}

struct BakeVSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

BakeVSOut BakeVS(uint vid)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    BakeVSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.ndc = corners[vid];
    return o;
}

// ピクセルのキューブ面方向 (ndc.y は上向き正 = gFaceUp と同符号)
float3 BakeDir(float2 ndc)
{
    return normalize(gFaceForward + ndc.x * gFaceRight + ndc.y * gFaceUp);
}
