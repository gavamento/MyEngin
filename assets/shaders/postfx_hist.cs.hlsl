// M44b: 自動露出のヒストグラム収集パス。HDR シーンの輝度を log2 域 [-10,+6] の
// 256 bin に量子化して集計する (bin 0 = ほぼ黒のレンジ外専用)。
// 16x16 スレッド、groupshared で局所集計 → グローバルへ InterlockedAdd。
// bin 量子化は PostFxMath.h::BinForLuminance とコメント同期 — 変更時は両方更新。
// CB は PostProcess.cpp の HistCB と同一レイアウト。

cbuffer HistCB : register(b0)
{
    float2 gSize; // シーンのフル解像度
    float2 _hPad;
};

Texture2D gScene : register(t0);              // HDR シーンカラー
RWStructuredBuffer<uint> gHist : register(u0); // 256 bin (毎フレーム 0 クリア済み)

groupshared uint sBins[256];

uint BinForLuminance(float lum)
{
    if (lum <= 1e-6f) {
        return 0;
    }
    const float t = clamp((log2(lum) + 10.0f) / 16.0f, 0.0f, 1.0f);
    return (uint)(t * 254.0f + 1.5f); // 1..255
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    // 16x16 = 256 スレッド = ちょうど 256 bin (gi が 1:1 で初期化/書き戻しを受け持つ)
    sBins[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (dtid.x < (uint)gSize.x && dtid.y < (uint)gSize.y) {
        const float3 c = gScene.Load(int3(dtid.xy, 0)).rgb;
        const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
        InterlockedAdd(sBins[BinForLuminance(lum)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (sBins[gi] > 0) {
        InterlockedAdd(gHist[gi], sBins[gi]);
    }
}
