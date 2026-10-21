// M94: スプライト用サーフェスシェーダー (Sprite2D.material) の作者向け include。
// 作者はこれを #include し、MyEnginePerMaterial と Texture2D を register 無しで宣言して
// float4 PSMain(MyeSpriteInput i) だけを書く (頂点シェーダーはエンジンが生成する)。
// メッシュ用 (MyEngineSurface.hlsli の VSMain/PSMain) と同じファイルに併記してもよい。
//
// ここが供給する識別子はすべてエンジン予約 (再宣言禁止): "MyEngine" で始まる cbuffer、"gMye" / "_mye" で始まる識別子。
//
// 出力 rgb は sRGB 符号化のまま scene RT へ書かれる (内蔵の sprite2d.hlsl と同じ契約)。
// Light2D のライトバッファの乗算 (lit) と、ブレンド (SRC_ALPHA / INV_SRC_ALPHA) はエンジンが PSMain の後で行う。

// PSMain への入力。world はスプライトの画素のワールド座標 (z は描画面)
struct MyeSpriteInput {
    float4 color; // Sprite2D.color (頂点色)
    float2 uv;    // テクスチャ UV (スライスの矩形の中)
    float3 world;
};

// 現在のスプライトのテクスチャとサンプラ (バッチごとに linear / point が入れ替わる)
Texture2D gMyeSpriteTex;
SamplerState gMyeSpriteSamp;

// スプライトのテクスチャを引く。分岐の中で呼ぶと勾配が不定になるので、一様な制御の中で呼ぶこと
float4 MyeSampleSprite(float2 uv)
{
    return gMyeSpriteTex.Sample(gMyeSpriteSamp, uv);
}

// テクスチャ 1 画素の UV 幅 (アトラスの全体寸法の逆数)
float2 MyeSpriteTexel()
{
    uint w;
    uint h;
    gMyeSpriteTex.GetDimensions(w, h);
    return 1.0f / float2(max(w, 1u), max(h, 1u));
}
