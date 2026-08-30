// GPU パーティクル描画: alive list + pool から直接ビルボード生成
// DrawInstancedIndirect (InstanceCount は CopyStructureCount で GPU 上のみで確定)

#include "particle_gpu_common.hlsli"
// M57追補: フロクセルの受け持ちの分け方と合成。register 宣言を 1 つも持たない純関数の束
// なので衝突しない。**particle_gpu_common.hlsli 側には入れない** — あちらは emit/sim CS も
// 読むので、描画専用のものを持ち込まない
#include "froxel_common.hlsli"
// M63a: 四隅の回転/ストレッチ。**CPU バックエンド (particle_render.hlsl) と同じ 1 本**を
// 呼ぶことが「2 実装が同じ絵を出す」(spec 7.5) の担保。register 宣言を持たないので衝突しない
#include "particle_billboard.hlsli"

cbuffer GpuRenderCB : register(b1)
{
    float4x4 gViewProj;
    float3   gCamRight;
    float    _q0;
    float3   gCamUp;
    float    _q1;
    float    gOffsetX; // 比較モードの横オフセット
    float3   _q2;
    // ---- M61g: ローカルシミュレーション空間 (末尾 append。C++ 側 GpuRenderCB と一致) ----
    float4x4 gEmitterWorld; // transpose 済み (mul(float4, M) 規約は gViewProj と同じ)
    float4   gSpaceParams;  // x = simulationSpace (1 = pos を gEmitterWorld で変換), yzw = 予約
    // ---- M57追補: 解析フォグ + フロクセル (末尾 append。C++ 側 GpuRenderCB と一致) ----
    // ★ここが 1 行も無かったのが M57e のやり残し。加算合成は背景の減衰を受けないので、
    //   周囲が霞むほど GPU 粒子だけが不自然にくっきり残っていた。
    // ★**gSpaceParams.yzw の予約枠は使わない** — あれは M61g「シミュレーション空間」の
    //   ブロックで、フォグとは無関係。混ぜると名前が嘘になり、次にローカル空間を拡張する
    //   人が .y を空きだと思って踏む。節約できるのも 16 バイトだけ (必要なスカラは 17 本)。
    // ★フラグを int でなく float で持つのは GpuParticleCB の家の流儀に合わせたもの
    float4   gFogParams;      // x=fogMode (-1=off/0=linear/1=exp/2=exp2) y=density z=start w=end
    float4   gFogColorBlend;  // xyz=フォグ色, w=blendAdditive (1=加算 / 0=alpha)。
                              // **必ず一緒に読む 2 つ**なので同じ float4 に置いている
    float4   gCameraPosParam; // xyz=カメラ位置 (VS の dist 用), w=予約
    float4   gFroxelParams;   // x=enabled, y=nearZ, z=farZ, w=sliceCount
    float4   gFroxelScreen;   // xy=画面サイズ (px。SV_Position → uv), zw=予約
    // ---- M63a: ビルボード変換 (末尾 append。C++ 側 GpuRenderCB と一致) ----
    // x=billboardMode (0=corner 素通し / 1=回転・ストレッチ), yzw=予約 (M63b がストレッチ係数を使う)
    float4   gBillboardParams;
};

StructuredBuffer<GpuParticle> gPoolSRV : register(t0);
StructuredBuffer<uint> gAliveList : register(t1);
Texture2D gDepth : register(t2); // M42b: シーン深度 (PS のみ。read-only DSV とセット)
Texture2D gTex   : register(t3); // M42c: フリップブックテクスチャ (未使用時は白)
// M57追補: フロクセル (rgb=積算内向き散乱 / a=透過率)。
// ★CPU 版は t3 だが、こちらは t3 をフリップブックが占有しているので **t4**
//   (froxel::kGpuParticleSrvSlot。check_rules.ps1 の規則 9 が機械照合する)
Texture3D gFroxelVolume : register(t4);
SamplerState gSamp : register(s0); // LINEAR/CLAMP — froxel もこれを流用する

// ---- M42追補: カーブ評価の HLSL ミラー (正本は ParticleCurves.h) ----
// ★**particle_gpu_common.hlsli には入れない** — あちらは emit/sim CS も読むので、描画専用の
//   ものを持ち込まない (M57追補が FroxelCompositeParticle で立てたのと同じ線引き)。
// ★lerp を使わず `a + (b - a) * f` と明示展開する — C++ 側 EvalParticleColor と同じ演算列に
//   揃えるため。ここを lerp にすると mad へ畳まれて最下位ビットが動きうる。
// ★中間キーが無効 (T が (0,1) の外) なら begin→end の 2 点線形へ縮退し、
//   従来の絵とビット同一になる (既存コンテンツのビット保存はこの縮退が担保する)。

// 寿命係数 age∈[0,1] での色。キー: begin(0) / [colorMid1@T1] / [colorMid2@T2] / end(1)
float4 EvalParticleColorGpu(float age)
{
    age = saturate(age);
    float t[4];
    float4 c[4];
    int n = 0;
    t[0] = 0.0f;
    c[0] = gColorBegin;
    n = 1;
    if (gParams5.x > 0.0f && gParams5.x < 1.0f)
    {
        t[n] = gParams5.x;
        c[n] = gColorMid1;
        ++n;
    }
    if (gParams5.y > 0.0f && gParams5.y < 1.0f)
    {
        t[n] = gParams5.y;
        c[n] = gColorMid2;
        ++n;
    }
    t[n] = 1.0f;
    c[n] = gColorEnd;
    ++n;

    // T 昇順に挿入ソート (n<=4、安定) — C++ 側と同じ
    for (int i = 1; i < n; ++i)
    {
        for (int j = i; j > 0 && t[j] < t[j - 1]; --j)
        {
            const float tswap = t[j];
            t[j] = t[j - 1];
            t[j - 1] = tswap;
            const float4 cswap = c[j];
            c[j] = c[j - 1];
            c[j - 1] = cswap;
        }
    }
    for (int k = 0; k + 1 < n; ++k)
    {
        if (age <= t[k + 1])
        {
            const float span = t[k + 1] - t[k];
            const float f = (span > 1e-6f) ? (age - t[k]) / span : 0.0f;
            return c[k] + (c[k + 1] - c[k]) * f;
        }
    }
    return c[n - 1];
}

// 寿命係数 age∈[0,1] でのサイズ倍率。キー: 1.0(0) / [sizeMidScale@sizeMidT] / sizeEndScale(1)
float EvalParticleSizeScaleGpu(float age)
{
    age = saturate(age);
    const float midT = gParams5.w;
    const float midScale = gParams5.z;
    const float endScale = gParams.z;
    if (midT > 0.0f && midT < 1.0f)
    {
        if (age <= midT)
        {
            return 1.0f + (midScale - 1.0f) * (age / midT);
        }
        const float f = (age - midT) / (1.0f - midT);
        return midScale + (endScale - midScale) * f;
    }
    return 1.0f + (endScale - 1.0f) * age;
}

struct VSOut
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    float  viewZ : TEXCOORD1; // M42b: ビュー空間深度 (= clip.w)
    float  age   : TEXCOORD2; // M42c: [0,1] 寿命係数 (フリップブック用)
    // M57追補: カメラからのワールド距離 (解析フォグ用)。
    // ★viewZ で代用してはいけない — あれはビュー空間深度で放射距離ではなく、
    //   60° FOV の画面端で ~15% ずれる。代用すると「同じシーンで CPU 粒子と GPU 粒子の
    //   霧の濃さが画面端だけ違う」= 比較モードで真っ先に目に付く食い違いを新たに作る
    float  dist  : TEXCOORD3;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const GpuParticle p = gPoolSRV[gAliveList[iid]];
    const float age = saturate(1.0f - p.life * p.invLife);
    // M42追補: 多点グラデーション。中間キーが無効なら従来の 2 点線形へビット同一に縮退する
    const float size = p.size0 * EvalParticleSizeScaleGpu(age);
    const float4 color = EvalParticleColorGpu(age);

    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? -1.0f : 1.0f);
    // M61g: ローカル空間 (simulationSpace=1) はプールの pos がエミッタローカル座標 —
    // ここでワールドへ変換する (CPU バックエンドの renderWorld 変換と同じ意味論)。
    // ビルボードサイズにはスケールを適用しない (v1 制限 — 張り出しは変換後の位置へ素のまま)
    float3 basePos = p.pos;
    if (gSpaceParams.x != 0.0f)
    {
        basePos = mul(float4(p.pos, 1.0f), gEmitterWorld).xyz;
    }
    // M63a: 回転。**CPU 側 particle_render.hlsl と同一の分岐・同一の関数**を通る。
    // 角度は閉形式 rot0 + rotVel*elapsed — sim CS が積分していないのはこのため。
    // ★off の分岐を外してはいけない (rot=0 でも sincos 乗算でビットが動きうる)
    float2 c = corner;
    if (gBillboardParams.x != 0.0f) {
        const float rot = ParticleRotationAt(p.rot0, p.rotVel,
                                             ParticleElapsedFromLife(p.life, p.invLife));
        c = ParticleBillboardCorner(corner, rot, 1.0f);
    }
    const float3 world = basePos + float3(gOffsetX, 0, 0)
        + (gCamRight * c.x + gCamUp * c.y) * size;

    VSOut o;
    o.pos = mul(float4(world, 1.0f), gViewProj);
    o.uv = corner * 0.5f + 0.5f;
    o.color = color;
    o.viewZ = o.pos.w; // 透視投影では clip.w = ビュー空間 z
    o.age = age;       // M42c: フリップブック用
    // M57追補: CPU 版 particle_render.hlsl と**同一式**。world には gOffsetX が既に
    // 入っているので、CPU 側が inst.pos に renderOffsetX を足してから測るのと結合順まで一致する
    o.dist = length(world - gCameraPosParam.xyz);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 col;
    if (gParams3.x != 0.0f)
    {
        // フリップブック (M42c): particle_render.hlsl PSMain のフリップブック分岐を移植 (同一式)
        const uint tx = (uint)max(1.0f, gParams3.y);
        const uint ty = (uint)max(1.0f, gParams3.z);
        const uint tiles = tx * ty;
        uint frame = (uint)max(0, (int)floor(i.age * gParams3.w * (float)tiles));
        frame = frame % tiles;
        const uint cx = frame % tx;
        const uint cy = frame / tx;
        const float2 uv = (i.uv + float2(cx, cy)) / float2(tx, ty);
        const float4 tex = gTex.Sample(gSamp, uv);
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

    // ---- M57追補: フォグ (M32c) + フロクセル (M57e) ----
    // **CPU バックエンド (particle_render.hlsl PSMain) と同一の意味論・同一の順序**
    // (フォグ → フロクセル → ソフトフェード)。ここが 1 行も無かったせいで、GPU に
    // 切り替えると粒子だけ霧が抜けていた。
    // 解析フォグが担うのは「視線がグリッドを出てから粒子まで」の残り区間だけ —
    // グリッドの中の粒子は残り 0m = f が厳密に 0 になり、フロクセル側だけが効く
    // (deferred_light の起点押し出しと同じ規約を距離側で書いたもの = 三重計上の回避)
    float fogDist = i.dist;
    if (gFroxelParams.x != 0.0f) {
        fogDist = i.dist * (1.0f - FroxelFogHandoffFraction(i.viewZ, gFroxelParams.z));
    }
    const float f = FogFactor((int)gFogParams.x, gFogParams.y, gFogParams.z, gFogParams.w, fogDist);
    const bool blendAdditive = (gFogColorBlend.w != 0.0f);
    if (blendAdditive) {
        col.rgb *= (1.0f - f); // 加算は減光
    } else {
        col.rgb = lerp(col.rgb, gFogColorBlend.rgb, f); // alpha はフォグ色へ補間
    }
    if (gFroxelParams.x != 0.0f) {
        col.rgb = FroxelCompositeParticle(gFroxelVolume, gSamp, i.pos.xy, gFroxelScreen.xy,
                                          i.viewZ, gFroxelParams.w, gFroxelParams.y,
                                          gFroxelParams.z, col.rgb, blendAdditive);
    }

    // ソフトパーティクル (M42b): 0=off (従来とビット同一)。CPU 版 particle_render.hlsl と同一式
    if (gParams2.x > 0.0f) {
        const float sceneZ =
            LinearizeDepth(gDepth.Load(int3(int2(i.pos.xy), 0)).r, gParams2.y, gParams2.z);
        col *= saturate((sceneZ - i.viewZ) / max(gParams2.x, 1e-4f));
    }
    return col;
}
