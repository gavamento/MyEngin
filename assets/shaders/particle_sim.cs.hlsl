// GPU パーティクル更新: aliveIn を積分し、生存者を aliveOut へ圧縮、死亡者は dead list へ返す

#include "particle_gpu_common.hlsli"

RWStructuredBuffer<GpuParticle> gPool : register(u0);
AppendStructuredBuffer<uint> gDeadList : register(u1);
RWStructuredBuffer<uint> gAliveOut : register(u2); // カウンタ付き
StructuredBuffer<uint> gAliveIn : register(t0);
Buffer<uint> gCounts : register(t1); // [1] = aliveInCount (typed — CopyStructureCount の宛先)
Texture2D<float> gDepth : register(t2); // M42e: 前フレームのシーン深度 (未バインド時は enabled=0)

// M42e: 非線形深度 [0,1] -> ビュー空間 z (particle_render 系と同一式)
float CollLinearizeDepth(float d)
{
    const float n = gCollScreen.z, f = gCollScreen.w;
    return n * f / max(f - d * (f - n), 1e-4f);
}

// M42e: ピクセル + 深度 -> ワールド位置再構成 (法線推定用)
float3 CollReconstructWorld(int2 pix, float d)
{
    const float2 uv = (float2(pix) + 0.5f) / gCollScreen.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 w = mul(float4(ndc, d, 1.0f), gCollInvViewProj);
    return w.xyz / max(abs(w.w), 1e-6f) * sign(w.w);
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
    // CPU 実装 (CpuParticleBackend::SimulateScalar) と同じ演算列
    float3 accel = gGravityWind.xyz + turb * float3(-p.vel.z, 0.0f, p.vel.x);
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
                    const float sceneZ = CollLinearizeDepth(d0);
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
