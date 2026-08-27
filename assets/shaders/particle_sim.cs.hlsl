// GPU パーティクル更新: aliveIn を積分し、生存者を aliveOut へ圧縮、死亡者は dead list へ返す

#include "particle_gpu_common.hlsli"

RWStructuredBuffer<GpuParticle> gPool : register(u0);
AppendStructuredBuffer<uint> gDeadList : register(u1);
RWStructuredBuffer<uint> gAliveOut : register(u2); // カウンタ付き
StructuredBuffer<uint> gAliveIn : register(t0);
Buffer<uint> gCounts : register(t1); // [1] = aliveInCount (typed — CopyStructureCount の宛先)
Texture2D<float> gDepth : register(t2); // M42e: 前フレームのシーン深度 (未バインド時は enabled=0)

// M42e: ピクセル + 深度 -> ワールド位置再構成 (法線推定用)
float3 CollReconstructWorld(int2 pix, float d)
{
    const float2 uv = (float2(pix) + 0.5f) / gCollScreen.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 w = mul(float4(ndc, d, 1.0f), gCollInvViewProj);
    return w.xyz / max(abs(w.w), 1e-6f) * sign(w.w);
}

// ---- M61d: カールノイズ乱流場 (turbulenceMode=1) ----
// C++ ミラー: ParticleCurves.h の CurlNoiseHash / CurlValueNoise / CurlPot* / EvalCurlNoise。
// コメント同期のみの一致 — 変更は必ず両方同時に。乱数は使わない (位置と時間の純関数)。
// 中心差分の刻みは C++ 側 kCurlNoiseEps = 0.25 と同値

// 整数格子ハッシュ (32bit finalizer)。int→uint と乗算オーバーフローは wrap で C++ と同値
uint CurlNoiseHash(int3 c)
{
    uint h = (uint)c.x * 0x8da6b343u + (uint)c.y * 0xd8163841u + (uint)c.z * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x7feb352du;
    h ^= h >> 16;
    return h;
}

// 格子点の値 [-1, 1) (上位 24bit → [0,1) → [-1,1) は Pcg32::NextFloat01 と同じ量子化)
float CurlNoiseLattice(int3 c)
{
    return (float)(CurlNoiseHash(c) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// 3D バリューノイズ (トリリニア + quintic フェード = C2 連続。中心差分カール用)
float CurlValueNoise(float3 p)
{
    const float3 f = floor(p);
    const int3 c = int3(f);
    float3 t = p - f;
    t = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    const float v000 = CurlNoiseLattice(c + int3(0, 0, 0));
    const float v100 = CurlNoiseLattice(c + int3(1, 0, 0));
    const float v010 = CurlNoiseLattice(c + int3(0, 1, 0));
    const float v110 = CurlNoiseLattice(c + int3(1, 1, 0));
    const float v001 = CurlNoiseLattice(c + int3(0, 0, 1));
    const float v101 = CurlNoiseLattice(c + int3(1, 0, 1));
    const float v011 = CurlNoiseLattice(c + int3(0, 1, 1));
    const float v111 = CurlNoiseLattice(c + int3(1, 1, 1));
    const float x00 = v000 + (v100 - v000) * t.x;
    const float x10 = v010 + (v110 - v010) * t.x;
    const float x01 = v001 + (v101 - v001) * t.x;
    const float x11 = v011 + (v111 - v011) * t.x;
    const float y0 = x00 + (x10 - x00) * t.y;
    const float y1 = x01 + (x11 - x01) * t.y;
    return y0 + (y1 - y0) * t.z;
}

// ポテンシャル場の 3 相 (オフセット違いで相関を切る。定数は C++ 側と一致)
float CurlPotX(float3 p) { return CurlValueNoise(p); }
float CurlPotY(float3 p) { return CurlValueNoise(p + float3(31.416f, 17.923f, 43.651f)); }
float CurlPotZ(float3 p) { return CurlValueNoise(p + float3(-47.317f, 61.139f, -21.744f)); }

// カールノイズ場。p = 粒子位置 × noiseFrequency、t = noiseTime × noiseSpeed。
// 時間は斜めドリフト (場全体の平行移動) として注入 — 軸沿いだと格子の縞が流れて見える
float3 EvalCurlNoise(float3 p, float t)
{
    const float3 q = p + t * float3(1.0f, 0.35f, 0.71f);
    const float e = 0.25f; // kCurlNoiseEps
    const float inv2e = 1.0f / (2.0f * e);
    const float dFzDy = (CurlPotZ(q + float3(0, e, 0)) - CurlPotZ(q - float3(0, e, 0))) * inv2e;
    const float dFyDz = (CurlPotY(q + float3(0, 0, e)) - CurlPotY(q - float3(0, 0, e))) * inv2e;
    const float dFxDz = (CurlPotX(q + float3(0, 0, e)) - CurlPotX(q - float3(0, 0, e))) * inv2e;
    const float dFzDx = (CurlPotZ(q + float3(e, 0, 0)) - CurlPotZ(q - float3(e, 0, 0))) * inv2e;
    const float dFyDx = (CurlPotY(q + float3(e, 0, 0)) - CurlPotY(q - float3(e, 0, 0))) * inv2e;
    const float dFxDy = (CurlPotX(q + float3(0, e, 0)) - CurlPotX(q - float3(0, e, 0))) * inv2e;
    return float3(dFzDy - dFyDz, dFxDz - dFzDx, dFyDx - dFxDy);
}

[numthreads(256, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint aliveCount = gCounts[1];
    if (tid.x >= aliveCount) {
        return;
    }
    const uint slot = gAliveIn[tid.x];
    GpuParticle p = gPool[slot];

    const float dt = gGravityWind.w;
    const float turb = gParams.y;
    // CPU 実装 (CpuParticleBackend::SimulateScalar) と同じ演算列。
    // M61d: turbulenceMode=1 は渦の代わりに位置ベースのカールノイズ場 (CPU 側と同式)
    float3 accel;
    if (gParams4.x > 0.5f) {
        accel = gGravityWind.xyz
              + turb * EvalCurlNoise(p.pos * gParams4.y, gParams4.w * gParams4.z);
    } else {
        accel = gGravityWind.xyz + turb * float3(-p.vel.z, 0.0f, p.vel.x);
    }
    p.vel += accel * dt;
    p.pos += p.vel * dt;
    p.life -= dt;

    // ---- 深度バッファ衝突 (M42e、GPU 限定の見た目効果) ----
    // 前フレーム深度に投影して貫通していたら反射。画面外/空 (depth=1) は素通し。
    // C++ ミラー: ParticleCurves.h の ParticleClipToUv / ReflectWithRestitution (selftest 対象)
    if (gCollParams.x > 0.5f) {
        const float4 clip = mul(float4(p.pos, 1.0f), gCollViewProj);
        if (clip.w > 0.0f) {
            const float2 ndc = clip.xy / clip.w;
            const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            if (all(uv >= 0.0f) && all(uv <= 1.0f)) {
                const int2 maxPix = int2(gCollScreen.xy) - 2;
                const int2 pix = clamp(int2(uv * gCollScreen.xy), int2(0, 0), maxPix);
                const float d0 = gDepth.Load(int3(pix, 0));
                if (d0 < 1.0f) {
                    const float sceneZ = LinearizeDepth(d0, gCollScreen.z, gCollScreen.w);
                    const float pen = clip.w - sceneZ; // >0 = 表面より奥
                    // 貫通が (0, 粒子サイズ + thickness) 内のときだけ反射 (奥の遠景は素通し)
                    if (pen > 0.0f && pen < p.size0 + gCollParams.z) {
                        const float3 p0 = CollReconstructWorld(pix, d0);
                        const float3 px1 = CollReconstructWorld(
                            pix + int2(1, 0), gDepth.Load(int3(pix + int2(1, 0), 0)));
                        const float3 py1 = CollReconstructWorld(
                            pix + int2(0, 1), gDepth.Load(int3(pix + int2(0, 1), 0)));
                        float3 n = normalize(cross(py1 - p0, px1 - p0));
                        // 巻き方に依存せず「速度と逆向き」に揃える (3 タップ法線の符号安定化)
                        if (dot(n, p.vel) > 0.0f) {
                            n = -n;
                        }
                        p.vel = reflect(p.vel, n) * gCollParams.y;
                        p.pos += n * pen; // 表面外へ押し戻し
                    }
                }
            }
        }
    }

    if (p.life <= 0.0f) {
        gDeadList.Append(slot);
        return;
    }
    gPool[slot] = p;
    const uint outIndex = gAliveOut.IncrementCounter();
    gAliveOut[outIndex] = slot;
}
