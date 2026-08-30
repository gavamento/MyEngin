// CPU パーティクル描画 (ビルボード展開を VS で行う)
// DrawInstanced(4, count) + TRIANGLESTRIP。頂点入力なし (SV_VertexID / SV_InstanceID)

#include "common.hlsli" // LinearizeDepth (M55a で共有化)。register 宣言は含まないので衝突しない
#include "froxel_common.hlsli" // M57e: フロクセルのサンプル座標と受け持ちの分け方 (同上)
#include "particle_billboard.hlsli" // M63a: 四隅の回転/ストレッチ (GPU バックエンドと共有する唯一の式)

cbuffer ParticleCB : register(b0)
{
    float4x4 gViewProj;
    float3   gCamRight;
    float    _p0;
    float3   gCamUp;
    float    _p1;
    uint     gBaseIndex;
    uint     gUseTexture;  // 0=procedural 円 / 1=フリップブックテクスチャ
    int      gFlipTilesX;
    int      gFlipTilesY;
    float    gFlipCycles;  // 寿命あたりのフリップブック周回数
    int      gBlendAdditive; // 1=additive (fog=減光) / 0=alpha (fog=色 lerp)
    int      gFogMode;       // -1=off / 0=linear 1=exp 2=exp2
    float    _p2;
    float3   gCameraPos;
    float    gFogDensity;
    float3   gFogColor;
    float    gFogStart;
    float    gFogEnd;
    // M42b: ソフトパーティクル (旧 _p3 パディングを転用、CB サイズ不変)
    float    gSoftFade; // 深度フェード距離 (0=off)。ParticleCurves.h::SoftFadeFactor と同一式
    float    gNearZ;    // 深度線形化用 (common.hlsli::LinearizeDepth へ渡す)
    float    gFarZ;
    // ---- M57e: フロクセル (末尾 append。0 = 従来と 1 ビットも変わらない) ----
    // ★粒子に適用しないと「霧の中で粒子だけが浮く」— 加算合成は背景の減衰を
    //   受けないので、周囲が霞むほど粒子だけが不自然にくっきり残る
    int      gFroxelEnabled;
    float    gFroxelNearZ;
    float    gFroxelFarZ;
    float    gFroxelSlices;
    float2   gFroxelScreenSize; // SV_Position → uv
    float2   _froxelPad;
    // ---- M63a: ビルボード変換 (末尾 append。0 = 従来と 1 ビットも変わらない) ----
    int      gBillboardMode; // 0=corner 素通し / 1=回転・ストレッチを適用
    float3   _billboardPad;
    // ---- M63c: フリップブック (末尾 append。0 = 従来と 1 ビットも変わらない) ----
    // ★gFlipMode が 0 のとき PS は **VS が送ってきた flip を読まない** — その場で age から
    //   作る従来の式を通る。「fps=0 なら値は同じ」ではない: CPU 充填ループが作った値は
    //   ラスタライザ補間を通っていない別の道の値で、最下位ビットが動きうる
    int      gFlipMode;  // 0=従来 (PS が age から作る) / 1=VS 経由の連続コマ位置を使う
    int      gFlipBlend; // 1=隣のコマと frac で補間 (PS の Sample が 2 回になる)
    float2   _flipPad;
};

struct ParticleInstance
{
    float3 pos;
    float  size;
    float4 color;
    float  age;   // [0,1] 寿命係数
    // ---- M63a: 旧 _pad の 12B を意味づけし直した (48B のまま) ----
    // CPU 側 (CpuParticleBackend.cpp の ParticleInstance) が畳んで送る 3 スカラ。
    // ★速度は送られてこない — ストレッチは CPU が「画面角 + 長軸倍率」へ落としてある
    float  rot;       // 回転角 [rad] (初期回転 + 角速度*経過 + 速度の画面角)
    float  stretch;   // 長軸倍率 (1.0 = 伸ばさない)
    float  flipFrame; // フリップブックの連続コマ位置 (M63c)
};
StructuredBuffer<ParticleInstance> gParticles : register(t0);

Texture2D    gTex   : register(t1);
Texture2D    gDepth : register(t2); // M42b: シーン深度 (read-only DSV とセットでバインド)
Texture3D    gFroxelVolume : register(t3); // M57e (rgb=積算内向き散乱 / a=透過率)
SamplerState gSamp  : register(s0); // LINEAR/CLAMP — froxel もこれを流用する

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  age   : TEXCOORD1;
    float  dist  : TEXCOORD2; // カメラからのワールド距離 (フォグ用)
    float  viewZ : TEXCOORD3; // ビュー空間深度 (M42b ソフトフェード用、= clip.w)
    float  flip  : TEXCOORD4; // M63c: 連続コマ位置 (CPU が充填ループで畳んだもの)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const ParticleInstance p = gParticles[gBaseIndex + iid];
    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    // M63a: **この分岐が既定エミッタの絵をビット保存している唯一の仕掛け。**
    // 「rot=0 / stretch=1 なら同じ値」ではない — 通せば `x*1.0` と `cos(0)` 乗算が入り、
    // 最下位ビットが動きうる。分岐を外すとスクショ golden が全部赤くなる
    float2 c = corner;
    if (gBillboardMode != 0) {
        c = ParticleBillboardCorner(corner, p.rot, p.stretch);
    }
    const float3 world = p.pos + (gCamRight * c.x + gCamUp * c.y) * p.size;

    VSOut o;
    o.pos = mul(float4(world, 1.0f), gViewProj);
    o.uv = corner * 0.5f + 0.5f;
    o.color = p.color;
    o.age = p.age;
    o.dist = length(world - gCameraPos);
    o.viewZ = o.pos.w; // 透視投影では clip.w = ビュー空間 z
    // M63c: フリップブックの連続コマ位置。**CPU が畳んで送ってくる**ので VS は素通し。
    // (GPU バックエンドはプールの flipU を VS が読めるので、あちらは VS で作る)
    o.flip = p.flipFrame;
    return o;
}

// 距離フォグ係数。ApplyFog へは寄せない — 粒子は additive なら「減光」、alpha なら
// 「フォグ色へ補間」と合成の仕方が分かれるので、色ではなく係数が要る。
// M57追補: 式そのものは common.hlsli::FogFactor へ移して GPU バックエンドと共有した。
// ここに残るのは CB フィールドを束ねるだけの名前 — .hlsli は register / CB 名を持たない
// 契約なので、「CB を読む部分」はシェーダ側に残す必要がある
float ParticleFogFactor(float dist)
{
    return FogFactor(gFogMode, gFogDensity, gFogStart, gFogEnd, dist);
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 col;
    if (gUseTexture != 0)
    {
        // フリップブック: 連続コマ位置でタイルを選ぶ (tilesX*tilesY コマ)。
        // M63c: タイル分割と 2 コマ補間は particle_billboard.hlsli の SampleFlipTile へ
        // 寄せた — GPU バックエンドの PS が同じ 1 本を呼ぶ (M42c 以来ここは手写しだった)。
        // ★gFlipMode==0 の枝は M42c の式そのまま。`i.age * gFlipCycles * (float)tiles` を
        //   **この場で**評価するのが既存 golden のビット保存条件で、VS 経由の値へ
        //   置き換えてはいけない
        const uint tx = (uint)max(1, gFlipTilesX);
        const uint ty = (uint)max(1, gFlipTilesY);
        const uint tiles = tx * ty;
        const float frame = (gFlipMode != 0) ? i.flip : (i.age * gFlipCycles * (float)tiles);
        const float4 tex = SampleFlipTile(gTex, gSamp, i.uv, frame, tx, ty, gFlipBlend != 0);
        col = float4(i.color.rgb * tex.rgb, i.color.a * tex.a);
    }
    else
    {
        // procedural ソフト円形 (テクスチャ未指定時)
        const float2 d = i.uv * 2.0f - 1.0f;
        float m = saturate(1.0f - dot(d, d));
        m *= m;
        col = float4(i.color.rgb * m, i.color.a * m);
    }

    // フォグ (M32c): additive は減光、alpha はフォグ色へ補間。
    // M57e: フロクセルが on のときは受け持ちを分ける — 解析フォグが担うのは
    // 「視線がグリッドを出てから粒子まで」の残り区間だけ。グリッドの中の粒子は
    // 残り 0m = f が厳密に 0 になり、フロクセル側だけが効く (deferred_light と同じ規約)
    float fogDist = i.dist;
    if (gFroxelEnabled != 0) {
        fogDist = i.dist * (1.0f - FroxelFogHandoffFraction(i.viewZ, gFroxelFarZ));
    }
    const float f = ParticleFogFactor(fogDist);
    if (gBlendAdditive != 0) {
        col.rgb *= (1.0f - f);
    } else {
        col.rgb = lerp(col.rgb, gFogColor, f);
    }
    // M57e: フロクセルの合成。**加算合成には内向き散乱を足さない** — 背後のサーフェス
    // (またはスカイ) が既に 1 回足しているので、加算で重ねるたびに足すと粒子の枚数ぶん
    // 霧が濃くなる。加算の粒子が受け取るのは「自分からカメラまでの減衰」だけ。
    // M57追補: 2 分岐を froxel_common.hlsli::FroxelCompositeParticle へ移して GPU
    // バックエンドと共有した (式は 1 ビットも変えていない)
    if (gFroxelEnabled != 0) {
        col.rgb = FroxelCompositeParticle(gFroxelVolume, gSamp, i.pos.xy, gFroxelScreenSize,
                                          i.viewZ, gFroxelSlices, gFroxelNearZ, gFroxelFarZ,
                                          col.rgb, gBlendAdditive != 0);
    }

    // ソフトパーティクル (M42b): シーン深度との差でフェード。0=off (従来とビット同一)。
    // ParticleCurves.h::SoftFadeFactor と同一式 (selftest はそちらを検証)
    if (gSoftFade > 0.0f) {
        const float sceneZ =
            LinearizeDepth(gDepth.Load(int3(int2(i.pos.xy), 0)).r, gNearZ, gFarZ);
        const float fade = saturate((sceneZ - i.viewZ) / max(gSoftFade, 1e-4f));
        col *= fade; // rgb + a 両方 -> additive/alpha どちらのブレンドでも正しく消える
    }
    return col;
}
