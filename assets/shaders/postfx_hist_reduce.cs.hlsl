// M44b: 自動露出の縮約パス。ヒストグラムの加重平均輝度から目標露出
// (0.18 / avgLum を [aeMin, aeMax] に clamp) を求め、指数平滑で gExposure[0] を更新する。
// bin 0 (ほぼ黒 — 空/レターボックス等) は平均から除外し、有効画素ゼロなら前回値を保持。
// GPU 内で完結しリードバック無し = 決定論 (WorldHash) に非干渉。
// aeInstant=1 (リプレイ検証/スクショ時に EngineLoop が設定) は 1 フレームで収束させ、
// 決定的スクショを成立させる。逆量子化は PostFxMath.h::LumForBin とコメント同期。
// CB は PostProcess.cpp の AeReduceCB と同一レイアウト。

cbuffer AeReduceCB : register(b0)
{
    float gAeSpeed;   // 適応速度 (1/s)
    float gAeMin;     // 露出倍率の下限
    float gAeMax;     // 上限
    int   gAeInstant; // 1 = 1 フレーム収束 (決定的スクショ用)
};

StructuredBuffer<uint> gHist : register(t0);       // 256 bin
RWStructuredBuffer<float> gExposure : register(u0); // [0] = 現在の露出倍率 (初期値 1)

float LumForBin(uint bin)
{
    return exp2(((float)bin - 1.0f) / 254.0f * 16.0f - 10.0f);
}

[numthreads(1, 1, 1)]
void CSMain()
{
    // 256 bin の直列ループ (フレームに 1 回だけなので単一スレッドで十分)
    float sum = 0.0f;
    float total = 0.0f;
    for (uint i = 1; i < 256; ++i) {
        const float count = (float)gHist[i];
        sum += count * LumForBin(i);
        total += count;
    }
    const float prev = gExposure[0];
    float target = prev;
    if (total > 0.0f) {
        const float avg = sum / total;
        target = clamp(0.18f / max(avg, 1e-4f), gAeMin, gAeMax);
    }
    // 名目 60Hz dt の指数平滑 (v1: 適応の実速度は描画レート依存 — 見た目効果のみ)
    const float a = (gAeInstant != 0) ? 1.0f : (1.0f - exp(-gAeSpeed / 60.0f));
    gExposure[0] = lerp(prev, target, a);
}
