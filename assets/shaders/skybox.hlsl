// スカイボックス (M29d、gradient)。フルスクリーン三角形を z=1 (far) で描き、
// 深度 LESS_EQUAL でジオメトリの無いピクセルだけを塗る。
// 視線方向は invViewProj で NDC の near/far 2 点を逆射影して求める。
// CB は b3 (b0-b2 はメッシュ描画の PerFrame/PerObject/Material が使用中)。

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;
    float4 gHorizonColor;
    float4 gBottomColor;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // フルスクリーン三角形 (deferred_light.hlsl と同じ頂点列)。z=1 = far 平面
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    o.ndc = corners[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // NDC の far/near 点をワールドへ戻し、視線方向を得る
    float4 pf = mul(float4(i.ndc, 1.0f, 1.0f), gInvViewProj);
    float4 pn = mul(float4(i.ndc, 0.0f, 1.0f), gInvViewProj);
    const float3 dir = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    const float t = dir.y;
    float3 c;
    if (t >= 0.0f) {
        c = lerp(gHorizonColor.rgb, gTopColor.rgb, saturate(t * 1.4f));
    } else {
        c = lerp(gHorizonColor.rgb, gBottomColor.rgb, saturate(-t * 1.4f));
    }
    return float4(c, 1.0f);
}
