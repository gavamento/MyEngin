// M63d: パーティクルのライティング (拡散のみ + 平行光の CSM 影 + IBL irradiance)。
//
// **CPU バックエンドと GPU バックエンドの共有点その 2** (その 1 = particle_billboard.hlsli)。
// 粒子は M63c までずっと完全 unlit で、点光源の隣でも影の中でも同じ色で光っていた。
//
// ★particle_billboard.hlsli と違い、**このファイルは register 宣言を持つ**。
//   CB (1264B) とライト配列のレイアウトを 2 つのシェーダへ手写しすると、片方だけ
//   フィールドを足した瞬間に「GPU 粒子だけ影の位置がずれる」という、コンパイルも実行も
//   通るのに絵だけ静かに割れる形で壊れる。宣言を 1 箇所にするほうが強い
//   (M58d が terrain_common.hlsli で cbuffer 宣言を共有へ倒したのと同じ判断)。
//   スロット番号だけはシェーダごとに違う (CPU は t4 が空き / GPU は t4 をフロクセルが占有)
//   ので、**呼ぶ側が include の前にマクロで指定する**。
//
// ★マクロに既定値を置かない。既定 (t4) のまま GPU 側が include すると
//   フロクセルのボリュームと衝突して**エラーも出ずに**絵が壊れるので、
//   指定漏れは #error で止める。
//
// ★common.hlsli を include しない — あちらはインクルードガードを持たないので、
//   ホスト側が既に読んでいるところへ二重に持ち込むと再定義エラーになる。
//   Light / MAX_LIGHTS / LightSample / SampleShadowCSM は**ホストが先に読んでいる前提**。

#ifndef MYE_PARTICLE_LIGHT_HLSLI
#define MYE_PARTICLE_LIGHT_HLSLI

#ifndef MAX_LIGHTS
#error "particle_light.hlsli は common.hlsli の後に include すること"
#endif
#ifndef MYE_PARTICLE_LIGHT_SLOT_CB
#error "MYE_PARTICLE_LIGHT_SLOT_CB (例: b1) を include の前に定義すること"
#endif
#ifndef MYE_PARTICLE_LIGHT_SLOT_CSM
#error "MYE_PARTICLE_LIGHT_SLOT_CSM (例: t4) を include の前に定義すること"
#endif
#ifndef MYE_PARTICLE_LIGHT_SLOT_IRR
#error "MYE_PARTICLE_LIGHT_SLOT_IRR (例: t5) を include の前に定義すること"
#endif
#ifndef MYE_PARTICLE_LIGHT_SLOT_SAMP
#error "MYE_PARTICLE_LIGHT_SLOT_SAMP (例: s1) を include の前に定義すること"
#endif

// C++ 側の正本は RenderTypes.h の mye::particlelight::ParticleLightCB (1264B の
// static_assert つき)。**フィールドを足すときは必ず両方同時に**。
// ★ビューにつき 1 回しか変わらないので、アップロードはエミッタループの外で 1 回だけ
//   (GpuParticleCB へ混ぜると 100 エミッタで毎フレーム 100KB の転送になる)
cbuffer ParticleLightCB : register(MYE_PARTICLE_LIGHT_SLOT_CB)
{
    // CSM 各カスケードの transpose(lightView*lightProj)。SampleShadowCSM の
    // mul(float4, M) 規約に合わせて **転置済みのまま**渡ってくる
    float4x4 gPlCascadeVP[3];
    float4   gPlAmbient; // xyz = アンビエント, w = iblEnabled (1 = irradiance を引く)
    float4   gPlParams;  // x = lightCount, y = cascadeCount (0=影なし), z = shadowTexel, w = 予約
    float4   gPlCamFwd;  // xyz = カメラ前方 (ワールド)。球面法線の 3 本目の軸
    Light    gPlLights[MAX_LIGHTS];
};

Texture2DArray         gPlShadowMap  : register(MYE_PARTICLE_LIGHT_SLOT_CSM);
TextureCube            gPlIrradiance : register(MYE_PARTICLE_LIGHT_SLOT_IRR);
SamplerComparisonState gPlShadowSamp : register(MYE_PARTICLE_LIGHT_SLOT_SAMP);

// ビルボードを「カメラを向いた球」とみなした法線 (lightingMode=2)。
// d = uv * 2 - 1 (四隅の素の corner と同じ [-1,1] 空間)。
//
// ★d は**回転・ストレッチを通す前**の corner でなければならない。uv は
//   `corner * 0.5 + 0.5` で作られていて既にその条件を満たしている — 変換後の c から
//   作ると、回る粒子の陰影が粒子と一緒に回ってしまう (球は回しても同じ形のはず)。
// ★中心 (d=0) で -camFwd = カメラを向く。camFwd は視線の進行方向なので符号を反転する。
// ★円の外 (dot(d,d) > 1) は saturate で縁の法線へ潰す。procedural 円もフリップブックも
//   そこは alpha ≈ 0 なので、値が飛ばないことだけが要件
float3 ParticleSphericalNormal(float2 d, float3 camRight, float3 camUp, float3 camFwd)
{
    const float r2 = saturate(dot(d, d));
    const float z = sqrt(1.0f - r2);
    return normalize(camRight * d.x + camUp * d.y - camFwd * z);
}

// ラップ拡散。煙や埃は光を透かすので、素の Lambert だと裏側が真っ黒になって
// 「板が置いてある」ようにしか見えない。
// ★wrap = 0 で saturate(ndl) = 素の Lambert へ**厳密に**縮退する (0 除算も起きない)。
// ★ndl は saturate 前の生の内積を渡すこと (裏側の負値こそがラップの入力)
float ParticleWrapDiffuse(float ndl, float wrap)
{
    return saturate((ndl + wrap) / (1.0f + wrap));
}

// 粒子 1 点ぶんの受光係数。**アルベドは掛けない** — 呼び出し側が col.rgb へ掛ける
// (加算合成でもアルファ合成でも「素の色 × 受光」で意味が同じになる形)。
//
// ★スペキュラを持たない。ビルボードには roughness も F0 も無く、Cook-Torrance を
//   通しても意味のある値にならない (M63d の明示的な除外。鏡面 IBL も同じ理由)。
// ★局所ライトの影 (シャドウアトラス) も持たない。局所光のビームはフロクセルが担当済みで、
//   CB を +1.5KB する見返りが薄い。common.hlsli の全要素 1.0 オーバーロードと同じく
//   「恒等に落として将来の余地を残す」形にしてある。
// ★intensity は**直接光にだけ**掛ける。環境項まで掛けると intensity=0 が真っ黒になり、
//   「陰影を弱めたいだけ」の調整ができなくなる
float3 ParticleLightAt(float3 N, float3 posW, float wrap, float intensity, int receiveShadow,
                       SamplerState envSamp)
{
    // 平行光の CSM 影。メッシュと**同じ SampleShadowCSM** を通す = 箱の影の境界が
    // 地面と粒子でずれない
    float dirShadow = 1.0f;
    const int cascades = (int)gPlParams.y;
    if (receiveShadow != 0 && cascades > 0) {
        dirShadow = SampleShadowCSM(gPlShadowMap, gPlShadowSamp, gPlCascadeVP[0],
                                    gPlCascadeVP[1], gPlCascadeVP[2], cascades, posW,
                                    gPlParams.z);
    }

    float3 lit = float3(0.0f, 0.0f, 0.0f);
    const int count = (int)gPlParams.x;
    for (int i = 0; i < count; ++i) {
        const Light L = gPlLights[i];
        float3 toL;   // 表面 → 光源
        float atten;
        // M63d: **メッシュと同じ 1 本** (common.hlsli::LightSample)。距離減衰の分母や
        // スポットの二乗フォールオフを手写しすると、同じシーンで「メッシュと粒子で
        // 光の届き方が違う」が静かに起きる
        LightSample(L, posW, toL, atten);
        if (L.type == 0) {
            atten *= dirShadow;
        }
        lit += L.color * (L.intensity * atten * ParticleWrapDiffuse(dot(N, toL), wrap));
    }

    // 環境項: IBL があれば irradiance キューブ、無ければ定数アンビエント
    // (ApplyLighting の iblEnabled 分岐と同じ意味論。1/PI を省く規約もそのまま)
    float3 env = gPlAmbient.rgb;
    if (gPlAmbient.w != 0.0f) {
        env = gPlIrradiance.SampleLevel(envSamp, N, 0).rgb;
    }
    return env + lit * intensity;
}

#endif // MYE_PARTICLE_LIGHT_HLSLI
