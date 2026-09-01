// M65e: 音響の残光ボリュームを読む式の正本。
//
// 消費者は 5 本 (deferred_light / forward_lit / forward_lit_instanced / forward_skinned /
// forward_terrain)。5 箇所に同じ式を書くと「片方だけ直して床だけ光り方が違う」が必ず
// 起きるので、froxel_common.hlsli / terrain_common.hlsli と同じ流儀で式だけを切り出した。
// **common.hlsli には置けない** — あちらは「register 宣言を持たない」契約で、ここも
// その契約は守る (テクスチャもサンプラも**引数で受け取る**。宣言は消費側が持つ)。
//
// ★ここの復号は **C++ の mye::acoustic::DecodeGlow (AcousticGrid.h) と同一式**。
//   格納側が sqrt を 2 回 (ガンマ 1/4) なので、復号は 4 乗ちょうど。片方だけ直すと
//   selftest は緑のまま絵の明るさだけが静かにずれる。

#ifndef MYE_ACOUSTIC_COMMON_HLSLI
#define MYE_ACOUSTIC_COMMON_HLSLI

// C++ の mye::acoustic::kSrvSlot / kForwardSrvSlot (RenderTypes.h) と一致検査される
// (tools\check_rules.ps1 規則 9)。
// ★t13 は「SSR の予約席だったが SSR (M56d) が光パスの**出力**を読む別パスになったので
//   空いたまま」だった席。M65e がここを取る = 統合契約 予約 2 の更新。取ったことで
//   Deferred の gbSrvs / nullSrvs は **[16] のまま本数が変わらない** —
//   M57d/e が 3 回踏んだ「SRV 剥がし忘れ」を構造的に回避できるのがこの席を選んだ理由。
// ★Forward 側の t8 は本数が 7 -> 8 に増える。**null を張り直す側も 8 にすること** —
//   張り忘れではなく剥がし忘れが実害を出す (次フレームまで生き残る)。
#define MYE_ACOUSTIC_SRV_SLOT 13
#define MYE_ACOUSTIC_FWD_SRV_SLOT 8

// register(tN) を #define 1 個から作る。
// ★「#define と register(t13) が両方ある」形にすると**同じファイルの中で食い違える**ので、
//   数字はこのファイルの 1 箇所だけに置いて連結で組み立てる (C++ 側との照合が
//   check_rules で機械化されているのに、HLSL 内で割れたら意味が無い)
#define MYE_ACOUSTIC_CAT2(a, b) a##b
#define MYE_ACOUSTIC_CAT(a, b) MYE_ACOUSTIC_CAT2(a, b)
#define MYE_ACOUSTIC_REG(n) register(MYE_ACOUSTIC_CAT(t, n))

// 残光ボリュームを 1 点サンプルする。戻り値は**符号化済みの値 [0,1]** (エネルギーではない)。
//
// ★法線方向へ押し出すのが要点。閉セル (壁の中) は波が絶対に訪れないので残光は
//   **開セル側にしかない**。壁面そのものをサンプルすると常に 0 = 「壁が光らない」に
//   なる。push = 0.75 * cellSize は「隣の開セルの中心へ確実に届き、かつ 2 セル先までは
//   行かない」距離。
// ★グリッドの外は**厳密に 0 を返す**。サンプラは CLAMP なので、これを省くと端の値が
//   ボリュームの外へ無限に伸びて「部屋の外の地面がずっと光る」になる。
float AcousticSample(Texture3D tex, SamplerState samp, float3 posW, float3 N, float3 gridMin,
                     float3 invSize, float push)
{
    const float3 uvw = (posW + N * push - gridMin) * invSize;
    if (any(uvw != saturate(uvw))) {
        return 0.0f;
    }
    return tex.SampleLevel(samp, uvw, 0).r;
}

// 符号化値 -> エネルギー。**C++ の DecodeGlow と同一式** (ガンマ 1/4 の逆 = 4 乗)
float AcousticDecode(float t)
{
    const float t2 = t * t;
    return t2 * t2;
}

// 残光の色。
// ★**音色 (tone 0..3) はこのボリュームに入っていない** — 1 セル 1 バイトなので
//   強さしか持てない (v1 の境界。M65d の判断)。代わりに「強い = 近い / 新しい」を
//   暖色、「弱い = 遠い / 古い」を寒色に割り当てる。企画 §3-5 の「記憶の地図」は
//   どのくらい前にどのくらい近くで鳴ったかが読めればよく、材質の別は
//   デバッグ線 (AcousticDebugDraw) とリスナーの鏡 (lastTone) が持っている。
float3 AcousticTint(float t)
{
    // ★変数名に near / far を使わないこと — HLSL の予約語ではないが、
    //   MSVC の legacy マクロと同名で移植時に静かに壊れる系統の名前
    const float3 cold = float3(0.10f, 0.34f, 0.90f); // 遠い / 古い = 冷たい青
    const float3 warm = float3(0.95f, 0.92f, 0.78f); // 近い / 新しい = 白に近い
    return lerp(cold, warm, t * t);
}

// 最終的な加算項。**合成はどの消費者もこの 1 本を通す**。
// 明るさに t*t (= sqrt(エネルギー)) を使うのは、
//   ・エネルギーそのもの (逆二乗) だと数メートル先で真っ黒になり「波が壁を描く」が消える
//   ・符号化値そのもの (エネルギーの 1/4 乗) だと平坦すぎて音源の位置が読めない
// の中間を取ったから。t が 0 のとき厳密に 0 を返す = 未到達セルは 1 命令も足さない。
float3 AcousticRadiance(float t, float intensity)
{
    return AcousticTint(t) * (t * t * intensity);
}

#endif // MYE_ACOUSTIC_COMMON_HLSLI
