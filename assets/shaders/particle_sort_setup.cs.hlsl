// M42追補: GPU alpha ソート ① キー生成 + 間接引数の書き出し
// alive list を (キー, スロット) の配列へ展開し、後続パスの DispatchIndirect 引数を
// **生存数から** 決めて書く。CPU は生存数をリードバックしない設計 (ADR-008) なので、
// 「どれだけ働くか」を決められるのは GPU 側だけ。

#include "particle_gpu_common.hlsli" // GpuParticle 構造体のため (b0 の CB は読まない)
#include "particle_sort_common.hlsli"

StructuredBuffer<GpuParticle> gPoolSRV : register(t0);
StructuredBuffer<uint> gAliveList : register(t1);
Buffer<uint> gCounts : register(t2); // [2] = sim 後の生存数 (CopyStructureCount の宛先)

RWStructuredBuffer<uint> gKeys : register(u0);
RWStructuredBuffer<uint> gIdx : register(u1);
// DispatchIndirect の引数列 (16B ストライド × パス数)。
// ★このバッファは MISC_DRAWINDIRECT_ARGS **と ALLOW_RAW_VIEWS の両方**が要る —
//   後者が無いと raw UAV の生成が E_INVALIDARG で落ちる (WARP で実測)
RWByteAddressBuffer gArgs : register(u2);

[numthreads(256, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint sortCapacity = gSortDims.x;
    const uint alive = gCounts[2];
    const uint pad = ParticleSortPad(alive, sortCapacity);

    const uint i = tid.x;
    if (i < pad) {
        if (i < alive) {
            const uint slot = gAliveList[i];
            // M61g: ローカル空間のプールはワールドへ変換してから測る
            // (CPU 側 CpuParticleBackend::Render の wxScratch_ 経路と同じ意味論)
            float3 p = gPoolSRV[slot].pos;
            if (gSortSpace.x != 0.0f) {
                p = mul(float4(p, 1.0f), gSortEmitterWorld).xyz;
            }
            // ★比較モードの横オフセット (renderOffsetX) は**足さない** — CPU 側も
            //   オフセット前の位置で測っている (全粒子共通の平行移動は順序を変えない)
            gKeys[i] = ParticleSortKeyFromViewZ(dot(p, gSortViewZAxis.xyz));
            gIdx[i] = slot;
        } else {
            // 番兵: キー 0 (= 最小 = 最後尾)。添字は実スロットと衝突しない値にして
            // 番兵どうしの tie-break を安定させる
            gKeys[i] = 0u;
            gIdx[i] = 0xFFFFFFFFu;
        }
    }

    // 間接引数はスレッド 0 が全パスぶん書く。**パス表の並びは C++ 側
    // ParticleSortBuildPasses と 1 対 1** — 順序がずれると全く別のネットワークになる
    if (i == 0u) {
        uint slotIndex = 0;
        gArgs.Store3(0, uint3(pad / MYE_PARTICLE_SORT_BLOCK, 1, 1)); // [0] ブロックソート
        slotIndex = 1;
        for (uint k = MYE_PARTICLE_SORT_BLOCK * 2; k <= sortCapacity; k <<= 1) {
            const uint blockGroups = (k <= pad) ? (pad / MYE_PARTICLE_SORT_BLOCK) : 0u;
            const uint pairs = pad >> 1;
            const uint mergeGroups =
                (k <= pad) ? ((pairs + MYE_PARTICLE_SORT_MERGE_THREADS - 1)
                              / MYE_PARTICLE_SORT_MERGE_THREADS)
                           : 0u;
            for (uint j = k >> 1; j >= MYE_PARTICLE_SORT_BLOCK; j >>= 1) {
                gArgs.Store3(slotIndex * 16, uint3(mergeGroups, 1, 1));
                ++slotIndex;
            }
            gArgs.Store3(slotIndex * 16, uint3(blockGroups, 1, 1)); // ブロック内マージ
            ++slotIndex;
        }
    }
}
