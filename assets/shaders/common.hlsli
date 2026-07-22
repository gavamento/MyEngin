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

// CSM のカスケード選択付き PCF (M38d)。カスケード 0 (最詳細) から順に posW を射影し、
// 最初にマップ範囲へ収まったスライスで 3x3 比較平均。どれにも入らなければ 1 (影なし)。
// 範囲ベース選択なので view 深度の受け渡しが不要 (splits はデバッグ用に CB へ残す)。
float SampleShadowCSM(Texture2DArray shadowMap, SamplerComparisonState samp, float4x4 vp0,
                      float4x4 vp1, float4x4 vp2, int count, float3 posW, float texelSize)
{
    float4x4 vps[3] = { vp0, vp1, vp2 };
    float result = 1.0f;
    bool found = false;
    for (int c = 0; c < count; ++c) {
        if (found) {
            continue;
        }
        float4 lp = mul(float4(posW, 1.0f), vps[c]);
        lp.xyz /= lp.w;
        const float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;
        // 端 1% は次のカスケードへ (境界の PCF はみ出し回避)
        if (uv.x < 0.01f || uv.x > 0.99f || uv.y < 0.01f || uv.y > 0.99f || lp.z > 1.0f
            || lp.z < 0.0f) {
            continue;
        }
        const float d = lp.z - 0.0008f;
        float sum = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                sum += shadowMap.SampleCmpLevelZero(
                    samp, float3(uv + float2(x, y) * texelSize, float(c)), d);
            }
        }
        result = sum / 9.0f;
        found = true;
    }
    return result;
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

// roughness 考慮版 (IBL の拡散/鏡面配分用、M38c)
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float rough)
{
    const float3 fmax = max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), F0);
    return F0 + (fmax - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
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
// M38c: 環境項は iblEnabled != 0 なら split-sum IBL (irradiance + prefiltered + BRDF LUT)、
// 無効なら従来の定数アンビエント。IBL テクスチャは呼び出しシェーダのスロットから引数で渡す
// (forward=t3-5/s2、deferred=t5-7/s0 — スロットが異なるため)。
// ao (M38e): 環境項に掛ける遮蔽係数 (1=遮蔽なし。SSAO は Deferred のみ、Forward は 1 を渡す)
float3 ApplyLighting(float3 albedo, float3 normal, float3 posW, float3 cameraPos, float metallic,
                     float roughness, float3 ambient, Light lights[MAX_LIGHTS], int count,
                     float dirShadow, int iblEnabled, float iblSpecMips, TextureCube iblIrradiance,
                     TextureCube iblPrefiltered, Texture2D iblBrdfLut, SamplerState iblSampler,
                     float ao)
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
    // 環境項 (M38c): IBL (split-sum) または従来の定数アンビエント
    float3 ambientTerm;
    if (iblEnabled != 0) {
        const float3 kS = FresnelSchlickRoughness(ndv, F0, roughness);
        const float3 kD = (1.0f - kS) * (1.0f - metallic);
        // irradiance は「平均入射色」に正規化済み (直接光の 1/PI 省略規約と整合)
        const float3 diffuse = iblIrradiance.SampleLevel(iblSampler, N, 0).rgb * albedo * kD;
        const float3 R = reflect(-V, N);
        const float3 pre =
            iblPrefiltered.SampleLevel(iblSampler, R, roughness * iblSpecMips).rgb;
        const float2 brdf =
            iblBrdfLut.SampleLevel(iblSampler, float2(ndv, roughness), 0).rg;
        ambientTerm = diffuse + pre * (F0 * brdf.x + brdf.y);
    } else {
        // 簡易アンビエント (スカイ無し。誘電体のみ拡散に寄与 — 従来挙動)
        ambientTerm = ambient * albedo * (1.0f - metallic);
    }
    return ambientTerm * ao + Lo; // SSAO は環境項のみ減衰 (直接光には掛けない)
}


// ---- 距離フォグ (M29d) ----
// mode: -1=無効 / 0=linear (start..end) / 1=exp (1-e^-ρd) / 2=exp2 (1-e^-(ρd)²)。
// dist はカメラからのワールド距離。lit 済みの色をフォグ色へ補間する
float3 ApplyFog(float3 color, float3 fogColor, int fogMode, float density, float fogStart,
                float fogEnd, float dist)
{
    if (fogMode < 0) {
        return color;
    }
    float f = 0.0f;
    if (fogMode == 0) {
        f = saturate((dist - fogStart) / max(fogEnd - fogStart, 0.001f));
    } else if (fogMode == 1) {
        f = 1.0f - exp(-density * dist);
    } else {
        const float e = density * dist;
        f = 1.0f - exp(-e * e);
    }
    return lerp(color, fogColor, f);
}
