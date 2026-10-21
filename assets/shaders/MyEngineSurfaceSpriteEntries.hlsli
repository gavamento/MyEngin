// M94: スプライト用サーフェスシェーダーの生成エントリ本体 (作者には見えない)。
// ShaderManager::CompileSurfaceProgram が「作者ソース + このファイル」を 1 つの翻訳単位として
// MyeVSSprite2D / MyePSSprite2D を個別にコンパイルする (MyEngineSurfaceSprite.hlsli を include した作者ソースだけ)。
// 作者側の PSMain(MyeSpriteInput) を指す — 規約と違うとここでコンパイルエラーになる。
//
// 頂点は CPU 側でワールド座標に展開済み (sprite2d.hlsl と同じ VB) なので、VS は ViewProj を掛けるだけ。

cbuffer MyEngineSprite2D
{
    float4x4 gMyeSpriteViewProj;
    float4 gMyeSpriteLit; // x = 1 のときライトバッファを乗算する, yz = 1 / ライトバッファの寸法
};

Texture2D gMyeLightBuffer;
SamplerState gMyeLightSamp; // 線形 Clamp

struct MyeSpriteVSIn {
    float3 pos : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct MyeSpriteVSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
    float3 world : TEXCOORD1;
};

MyeSpriteVSOut MyeVSSprite2D(MyeSpriteVSIn v)
{
    MyeSpriteVSOut o;
    o.pos = mul(float4(v.pos, 1.0f), gMyeSpriteViewProj);
    o.uv = v.uv;
    o.color = v.color;
    o.world = v.pos;
    return o;
}

float4 MyePSSprite2D(MyeSpriteVSOut i) : SV_Target
{
    MyeSpriteInput si;
    si.color = i.color;
    si.uv = i.uv;
    si.world = i.world;
    float4 c = PSMain(si);
    if (gMyeSpriteLit.x > 0.5f) {
        // SV_POSITION は PS ではピクセル座標 → ライトバッファの UV
        c.rgb *= gMyeLightBuffer.SampleLevel(gMyeLightSamp, i.pos.xy * gMyeSpriteLit.yz, 0).rgb;
    }
    return c;
}
