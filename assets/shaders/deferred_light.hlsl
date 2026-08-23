// Deferred ライティングパス: フルスクリーン三角形で GBuffer を解決する。
// ライティング計算は Forward と同じ common.hlsli の関数を使う (見た目の一致)

#include "common.hlsli"
// M57d: フロクセルのグリッド幾何とサンプル座標の式 (register 宣言は持たないヘッダ)
#include "froxel_common.hlsli"

cbuffer LightPass : register(b0)
{
    float3 gAmbient;
    int    gLightCount;
    float4 gClearColor; // ジオメトリの無いピクセルの色
    Light  gLights[MAX_LIGHTS];
    float4x4 gShadowVP;
    float    gShadowTexel;
    int      gShadowEnabled;
    float2   _pad1;
    float3   gCameraPos;
    float    _pad2;
    // ---- フォグ (M29d、末尾 append) ----
    float3   gFogColor;
    int      gFogMode; // -1=無効
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
    // ---- IBL (M38c、末尾 append) ----
    int      gIblEnabled;
    float    gIblSpecMips;
    float2   _iblPad;
    // ---- CSM (M38d、末尾 append)。gShadowVP はカスケード 0 ----
    float4x4 gShadowVP12[2];
    float4   gCascadeInfo; // xyz = split far 境界 / w = カスケード数
    // ---- SSAO (M38e、末尾 append) ----
    float2   gScreenSize;  // フル解像度 (SV_Position → uv 変換用)
    int      gSsaoEnabled; // 0=無効
    float    _ssaoPad;
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等) ----
    float    gFogHeightFalloff;      // 0 = 高さ一様 (従来)
    float    gFogBaseHeight;
    float    gFogInscatterIntensity; // 0 = 無効
    float    gFogInscatterPower;
    float3   gSunDirection;          // 光の進行方向 (正規化)
    float    _fogPad2;
    float3   gSunColor;              // リニア・強度込み (平行光無し = 黒 + intensity 0)
    float    _fogPad3;
    // ---- M46f: RT GI 合成 (末尾 append)。0 = 従来と完全に同一の式 ----
    int      gRtGiEnabled;
    // ---- M46g: RT 影 (末尾 append)。0 = 従来どおり CSM をサンプルする ----
    int      gRtShadowEnabled;
    // ---- M46h: RT 反射 (末尾 append)。0 = スペキュラ環境項は従来どおり IBL のみ ----
    int      gRtReflEnabled;
    float    gRtReflFadeStart; // ここから gRtReflMaxRough まで IBL へ smoothstep で戻す
    float    gRtReflMaxRough;  // これを超える roughness はレイを撃っていない
    float3   _rtPad;
    // ---- M54c: 局所ライト (スポット/点) のシャドウアトラス (末尾 append)。
    //      0 = 従来と完全に同一の式 (Light.shadowFaces も 0 のまま) ----
    int      gShadowAtlasEnabled;
    float    gShadowAtlasTexel; // 1/アトラス解像度
    float2   _atlasPad;
    ShadowTile gShadowTiles[MYE_MAX_SHADOW_TILES];
    // ---- M57d: フロクセル・ボリュメトリック (末尾 append)。
    //      0 = 従来と完全に同一の式 (ApplyFog をそのまま呼ぶ経路へ落ちる) ----
    int      gFroxelEnabled;
    float    gFroxelNearZ;   // グリッドの手前端 (カメラの near とは限らない)
    float    gFroxelFarZ;    // グリッドの奥端。ここから先は解析フォグが持つ
    float    gFroxelSlices;  // スライス数 (float で持つ = 全部の式が float 演算)
    // dot(float4(posW,1), これ) = view 深度。view 行列の第 3 列そのもの。
    // ★カメラ前方ベクトルを正規化して内積する式にしない — 非一様スケールの入った
    //   ビュー行列で静かにずれる。列をそのまま渡せば定義どおりになる
    float4   gFroxelViewZRow;
};

Texture2D gAlbedo    : register(t0);
Texture2D gNormal    : register(t1);
Texture2D gPosition  : register(t2); // ワールド座標 (GBuffer)
Texture2D gMaterial  : register(t3); // r=metallic g=roughness
Texture2DArray gShadowMap : register(t4); // M38d: CSM カスケード配列
TextureCube gIblIrradiance  : register(t5); // M38c
TextureCube gIblPrefiltered : register(t6);
Texture2D   gIblBrdfLut     : register(t7);
Texture2D   gSsao           : register(t8); // M38e (半解像度、ブラー済み AO)
Texture2D   gRtGi           : register(t9); // M46f (内部解像度、demodulated 入射放射輝度)
Texture2D   gRtShadow       : register(t10); // M46g (フル解像度 R8、太陽の可視率)
Texture2D   gRtRefl         : register(t11); // M46h (内部解像度、反射方向の入射放射輝度)
Texture2D   gShadowAtlas    : register(t12); // M54c (局所ライトの深度アトラス、R32_FLOAT)
// t13 = SSR (M56d) / t14 = 反射プローブ (M56f) は統合契約 予約 2 の空席。
// **M57 のブランチには実体が無いが番号は空けてある** — froxel を詰めて t13 に置くと
// 統合時に M56 と無言で潰し合う (レジスタ番号の食い違いはコンパイルが通ってしまう)
Texture3D   gFroxelVolume   : register(t15); // M57d (rgb=積算内向き散乱 / a=透過率)
SamplerState gIblSampler : register(s0); // LINEAR/CLAMP (M38c、s0 は光パスで空きだった)
SamplerComparisonState gShadowSampler : register(s1);

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // フルスクリーン三角形
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    const float4 albedo = gAlbedo.Load(pixel);
    if (albedo.a < 0.5f) {
        // ジオメトリ無し = 背景。**M57e: ここにもフロクセルを載せる。**
        // スカイボックスが無いシーン (--render-demo が実例) では clearColor が
        // そのまま地平線の上に出るので、床だけに霧が乗ると水平線に段ができる。
        // 深度が無いので「グリッド全体ぶん」を引く (= 最遠テクセル)。
        // ★グリッドより奥の解析フォグは**掛けない** — 背景に ApplyFog が掛かる挙動は
        //   M29d 以来一度も無く、ここで足すと froxel off の絵まで動かしたくなる。
        //   フロクセル区間ぶんの段だけを消す、が M57e の受け持ち
        if (gFroxelEnabled != 0) {
            const float2 bguv = i.pos.xy / gScreenSize;
            const float4 bgvol = gFroxelVolume.SampleLevel(
                gIblSampler, float3(bguv, FroxelSampleWFar(gFroxelSlices)), 0);
            return float4(gClearColor.rgb * bgvol.a + bgvol.rgb, gClearColor.a);
        }
        return gClearColor;
    }
    const float3 n = normalize(gNormal.Load(pixel).xyz * 2.0f - 1.0f);
    const float3 posW = gPosition.Load(pixel).xyz;
    const float4 matG = gMaterial.Load(pixel);
    const float2 mr = matG.rg; // metallic, roughness
    float dirShadow = 1.0f;
    if (gRtShadowEnabled != 0) {
        // M46g: レイトレの可視率で置き換える (フル解像度なので Load でぴったり一致)。
        // カスケード選択も深度バイアスも無いので継ぎ目・アクネ・ピーターパンが出ない
        dirShadow = gRtShadow.Load(pixel).r;
    } else if (gShadowEnabled != 0) {
        dirShadow = SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0],
                                    gShadowVP12[1], (int)gCascadeInfo.w, posW, gShadowTexel);
    }
    float ao = 1.0f;
    if (gSsaoEnabled != 0) {
        ao = gSsao.SampleLevel(gIblSampler, i.pos.xy / gScreenSize, 0).r; // M38e
    }
    // M54c: 局所ライトの影を先に解決して配列で渡す (ApplyLighting にテクスチャを
    // 持ち込まないための規約)。M54e で Forward 3 本と同じ関数へ畳んだ
    float localShadow[MAX_LIGHTS];
    ResolveLocalShadows(gShadowAtlas, gShadowSampler, gShadowTiles, gLights, gLightCount,
                        gShadowAtlasEnabled, posW, gShadowAtlasTexel, localShadow);
    float3 color;
    if (gRtGiEnabled != 0 || gRtReflEnabled != 0) {
        // M46f/M46h: GI と反射は内部解像度 (rtResolutionScale) なので
        // s0 = LINEAR/CLAMP で引き上げる。off 側の項は 0 のまま渡す (合成側が見ない)
        const float2 rtUv = i.pos.xy / gScreenSize;
        float3 gi = float3(0.0f, 0.0f, 0.0f);
        if (gRtGiEnabled != 0) {
            gi = gRtGi.SampleLevel(gIblSampler, rtUv, 0).rgb;
        }
        float3 refl = float3(0.0f, 0.0f, 0.0f);
        if (gRtReflEnabled != 0) {
            refl = gRtRefl.SampleLevel(gIblSampler, rtUv, 0).rgb;
        }
        color = ApplyLightingHybrid(albedo.rgb, n, posW, gCameraPos, mr.x, mr.y, gAmbient,
                                    gLights, gLightCount, dirShadow, localShadow, gIblEnabled,
                                    gIblSpecMips, gIblIrradiance, gIblPrefiltered, gIblBrdfLut,
                                    gIblSampler, ao, gi, gRtGiEnabled, refl, gRtReflEnabled,
                                    gRtReflFadeStart, gRtReflMaxRough);
    } else {
        color = ApplyLighting(albedo.rgb, n, posW, gCameraPos, mr.x, mr.y, gAmbient,
                              gLights, gLightCount, dirShadow, localShadow, gIblEnabled,
                              gIblSpecMips, gIblIrradiance, gIblPrefiltered, gIblBrdfLut,
                              gIblSampler, ao);
    }
    // M46i: 自己発光 (G-Buffer の b に正規化して詰めてある)。ライティングに依らず
    // 放射する分を足す。発光なしのマテリアルは b が厳密に 0 なので加算項もちょうど 0 になり、
    // M46i 以前の出力とビット単位で一致する
    color += albedo.rgb * DecodeEmissive(matG.b);
    // ---- 大気散乱 (M29d + M43a の解析フォグ、M57d でフロクセルと分担) ----
    // ★ここが「霧を 3 回足さない」ための唯一の分岐点。同じ大気散乱を表現する仕組みが
    //   3 つある (解析フォグ / フロクセル / ゴッドレイ) ので、素直に全部足すと 3 倍になる。
    //   受け持ちはこう決めた:
    //     ・[near, gFroxelFarZ]  = フロクセル (局所ライトのビームと影を持てる)
    //     ・gFroxelFarZ より奥   = 解析フォグ (起点を押し出して**残り区間だけ**)
    //     ・ゴッドレイ           = フロクセル稼働中は RenderSystem が自動 off
    //   グリッドの中のピクセルでは起点が posW と厳密に一致する = 距離 0 = ApplyFog は恒等
    if (gFroxelEnabled != 0) {
        const float viewZ = dot(float4(posW, 1.0f), gFroxelViewZRow);
        float3 fogOrigin = posW; // グリッドの中 = 解析フォグの残りはゼロ
        if (viewZ > gFroxelFarZ) {
            fogOrigin = gCameraPos
                + (posW - gCameraPos) * FroxelFogHandoffFraction(viewZ, gFroxelFarZ);
        }
        color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                         fogOrigin, posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                         gSunColor, gFogInscatterIntensity, gFogInscatterPower);
        // 積分結果は「カメラからそのスライスの奥端まで」。s0 = LINEAR/CLAMP で引く
        // (専用サンプラは作らない = 統合契約 予約 2「サンプラは増やさない」)
        const float2 fuv = i.pos.xy / gScreenSize;
        const float fw = FroxelSampleW(viewZ, gFroxelSlices, gFroxelNearZ, gFroxelFarZ);
        const float4 vol = gFroxelVolume.SampleLevel(gIblSampler, float3(fuv, fw), 0);
        color = color * vol.a + vol.rgb; // Lo = scene·T + inscatter
    } else {
        color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                         gCameraPos, posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                         gSunColor, gFogInscatterIntensity, gFogInscatterPower); // M29d+M43a
    }
    return float4(color, 1.0f);
}
