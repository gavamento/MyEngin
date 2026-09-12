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
// 2026-09-12「描画だけ円」: 見通しビット (Texture3D<uint>、bit s = 波スロット s)。
// C++ の acoustic::kFrontSrvSlot / kFrontForwardSrvSlot と規則 9 で照合される
#define MYE_ACOUSTIC_FRONT_SRV_SLOT 16
#define MYE_ACOUSTIC_FRONT_FWD_SRV_SLOT 9
#define MYE_ACOUSTIC_WAVE_SLOTS 16

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

// 解析的な波面 = 「描画だけ円」(2026-09-12)。戻り値は AcousticSample と同じ**符号化値** [0,1]。
//
// 残光ボリュームの等距離面はチャンファ距離の性質で八角形になる (半径が方位で 8〜10% ずれる)。
// sim はそのままに絵だけ真円にするため、CPU (AcousticField::UpdateFrontPreview) が
//   ・mask  : セルごとの見通しビット (bit s = 波スロット s の円をこのセルに描いてよい)
//   ・waves : waves[2s] = (原点 xyz, 半径 R [m]) / waves[2s+1] = (振幅, 上限距離 [m], tick/m, 名残 tick)
//   ・front : x = 残光の 1 tick 残存率 / y = cellSize [m] / z = 有効 / w = 波の数
// を渡す。明るさは残光と**同じ式** (逆二乗 → ガンマ 1/4 の符号化値、同じ速さで薄れる)
// なので、円の内側では残光の値とほぼ一致し、max 合成しても継ぎ目が出ない。
// ★見通しビットが立たないセル (角を曲がって届く所) は 0 を返す = 残光の形のまま
// ★距離は**押し出す前の posW** で測る (壁面の距離は面そのものの位置)。マスクだけ押し出した
//   位置で引く (閉セルにはビットが立たないので、AcousticSample と同じ理由)
float AcousticFront(Texture3D<uint> mask, float3 posW, float3 N, float3 gridMin, float3 invSize,
                    float push, float4 front, float4 waves[MYE_ACOUSTIC_WAVE_SLOTS * 2])
{
    const float3 uvw = (posW + N * push - gridMin) * invSize;
    if (any(uvw != saturate(uvw))) {
        return 0.0f;
    }
    uint3 dim;
    mask.GetDimensions(dim.x, dim.y, dim.z);
    const int3 cell = min(int3(uvw * float3(dim)), int3(dim) - 1);
    const uint bits = mask.Load(int4(cell, 0)).r;
    if (bits == 0u) {
        return 0.0f;
    }
    const float cellSize = front.y;
    const int count = (int)front.w;
    float best = 0.0f;
    [loop]
    for (int s = 0; s < count; ++s) {
        if ((bits & (1u << (uint)s)) == 0u) {
            continue;
        }
        const float4 a = waves[s * 2];
        const float4 b = waves[s * 2 + 1];
        const float d = length(posW - a.xyz);
        if (d >= a.w || d >= b.y) {
            continue; // 波面の外 / 到達上限の外 (EnergyAt と同じく厳密 0)
        }
        // 残光と同じ EnergyAt: minD = 1 セルで 1、以遠は逆二乗
        const float ratio = cellSize / max(d, cellSize);
        const float e = b.x * ratio * ratio;
        float t = sqrt(sqrt(saturate(e)));
        // 波面の縁は半セルで立ち上げる (セルの階段ではなく円で切る)
        t *= saturate((a.w - d) / (0.5f * cellSize));
        // 残光と同じ減衰: 波面が過ぎてからの tick 数 (+ 消えた波の名残)。
        // ★残光の減衰は uint8 の**切り捨て**乗算 (AcousticField::DecayVisual) なので、
        //   keep^age の指数関数ではない — v*(1-keep) < 1 になった先は毎 tick ちょうど 1 ずつ
        //   減る**線形**の尾になる (keep 0.995 なら v <= 200 から。255 → 0 が 227 tick)。
        //   指数のままにすると残光が消えた後も円が薄く残り、部屋全体が持ち上がる (実測)。
        //   同じ 2 段階 (乗算で v* まで → 1/tick) をここで再現する
        const float keep = front.x;
        const float age = (a.w - d) * b.z + b.w;
        const float vStar = 1.0f / max(1.0f - keep, 1e-4f);
        float v = t * 255.0f;
        float n = age;
        if (v > vStar) {
            const float n1 = min(n, log(vStar / v) / log(keep));
            v = v * exp(n1 * log(keep)) - 0.5f * n1; // 乗算 + 切り捨てぶん (平均 0.5/tick)
            n -= n1;
        }
        v -= n;
        t = max(v, 0.0f) / 255.0f;
        best = max(best, t);
    }
    return best;
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

// 面の色を混ぜる帯 (符号化値 t で指定)。t がこの帯より上 = 強い (近い / 新しい) 残光だけに
// 面の albedo が乗る。
// ★加算の距離色だけだと、暗闇では面の元の色が 0 なので**床材の色が一切出ない**
//   (三校で「床の色が分からず、床材の境目 = 足音の変わり目が読めない」になった)。
// ★遠い / 古い残光 (t < Lo) は常に距離色のまま — 企画 §3-4「材質は踏むか光を置くまで
//   分からない」を遠くでは崩さない。近くだけ色が乗るのは §3-3「呼吸で直下の床材だけは
//   分かる」とも噛み合う
// ★帯の位置は t = amp^(1/4) * sqrt(cellSize / d) (EnergyAt の逆二乗 + ガンマ 1/4) から決めた。
//   cellSize 0.5 で amp 1 の足音なら「2m 以内は面の色 / 5.6m から先は距離色」、
//   amp 0.3 の忍び足なら「1.1m 以内 / 3m から先」。0.55〜0.80 では音源の真下 1m 未満にしか
//   色が乗らず、音響デモで変化が 648 px しか無かった
static const float kAcousticAlbedoLo = 0.30f;
static const float kAcousticAlbedoHi = 0.50f;
// albedo に掛ける倍率。床材の albedo は 0.2〜0.6 程度なので、そのままだと距離色 (白 ≒ 0.9)
// より暗く沈んで「色は分かるが形が見えない」になる。2 倍で距離色と同程度の明るさに寄せる
static const float kAcousticAlbedoGain = 2.0f;

// 最終的な加算項。**合成はどの消費者もこの 1 本を通す**。
// 明るさに t*t (= sqrt(エネルギー)) を使うのは、
//   ・エネルギーそのもの (逆二乗) だと数メートル先で真っ黒になり「波が壁を描く」が消える
//   ・符号化値そのもの (エネルギーの 1/4 乗) だと平坦すぎて音源の位置が読めない
// の中間を取ったから。t が 0 のとき厳密に 0 を返す = 未到達セルは 1 命令も足さない。
// albedoMix = 0 なら lerp の重みが厳密に 0 = 距離色そのもの (従来とビット恒等)。
float3 AcousticRadiance(float t, float intensity, float3 albedo, float albedoMix)
{
    const float w = albedoMix * smoothstep(kAcousticAlbedoLo, kAcousticAlbedoHi, t);
    const float3 tint = lerp(AcousticTint(t), albedo * kAcousticAlbedoGain, w);
    return tint * (t * t * intensity);
}

#endif // MYE_ACOUSTIC_COMMON_HLSLI
