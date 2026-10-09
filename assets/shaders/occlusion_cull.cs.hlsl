// GPU オクルージョンカリングの判定 + 詰め込み (Deferred の GBuffer 不透明)。
// 1 グループ = 1 描画コマンド (インスタンス run か単発)。グループ内でコマンドの項目を走査して
// 描く項目だけを元の並びのまま詰め (prefix sum。アトミックで詰めると順序が実行ごとに変わり、
// 同深度で重なるインスタンスの絵が揺れる)、DrawIndexedInstancedIndirect の引数を書く。
//
// モード:
//   0 SELECT : フェーズ 1。前フレームに可視だった項目 (と常に描く項目) を詰める
//   1 TEST   : フェーズ 2。HZB (max-Z) で全項目を判定し、フェーズ 1 で描いていない可視の項目を詰める。
//              判定結果を可視ビットへ書く (次フレームの SELECT が読む)
//   2 PASS   : HZB が作れなかったとき。判定せず全項目を可視とみなす (描き漏らさない)
//
// **CPU ミラー: OcclusionMath.h の ProjectAabb / IsOccluded — 変更時は両方更新** (OcclusionSelfTest が検証)。
// 深度は非線形のデバイス深度 (小さいほど手前)。箱の最前面が HZB の max-Z (範囲で最も奥) より
// さらに奥なら、範囲のどの画素にも手前に隠す物があるので隠れている。

// **C++ 側の mye::occlusion の同名定数 (OcclusionMath.h) と必ず一致させること**。規則 9 が整数分を検査する
#define MYE_OCC_TG 64
#define MYE_OCC_BIAS_STEPS 32
#define MYE_OCC_MARGIN_PX 1
#define MYE_OCC_MIN_W 0.001f

#define MYE_OCC_MODE_SELECT 0
#define MYE_OCC_MODE_TEST 1
#define MYE_OCC_MODE_PASS 2

#define MYE_OCC_FLAG_ALWAYS 1u // 判定せず常に描く (フェーズ 1)
#define MYE_OCC_FLAG_NEW 2u    // 前フレームの可視ビットが無効 (新規 / 前フレームは視錐台の外)
#define MYE_OCC_FLAG_NOSLOT 4u // 可視ビットを持たない (書かない)

// 統計 (gStats のバイト位置)
#define MYE_OCC_STAT_PHASE1 0u
#define MYE_OCC_STAT_PHASE2 4u
#define MYE_OCC_STAT_OCCLUDED 8u

cbuffer OcclusionCB : register(b0)
{
    float4x4 gViewProj; // transpose(view * proj)。GBuffer と同じジッタ込み
    uint2 gScreen;      // HZB 0 段目の寸法
    uint gMipCount;
    uint gMode;
    uint gCmdCount;
    uint gArgsByteBase; // 20 * コマンド数 * フェーズ
    uint gRemapBase;    // フェーズ * ワールド行列数
    uint gPad;
};

struct OccItem
{
    float3 bmin;
    uint slot; // 可視ビットの添字 (エンティティ番号)
    float3 bmax;
    uint flags;
};

struct OccCmd
{
    uint firstItem;
    uint itemCount;
    uint instanceBase; // run の world 行列の開始位置 (インスタンス化しない項目では未使用)
    uint isInstanced;
    uint indexCount;
    uint startIndex;
    uint pad0;
    uint pad1;
};

StructuredBuffer<OccItem> gItems : register(t0);
StructuredBuffer<OccCmd> gCmds : register(t1);
Texture2D<float> gHzb : register(t2);

RWByteAddressBuffer gArgs : register(u0);
RWStructuredBuffer<uint> gRemap : register(u1);
RWStructuredBuffer<uint> gVis : register(u2);
RWByteAddressBuffer gStats : register(u3);

groupshared uint gsScan[MYE_OCC_TG];

// true = 可視 (描く)。OcclusionMath.h の ProjectAabb + IsOccluded と同じ式
bool TestVisible(float3 bmin, float3 bmax)
{
    float minSx = 1.0e30f;
    float minSy = 1.0e30f;
    float maxSx = -1.0e30f;
    float maxSy = -1.0e30f;
    float minZ = 1.0e30f;
    [unroll]
    for (uint c = 0; c < 8; ++c) {
        const float3 p = float3((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                                (c & 4) ? bmax.z : bmin.z);
        const float4 clip = mul(float4(p, 1.0f), gViewProj);
        if (clip.w <= MYE_OCC_MIN_W) {
            return true; // ニア面をまたぐ (または背後)
        }
        const float invW = 1.0f / clip.w;
        const float sx = (clip.x * invW * 0.5f + 0.5f) * (float)gScreen.x;
        const float sy = (0.5f - clip.y * invW * 0.5f) * (float)gScreen.y;
        minSx = min(minSx, sx);
        maxSx = max(maxSx, sx);
        minSy = min(minSy, sy);
        maxSy = max(maxSy, sy);
        minZ = min(minZ, clip.z * invW);
    }
    if (minZ < 0.0f) {
        return true;
    }
    const float x0 = minSx - (float)MYE_OCC_MARGIN_PX;
    const float y0 = minSy - (float)MYE_OCC_MARGIN_PX;
    const float x1 = maxSx + (float)MYE_OCC_MARGIN_PX;
    const float y1 = maxSy + (float)MYE_OCC_MARGIN_PX;
    if (x0 < 0.0f || y0 < 0.0f || x1 > (float)gScreen.x || y1 > (float)gScreen.y) {
        return true; // 画面からはみ出す
    }

    const float size = max(x1 - x0, y1 - y0);
    uint mip = 0;
    while (mip < gMipCount - 1 && (float)(1u << mip) < size) {
        ++mip;
    }
    const int wm = (int)max(1u, gScreen.x >> mip);
    const int hm = (int)max(1u, gScreen.y >> mip);
    const int tx0 = clamp((int)floor(x0 * (float)wm / (float)gScreen.x), 0, wm - 1);
    const int ty0 = clamp((int)floor(y0 * (float)hm / (float)gScreen.y), 0, hm - 1);
    const int tx1 = clamp((int)floor(x1 * (float)wm / (float)gScreen.x), 0, wm - 1);
    const int ty1 = clamp((int)floor(y1 * (float)hm / (float)gScreen.y), 0, hm - 1);

    float maxZ = 0.0f;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            maxZ = max(maxZ, gHzb.Load(int3(tx, ty, (int)mip)));
        }
    }
    return !(minZ > maxZ + (float)MYE_OCC_BIAS_STEPS / 16777215.0f);
}

[numthreads(MYE_OCC_TG, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint gtid : SV_GroupIndex)
{
    // コマンド数が 65535 を超えても動くよう 2 次元で撃つ (CPU 側 Dispatch と対)
    const uint cmdIdx = gid.x + gid.y * 65535u;
    if (cmdIdx >= gCmdCount) {
        return; // グループ全体で一様
    }
    const OccCmd cmd = gCmds[cmdIdx];

    uint total = 0;
    uint occluded = 0;
    for (uint chunk = 0; chunk < cmd.itemCount; chunk += MYE_OCC_TG) {
        const uint i = chunk + gtid;
        uint keep = 0;
        if (i < cmd.itemCount) {
            const OccItem it = gItems[cmd.firstItem + i];
            const bool always = (it.flags & MYE_OCC_FLAG_ALWAYS) != 0;
            // フェーズ 1 で描いた項目か (SELECT と TEST で同じ式)
            const bool drawnInPhase1 =
                always || (((it.flags & MYE_OCC_FLAG_NEW) == 0) && gVis[it.slot] != 0);
            if (gMode == MYE_OCC_MODE_SELECT) {
                keep = drawnInPhase1 ? 1u : 0u;
            } else {
                bool visible = true;
                if (!always && gMode == MYE_OCC_MODE_TEST) {
                    visible = TestVisible(it.bmin, it.bmax);
                    occluded += visible ? 0u : 1u;
                }
                if ((it.flags & MYE_OCC_FLAG_NOSLOT) == 0) {
                    gVis[it.slot] = visible ? 1u : 0u;
                }
                keep = (visible && !drawnInPhase1) ? 1u : 0u;
            }
        }

        // グループ内の包括的 prefix sum (Hillis-Steele)。並びを保ったまま詰めるため
        gsScan[gtid] = keep;
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint offset = 1; offset < MYE_OCC_TG; offset <<= 1) {
            const uint add = (gtid >= offset) ? gsScan[gtid - offset] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gsScan[gtid] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (keep != 0 && cmd.isInstanced != 0) {
            gRemap[gRemapBase + cmd.instanceBase + total + gsScan[gtid] - 1u] = cmd.instanceBase + i;
        }
        const uint chunkTotal = gsScan[MYE_OCC_TG - 1];
        GroupMemoryBarrierWithGroupSync(); // 次の反復が gsScan を書き換える前に全員が読み終える
        total += chunkTotal;
    }

    if (gtid == 0) {
        const uint o = gArgsByteBase + cmdIdx * 20u;
        gArgs.Store(o, cmd.indexCount);
        gArgs.Store(o + 4u, total);
        gArgs.Store(o + 8u, cmd.startIndex);
        gArgs.Store(o + 12u, 0u);
        gArgs.Store(o + 16u, 0u);
        if (total != 0) {
            gStats.InterlockedAdd((gMode == MYE_OCC_MODE_SELECT) ? MYE_OCC_STAT_PHASE1
                                                                  : MYE_OCC_STAT_PHASE2,
                                  total);
        }
    }
    if (occluded != 0) {
        gStats.InterlockedAdd(MYE_OCC_STAT_OCCLUDED, occluded);
    }
}
