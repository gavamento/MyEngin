// M42追補: GPU alpha ソートの共有定義 (setup / lds / merge の 3 本が読む)
//
// ★C++ ミラー: ParticleCurves.h の kParticleSort* 定数と ParticleSort* 群。
//   添字演算はコメント同期のみの一致 — 片方だけ直すと「重なり順だけが微妙に違う」形で
//   静かに壊れる (絵でしか気づけない)。**変更は必ず両方同時に。**
//   ブロック長とスレッド数の 3 本だけは check_rules.ps1 規則 9 が機械照合する。
//
// ★b0 は particle_gpu_common.hlsli の GpuParticleCB が占有している (setup CS は
//   GpuParticle 構造体のためにあちらを include する) ので、ソート側は b1 に置く。
//   3 本とも b1 で統一すること — 片方だけ番号を変えると CB が丸ごと 0 で静かに壊れる。

#define MYE_PARTICLE_SORT_BLOCK         2048
#define MYE_PARTICLE_SORT_THREADS       1024
#define MYE_PARTICLE_SORT_MERGE_THREADS 256

cbuffer ParticleSortCB : register(b1)
{
    float4   gSortViewZAxis;    // xyz = view 行列の第 3 列 (_13, _23, _33)。w = 予約
    uint4    gSortDims;         // x = sortCapacity, yzw = 予約
    uint4    gSortPass;         // x = k (0 = ブロックソート), y = j, zw = 予約
    float4x4 gSortEmitterWorld; // transpose 済み (simulationSpace=1 のときだけ読む)
    float4   gSortSpace;        // x = simulationSpace, yzw = 予約
};

// v 以上の最小の 2 冪 (C++ 側 ParticleSortNextPow2 と同値)
uint ParticleSortNextPow2(uint v)
{
    uint p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

// 生存数 → 実際に回す範囲 (2 冪)。**実仕事量を GPU 側が決める**の正体。
// setup が k > pad のパスへグループ数 0 を書き、merge/lds はここでスレッドを刈る。
// C++ 側 ParticleSortPadFor と同値
uint ParticleSortPad(uint alive, uint sortCapacity)
{
    uint pad = ParticleSortNextPow2(alive);
    pad = max(pad, (uint)MYE_PARTICLE_SORT_BLOCK);
    return min(pad, sortCapacity);
}

// float を順序保存 uint へ (整数比較で float の大小がそのまま出る)。
// IEEE754 は正値のビット列が単調増加・負値が逆順なので符号で場合分けする定番の変換。
// 粒子位置は有限なので NaN は入らない。C++ 側 ParticleSortKeyFromViewZ と同値
uint ParticleSortKeyFromViewZ(float z)
{
    const uint b = asuint(z);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

// 「a が先 (手前に描かれる)」= viewZ 降順 → 同値は添字昇順。
// CpuParticleBackend の比較子 `za != zb ? za > zb : a < b` と同じ規則
bool ParticleSortBefore(uint keyA, uint idxA, uint keyB, uint idxB)
{
    return (keyA != keyB) ? (keyA > keyB) : (idxA < idxB);
}

// 圧縮スレッド添字 t → 比較ペアの手前側 i (相手は i | j)。j は 2 冪。
// 下位 (j-1) ビットはそのまま、それより上を 1 ビット押し上げて j のビットを空ける
// = 全スレッドがちょうど 1 ペアを担当する (半分が遊ぶ i^j 方式より速い)
uint ParticleSortPairIndex(uint t, uint j)
{
    return (t & (j - 1)) | ((t & ~(j - 1)) << 1);
}
