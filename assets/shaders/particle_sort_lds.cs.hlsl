// M42追補: GPU alpha ソート ③ ブロック内ソート / ブロック内マージ (共有メモリ)
// gSortPass.x == 0 … ブロックソート (k = 2..BLOCK をここで回し切る)
// gSortPass.x != 0 … ブロック内マージ (その k について j = BLOCK/2..1 を回し切る)
//
// **これがハイブリッドの軽い側**。alive が BLOCK 以下なら pad == BLOCK になり、
// 全域マージのパスは間接引数 0 グループで即帰るので、実質このディスパッチ 1 本で終わる。

#include "particle_sort_common.hlsli"

RWStructuredBuffer<uint> gKeys : register(u0);
RWStructuredBuffer<uint> gIdx : register(u1);
Buffer<uint> gCounts : register(t0); // 未使用 (merge と同じバインドで揃えるため宣言のみ)

groupshared uint sKey[MYE_PARTICLE_SORT_BLOCK];
groupshared uint sIdx[MYE_PARTICLE_SORT_BLOCK];

void SortCompareExchange(uint a, uint b, bool dir)
{
    if (ParticleSortBefore(sKey[a], sIdx[a], sKey[b], sIdx[b]) != dir) {
        const uint tk = sKey[a];
        sKey[a] = sKey[b];
        sKey[b] = tk;
        const uint ti = sIdx[a];
        sIdx[a] = sIdx[b];
        sIdx[b] = ti;
    }
}

[numthreads(MYE_PARTICLE_SORT_THREADS, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint base = gid.x * MYE_PARTICLE_SORT_BLOCK;
    const uint t = gtid.x;

    // 2 要素 / スレッドで LDS へ (uint 2 本 × 2048 = 16KB)
    sKey[t] = gKeys[base + t];
    sIdx[t] = gIdx[base + t];
    sKey[t + MYE_PARTICLE_SORT_THREADS] = gKeys[base + t + MYE_PARTICLE_SORT_THREADS];
    sIdx[t + MYE_PARTICLE_SORT_THREADS] = gIdx[base + t + MYE_PARTICLE_SORT_THREADS];
    GroupMemoryBarrierWithGroupSync();

    // 2 つのモードを 1 つのループ入れ子で書く — 分岐の中にバリアを置くと
    // 「varying flow control 内の同期」として fxc に蹴られうるため
    const uint passK = gSortPass.x;
    const uint kBegin = (passK == 0u) ? 2u : passK;
    const uint kEnd = (passK == 0u) ? (uint)MYE_PARTICLE_SORT_BLOCK : passK;
    for (uint k = kBegin; k <= kEnd; k <<= 1) {
        const uint jBegin = (passK == 0u) ? (k >> 1) : ((uint)MYE_PARTICLE_SORT_BLOCK >> 1);
        for (uint j = jBegin; j > 0; j >>= 1) {
            const uint a = ParticleSortPairIndex(t, j);
            // ★向きは**グローバル添字**で決める。k == BLOCK のとき隣り合うブロックが
            //   逆向きに整うのが後段マージの前提で、ローカル添字だと全ブロックが同じ
            //   向きに揃ってネットワークが破綻する (ここを間違えると絵が微妙に乱れるだけで
            //   クラッシュもしないので、selftest のネットワーク照合が唯一の防波堤)
            SortCompareExchange(a, a | j, ((base + a) & k) == 0u);
            GroupMemoryBarrierWithGroupSync();
        }
    }

    gKeys[base + t] = sKey[t];
    gIdx[base + t] = sIdx[t];
    gKeys[base + t + MYE_PARTICLE_SORT_THREADS] = sKey[t + MYE_PARTICLE_SORT_THREADS];
    gIdx[base + t + MYE_PARTICLE_SORT_THREADS] = sIdx[t + MYE_PARTICLE_SORT_THREADS];
}
