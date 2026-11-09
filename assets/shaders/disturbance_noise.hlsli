// かき乱しフィールド (M96) の霧のノイズ: 数値のノイズ (3D の値ノイズの fBm) とマップのノイズ (テクスチャを真上から敷く)。
//
// ★C++ の DisturbanceNoise.h (DisturbanceNoiseCB / kDisturbanceNoisePeriod / kDisturbanceNoiseMaxOctaves) と一致させること
//   (規則 9 が定数を、DisturbanceNoise.h の static_assert が CB の大きさを見る)。
// ★見た目だけ。sim と FieldSample は読まない (霧の判定はノイズの無い濃さのまま)。
// ★強さ 0 のノイズは読まずに 1 を返す = 霧の濃さはビット一致 (既定の絵は 1 画素も動かない)。
// ★時刻は C++ が「流れ × 時刻」を周期で折り返して CB に入れる (長く回しても float の桁が落ちない)。
//   シェーダは時刻を読まない
#ifndef MYE_DISTURBANCE_NOISE_HLSLI
#define MYE_DISTURBANCE_NOISE_HLSLI

// 値ノイズの格子の周期 [格子] (C++ の kDisturbanceNoisePeriod。2 の冪 = 負の格子番号もビット積で折り返せる)
#define MYE_DFLD_NOISE_PERIOD 256
// 重ね数の上限 (C++ の kDisturbanceNoiseMaxOctaves)
#define MYE_DFLD_NOISE_MAX_OCTAVES 6
// fBm は重ねるほど 0.5 の周りへ寄るので、この倍率で 0.5 から引き伸ばしてから [0,1] へ切る (平均は 0.5 のまま)
#define MYE_DFLD_NOISE_CONTRAST 2.0f

// C++ の DisturbanceNoiseCB と同じ並び (float4 x 3 = 48 バイト)
struct DisturbanceNoise
{
    float4 a; // xyz = 数値のノイズの座標のずらし [格子], w = 1 / 大きさ [1/m]
    float4 b; // x = 数値のノイズの強さ (0 = 掛けない), y = 重ね数, z = 形の変化の位相 [格子], w = マップの強さ (0 = 掛けない)
    float4 c; // xy = マップの UV のずらし, z = 1 / マップの大きさ [1/m], w = 1: マップを張った
};

// 格子点のハッシュ [0, 2^32)。格子番号は周期で折り返す (タイルする = ずらしを周期で折り返しても模様が飛ばない)
uint DfldNoiseHash(int3 cell)
{
    const uint3 p = (uint3)cell & (uint)(MYE_DFLD_NOISE_PERIOD - 1);
    uint h = p.x * 73856093u ^ p.y * 19349663u ^ p.z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float DfldNoiseLattice(int3 cell)
{
    return (float)DfldNoiseHash(cell) * (1.0f / 4294967295.0f);
}

// 3D の値ノイズ [0,1] (格子点の乱数を smoothstep で三線形補間)
float DfldValueNoise(float3 q)
{
    const float3 fl = floor(q);
    const int3 i = (int3)fl;
    const float3 f = q - fl;
    const float3 u = f * f * (3.0f - 2.0f * f);
    const float n000 = DfldNoiseLattice(i);
    const float n100 = DfldNoiseLattice(i + int3(1, 0, 0));
    const float n010 = DfldNoiseLattice(i + int3(0, 1, 0));
    const float n110 = DfldNoiseLattice(i + int3(1, 1, 0));
    const float n001 = DfldNoiseLattice(i + int3(0, 0, 1));
    const float n101 = DfldNoiseLattice(i + int3(1, 0, 1));
    const float n011 = DfldNoiseLattice(i + int3(0, 1, 1));
    const float n111 = DfldNoiseLattice(i + int3(1, 1, 1));
    const float x00 = lerp(n000, n100, u.x);
    const float x10 = lerp(n010, n110, u.x);
    const float x01 = lerp(n001, n101, u.x);
    const float x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// fBm [0,1]。重ねるごとに周波数 2 倍・振幅 1/2。重ねごとのずらしは整数 (周期を保つ)
float DfldNoiseFbm(float3 q, int octaves)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    [loop] for (int k = 0; k < MYE_DFLD_NOISE_MAX_OCTAVES; ++k) {
        if (k >= octaves) {
            break;
        }
        sum += amp * DfldValueNoise(q + float3(17.0f, 31.0f, 47.0f) * (float)k);
        total += amp;
        q *= 2.0f;
        amp *= 0.5f;
    }
    return sum / max(total, 1e-6f);
}

// ノイズ n [0,1] と強さ s から濃さの倍率 1 + s × (2n - 1) (>= 0)。平均の濃さを変えずにムラだけを付ける
float DfldNoiseGain(float n, float strength)
{
    return max(1.0f + strength * (2.0f * n - 1.0f), 0.0f);
}

// 数値のノイズだけの倍率 (マップを読めない段、またはマップの無い場)
float DfldNoiseProcedural(DisturbanceNoise N, float3 posW)
{
    if (N.b.x <= 0.0f) {
        return 1.0f;
    }
    const float3 q = posW * N.a.w + N.a.xyz + float3(0.0f, N.b.z, 0.0f);
    const float n = DfldNoiseFbm(q, (int)N.b.y);
    return DfldNoiseGain(saturate(0.5f + (n - 0.5f) * MYE_DFLD_NOISE_CONTRAST), N.b.x);
}

// 数値のノイズ × マップのノイズの倍率。マップは R をリニアで読み、ワールドの xz に敷く (map は wrap のサンプラで読む)
float DfldNoiseFactor(DisturbanceNoise N, float3 posW, Texture2D map, SamplerState wrapSampler)
{
    float gain = DfldNoiseProcedural(N, posW);
    if (N.b.w > 0.0f && N.c.w > 0.5f) {
        const float m = map.SampleLevel(wrapSampler, posW.xz * N.c.z + N.c.xy, 0).r;
        gain *= DfldNoiseGain(m, N.b.w);
    }
    return gain;
}

#endif
