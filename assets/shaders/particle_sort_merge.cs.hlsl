// M42追補: GPU alpha ソート ② 全域比較交換 (j >= ブロック長 = LDS に収まらない距離)
// 1 スレッド = 1 ペア。ペアは互いに素なので同期は要らない (パス境界が D3D の暗黙バリア)。

#include "particle_sort_common.hlsli"

RWStructuredBuffer<uint> gKeys : register(u0);
RWStructuredBuffer<uint> gIdx : register(u1);
Buffer<uint> gCounts : register(t0); // [2] = sim 後の生存数 (pad をここから引き直す)

[numthreads(MYE_PARTICLE_SORT_MERGE_THREADS, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint pad = ParticleSortPad(gCounts[2], gSortDims.x);
    const uint pairs = pad >> 1;
    if (tid.x >= pairs) {
        return; // 間接引数は 256 の倍数へ切り上げているので端数を刈る
    }
    const uint k = gSortPass.x;
    const uint j = gSortPass.y;
    const uint a = ParticleSortPairIndex(tid.x, j);
    const uint b = a | j;
    const bool dir = ((a & k) == 0u);

    const uint ka = gKeys[a];
    const uint ia = gIdx[a];
    const uint kb = gKeys[b];
    const uint ib = gIdx[b];
    if (ParticleSortBefore(ka, ia, kb, ib) != dir) {
        gKeys[a] = kb;
        gIdx[a] = ib;
        gKeys[b] = ka;
        gIdx[b] = ia;
    }
}
