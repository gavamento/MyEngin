// M56d: SSR (スクリーンスペース反射) — HZB (M56c) の min-Z ピラミッドを階層的に辿る。
//
// ライトパス (+ スカイボックス) が書き終えたシーン色を読み、反射の**差分**だけを
// 加算合成で足し戻す。素の反射を上乗せしないのは、ライトパスが環境スペキュラ
// (`iblPrefiltered * ao * 環境BRDF`) を既に足しているから — そのまま足すと同じ光を
// 二重に数える。`(反射 - IBL スペキュラ) * 環境BRDF * 重み` を足すと、結果は
// common.hlsli の ApplyLightingHybrid が RT 反射に対してやっている「同次元の放射輝度を
// smoothstep で差し替える」式と一致し、重み 0 でちょうど 0 が足される (= 恒等)。
//
// ★交差判定は**深度から復元したワールド位置**で行う。GBuffer RT2 (R16G16B16A16F の
//   ワールド座標) は原点から離れると刻みが粗くなり、鏡像がガタつく。
//   逆投影に使うのは**ジッタ込みの view*proj** — 深度バッファをラスタライズした行列と
//   同じものでないと復元位置が半ピクセルずれる。
//
// ★階層トレースの核心は「セルを跨ぐ前に min 面に届くか」の比較。
//   セルの min-Z より手前のまま境界へ抜けるなら、そのセルには何も無い = 粗い段へ上がって
//   大股に進む。境界より手前で min 面に届くなら、そのセルの中に交差の候補がある =
//   細かい段へ降りる。**入口の 1 点だけを見て「手前だから空」と判断すると、セルの中で
//   面を突き抜ける**ので、必ず「境界に着くまでに届くか」で比べること。

#include "common.hlsli" // LinearizeDepth (M55a の共有版) / ApplyFog / FresnelSchlick 系

// **C++ 側の kSsrMaxSteps (SsrPass.h) と必ず一致させること** — 食い違っても絵は出る
// (反射が途中で切れるだけ) ので、規則 9 の静的検査が唯一の防波堤
#define MYE_SSR_MAX_STEPS 64

cbuffer SsrCB : register(b0)
{
    float4x4 gSsrViewProj;    // transpose(view*proj)、ジッタ込み
    float4x4 gSsrInvViewProj; // その逆行列 (同じく transpose 済み)
    float3   gSsrCameraPos;
    float    gSsrIntensity;
    float2   gSsrScreenSize;
    float    gSsrNearZ;
    float    gSsrFarZ;
    float    gSsrMaxRough;    // これ以上の roughness には 1 ビットも足さない
    float    gSsrThickness;   // 交差とみなす面の厚み [world]
    float    gSsrMaxDistance; // 光線のワールド長
    float    gSsrEdgeFade;    // 画面端のフェード幅 (UV 比)
    int      gSsrMipCount;
    int      gSsrIblEnabled;  // 1 = ライトパスが IBL スペキュラを足している → 差し引く
    float    gSsrIblSpecMips;
    int      gSsrAoEnabled;
    int      gSsrFogMode;     // -1 = フォグ無効
    float    gSsrFogDensity;
    float    gSsrFogStart;
    float    gSsrFogEnd;
    float    gSsrFogHeightFalloff;
    float    gSsrFogBaseHeight;
    float2   _ssrPad;
    // ---- M56f: ローカル反射プローブ (末尾 append)。0 = M56d と 1 ビットも変わらない ----
    // **引く基準値**を差し替えるためだけに要る。ライトパスがプローブで置き換えた画素から
    // 素の IBL を引くと、その差だけプローブの寄与が二重に乗る (絵は普通に出る)
    int      gSsrProbeCount;
    float    gSsrProbeSpecMips;
    float2   _ssrProbePad;
    ReflProbe gSsrProbes[MYE_MAX_REFLECTION_PROBES];
};

Texture2D   gSsrScene   : register(t0); // ライトパス出力のコピー (SRV 専用)
Texture2D<float> gSsrHzb : register(t1); // min-Z ピラミッド (mip 0 = 深度そのもの)
Texture2D   gSsrAlbedo  : register(t2); // a < 0.5 = ジオメトリ無し
Texture2D   gSsrNormal  : register(t3); // ワールド法線 *0.5+0.5
Texture2D   gSsrMaterial : register(t4); // r=metallic g=roughness
TextureCube gSsrIblPrefiltered : register(t5);
Texture2D   gSsrIblBrdfLut     : register(t6);
Texture2D   gSsrAo             : register(t7); // 半解像度 AO (ライトパスと同じ引き方)
TextureCubeArray gSsrProbeCubes : register(t8); // M56f (光パスの t14 と同じ実体)
SamplerState gSsrSamp : register(s0);          // LINEAR/CLAMP (光パスの iblSampler_ を流用)

// **CPU ミラー: SsrPass.h の同名関数 — 変更時は両方更新** (SsrSelfTest が検証)。
// 辺 cellSize px のセルの外へ出るのに必要な t の増分。**必ず minAdvance 以上**を返す —
// 境界のちょうど上に居る場合や軸に平行な光線で 0 を返すと、光線が同じ場所で反復を使い切る
float SsrCellAdvance(float px, float py, float dx, float dy, float cellSize, float minAdvance)
{
    const float big = 1.0e30f;
    float tx = big;
    float ty = big;
    if (abs(dx) > 1e-8f) {
        const float plane = (floor(px / cellSize) + ((dx > 0.0f) ? 1.0f : 0.0f)) * cellSize;
        tx = (plane - px) / dx;
    }
    if (abs(dy) > 1e-8f) {
        const float plane = (floor(py / cellSize) + ((dy > 0.0f) ? 1.0f : 0.0f)) * cellSize;
        ty = (plane - py) / dy;
    }
    const float adv = min(tx, ty) + 0.5f * minAdvance;
    return (adv > minAdvance) ? adv : minAdvance;
}

// **CPU ミラー: SsrPass.h の SsrReflWeight**。maxRough 以上でちょうど 0
float SsrReflWeight(float roughness, float maxRough)
{
    const float fadeStart = maxRough * (2.0f / 3.0f); // kSsrFadeStartRatio
    return 1.0f - smoothstep(fadeStart, maxRough, roughness);
}

// **CPU ミラー: SsrPass.h の SsrEdgeFade**
float SsrEdgeFade(float2 uv, float width)
{
    if (width <= 0.0f) {
        return (all(uv >= 0.0f) && all(uv <= 1.0f)) ? 1.0f : 0.0f;
    }
    const float2 d2 = min(uv, 1.0f - uv);
    return saturate(min(d2.x, d2.y) / width);
}

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

// 画面 UV + デバイス深度 → ワールド座標
float3 SsrWorldFromDepth(float2 uv, float d)
{
    const float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    const float4 w = mul(clip, gSsrInvViewProj);
    return w.xyz / w.w;
}

// クリップ座標 → 画面座標 (xy = px、z = デバイス深度)
float3 SsrScreenFromClip(float4 c)
{
    const float3 ndc = c.xyz / c.w;
    return float3((ndc.x * 0.5f + 0.5f) * gSsrScreenSize.x,
                  (0.5f - ndc.y * 0.5f) * gSsrScreenSize.y, ndc.z);
}

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    const float4 albedo = gSsrAlbedo.Load(pixel);
    if (albedo.a < 0.5f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // ジオメトリ無し (空) — 反射しない
    }
    const float4 matG = gSsrMaterial.Load(pixel);
    const float metallic = matG.r;
    const float roughness = matG.g;
    const float roughWeight = SsrReflWeight(roughness, gSsrMaxRough);
    if (roughWeight <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // 粗い面は IBL に任せる (厳密に 0 を足す)
    }
    const float d0 = gSsrHzb.Load(int3(pixel.xy, 0));
    if (d0 >= 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float2 uv0 = i.pos.xy / gSsrScreenSize;
    const float3 posW = SsrWorldFromDepth(uv0, d0);
    const float3 N = normalize(gSsrNormal.Load(pixel).xyz * 2.0f - 1.0f);
    const float3 V = normalize(gSsrCameraPos - posW);
    const float3 R = reflect(-V, N);

    // 受け面から離す。1 画素の覆うワールド幅が距離に比例するので、バイアスも比例させる
    const float viewZ = LinearizeDepth(d0, gSsrNearZ, gSsrFarZ);
    const float3 startW = posW + N * (viewZ * 0.002f); // kSsrNormalBiasRel

    float4 c0 = mul(float4(startW, 1.0f), gSsrViewProj);
    float4 c1 = mul(float4(startW + R * gSsrMaxDistance, 1.0f), gSsrViewProj);
    const float kMinW = 0.01f;
    if (c0.w <= kMinW) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (c1.w <= kMinW) {
        // 光線がカメラの背後へ抜ける — near 面の手前で切る (w は t に対して線形)
        c1 = lerp(c0, c1, (kMinW - c0.w) / (c1.w - c0.w));
    }
    const float3 s0 = SsrScreenFromClip(c0);
    const float3 s1 = SsrScreenFromClip(c1);
    const float3 dS = s1 - s0;
    const float len2 = dot(dS.xy, dS.xy);
    if (len2 < 1e-6f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // 画面上でほぼ動かない光線 (真正面の鏡)
    }
    const float minAdvance = 0.5f / sqrt(len2); // kSsrMinPixelStep = 0.5 px 相当

    const int maxLevel = gSsrMipCount - 1;
    int level = 0;
    float t = minAdvance; // 出発点そのものは判定しない (自分の深度に当たるだけ)
    bool hit = false;
    float tHit = 0.0f;
    [loop] for (int step = 0; step < MYE_SSR_MAX_STEPS; ++step) {
        if (hit || t >= 1.0f) {
            break;
        }
        const float3 p = s0 + dS * t;
        if (p.x < 0.0f || p.y < 0.0f || p.x >= gSsrScreenSize.x || p.y >= gSsrScreenSize.y) {
            break; // 画面の外 = 情報が無い
        }
        const float cell = exp2((float)level);
        const float minZ = gSsrHzb.Load(int3(int2(p.xy / cell), level));
        // セル境界へ抜ける t と、min 面に届く t
        const float tCell = t + SsrCellAdvance(p.x, p.y, dS.x, dS.y, cell, minAdvance);
        float tPlane = 1.0e30f;
        if (dS.z > 1e-9f && minZ < 0.99999f) { // kSsrSkyDepth: 空しか無いセルは常に素通り
            tPlane = (minZ - s0.z) / dS.z;
        }
        if (tPlane > tCell) {
            // このセルの中では min 面に届かない = 何にも当たらない → 粗い段へ上がる
            t = tCell;
            level = min(level + 1, maxLevel);
        } else if (level > 0) {
            t = max(t, tPlane); // 交差の候補がある → 進めずに細かい段へ降りる
            level -= 1;
        } else {
            // mip 0 = 実際の深度。tPlane <= t なら光線は既に面の裏側 (粗い段で飛び越した)
            const float tc = max(t, tPlane);
            const float rayLin = LinearizeDepth(s0.z + dS.z * tc, gSsrNearZ, gSsrFarZ);
            const float sceneLin = LinearizeDepth(minZ, gSsrNearZ, gSsrFarZ);
            if (rayLin - sceneLin < gSsrThickness) {
                hit = true;
                tHit = tc;
            } else {
                t = tCell; // 面の裏を通過した = このセルは当たりではない
            }
        }
    }
    if (!hit) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f); // 当たらなければ IBL のまま (何も足さない)
    }

    const float2 hitUv = (s0.xy + dS.xy * tHit) / gSsrScreenSize;
    const float conf = SsrEdgeFade(hitUv, gSsrEdgeFade);
    const float w = saturate(roughWeight * conf * gSsrIntensity);
    if (w <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // ---- 差分の組み立て (ライトパスのスペキュラ環境項と同じ次元・同じ係数) ----
    const float3 hitColor = gSsrScene.SampleLevel(gSsrSamp, hitUv, 0).rgb;
    const float ndv = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallic);
    const float2 brdf = gSsrIblBrdfLut.SampleLevel(gSsrSamp, float2(ndv, roughness), 0).rg;
    float ao = 1.0f;
    if (gSsrAoEnabled != 0) {
        ao = gSsrAo.SampleLevel(gSsrSamp, uv0, 0).r; // ライトパスと同じ引き方
    }
    float3 iblSpec = float3(0.0f, 0.0f, 0.0f);
    if (gSsrIblEnabled != 0) {
        iblSpec =
            gSsrIblPrefiltered.SampleLevel(gSsrSamp, R, roughness * gSsrIblSpecMips).rgb * ao;
    }
    // M56f: フォールバック連鎖 **SSR → ローカルプローブ → グローバル env**。
    // 引く基準値はライトパスが実際に足したもの = プローブが効いている画素では
    // 「lerp(IBL, プローブ, 重み)」。ここを素の IBL のままにすると、SSR とプローブを
    // 同時に on にした画素だけプローブの寄与が二重に乗る (deferred_light.hlsl の対)
    if (gSsrProbeCount > 0) {
        float pw = 0.0f;
        const float3 probeSpec = ReflProbeRadiance(gSsrProbeCubes, gSsrSamp, gSsrProbes,
                                                   gSsrProbeCount, posW, R, roughness,
                                                   gSsrProbeSpecMips, pw);
        iblSpec = lerp(iblSpec, probeSpec * ao, pw);
    }
    float3 delta = (hitColor - iblSpec) * (F0 * brdf.x + brdf.y) * w;

    // ライトパスは環境項を足した**後**にフォグを掛けている。後から足す差分にも
    // 同じ透過率を掛けないと、遠くの反射だけが霧を突き抜ける。
    // 係数そのものは ApplyFog を「黒 → 白」で呼んで取り出す (式を複製しない)
    if (gSsrFogMode >= 0) {
        const float3 zero3 = float3(0.0f, 0.0f, 0.0f);
        const float f = ApplyFog(zero3, float3(1.0f, 1.0f, 1.0f), gSsrFogMode, gSsrFogDensity,
                                 gSsrFogStart, gSsrFogEnd, gSsrCameraPos, posW,
                                 gSsrFogHeightFalloff, gSsrFogBaseHeight, float3(0, -1, 0), zero3,
                                 0.0f, 1.0f)
                            .x;
        delta *= (1.0f - f);
    }
    return float4(delta, 0.0f);
}
