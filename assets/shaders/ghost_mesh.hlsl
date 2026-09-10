// 分岐のゴースト (M72i): メッシュを分岐色の半透明で描く (SceneView 専用、エディタの補助描画)。
// 深度テストあり・深度書き込みなし・アルファブレンド。法線で軽く陰影を付けるだけ
// (ライティングもマテリアルも読まない = 「どこに居るか」が読めれば十分)。
// CB は forward_lit / picking と同じ column_major (CPU 側で転置してアップロード) 規約。
// 入力は MeshVertex の先頭 2 要素 (POSITION / NORMAL) だけ宣言する — 残りは反射で無視される。

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float4 gLightDir; // xyz = ワールド空間の光の向き (正規化済み、面から光源へ)
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4 gColor; // rgb = 分岐色、a = 不透明度
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 normalW : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(mul(float4(i.pos, 1.0), gWorld), gViewProj);
    o.normalW = normalize(mul(i.normal, (float3x3)gWorld));
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    const float ndl = saturate(dot(normalize(i.normalW), gLightDir.xyz));
    const float shade = 0.55 + 0.45 * ndl;
    return float4(gColor.rgb * shade, gColor.a);
}
