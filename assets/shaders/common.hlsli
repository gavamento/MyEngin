// 共通ライティングヘルパ。
// このファイルの変更は include 依存グラフ経由で全依存シェーダを再コンパイルさせる (spec 8.1)

float3 ApplyDirectionalLight(float3 albedo, float3 normal, float3 lightDir, float3 lightColor,
                             float intensity, float3 ambient)
{
    const float ndl = saturate(dot(normal, -lightDir));
    return albedo * (ambient + lightColor * intensity * ndl);
}

#define MAX_LIGHTS 16

// GPU ライト 1 個 (C++ 側 GpuLight とレイアウト一致 = 64 バイト)
struct Light
{
    float3 position;  // Point/Spot: ワールド位置
    float  range;     // Point/Spot: 減衰半径
    float3 direction; // 光の進行方向 (正規化、Dir/Spot)
    float  intensity;
    float3 color;
    int    type;      // 0=Directional 1=Point 2=Spot
    float  cosInner;  // Spot: cos(内角)
    float  cosOuter;  // Spot: cos(外角)
    float2 _pad;
};

// シャドウマップの PCF サンプル (M17)。posW をライトクリップ空間へ射影し 3x3 比較平均。
// 戻り値 1=完全に照らされる, 0=完全に影。範囲外は 1 (影なし)。
float SampleShadowPCF(Texture2D shadowMap, SamplerComparisonState samp, float4x4 lightViewProj,
                      float3 posW, float texelSize)
{
    float4 lp = mul(float4(posW, 1.0f), lightViewProj);
    lp.xyz /= lp.w;
    const float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lp.z > 1.0f) {
        return 1.0f; // シャドウマップ範囲外は影を落とさない
    }
    const float d = lp.z - 0.0008f; // 定数バイアス (ラスタライザ側の傾斜バイアスと併用)
    float sum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            sum += shadowMap.SampleCmpLevelZero(samp, uv + float2(x, y) * texelSize, d);
        }
    }
    return sum / 9.0f;
}

// ノーマルマップの摂動 (M17.3)。Christian Schüler の微分ベース TBN
// ("Normal Mapping Without Precomputed Tangents") — 頂点タンジェント不要。
// 画面空間の posW/uv 微分から接空間基底を再構成する。gHasNormal が uniform なので
// 呼び出しを uniform 分岐内に置けば ddx/ddy の勾配計算は安全。
// tsN: 接空間法線 ([-1,1] にデコード済み)。戻り値: 摂動済みワールド法線 (正規化)。
float3 PerturbNormal(float3 N, float3 posW, float2 uv, float3 tsN)
{
    const float3 dp1 = ddx(posW);
    const float3 dp2 = ddy(posW);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, N);
    const float3 dp1perp = cross(N, dp1);
    const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    const float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    // TBN の行 = (T,B,N)。行ベクトル規約なので mul(tsN, TBN) で接空間→ワールド変換
    const float3x3 TBN = float3x3(T * invmax, B * invmax, N);
    return normalize(mul(tsN, TBN));
}

// ---- Cook-Torrance PBR (metallic-roughness ワークフロー、M17) ----
static const float MYE_PI = 3.14159265358979f;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float DistributionGGX(float3 N, float3 H, float rough)
{
    const float a = rough * rough;
    const float a2 = a * a;
    const float ndh = saturate(dot(N, H));
    const float d = ndh * ndh * (a2 - 1.0f) + 1.0f;
    return a2 / max(MYE_PI * d * d, 1e-6f);
}

float GeometrySchlickGGX(float ndv, float rough)
{
    const float r = rough + 1.0f;
    const float k = (r * r) / 8.0f;
    return ndv / (ndv * (1.0f - k) + k);
}

float GeometrySmith(float ndv, float ndl, float rough)
{
    return GeometrySchlickGGX(ndv, rough) * GeometrySchlickGGX(ndl, rough);
}

// 全ライトを Cook-Torrance で積算して最終色を返す (Forward / Deferred 共通)。
// posW はワールド座標、cameraPos は視点。dirShadow は平行光 (type 0) のシャドウ係数 (1=影なし)。
float3 ApplyLighting(float3 albedo, float3 normal, float3 posW, float3 cameraPos, float metallic,
                     float roughness, float3 ambient, Light lights[MAX_LIGHTS], int count,
                     float dirShadow)
{
    const float3 N = normal;
    const float3 V = normalize(cameraPos - posW);
    const float ndv = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < count; ++i)
    {
        const Light L = lights[i];
        float3 toLightDir; // 表面 → 光源
        float atten = 1.0f;
        if (L.type == 0) // Directional
        {
            toLightDir = -L.direction;
            atten = dirShadow;
        }
        else // Point / Spot
        {
            const float3 toLight = L.position - posW;
            const float dist = length(toLight);
            toLightDir = toLight / max(dist, 1e-4f);
            const float d = saturate(1.0f - dist / max(L.range, 1e-4f));
            atten = d * d;
            if (L.type == 2) // Spot
            {
                const float cosA = dot(-toLightDir, L.direction);
                const float spot =
                    saturate((cosA - L.cosOuter) / max(L.cosInner - L.cosOuter, 1e-4f));
                atten *= spot * spot;
            }
        }
        const float3 Ldir = toLightDir;
        const float3 H = normalize(V + Ldir);
        const float ndl = saturate(dot(N, Ldir));
        const float3 radiance = L.color * (L.intensity * atten);

        const float NDF = DistributionGGX(N, H, roughness);
        const float G = GeometrySmith(ndv, ndl, roughness);
        const float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
        const float3 specular = (NDF * G * F) / max(4.0f * ndv * ndl, 1e-4f);
        const float3 kd = (1.0f - F) * (1.0f - metallic); // 金属は拡散なし
        // 拡散は 1/PI を省く (既存コンテンツの明るさを維持。ライト強度の再調整を避ける)。
        Lo += (kd * albedo + specular) * radiance * ndl;
    }
    // 簡易アンビエント (IBL 無し。誘電体のみ拡散に寄与)
    const float3 ambientTerm = ambient * albedo * (1.0f - metallic);
    return ambientTerm + Lo;
}

