// M56c: HZB (min-Z 階層深度) の 1 段ぶんの縮小。
// 入力 1 枚 → 出力 1 枚の**汎用の min リダクション**で、同じシェーダを
//   段 0: シーン深度 (R24_UNORM_X8) → ピラミッド mip 0   (src == dst = 素通しコピー)
//   段 n: ピラミッド mip n-1 → mip n                     (2x2、奇数辺は 3x2 / 3x3)
// の両方に使い回す。src/dst の寸法だけで畳み方が決まる書き方にしてあるので、
// 「コピー専用のシェーダ」も「奇数辺かどうかの分岐」も要らない。
//
// ★奇数辺 (例 15 → 7) を素朴に 2x2 で畳むと最後の 1 行 / 1 列がどの出力にも入らない。
//   そこに壁があるのに HZB が「空」と答える = SSR の光線が壁を貫通する、という形でしか
//   現れず絵から原因を追えない。区間を切り上げ側へ広げて取りこぼしをゼロにしてある
//   (min なので重なって読んでも結果は変わらない)。
//   **CPU ミラー: HzbPass.h::HzbReduceSpan — 変更時は両方更新** (HzbSelfTest が検証)。
//
// 深度は**非線形のデバイス深度**のまま持つ (線形化は消費者の仕事 = common.hlsli の
// LinearizeDepth)。min が「手前」を意味するのは、このリポジトリが reversed-Z を使わず
// 深度を 1.0 (最遠) でクリアしているから。

// **C++ 側の kHzbThreadGroupSize (HzbPass.h) と必ず一致させること** — 食い違うと
// 画面の右端・下端だけが縮小されずに前フレームの値が残る。規則 9 が静的に検査する
#define MYE_HZB_TG 8

cbuffer HzbCB : register(b0)
{
    uint2 gHzbSrcSize; // 入力の寸法 (texel)
    uint2 gHzbDstSize; // 出力の寸法 (texel)
};

Texture2D<float> gHzbSrc : register(t0);
RWTexture2D<float> gHzbDst : register(u0);

// 出力テクセル i が読むべき入力の区間 [begin, end] (両端含む)
void HzbSpan(uint dstIndex, uint srcExtent, uint dstExtent, out uint begin, out uint end)
{
    begin = (dstIndex * srcExtent) / dstExtent;
    // ceil((i+1)*src/dst) - 1。分子は必ず dst 以上なので商は 1 以上 = uint で underflow しない
    end = ((dstIndex + 1) * srcExtent + dstExtent - 1) / dstExtent - 1;
    end = min(end, srcExtent - 1);
    end = max(end, begin);
}

[numthreads(MYE_HZB_TG, MYE_HZB_TG, 1)]
void CSMain(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= gHzbDstSize.x || dt.y >= gHzbDstSize.y) {
        return; // 端数スレッド (ディスパッチは 8 の倍数へ切り上げている)
    }

    uint x0, x1, y0, y1;
    HzbSpan(dt.x, gHzbSrcSize.x, gHzbDstSize.x, x0, x1);
    HzbSpan(dt.y, gHzbSrcSize.y, gHzbDstSize.y, y0, y1);

    // 初期値 1.0 = 最遠。深度は [0,1] なので、どの入力を読んでも必ず下回るか等しい
    float m = 1.0f;
    for (uint y = y0; y <= y1; ++y) {
        for (uint x = x0; x <= x1; ++x) {
            m = min(m, gHzbSrc.Load(int3(int(x), int(y), 0)));
        }
    }
    gHzbDst[dt.xy] = m;
}
