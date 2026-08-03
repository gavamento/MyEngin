// M46b: RT デバッグ表示をシーンの上に貼り付けるだけのフルスクリーンパス。
// 解像度は描画先と一致している前提なので Load で 1:1 に読む (サンプラ不要)。

Texture2D gSrc : register(t0);

struct VSOut {
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(gSrc.Load(int3(int2(i.pos.xy), 0)).rgb, 1.0f);
}
