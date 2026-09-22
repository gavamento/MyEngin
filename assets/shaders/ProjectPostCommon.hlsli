// ProjectPostCommon.hlsli  プロジェクトポストシェーダ共通バインド (M78b)
// ユーザーが作る *.post.hlsl は先頭でこのファイルを include すれば
// SceneColor / Depth / サンプラ等にアクセスできる。
// エンジン予約スロットをここで宣言し、ユーザー宣言と名前空間を分ける。
//
// Properties の 2D 既定名 (white/gray/black/bump): docs/project-shaders-tex2d-defaults.md

// ---- エンジン共通 CB (b0) ----
cbuffer MyEnginePostFrame : register(b0)
{
    float gScreenW;     // フル解像度 幅 (px)
    float gScreenH;     // フル解像度 高さ (px)
    float gInvScreenW;  // 1 / gScreenW
    float gInvScreenH;  // 1 / gScreenH
};

// ---- エンジン予約テクスチャ (t0, t1) ----
// ユーザー 2D テクスチャは t2 以降を使う (ShaderManager が割当)
Texture2D gSceneColor : register(t0); // HDR シーンカラー (R16G16B16A16F、BeforeTonemap)
                                      // または LDR (R8G8B8A8_UNORM、AfterTonemap)
Texture2D gSceneDepth : register(t1); // シーン深度 (R32_TYPELESS/FLOAT)。null 時は未使用

// ---- エンジン予約サンプラ (s0) ----
SamplerState gLinearClamp : register(s0);

// ---- フルスクリーン三角形 VS (既存 postfx と同じ規約) ----
struct ProjectPostVSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

ProjectPostVSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    ProjectPostVSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv  = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

// ---- ユーティリティ: UV → SceneColor サンプル ----
float4 SampleSceneColor(float2 uv)
{
    return gSceneColor.Sample(gLinearClamp, uv);
}
