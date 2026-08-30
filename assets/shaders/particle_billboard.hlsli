// M63a: パーティクルのビルボード四隅変換 (回転 + 長軸ストレッチ)。
//
// **CPU バックエンドと GPU バックエンドの唯一の共有点。** 粒子の描画は
// particle_render.hlsl (CPU インスタンス経路) と particle_render_gpu.hlsl (GPU プール経路) の
// 2 実装を持つが、spec 7.5 は両者が同じ絵を出すことを要求している。四隅の作り方を 2 箇所に
// 手写しすると「片方だけ直して CPU と GPU で回転の向きが逆」が必ず起きる — M42追補 が
// alpha ソートで実際に踏んだ形 (キーの式を ParticleAlphaSortViewZ へ寄せて decided した) と同じ。
//
// **particle_gpu_common.hlsli には置けない** — あちらは emit / sim CS も読むので、
// 描画専用のものを持ち込まない (M57追補 が FroxelCompositeParticle で立てた線引きと同じ)。
// **common.hlsli にも置けない** — あちらはエンジン全体の共有で、粒子固有の規約を混ぜない。
//
// ★ここの関数はすべて **C++ の mye::ParticleCurves.h と同一式**。
//   機械照合は無い (check_rules の $constGroups は「1 ファイル 1 整数」しか比べられない) ので、
//   片方だけ直すと ParticleSelfTest は緑のまま絵だけが静かに割れる。必ず両方同時に変更すること。

#ifndef MYE_PARTICLE_BILLBOARD_HLSLI
#define MYE_PARTICLE_BILLBOARD_HLSLI

// 四隅の変換: 長軸ストレッチ (X 軸) → 回転。C++ ミラー: ParticleBillboardCornerCpu。
// ★stretch を **X 軸**に掛けるのが規約。速度ストレッチ (M63b) は「速度の画面角」を rot へ
//   足し込むことで長軸を速度方向へ向ける — 軸を増やさずに回転と枠 (rot 1 本) を共有できる。
// ★rot=0 / stretch=1 でも `x*1` と `cos(0)` 乗算を通るので**ビット同一ではない**。
//   恒等をビット保存するのは呼び出し側 (gBillboardMode の分岐) の責務。
//   これを忘れると既定エミッタの絵が動き、スクショ golden が全部赤くなる。
float2 ParticleBillboardCorner(float2 corner, float rot, float stretch)
{
    const float cx = corner.x * stretch;
    float s, c;
    sincos(rot, s, c);
    return float2(cx * c - corner.y * s, cx * s + corner.y * c);
}

// 粒子が生まれてからの経過秒。life = 残り秒 / invLife = 1/寿命 なので 1/invLife が寿命。
// C++ ミラー: ParticleElapsedFromLife。
// ★subframeEmission で life が lifetime + f*dt へ前倒しされている粒子は僅かに負を返すが、
//   角度にもコマ位置にも連続に効くだけで無害 (saturate すると湧いた瞬間だけ段差が出る)。
float ParticleElapsedFromLife(float life, float invLife)
{
    return 1.0f / invLife - life;
}

// 回転角の閉形式。C++ ミラー: ParticleRotationAt。
// ★**sim では積分しない** — 定数角速度なら積分結果がこの式と厳密に一致するので、
//   particle_sim.cs.hlsl を 1 行も触らずに回転が入る。角速度に減衰を入れる日が来たら
//   この閉形式は壊れるので、そのときは sim 状態へ移すこと。
float ParticleRotationAt(float rot0, float rotVel, float elapsed)
{
    return rot0 + rotVel * elapsed;
}

#endif // MYE_PARTICLE_BILLBOARD_HLSLI
