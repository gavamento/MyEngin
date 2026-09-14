// Deferred ライティングパス: フルスクリーン三角形で GBuffer を解決する。
// ライティング計算は Forward と同じ common.hlsli の関数を使う (見た目の一致)

#include "common.hlsli"
// M57d: フロクセルのグリッド幾何とサンプル座標の式 (register 宣言は持たないヘッダ)
#include "froxel_common.hlsli"
#include "acoustic_common.hlsli" // M65e: 残光の式の正本 (register 宣言は持たない)

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
    // ---- M56f: ローカル反射プローブ (末尾 append)。0 = 従来と完全に同一の式 ----
    int      gProbeCount;
    float    gProbeSpecMips; // プリフィルタ済みキューブの最終 mip index
    float2   _probePad;
    ReflProbe gProbes[MYE_MAX_REFLECTION_PROBES];
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
    // ---- M65e: 音響の残光ボリューム (末尾 append)。
    //      **w = 0 で従来と完全に同一の式** (下の分岐に一度も入らない) ----
    float4   gAcousticGridMin; // xyz = セル(0,0,0) の最小角のワールド座標
    float4   gAcousticInvSize; // xyz = 1/(dim*cellSize)
    float4   gAcousticParams;  // x=強さ y=法線押し出し[m] z=面の色の混ぜ具合 w=有効
    // ---- 2026-09-12「描画だけ円」(末尾 append。front.z = 0 で従来と同一の式)。
    //      形は RenderTypes.h の AcousticCB (C++ ミラーは 3 つとも同じ構造体) ----
    float4   gAcousticFront;   // x=残光の残存率/tick y=cellSize z=有効 w=波の数
    float4   gAcousticWaves[MYE_ACOUSTIC_WAVE_SLOTS * 2]; // [2s]=(原点,半径) [2s+1]=(振幅,上限,tick/m,名残)
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
// t13 = 音響の残光 (M65e)。番号の正本は acoustic_common.hlsli の MYE_ACOUSTIC_SRV_SLOT
// (C++ と機械照合される)。光パスの SRV は t0-t16 の 17 本 (剥がし忘れの注意も同 hlsli)
Texture3D   gAcousticGlow   : MYE_ACOUSTIC_REG(MYE_ACOUSTIC_SRV_SLOT); // M65e (r=符号化残光)
// 2026-09-12「描画だけ円」: 見通しビット (Load で読む整数テクスチャ。番号の正本は同 hlsli)
Texture3D<uint> gAcousticFrontMask : MYE_ACOUSTIC_REG(MYE_ACOUSTIC_FRONT_SRV_SLOT);
TextureCubeArray gProbeCubes : register(t14); // M56f (プリフィルタ済みプローブ、6 面 × N)
Texture3D   gFroxelVolume   : register(t15); // M57d (rgb=積算内向き散乱 / a=透過率)
SamplerState gIblSampler : register(s0); // LINEAR/CLAMP (M38c)。SSAO / RT / プローブ / 残光 / フロクセルもこれを流用する
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
        // ★グリッドより奥の解析フォグは**掛けない** — 背景は ApplyFog を受けない仕様
        //   (froxel off の経路も掛けていない) で、ここだけ足すと froxel の on/off で背景の色が
        //   食い違う。ここで消すのはフロクセル区間ぶんの段だけ
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
    // 持ち込まないための規約)。Forward 3 本と同じ関数
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
    // ---- M56f: ローカル反射プローブ ----
    // ★足すのは反射そのものではなく **「グローバル env との差分」**。SSR (M56d) と同じ理屈で、
    //   上の ApplyLighting[Hybrid] が既にスペキュラ環境項 (`iblPrefiltered * ao * 環境BRDF`) を
    //   足しているので、生の放射輝度を上乗せすると同じ光を二重に数える。
    //   `(プローブ - IBL) * ao * 環境BRDF * 重み` を足すと、結果は
    //   「スペキュラ放射輝度を lerp で差し替えた」値とちょうど一致し、**重み 0 で厳密に 0**。
    //   Forward と共有している ApplyLighting 系のシグネチャを増やさずに済む。
    // ★RT 反射が効いている画素では、その重みぶんは既にレイの結果で置き換わっているので
    //   (1-wRt) を掛ける。**フォールバック連鎖は SSR → プローブ → グローバル env** で、
    //   SSR 側は「自分が引く基準値」に同じプローブ放射輝度を使う (ssr_trace.hlsl)
    if (gProbeCount > 0) {
        const float3 V = normalize(gCameraPos - posW);
        const float3 R = reflect(-V, n);
        float pw = 0.0f;
        const float3 probeSpec = ReflProbeRadiance(gProbeCubes, gIblSampler, gProbes, gProbeCount,
                                                   posW, R, mr.y, gProbeSpecMips, pw);
        if (pw > 0.0f) {
            float3 iblSpec = float3(0.0f, 0.0f, 0.0f);
            if (gIblEnabled != 0) {
                iblSpec = gIblPrefiltered.SampleLevel(gIblSampler, R, mr.y * gIblSpecMips).rgb;
            }
            const float wRt =
                (gRtReflEnabled != 0) ? RtReflWeight(mr.y, gRtReflFadeStart, gRtReflMaxRough) : 0.0f;
            const float ndv = saturate(dot(n, V));
            const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, mr.x);
            const float2 brdf = gIblBrdfLut.SampleLevel(gIblSampler, float2(ndv, mr.y), 0).rg;
            color += (probeSpec - iblSpec) * ao * (F0 * brdf.x + brdf.y) * (pw * (1.0f - wRt));
        }
    }
    // M46i: 自己発光 (G-Buffer の b に正規化して詰めてある)。ライティングに依らず
    // 放射する分を足す。発光なしのマテリアルは b が厳密に 0 なので加算項もちょうど 0
    // (発光を使わない絵はビット単位で変わらない)
    color += albedo.rgb * DecodeEmissive(matG.b);
    // ---- M65e: 音響の残光 (末尾 append。gAcousticParams.w == 0 で従来とビット恒等) ----
    // ★足すのは**フォグより前**。残光は面から出ていく放射なので、霧が掛かる側に居るのが
    //   正しい (後ろに足すと霧の向こうの壁だけが素の明るさで光る = 奥行きが死ぬ)。
    // ★サンプラは s0 (IBL 用 LINEAR/CLAMP) を流用する = サンプラは 1 つも増えない
    //   (統合契約 予約 2)。CLAMP の外側漏れは AcousticSample が範囲判定で殺している
    if (gAcousticParams.w != 0.0f) {
        const float glow = AcousticSample(gAcousticGlow, gIblSampler, posW, n,
                                          gAcousticGridMin.xyz, gAcousticInvSize.xyz,
                                          gAcousticParams.y);
        // 解析的な波面 (円) と max 合成。gAcousticFront.z = 0 なら従来とビット恒等
        const float front = (gAcousticFront.z != 0.0f)
            ? AcousticFront(gAcousticFrontMask, posW, n, gAcousticGridMin.xyz, gAcousticInvSize.xyz,
                            gAcousticParams.y, gAcousticFront, gAcousticWaves)
            : 0.0f;
        color += AcousticRadiance(max(glow, front), gAcousticParams.x, albedo.rgb, gAcousticParams.z);
    }
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
