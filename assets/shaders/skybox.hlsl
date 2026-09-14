// スカイボックス (M29d、gradient)。フルスクリーン三角形を z=1 (far) で描き、
// 深度 LESS_EQUAL でジオメトリの無いピクセルだけを塗る。
// 視線方向は invViewProj で NDC の near/far 2 点を逆射影して求める。
// CB は b3 (b0-b2 はメッシュ描画の PerFrame/PerObject/Material が使用中)。

// M57e: フロクセルのサンプル座標 (register 宣言を持たないヘッダ)
#include "froxel_common.hlsli"

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;
    float4 gHorizonColor;
    float4 gBottomColor;
    // ---- M57e: フロクセル (末尾 append。x=0 = 従来と 1 ビットも変わらない) ----
    // 空は深度を持たないので「グリッド全体ぶん」を引く (FroxelSampleWFar)。
    // ★グリッドより奥の解析フォグは掛けない — 空に ApplyFog が掛かる挙動は M29d 以来
    //   一度も無く、足すと濃霧のとき空が丸ごとフォグ色に潰れる。フロクセル区間ぶんの
    //   段 (= 地表と空の食い違い) だけを消すのが M57e の受け持ち
    float4 gSkyFroxel;       // x = enabled / y = スライス数 / zw = 未使用
    float4 gSkyFroxelScreen; // xy = レンダーターゲット実寸 (px) / zw = 未使用
    // ---- 2026-09-14: 手続きの星空 (末尾 append。x=0 = 従来と 1 ビットも変わらない) ----
    // ★skybox_cubemap.hlsl は宣言しない — 同じバッファの前半だけを読むので、後ろに足した分は見えないだけ
    float4 gStars;     // x = 星のあるセルの割合 / y = 明るさ / z = 瞬きの深さ / w = キューブ 1 面の分割数
    float4 gStarsTime; // x = 瞬きの時刻 (描画通番 / 60) / yzw = 未使用
};

// ★t7 / s2 は **SkyboxPath 自身が張らない** — ホストのパス (ForwardPath / DeferredPath) が
//   フレーム内で既に張っているものをそのまま読む。ここで別のスロットへ張ると、
//   Forward ではスカイの直後に描く半透明メッシュの t1 (CSM) / s0 (異方性 WRAP) を
//   潰してしまう (スカイは不透明と透明の間に入るパスなので、触った SRV が後段へ漏れる)。
//   t7 = フロクセル積分結果 (統合契約 予約 2) / s2 = IBL 用 LINEAR/CLAMP
Texture3D    gFroxelVolume  : register(t7); // M57e
SamplerState gFroxelSampler : register(s2); // LINEAR/CLAMP (ホストがフレーム頭で張る)

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

// 整数ハッシュ (lowbias32)。★sin(dot(...)) 型のハッシュを使わない — 引数が大きいと GPU ごとに
//   精度が変わり、同じ方向の星が機種によって出たり消えたりする
uint StarHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// ハッシュの下位 24bit を [0,1) へ (float の仮数に収まる桁だけ使う = 丸めて 1.0 にならない)
float StarRand(uint x)
{
    return float(StarHash(x) & 0x00FFFFFFU) / 16777216.0f;
}

// 視線方向 dir に見える星の放射輝度 (リニア) を返す。pxCells = 画面 1 px がセル何個分か。
// ★空をキューブの 6 面に分けてセルを切る。緯度経度で切ると極でセルが潰れ、天頂に星が密集する
float3 StarField(float3 dir, float pxCells)
{
    const float3 a = abs(dir);
    uint face = 0U;
    float2 uv = float2(0.0f, 0.0f);
    if (a.x >= a.y && a.x >= a.z) {
        face = (dir.x > 0.0f) ? 0U : 1U;
        uv = dir.zy / a.x;
    } else if (a.y >= a.z) {
        face = (dir.y > 0.0f) ? 2U : 3U;
        uv = dir.xz / a.y;
    } else {
        face = (dir.z > 0.0f) ? 4U : 5U;
        uv = dir.xy / a.z;
    }
    const float cells = gStars.w;
    const float2 p = (uv * 0.5f + 0.5f) * cells;
    // uv がちょうど 1 の画素でセル番号が cells になり、隣の行へ桁あふれしないよう丸める
    const float2 cell = min(floor(p), cells - 1.0f);
    const float2 f = p - cell;
    // 分割数は C++ 側で 1024 までに丸めてあるので、面 6 × 1024 × 1024 が uint に収まる
    const uint id = (face * 1024U + uint(cell.y)) * 1024U + uint(cell.x);
    const uint h = StarHash(id * 0x9E3779B9U + 0x632BE5ABU);
    if (StarRand(h) >= gStars.x) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    // 中心はセルの内側 [0.25, 0.75] に置く = 円がセル境界を跨がない (隣のセルを調べずに済む)
    const float2 c = 0.25f + 0.5f * float2(StarRand(h ^ 0x68BC21EBU), StarRand(h ^ 0x02E5BE93U));
    // 明るさは 6 乗で偏らせる = 暗い星が大半で、明るい星はまれ (実際の夜空の見え方に寄せる)
    const float bright = lerp(0.12f, 1.0f, pow(StarRand(h ^ 0x967A889BU), 6.0f));
    // 半径はセル単位。★画面上 1 px を下回ると視点を回したときに星が点いたり消えたりするので、
    //   1 px の 0.7 倍までは広げ、その分だけ明るさを面積比で割って見た目の総量を保つ
    const float r0 = 0.08f;
    const float r = max(r0, pxCells * 0.7f);
    const float core = saturate(1.0f - length(f - c) / r);
    const float energy = (r0 * r0) / (r * r);
    float twinkle = 1.0f;
    if (gStars.z > 0.0f) {
        const float rate = 0.6f + 1.8f * StarRand(h ^ 0x1B873593U);
        const float phase = 6.2831853f * StarRand(h ^ 0xCC9E2D51U);
        twinkle = 1.0f - gStars.z * (0.5f + 0.5f * sin(gStarsTime.x * rate + phase));
    }
    // 色はわずかに暖色〜寒色へ振る (真っ白の点が並ぶと人工的に見える)
    const float3 tint = lerp(float3(1.0f, 0.86f, 0.72f), float3(0.76f, 0.86f, 1.0f),
                             StarRand(h ^ 0xE6546B64U));
    return tint * (gStars.y * bright * core * core * energy * twinkle);
}

float4 PSMain(VSOut i) : SV_Target
{
    // NDC の far/near 点をワールドへ戻し、視線方向を得る
    float4 pf = mul(float4(i.ndc, 1.0f, 1.0f), gInvViewProj);
    float4 pn = mul(float4(i.ndc, 0.0f, 1.0f), gInvViewProj);
    const float3 dir = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    // ★星の太さに使う画素の大きさは分岐の外で取る (動的分岐の中の勾配命令は未定義)。
    //   キューブ面の uv ではなく連続な dir から測る — 面の継ぎ目で uv が飛び、そこだけ星が膨らむため
    const float pxCells = length(fwidth(dir)) * gStars.w * 0.5f;
    const float t = dir.y;
    float3 c;
    if (t >= 0.0f) {
        c = lerp(gHorizonColor.rgb, gTopColor.rgb, saturate(t * 1.4f));
    } else {
        c = lerp(gHorizonColor.rgb, gBottomColor.rgb, saturate(-t * 1.4f));
    }
    if (gStars.x > 0.0f) {
        // 地平線へ向かって薄める = 大気で星が消える見え方。地平線際の建物の輪郭を星で浮かせない効果もある
        c += StarField(dir, pxCells) * smoothstep(0.0f, 0.3f, t);
    }
    if (gSkyFroxel.x != 0.0f) {
        const float4 v = gFroxelVolume.SampleLevel(
            gFroxelSampler,
            float3(i.pos.xy / gSkyFroxelScreen.xy, FroxelSampleWFar(gSkyFroxel.y)), 0);
        c = c * v.a + v.rgb;
    }
    return float4(c, 1.0f);
}
