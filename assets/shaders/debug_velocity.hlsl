// M55c: GBuffer RT4 (velocity、R16G16_FLOAT) の可視化。
// velocity を読む本番の消費者は M55d (TAA) / M55e (モーションブラー v2) / M55f (RT) まで
// 存在しないので、「本当に書けているか」を人間が確かめられる唯一の口がこれになる。
// 既定 off (RenderView::velocityDebug == 0) のときは呼ばれない = 絵は 1 ビットも変わらない。
//
// 表示規約: 灰 (0.5,0.5,0.5) = 静止。赤寄り = 右 / 青寄り = 左 (u 速度)、
//           緑寄り = 下 (v 速度)。
// ★感度は **UV でなくピクセル/フレーム**で決める。UV の変位は 1e-3 のオーダーで、
//   「倍率いくつ」だと解像度が変わるたびに絵の意味が変わってしまう。
//   gVelPxRange [px/frame] で振り切る、と決めておけば数値の読み方が固定される。

cbuffer VelocityDebugCB : register(b0)
{
    float2 gVelDstSize; // 描画先の解像度 (px)
    float  gVelPxRange; // この速度 [px/frame] で色が振り切る
    float  _velDbgPad;
};

Texture2D<float2> gVelocity : register(t0);

struct VSOut
{
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
    // velocity は GBuffer と同解像度なので Load (サンプラ不要 = 補間で嘘をつかない)
    const int2 px = clamp(int2(i.pos.xy), int2(0, 0), int2(gVelDstSize) - int2(1, 1));
    // UV 速度 → ピクセル/フレーム → 表示レンジ [-1,1]
    const float2 v = gVelocity.Load(int3(px, 0)) * gVelDstSize / max(gVelPxRange, 1e-3f);
    const float3 c = float3(saturate(0.5f + v.x), saturate(0.5f + v.y), saturate(0.5f - v.x));
    return float4(c, 1.0f);
}
