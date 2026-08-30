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

// M63b: 速度ストレッチ。速度を画面基底へ射影して「長軸を向ける角度」と「長軸倍率」へ畳む。
// C++ ミラー: EvalParticleStretchCpu。
//
// ★倍率の元は **3D 速度の長さではなく射影後の長さ**。3D 長で測るとカメラへ真っ直ぐ飛ぶ
//   粒子が「速いので長く伸びる」のに射影成分は ~0 = atan2 の向きが毎フレーム暴れ、
//   長い線がランダムな向きへ回る。射影長ならその状況が閾値に落ちて stretch=1 になる。
// ★角度は rot へ**加算**する (軸を増やさない M63a の枠の共有)。
// ★CPU 側とのビット一致は**保証しない** — atan2 / sqrt の実装が libm と GPU で違う。
//   突き合わせるのは「向きと伸び方が同じか」であって画素の完全一致ではない
//   (particle_cpu / particle_gpu の golden が別々に版管理されているのはこのため)。
//   実測 (WARP, M63b 時点) では伸びの領域は画素一致し、golden 間の差 217→221 画素は
//   M63a から在る回転部の差のまま — つまり**この関数は現状ずれを増やしていない**。
void EvalParticleStretch(float3 v, float3 camRight, float3 camUp, float stretchScale,
                         float stretchMax, out float angle, out float stretch)
{
    const float dr = dot(v, camRight);
    const float du = dot(v, camUp);
    const float speed = sqrt(dr * dr + du * du);
    if (speed < 1e-4f) {
        angle = 0.0f;
        stretch = 1.0f;
        return;
    }
    angle = atan2(du, dr);
    stretch = clamp(1.0f + speed * stretchScale, 1.0f, max(1.0f, stretchMax));
}

// ---- M63c: フリップブック (固定 fps / コマ間補間 / ランダム開始) ----
// C++ ミラー: ParticleCurves.h の ParticleFlipFrameAt / ParticleFlipTilePos /
// ParticleFlipTileUvCpu (UV は検査用ミラー — 実際にサンプルするのは下の SampleFlipTile)。
//
// ★タイル UV の式は M42c から **CPU 経路と GPU 経路の PS へ手写し**されていた。
//   コマ間補間で「2 コマ目の UV」を作る必要が出た時点で写しが 4 箇所になるので、
//   ここへ寄せて 1 本にする (回転で particle_billboard.hlsli を作ったのと同じ理由)。

// 連続コマ位置。C++ ミラー: ParticleFlipFrameAt。
// ★flipFps <= 0 && !randomStart では `age * flipCycles * tiles` へ**演算列ごと**縮退する
//   — 従来 PS に書かれている式そのもの。ここが崩れると既存のフリップブックが動く。
float ParticleFlipFrameAt(float age, float elapsed, float flipCycles, float flipFps,
                          float flipU, float tiles, bool randomStart)
{
    float f = (flipFps > 0.0f) ? (elapsed * flipFps) : (age * flipCycles * tiles);
    if (randomStart) {
        f += flipU * tiles; // ずらし量は「コマ数」単位 = 位相であって送り速度ではない
    }
    return f;
}

// 連続コマ位置 → 表示コマ / ブレンド先 / 補間係数。C++ ミラー: ParticleFlipTilePos。
// ★負 (subframeEmission で life が前倒しされた粒子) は 0 へ丸める = 従来の
//   `(uint)max(0, (int)floor(...))` と同じ扱い。
// ★ブレンド先は **先頭コマへ循環** — 非ブレンド経路の `frame % tiles` と同じ規約。
void ParticleFlipTilePos(float frame, uint tiles, out uint idx, out uint next, out float blend)
{
    const float f = max(frame, 0.0f);
    const float fl = floor(f);
    idx = (uint)fl % tiles;
    next = (idx + 1u) % tiles;
    blend = f - fl;
}

// コマ番号 → アトラス内の UV。C++ ミラー: ParticleFlipTileUvCpu。
float2 ParticleFlipTileUV(float2 uv, uint frame, uint tx, uint ty)
{
    const uint cx = frame % tx;
    const uint cy = frame / tx;
    return (uv + float2(cx, cy)) / float2(tx, ty);
}

// アトラスから 1 コマを取る。blendFrames で隣のコマとの 2 サンプル補間。
// ★blendFrames が false のときは **Sample 1 回だけ / lerp を通らない**。従来経路
//   (M42c) と演算列が 1 つも変わらないのが、既存 golden をビット保存する条件。
// ★mip は張らない前提 (アトラスはタイル境界を跨ぐ mip でコマ同士が混ざる)。
//   DemoContent の vdemo_flipbook が mips=false で作っているのはこのため。
float4 SampleFlipTile(Texture2D tex, SamplerState samp, float2 uv, float frame,
                      uint tx, uint ty, bool blendFrames)
{
    uint idx, next;
    float blend;
    ParticleFlipTilePos(frame, tx * ty, idx, next, blend);
    const float4 c0 = tex.Sample(samp, ParticleFlipTileUV(uv, idx, tx, ty));
    if (!blendFrames) {
        return c0;
    }
    const float4 c1 = tex.Sample(samp, ParticleFlipTileUV(uv, next, tx, ty));
    return lerp(c0, c1, blend);
}

#endif // MYE_PARTICLE_BILLBOARD_HLSLI
