// 共通ライティングヘルパ。
// このファイルの変更は include 依存グラフ経由で全依存シェーダを再コンパイルさせる (spec 8.1)

// ---- 深度ユーティリティ (M55a) ----
// 透視投影の非線形深度 [0,1] → ビュー空間 z。逆行列を要求せず near/far だけで解けるので、
// 深度 SRV さえあればどのパスからでも呼べる (DoF / ソフトパーティクル / 今後の HZB・SSR・froxel)。
// 分母の 1e-4 クランプは d==1 かつ near==0 のゼロ除算よけ — 一本化前の 5 つの複製すべてに
// 同じ形で入っていたので、そのまま共有版の仕様として残す。
// **CPU ミラー: PostFxMath.h::LinearizeDepth — 変更時は両方更新** (RenderSelfTest が検証)。
// ★このファイルは register 宣言を 1 つも持たない (純関数 + 構造体だけ) ので、
//   postfx / particle 系のように独自のスロット割当を持つシェーダからも安全に include できる。
//   cs_5_0 でも通る — 未使用の PerturbNormal (ddx/ddy) は検証前に落とされるため。
float LinearizeDepth(float d, float nearZ, float farZ)
{
    return nearZ * farZ / max(farZ - d * (farZ - nearZ), 1e-4f);
}

// ---- 画面速度 (M55c: GBuffer RT4 = velocity) ----
// **velocity = 今フレームの UV − 前フレームの UV**。読む側は prevUv = uv - velocity。
// curClip は**ジッタ込み**の proj で作られている (ラスタライズと同じ行列でないとピクセル
// 中心とずれる) ので、NDC にしてからジッタを引き戻す。prevClip 側の行列は
// RenderView::prevViewProj = **非ジッタ**なので何も引かない — ここを揃えないと
// 「静止物が毎フレーム半ピクセル動く」velocity になり TAA が履歴を外す。
// w<=0 (カメラ背面) は 0 を返す = 消費側はカメラ再投影のみへ縮退する。
// **CPU ミラー: PostFxMath.h の mye::velocity::FromClip — 変更時は両方更新**
// (RenderSelfTest の TestVelocityUv が検証)。
float2 ComputeVelocityUv(float4 curClip, float4 prevClip, float2 jitterNdc)
{
    if (curClip.w <= 1e-6f || prevClip.w <= 1e-6f) {
        return float2(0.0f, 0.0f);
    }
    const float2 curNdc = curClip.xy / curClip.w - jitterNdc;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    return (curNdc - prevNdc) * float2(0.5f, -0.5f); // NDC は上向き / UV は下向き
}

float3 ApplyDirectionalLight(float3 albedo, float3 normal, float3 lightDir, float3 lightColor,
                             float intensity, float3 ambient)
{
    const float ndl = saturate(dot(normal, -lightDir));
    return albedo * (ambient + lightColor * intensity * ndl);
}

// C++ の kMaxLights (RenderTypes.h) / rt_common.hlsli の MYE_RT_MAX_LIGHTS と同値。
// tools\check_rules.ps1 の規則 9 が 3 者の一致を静的に検査する (M55a で登録)
#define MAX_LIGHTS 16

// 自己発光強度を G-Buffer (R8G8B8A8 の b チャンネル) へ詰めるときの正規化上限 (M46i)。
// **C++ 側の kEmissiveMaxIntensity (RenderTypes.h) と必ず一致させること** —
// tools\check_rules.ps1 の規則 9 が静的に検査する
#define MYE_EMISSIVE_MAX 8

// 自己発光強度 → G-Buffer の b チャンネル (0..1)。0 はちょうど 0 に落ちるので、
// 発光を使わないマテリアルは M46i 以前と 1 ビットも変わらない
float EncodeEmissive(float intensity)
{
    return saturate(intensity / (float)MYE_EMISSIVE_MAX);
}

float DecodeEmissive(float encoded)
{
    return encoded * (float)MYE_EMISSIVE_MAX;
}

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
    // ---- M54c: シャドウアトラス (旧 _pad の再利用。64 バイトのままなので既存 3 ミラー不変) ----
    int    shadowTile;  // アトラスのタイル index (先頭面)
    int    shadowFaces; // 0=影を投げない / 1=スポット / 6=点光源 (M54d)
};

// ---- 局所ライトのシャドウアトラス (M54c) ----
// C++ の kMaxShadowTiles (RenderTypes.h) と同値。
// tools\check_rules.ps1 の規則 9 が一致を静的に検査する
#define MYE_MAX_SHADOW_TILES 16

// アトラスのタイル 1 枚 (C++ の DeferredPath::ShadowTileCB と 96 バイトで一致)。
// SRV スロットを 1 本しか取れない (統合契約 予約 2) ので StructuredBuffer ではなく CB に置く
struct ShadowTile
{
    float4x4 lightViewProj; // transpose(lightView * lightProj)
    float4   uvScaleBias;   // xy = タイル UV → アトラス UV の拡大率 / zw = オフセット
    float4   params;        // x = 定数深度バイアス (NDC) / yzw = 予約 (M54d)
};

// 点光源のキューブ 6 面のうち dir がどの面へ落ちるか (M54d)。
// 面順は **D3D の cubemap 面順 (+X,-X,+Y,-Y,+Z,-Z)** — C++ 側 RenderSystem.cpp の
// kCubeFaces / EnvMapBaker.cpp の kFaces と同一表。3 者がずれると「影が隣の面から来る」
// という、絵は出るのに合わないだけの静かな壊れ方をする。
// ★絶対値最大の軸を選ぶ = ちょうど 90 度の境界で切れる。面 VP 側は 90 度より僅かに
//   広く (タイル境界に PCF 用の余白を 2 テクセル) 焼いてあるので、境界画素の 3x3 タップが
//   タイル外へ出てクランプされることがない (RenderSystem.cpp の ComputePointLightFaceVP)。
int CubeFaceIndex(float3 dir)
{
    const float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) {
        return dir.x >= 0.0f ? 0 : 1;
    }
    if (a.y >= a.z) {
        return dir.y >= 0.0f ? 2 : 3;
    }
    return dir.z >= 0.0f ? 4 : 5;
}

// ライト 1 本のアトラス内タイル index (M54d)。スポットは先頭タイルそのまま、点光源は
// 6 面のうち表面がどの面から見えるかで先頭 + 面番号。**光パス 3 経路 (M54e) で同じ式を
// 使うためにここへ置いてある** — 呼び出し側でインライン展開すると面順の定義が散る。
int ShadowTileIndexForLight(Light L, float3 posW)
{
    return (L.shadowFaces == 6) ? (L.shadowTile + CubeFaceIndex(posW - L.position)) : L.shadowTile;
}

// アトラスの 1 タイルを 3x3 PCF で引く (M54c)。戻り値 1=影なし / 0=完全に影。
// ★タイル外へのタップ漏れを clamp で殺している — アトラスは 1 枚のテクスチャなので、
//   サンプラのアドレスモードでは「隣のライトの深度」を拾うのを防げない。
//   タイル内側 1.5 テクセルへ寄せると、3x3 の中心が枠のきわに来ても隣を舐めない。
// ★w <= 0 (ライトの背後) を先に弾く。透視射影は w で割るので、これを通すと
//   背面の点が前面へ折り返して「ライトの真後ろだけ影が抜ける」形で壊れる。
float SampleShadowAtlas(Texture2D atlas, SamplerComparisonState samp, ShadowTile tile,
                        float3 posW, float atlasTexel)
{
    float4 lp = mul(float4(posW, 1.0f), tile.lightViewProj);
    if (lp.w <= 0.0f) {
        return 1.0f;
    }
    lp.xyz /= lp.w;
    const float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lp.z > 1.0f || lp.z < 0.0f) {
        return 1.0f; // タイルの外 = 深度を持っていない → 影を落とさない
    }
    const float2 lo = tile.uvScaleBias.zw + atlasTexel * 1.5f;
    const float2 hi = tile.uvScaleBias.zw + tile.uvScaleBias.xy - atlasTexel * 1.5f;
    const float2 base = uv * tile.uvScaleBias.xy + tile.uvScaleBias.zw;
    const float d = lp.z - tile.params.x;
    float sum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 t = clamp(base + float2(x, y) * atlasTexel, lo, hi);
            sum += atlas.SampleCmpLevelZero(samp, t, d);
        }
    }
    return sum / 9.0f;
}

// ライト配列ぶんの局所シャドウ係数をまとめて解決する (M54e)。
// **光パス 4 経路 (deferred_light / forward_lit / forward_lit_instanced / forward_skinned)
//   が呼ぶ**。M54c/M54d では Deferred にこのループを直書きしていたが、経路が 4 つに増えると
//   「1 箇所だけ面選択を忘れる」「1 箇所だけ enabled を見ない」が起きてもコンパイルは通り、
//   絵の食い違いにしか現れない (= Forward と Deferred の一致という ADR-007 の主張が
//   静かに壊れる) ので 1 本に畳んである。
// enabled == 0 のときは全要素が厳密に 1.0 = ApplyLighting の乗算が恒等になり、出力は
// M54c 以前とビット単位で一致する (「機能 off で直前コミットとビット一致」の根拠)。
//
// ★tiles を値渡しの配列で受けるのは ApplyLighting の lights[MAX_LIGHTS] と同じ流儀。
//   HLSL の関数は必ずインライン展開されるので、cbuffer 配列の動的添字にそのまま落ちる。
void ResolveLocalShadows(Texture2D atlas, SamplerComparisonState samp,
                         ShadowTile tiles[MYE_MAX_SHADOW_TILES], Light lights[MAX_LIGHTS],
                         int count, int enabled, float3 posW, float atlasTexel,
                         out float localShadow[MAX_LIGHTS])
{
    [unroll] for (int si = 0; si < MAX_LIGHTS; ++si) {
        localShadow[si] = 1.0f;
    }
    if (enabled == 0) {
        return;
    }
    for (int li = 0; li < count; ++li) {
        if (lights[li].shadowFaces > 0) {
            // M54d: 点光源は 6 面ぶんのタイルを連番で持つので、表面の向きで面を選ぶ
            const int ti = ShadowTileIndexForLight(lights[li], posW);
            localShadow[li] = SampleShadowAtlas(atlas, samp, tiles[ti], posW, atlasTexel);
        }
    }
}

// (M17 の単一シャドウマップ用 SampleShadowPCF は M54d で削除した — M38d の CSM 化で
//  呼び出しが消えて以来 5 マイルストーン誰も呼んでおらず、SampleShadowAtlas / SampleShadowCSM
//  と 3 つ目の「ほぼ同じ 3x3 PCF」が並ぶと、どれを直せばよいかが読み手に分からなくなる)

// CSM のカスケード選択付き PCF (M38d)。カスケード 0 (最詳細) から順に posW を射影し、
// 最初にマップ範囲へ収まったスライスで 3x3 比較平均。どれにも入らなければ 1 (影なし)。
// 範囲ベース選択なので view 深度の受け渡しが不要 (splits はデバッグ用に CB へ残す)。
// **カスケード数は C++ の ShadowPass::kCascades と一致必須** — 下の vps[3] の配列長は
// tools\check_rules.ps1 の規則 9 が機械照合する (M55a で登録)。ただし vp0/vp1/vp2 の
// 引数本数までは照合できないので、増やすときはこの 3 本と呼び出し側も手で直すこと。
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
// localShadow (M54c): 局所ライト (点/スポット) 1 本ごとの影係数 (1=影なし)。ライト配列と同添字。
//   ★テクスチャ引数をここへ持ち込まないための設計。呼び出し側が SampleShadowAtlas で
//     先に解決して配列で渡す。こうしておくと Forward 3 本 (forward_lit /
//     forward_lit_instanced / forward_skinned) は**1 文字も変えずに済む** — 下の
//     オーバーロードが全要素 1.0 の配列を作って呼ぶだけなので、乗算は厳密に恒等になる。
float3 ApplyLighting(float3 albedo, float3 normal, float3 posW, float3 cameraPos, float metallic,
                     float roughness, float3 ambient, Light lights[MAX_LIGHTS], int count,
                     float dirShadow, float localShadow[MAX_LIGHTS], int iblEnabled,
                     float iblSpecMips, TextureCube iblIrradiance, TextureCube iblPrefiltered,
                     Texture2D iblBrdfLut, SamplerState iblSampler, float ao)
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
            atten *= localShadow[i]; // M54c: シャドウアトラス (未割当のライトは厳密に 1.0)
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

// 局所ライトの影を持たない呼び出し用のオーバーロード (M54c、M54c 以前と同一シグネチャ)。
// Forward 3 本と ApplyLightingHybrid の非アトラス経路がここを通る。
// **1.0 の乗算は IEEE で厳密なので、この経路の出力は M54c 以前とビット単位で一致する**
// (受入基準「機能 off で直前コミットの PNG とビット一致」の根拠がこれ)
float3 ApplyLighting(float3 albedo, float3 normal, float3 posW, float3 cameraPos, float metallic,
                     float roughness, float3 ambient, Light lights[MAX_LIGHTS], int count,
                     float dirShadow, int iblEnabled, float iblSpecMips, TextureCube iblIrradiance,
                     TextureCube iblPrefiltered, Texture2D iblBrdfLut, SamplerState iblSampler,
                     float ao)
{
    float ones[MAX_LIGHTS];
    [unroll] for (int i = 0; i < MAX_LIGHTS; ++i) {
        ones[i] = 1.0f;
    }
    return ApplyLighting(albedo, normal, posW, cameraPos, metallic, roughness, ambient, lights,
                         count, dirShadow, ones, iblEnabled, iblSpecMips, iblIrradiance,
                         iblPrefiltered, iblBrdfLut, iblSampler, ao);
}

// M46h: レイトレ反射を IBL スペキュラへ混ぜる重み (1 = 反射 100% / 0 = IBL 100%)。
// しきい値は RtTypes.h が出所で CB 経由で渡る。RtMath.h の RtReflWeight と同一式
float RtReflWeight(float roughness, float fadeStart, float maxRough)
{
    return 1.0f - smoothstep(fadeStart, maxRough, roughness);
}

// ---- ローカル反射プローブ (M56f) ----
// 「この場所から見た景色」を焼いた cubemap (ProbeBaker) を、グローバルな env の**代わりに**
// スペキュラ環境項へ差し込む。ApplyLightingHybrid が RT 反射に対してやっている
// 「同次元の放射輝度を差し替える」規約と同じで、重み 0 でちょうど何も起きない。
//
// ★**ここに置いてある理由**: 光パス (deferred_light) と SSR (ssr_trace) の **2 経路**が
//   同じ値を要求する。光パスは「グローバル env との差分」を足し、SSR は「自分が引く
//   基準値」に使う — 片方だけ式を持つと、SSR とプローブを同時に on にした画素だけが
//   同じ光を二重に数える (絵は普通に出るので気付けない)。
// ★このファイルは register 宣言を持たない契約なので、テクスチャとプローブ配列は
//   ResolveLocalShadows と同じく**引数で受け取る** (HLSL の関数は必ず展開されるので、
//   cbuffer 配列の動的添字にそのまま落ちる)。
//
// C++ 側の kMaxReflectionProbes (RenderTypes.h) と同値。
// tools\check_rules.ps1 の規則 9 が一致を静的に検査する
#define MYE_MAX_REFLECTION_PROBES 8

// プローブ 1 個 (C++ の ReflectionProbeGpu と 48 バイトで一致)。
// ★箱は**軸平行** (v1)。回転を持たせると視差補正が箱のローカル空間への往復になり、
//   CPU ミラー (RenderTypes.h) との一致を保つ手間が跳ね上がる
struct ReflProbe
{
    float4 centerIntensity; // xyz = キャプチャ位置 (視差補正の原点) / w = 強度
    float4 boxMin;          // xyz = 影響ボックスの min (ワールド) / w = ブレンド距離
    float4 boxMax;          // xyz = 影響ボックスの max / w = 1 ならボックス投影を使う
};

// 影響の重み。箱の外 = 0 / 内側へブレンド距離ぶん入ると 1。
// **CPU ミラー: RenderTypes.h の ReflProbeWeight — 変更時は両方更新**
// (ProbeBakerSelfTest が検証)
float ReflProbeWeight(ReflProbe p, float3 posW)
{
    const float3 d = min(posW - p.boxMin.xyz, p.boxMax.xyz - posW);
    const float m = min(min(d.x, d.y), d.z); // 一番近い面までの距離 (負 = 箱の外)
    if (m <= 0.0f) {
        return 0.0f;
    }
    return saturate(m / max(p.boxMin.w, 1e-4f));
}

// 視差補正 (ボックス投影)。反射ベクトルを箱の内壁まで延ばし、**プローブの撮影位置から
// 見た向き**へ直す。これをしないと、箱の中を歩いたときに映り込みが動かない
// (無限遠のキューブマップと同じ挙動になる)。
// **CPU ミラー: RenderTypes.h の ReflProbeDir**
float3 ReflProbeDir(ReflProbe p, float3 posW, float3 R)
{
    if (p.boxMax.w < 0.5f) {
        return R; // 無限遠プローブ (視差補正なし)
    }
    // ★0 除算よけ。R の成分がちょうど 0 のとき (軸に平行な反射) は
    //   (面 - posW)/0 が numerator の符号次第で ±inf / NaN になる。NaN が 1 つ混ざると
    //   min の結果ごと壊れて「その画素だけ黒い」形で出るので、符号を保ったまま床を張る
    const float3 sgn = (R >= 0.0f) ? float3(1.0f, 1.0f, 1.0f) : float3(-1.0f, -1.0f, -1.0f);
    const float3 safeR = sgn * max(abs(R), 1e-6f);
    const float3 tmax = max((p.boxMax.xyz - posW) / safeR, (p.boxMin.xyz - posW) / safeR);
    const float t = min(min(tmax.x, tmax.y), tmax.z);
    if (t <= 0.0f) {
        return R; // 箱の外 (重み 0 のはずだが、念のため素の反射へ落とす)
    }
    return normalize((posW + R * t) - p.centerIntensity.xyz);
}

// 一番効いているプローブ 1 個を選ぶ (v1 は**混ぜない**)。戻り値 -1 = どれにも入っていない。
// ★同点は添字が小さい方。収集側が EntityID で決定論に並べているので、選択も決定論になる。
// **CPU ミラー: RenderTypes.h の ReflProbeSelect**
int ReflProbeSelect(ReflProbe list[MYE_MAX_REFLECTION_PROBES], int count, float3 posW,
                    out float weight)
{
    weight = 0.0f;
    int best = -1;
    for (int i = 0; i < count; ++i) {
        const float w = ReflProbeWeight(list[i], posW);
        if (w > weight) {
            weight = w;
            best = i;
        }
    }
    return best;
}

// プローブのスペキュラ放射輝度 (IBL の prefiltered と**同次元**)。weight は影響の重み。
// cubes の配列添字はプローブの添字そのもの (ベイカが同じ順で 6 面ずつ詰める)
float3 ReflProbeRadiance(TextureCubeArray cubes, SamplerState samp,
                         ReflProbe list[MYE_MAX_REFLECTION_PROBES], int count, float3 posW,
                         float3 R, float roughness, float specMips, out float weight)
{
    const int idx = ReflProbeSelect(list, count, posW, weight);
    if (idx < 0) {
        weight = 0.0f;
        return float3(0.0f, 0.0f, 0.0f);
    }
    const ReflProbe p = list[idx];
    const float3 dir = ReflProbeDir(p, posW, R);
    return cubes.SampleLevel(samp, float4(dir, (float)idx), roughness * specMips).rgb
        * p.centerIntensity.w;
}

// M46f/M46h: ハイブリッド合成版。**環境項だけ**をレイトレの結果で置き換える。
//   拡散 = rt_gi.cs.hlsl の出力 (albedo を掛けない demodulated 入射放射輝度、M46c の規約)。
//          IBL irradiance / 定数アンビエントとちょうど同じ位置に代入できる。
//   鏡面 = rt_refl.cs.hlsl の出力 (IBL のプリフィルタ済み放射輝度と同次元、M46h の規約)。
//          roughness で IBL へ smoothstep フォールバックする — 同次元なので段差が出ない。
// どちらも独立に on/off でき、両方 0 なら呼ばれない (呼び出し側が ApplyLighting を使う)。
//
// 直接光は式を複製せず ApplyLighting を「環境項ゼロ」(ambient=0 / iblEnabled=0) で呼んで
// Lo だけを取り出す — こうしておくと Forward と Deferred のライティングが永久に一致する。
// ao はレイトレ由来の項には掛けない (可視性はレイが持っている = 二重遮蔽になるため)。
// IBL 由来の環境項は従来どおり ao で減衰させる。**既存 ApplyLighting は無変更**。
// M54c: localShadow は直接光の側なので、そのまま ApplyLighting へ素通しする
// (レイトレ経路でも局所ライトの影はラスタのアトラスが担当する — RT 影は平行光だけ)。
float3 ApplyLightingHybrid(float3 albedo, float3 normal, float3 posW, float3 cameraPos,
                           float metallic, float roughness, float3 ambient,
                           Light lights[MAX_LIGHTS], int count, float dirShadow,
                           float localShadow[MAX_LIGHTS], int iblEnabled,
                           float iblSpecMips, TextureCube iblIrradiance,
                           TextureCube iblPrefiltered, Texture2D iblBrdfLut,
                           SamplerState iblSampler, float ao, float3 gi, int giEnabled,
                           float3 refl, int reflEnabled, float reflFadeStart, float reflMaxRough)
{
    const float3 zero3 = float3(0.0f, 0.0f, 0.0f);
    const float3 Lo =
        ApplyLighting(albedo, normal, posW, cameraPos, metallic, roughness, zero3, lights, count,
                      dirShadow, localShadow, 0, iblSpecMips, iblIrradiance, iblPrefiltered,
                      iblBrdfLut, iblSampler, ao);

    const float3 N = normal;
    const float3 V = normalize(cameraPos - posW);
    const float ndv = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 kS = FresnelSchlickRoughness(ndv, F0, roughness);
    const float3 kD = (1.0f - kS) * (1.0f - metallic); // 金属は拡散なし (IBL 拡散と同配分)

    // ---- 拡散環境項 ----
    float3 ambientTerm;
    if (giEnabled != 0) {
        ambientTerm = gi * albedo * kD; // AO は掛けない (GI が遮蔽を含む)
    } else if (iblEnabled != 0) {
        ambientTerm = iblIrradiance.SampleLevel(iblSampler, N, 0).rgb * albedo * kD * ao;
    } else {
        ambientTerm = ambient * albedo * (1.0f - metallic) * ao; // 従来の簡易アンビエント
    }

    // ---- スペキュラ環境項 (split-sum: 放射輝度 × 環境 BRDF) ----
    float3 specRadiance = zero3;
    bool hasSpec = false;
    if (iblEnabled != 0) {
        const float3 R = reflect(-V, N);
        specRadiance =
            iblPrefiltered.SampleLevel(iblSampler, R, roughness * iblSpecMips).rgb * ao;
        hasSpec = true;
    }
    if (reflEnabled != 0) {
        // 粗い面ほど IBL 側へ戻す (撃たなかった画素の反射バッファは 0 なので、
        // 重みが 0 になるしきい値と rt_refl.cs.hlsl のカットオフは同じ値を使う)
        specRadiance =
            lerp(specRadiance, refl, RtReflWeight(roughness, reflFadeStart, reflMaxRough));
        hasSpec = true;
    }
    if (hasSpec) {
        const float2 brdf = iblBrdfLut.SampleLevel(iblSampler, float2(ndv, roughness), 0).rg;
        ambientTerm += specRadiance * (F0 * brdf.x + brdf.y);
    }
    return ambientTerm + Lo;
}


// ---- 距離フォグの係数だけを返す版 (M57追補) ----
// **ApplyFog は色を返すので粒子には使えない** — 粒子は additive なら「減光 (1-f 倍)」、
// alpha なら「フォグ色へ lerp」と合成の仕方が分かれるので、必要なのは色ではなく係数 f。
// CPU バックエンド (particle_render.hlsl) と GPU バックエンド (particle_render_gpu.hlsl) が
// **この 1 本を共有する** — 片方だけ直すと「同じシーンで CPU 粒子と GPU 粒子の霧が違う」
// という、絵でしか気づけない形で割れる。
// ★下の ApplyFog は**この追補では 1 文字も変えない** — 全 lit シェーダが通る関数で、
//   1 ULP でも動くと golden 14 枚が全部動く。式の一本化はここの宿題ではない。
// **C++ ミラー: ParticleCurves.h::ParticleFogFactor** (変更時は両方更新)
float FogFactor(int fogMode, float density, float fogStart, float fogEnd, float dist)
{
    if (fogMode < 0) { return 0.0f; }
    if (fogMode == 0) { return saturate((dist - fogStart) / max(fogEnd - fogStart, 0.001f)); }
    if (fogMode == 1) { return 1.0f - exp(-density * dist); }
    const float e = density * dist;
    return 1.0f - exp(-e * e);
}

// ---- 距離フォグ (M29d) + ハイトフォグ/太陽インスキャッタ (M43a) ----
// mode: -1=無効 / 0=linear (start..end) / 1=exp (1-e^-ρd) / 2=exp2 (1-e^-(ρd)²)。
// lit 済みの色をフォグ色へ補間する。
// M43a: heightFalloff>0 のとき高度 exp 減衰密度 ρ(y)=e^{-k(y-base)} の視線積分を
// 「実効距離」として距離に置換。inscatterIntensity>0 のとき視線が太陽 (光の進行方向
// sunDir の逆) へ向くほどフォグ色を太陽色へ寄せる。
// **既定 (heightFalloff==0 && inscatterIntensity==0) は従来とビット同一**。
// C++ ミラー: PostFxMath.h (HeightFogEffectiveDistance / SunInscatterFactor) — 変更時は両方更新
float3 ApplyFog(float3 color, float3 fogColor, int fogMode, float density, float fogStart,
                float fogEnd, float3 cameraPos, float3 posW, float heightFalloff,
                float baseHeight, float3 sunDir, float3 sunColor, float inscatterIntensity,
                float inscatterPower)
{
    if (fogMode < 0) {
        return color;
    }
    const float trueDist = length(cameraPos - posW);
    float dist = trueDist;
    if (heightFalloff > 0.0f) {
        // ∫ρ = e^{-k(camY-base)} · (1-e^{-kΔy})/(kΔy) · dist (Δy=posY-camY)。
        // 指数は overflow 回避で ±60 に clamp (PostFxMath.h と同一)
        const float kd = heightFalloff * (posW.y - cameraPos.y);
        const float slope = (abs(kd) > 1e-4f) ? (1.0f - exp(-kd)) / kd : 1.0f;
        dist = trueDist
               * exp(clamp(-heightFalloff * (cameraPos.y - baseHeight), -60.0f, 60.0f)) * slope;
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
    if (inscatterIntensity > 0.0f) {
        const float3 rayDir = (posW - cameraPos) / max(trueDist, 1e-4f);
        const float sunAmount = pow(saturate(dot(rayDir, -sunDir)), max(inscatterPower, 1e-2f));
        fogColor = lerp(fogColor, sunColor, saturate(sunAmount * inscatterIntensity));
    }
    return lerp(color, fogColor, f);
}
