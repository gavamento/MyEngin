// GPU パーティクルの共通定義 (emit / sim / render で共有)

// M55a: LinearizeDepth を共有するために include する。common.hlsli は register 宣言を
// 一切持たない純関数/構造体だけの束なので、CS (emit/sim) へ持ち込んでも衝突しない。
// ★懸念だった「PerturbNormal の ddx/ddy が cs_5_0 で落ちる」は起きない —
//   D3DCompile は未使用関数を検証前に落とすため (fxc /T cs_5_0 /Ges /O3 で実測。
//   deferred_gbuffer.hlsl の VSMain が同じ理由で通っているのが既存の傍証)。
#include "common.hlsli"

struct GpuParticle
{
    float3 pos;
    float  life;     // 残り秒
    float3 vel;
    float  invLife;  // 1/寿命
    float  size0;
    // ---- M63a: 旧 _pad の 12B を意味づけし直した (48B のまま = StructureByteStride 不変) ----
    // **すべて放出時に決まる不変データ**。sim CS は 1 度も触らない (回転は描画 VS が
    // rot0 + rotVel*elapsed の閉形式で導出する) ので particle_sim.cs.hlsl は無改造。
    // ★CPU バックエンドと違い、GPU 側は速度 (vel) を VS から直接読めるので、
    //   ストレッチ用に別枠を持つ必要がない — だから 3 float でちょうど収まる
    float  rot0;   // 初期回転角 [rad]
    float  rotVel; // 角速度 [rad/s]
    float  flipU;  // フリップブック開始位相 [0,1) (M63c)
};

cbuffer GpuParticleCB : register(b0)
{
    float4 gGravityWind;   // xyz = gravity+wind, w = dt
    float4 gParams;        // x = emitCount, y = turbulence, z = sizeEndScale, w = capacity
    float4 gColorBegin;
    float4 gColorEnd;
    float4 gParams2;       // M42b: x = softFade (0=off), y = nearZ, z = farZ / w = 予約
    float4 gParams3;       // M42c: x = useTexture, y = flipTilesX, z = flipTilesY, w = flipCycles
    // ---- M42e: 深度バッファ衝突 (GPU バックエンド限定の見た目効果。sim CS のみ参照) ----
    float4x4 gCollViewProj;    // transpose 済み (mul(float4, M) 規約は gViewProj と同じ)
    float4x4 gCollInvViewProj; // transpose 済み
    // M63e: z (thickness) の 0.0f 固定を撤廃し、w の予約枠を摩擦へ回した
    float4 gCollParams;        // x = enabled, y = restitution, z = 追加 thickness, w = friction
    float4 gCollScreen;        // xy = 画面サイズ (px), z = nearZ, w = farZ
    // ---- M61d: カールノイズ乱流 (sim CS のみ参照。C++ 側 GpuParticleBackend.cpp の
    //      GpuParticleCB::params4 と一致 — 末尾 append なので既存フィールドのオフセット不変) ----
    float4 gParams4;           // x = turbulenceMode, y = noiseFrequency, z = noiseSpeed,
                               // w = noiseTime (ageTicks * dt — 実時間ではない)
    // ---- M42追補: 多点グラデーション (末尾 append。C++ 側 GpuParticleCB と一致。
    //      既存フィールドのオフセット不変。描画 VS だけが読む) ----
    // ★ここが 1 枠も無かったので GPU バックエンドは中間キーを**丸ごと無視**し、
    //   begin→end の 2 点線形だけで色を作っていた。colorMid* は alpha (w) も持つので、
    //   中間キーを設定した瞬間に煙のフェードが CPU と別物になる
    float4 gColorMid1;
    float4 gColorMid2;
    float4 gParams5;           // x = colorMidT1, y = colorMidT2, z = sizeMidScale, w = sizeMidT
    // ---- M63c: フリップブック (末尾 append。C++ 側 GpuParticleCB と一致。
    //      既存フィールドのオフセット不変。描画 VS (x/y/z) と PS (x/w) だけが読む) ----
    // ★x が 0 のとき PS は VS の flip を読まず、その場で age から作る従来の式を通る。
    //   「fps=0 なら値は同じ」ではない (VS で作った値はラスタライザ補間を通っていない)。
    // ★w (2 コマ補間) は **x が 0 のとき必ず 0** — C++ 側が useFlip でゲートしている。
    float4 gParams6;           // x = flipMode, y = flipFps, z = flipRandomStart, w = flipBlend
    // ---- M63d: ライティング (末尾 append。C++ 側 GpuParticleCB と一致。
    //      既存フィールドのオフセット不変。描画の VS (x が 1) と PS (x が 2) だけが読む) ----
    // ★x が 0 のとき VS も PS も ParticleLightAt を 1 度も呼ばない。「ライトが 0 本なら
    //   受光係数 1.0 だから分岐は要らない」ではない — アンビエントが 0 でないシーンでは
    //   env が乗って色が動く。
    // ★ライト配列そのものは**この CB に入れない** — ここはエミッタごと tick ごとに上がる
    //   CB で、1KB のライト配列を積むと 100 エミッタで毎フレーム 100KB 増える。
    //   ライトはビュー単位なので b2 (ParticleLightCB) 側に置いて 1 回だけ上げる
    float4 gParams7;           // x = lightingMode, y = lightWrap, z = lightIntensity,
                               // w = lightReceiveShadow
    // ---- M63e: 深度衝突の拡張 (末尾 append。C++ 側 GpuParticleCB と一致。
    //      既存フィールドのオフセット不変。sim CS だけが読む) ----
    // ★M42e の gCollParams.w は「予約」だったので、摩擦はそちらへ入れて反射の 4 引数
    //   (enabled / restitution / thickness / friction) を 1 レジスタに揃えてある。
    //   こちらは「反射しない側」= 寿命損失と解析床。
    // ★z (解析床) は gCollParams.x (深度衝突) と**独立したゲート**。深度衝突は画面外・空・
    //   背面ですり抜けるので、床だけを塞ぎたいという使い方が成立する
    float4 gCollParams2;       // x = collisionLifeLoss, y = 予約,
                               // z = floorEnabled, w = floorY
};

// ---- M63e: sim CS が M42e 以来**手写ししていた**式の共有点 ----
// C++ ミラー: ParticleCurves.h の ParticleClipToUv / ReflectWithFriction (selftest 対象)。
// コメント同期のみの一致 — 変更は必ず両方同時に。

// クリップ座標 -> スクリーン UV。背面 (w<=0) は false = 衝突判定しない
bool ParticleClipToUv(float4 clip, out float2 uv)
{
    uv = float2(0.0f, 0.0f);
    if (clip.w <= 0.0f) {
        return false;
    }
    uv = float2((clip.x / clip.w) * 0.5f + 0.5f, 0.5f - (clip.y / clip.w) * 0.5f);
    return true;
}

// 反射 + 反発 + 接線摩擦。friction=0 は**早期 return で純反射へ落とす** —
// t = restitution * (1-0) = restitution なので代数的には一致するが、
// (v - vn)*t - vn*r と (v - 2vn)*r は演算列が違うので float ではビットが揃わない。
// M42e からの絵を 1 ビットも動かさない条件がこの早期 return
float3 ReflectWithFriction(float3 vel, float3 n, float restitution, float friction)
{
    if (friction <= 0.0f) {
        return reflect(vel, n) * restitution;
    }
    const float3 vn = dot(vel, n) * n;               // 法線成分
    const float t = restitution * (1.0f - min(friction, 1.0f)); // 接線成分の係数
    return (vel - vn) * t - vn * restitution;
}
