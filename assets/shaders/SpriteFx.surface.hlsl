// SpriteFx.surface.hlsl  M94 サンプル: Sprite2D.material 用のスプライト効果 (白フラッシュ・輪郭線・ディゾルブ)。
// 輪郭線はスプライトの矩形の内側にしか描けない (頂点を広げない)。透明な余白のある絵で使うこと。
/*@MyEngineProperties
_FlashColor ("Flash Color", Color) = (1, 1, 1, 1)
[Range(0, 1)] _FlashAmount ("Flash Amount", Float) = 0
_OutlineColor ("Outline Color", Color) = (0, 0, 0, 1)
_OutlineWidth ("Outline Width (px)", Float) = 0
[Range(0, 1)] _Dissolve ("Dissolve", Float) = 0
@*/
#include "MyEngineSurfaceSprite.hlsli"

cbuffer MyEnginePerMaterial
{
    float4 _FlashColor;
    float _FlashAmount;
    float4 _OutlineColor;
    float _OutlineWidth;
    float _Dissolve;
};

// 画素ごとの 0..1 の乱数 (テクスチャの画素単位で同じ値になる = ドットが崩れない)
float DissolveNoise(float2 pixel)
{
    return frac(sin(dot(floor(pixel), float2(12.9898f, 78.233f))) * 43758.5453f);
}

float4 PSMain(MyeSpriteInput i) : SV_Target
{
    const float2 texel = MyeSpriteTexel();
    // サンプルはすべて分岐の外 (勾配を一様に保つ)
    const float4 tex = MyeSampleSprite(i.uv);
    const float2 d = texel * _OutlineWidth;
    const float neighbor = max(max(MyeSampleSprite(i.uv + float2(d.x, 0.0f)).a,
                                   MyeSampleSprite(i.uv - float2(d.x, 0.0f)).a),
                               max(MyeSampleSprite(i.uv + float2(0.0f, d.y)).a,
                                   MyeSampleSprite(i.uv - float2(0.0f, d.y)).a));

    float4 c = tex * i.color;
    c.rgb = lerp(c.rgb, _FlashColor.rgb, saturate(_FlashAmount));

    // 絵の外側で隣に絵がある画素だけが輪郭 (幅 0 なら無効)
    const float outline = saturate(neighbor - tex.a) * step(0.001f, _OutlineWidth);
    c.rgb = lerp(c.rgb, _OutlineColor.rgb, outline);
    c.a = max(c.a, outline * _OutlineColor.a);

    // ノイズが閾値より小さい画素を捨てる (0 のときは何も捨てない)
    clip(DissolveNoise(i.uv / texel) - _Dissolve);
    return c;
}
