// M57b: フロクセルへの「密度」と「局所ライトの散乱」の注入。
//
// 1 スレッド = 1 セル。各セルの中心をワールド座標へ戻し、そこに満ちている媒質の
//   rgb = 単位長あたりの内向き散乱 (in-scattering) 放射輝度
//   a   = 単位長あたりの消散係数 σ_t
// を書く。積分 (手前から舐めて透過率を掛けながら足す) は M57c の担当なので、
// **このパスは 1 セルの中だけで完結する = 隣のセルを読まない**。
//
// ★局所ライトのビームの実体は common.hlsli の SampleShadowAtlas そのもの (M54c/M54d)。
//   面の陰影と霧のビームが同じ深度アトラス・同じ PCF・同じ面選択を通るので、
//   「床には影が落ちているのに霧のビームは素通り」という食い違いが原理的に起きない。
//   ここで自前の影サンプルを書き起こすと、M54d の面選択 (CubeFaceIndex) や
//   タイル外クランプの規則が 2 箇所に散る。
//
// ★平行光 (type 0) はここでは足さない。太陽の大気散乱は今のところ common.hlsli の
//   ApplyFog (距離フォグ + M43a の太陽インスキャッタ) と postfx_godray_* が担当しており、
//   ここで素直に足すと同じ現象が 3 回計上される。**役割分担を決めるのは M57d** なので、
//   このサブでは「局所ライトだけをフロクセルに載せる」= 既存の霧と絶対に重ならない
//   範囲に限定してある。
#include "common.hlsli"

// C++ の mye::froxel::kGroupSize (src\Engine\Renderer\RenderTypes.h) と一致検査される
// (tools\check_rules.ps1 規則 9)。froxel_clear.cs.hlsl と同じ割り方 = 同じ (x,y) の Z 列
#define MYE_FROXEL_GROUP 8

// C++ の FroxelInjectCB (src\Engine\Renderer\FroxelPass.cpp) とレイアウト一致 (2720 バイト)
cbuffer FroxelInjectCB : register(b0)
{
    float4x4 gFroxelInvView; // transpose(inverse(view)): view 空間 → ワールド (行ベクトル規約)
    float3 gFroxelCameraPos;
    float gFroxelDensity; // 基準の消散係数 σ_t [1/m] (高度スケール前)
    uint3 gFroxelGridSize;
    uint gFroxelPad0;
    float gFroxelNearZ; // グリッドの深度範囲。far は「カメラの far」ではなく
    float gFroxelFarZ;  // maxDistance でも切る (遠方に解像度を捨てない)
    float gFroxelInvProj00; // 1 / proj._11 — NDC.xy と view 深度から view 座標を復元する
    float gFroxelInvProj11; // 1 / proj._22   (**ジッタ非適用の proj** から作る = M55b の規約)
    float gFroxelAnisotropy;   // HG 位相関数の g
    float gFroxelScatterAlbedo; // σ_s / σ_t (単一散乱アルベド)
    float gFroxelHeightFalloff; // M43a の height fog と同じ ρ(y)=e^{-k(y-base)}
    float gFroxelBaseHeight;
    float3 gFroxelAmbient; // 環境光ぶんの等方散乱 (霧が完全な黒に落ちないための下駄)
    int gFroxelLightCount;
    int gFroxelShadowAtlasEnabled; // 0 = アトラス無し (影なし = 全部 1.0)
    float gFroxelShadowAtlasTexel;
    float2 gFroxelPad1;
    Light gFroxelLights[MAX_LIGHTS];
    ShadowTile gFroxelShadowTiles[MYE_MAX_SHADOW_TILES];
};

// ★CS は PS とは別のバインド空間なので、統合契約 予約 2 の「Deferred 光パス t12 /
//   Forward t6」とは衝突しない。ここの t0/s0/u0 はこのディスパッチの中だけの番号
Texture2D gFroxelShadowAtlas : register(t0);
SamplerComparisonState gFroxelShadowSampler : register(s0);
RWTexture3D<float4> gFroxelScatter : register(u0);

// スライス境界の view 深度 (指数分布)。
// **C++ の froxel::SliceToViewDepth (RenderTypes.h) と同一式** — 片方だけ直すと
// M57c の再投影がグリッドの外を指すようになる
float FroxelSliceDepth(float slice)
{
    // max() が要る: 比が負になる経路は無いが、無いと fxc が X3571
    // (pow は負の底で動かない) を出す = 実行時コンパイルの警告が毎回ログに出る
    const float ratio = max(gFroxelFarZ / gFroxelNearZ, 1e-4f);
    return gFroxelNearZ * pow(ratio, slice / (float)gFroxelGridSize.z);
}

// Henyey-Greenstein 位相関数。cosTheta = dot(光の進行方向, セル→カメラ方向)。
// g > 0 = 前方散乱 = 「光源のほうを向くと明るい」。全立体角で積分すると 1。
// **C++ の froxel::HenyeyGreenstein (RenderTypes.h) と同一式** (RenderSelfTest が
// 正規化と対称性を検査している = 係数を落としても気付ける)
float HenyeyGreenstein(float cosTheta, float g)
{
    const float gg = clamp(g, -0.95f, 0.95f); // ±1 で分母が 0 に落ちる
    const float d = max(1.0f + gg * gg - 2.0f * gg * cosTheta, 1e-4f);
    // x^1.5 は pow ではなく x*sqrt(x) で書く。**WARP では pow が exp/log の 2 段になり、
    // これがセル × ライト本数ぶん効く** (実測でここだけ 1 割強)
    return (1.0f - gg * gg) / (4.0f * 3.14159265f * (d * sqrt(d)));
}

[numthreads(MYE_FROXEL_GROUP, MYE_FROXEL_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // グリッド寸法は kGroupSize の倍数とは限らない (90 は 8 で割り切れない)
    if (any(id >= gFroxelGridSize)) {
        return;
    }

    // ---- セル中心 → ワールド座標 ----
    // ★スライスの**中心**を代表点にする。境界を使うと隣のセルと同じ点を評価してしまい、
    //   1 スライスぶんの厚みが消える (手前のスライスほど薄いので近景で顕著に出る)。
    //   M57c のジッタはこの +0.5 を [0,1) の擬似乱数で置き換える形で入る
    const float viewZ = FroxelSliceDepth((float)id.z + 0.5f);
    const float2 uv = ((float2)id.xy + 0.5f) / (float2)gFroxelGridSize.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    // 透視射影の逆算 (行ベクトル規約: clip = view * proj、clip.w = view.z)。
    // 逆行列を掛けるより素直で、深度の非線形性を経由しないぶん精度も良い
    const float3 viewPos =
        float3(ndc.x * viewZ * gFroxelInvProj00, ndc.y * viewZ * gFroxelInvProj11, viewZ);
    const float3 posW = mul(float4(viewPos, 1.0f), gFroxelInvView).xyz;

    // ---- 密度 (消散係数) ----
    // M43a の height fog と同じ指数プロファイル。falloff == 0 なら exp(0) = 1 = 一様。
    // 分岐にしないのは、非一様フローを増やしても得るものが無いため (exp は十分安い)
    const float heightScale = exp(-gFroxelHeightFalloff * (posW.y - gFroxelBaseHeight));
    const float sigmaT = max(gFroxelDensity * heightScale, 0.0f);
    const float sigmaS = sigmaT * gFroxelScatterAlbedo;

    // ---- 散乱 ----
    const float3 toCamera = normalize(gFroxelCameraPos - posW);
    float3 inscatter = gFroxelAmbient; // 等方 (位相関数の全立体角積分が 1 なので係数も 1)

    for (int i = 0; i < gFroxelLightCount; ++i) {
        const Light L = gFroxelLights[i];
        if (L.type == 0) {
            continue; // 平行光は M57d が役割を決めるまで ApplyFog / godray の担当
        }
        const float3 toLight = L.position - posW;
        const float dist = length(toLight);
        const float3 toLightDir = toLight / max(dist, 1e-4f);
        // ★減衰は ApplyLighting (common.hlsli) と**同じ式**を使う。面の明るさと
        //   霧の明るさが別の減衰で走ると、光溜まりの縁で霧だけが先に消える
        const float d = saturate(1.0f - dist / max(L.range, 1e-4f));
        float atten = d * d;
        if (L.type == 2) {
            const float cosA = dot(-toLightDir, L.direction);
            const float spot = saturate((cosA - L.cosOuter) / max(L.cosInner - L.cosOuter, 1e-4f));
            atten *= spot * spot;
        }
        if (atten <= 0.0f) {
            continue; // 届かないライトの影サンプルを撃たない (WARP では効く)
        }
        // M54c/M54d: 影 = ビームの実体。未割当のライト (shadowFaces==0) は厳密に 1.0
        float shadow = 1.0f;
        if (gFroxelShadowAtlasEnabled != 0 && L.shadowFaces > 0) {
            const int ti = ShadowTileIndexForLight(L, posW);
            shadow = SampleShadowAtlas(gFroxelShadowAtlas, gFroxelShadowSampler,
                                       gFroxelShadowTiles[ti], posW, gFroxelShadowAtlasTexel);
        }
        // 位相関数の cosTheta は「光の進行方向 (-toLightDir) と、散乱後の進行方向
        // (セル → カメラ)」の内積
        const float phase = HenyeyGreenstein(dot(-toLightDir, toCamera), gFroxelAnisotropy);
        inscatter += L.color * (L.intensity * atten * shadow * phase);
    }

    gFroxelScatter[id] = float4(inscatter * sigmaS, sigmaT);
}
