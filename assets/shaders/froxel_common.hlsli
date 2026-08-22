// M57c: フロクセルの 3 パス (注入 / テンポラル / 積分) が共有する幾何と積分の式。
//
// **計画外の追加ファイル。** M57b までは注入 1 パスしか無かったので式はその 1 本に
// 直接書いてあったが、M57c でスライス深度と逆射影を読む者が 3 本に増える。
// 3 箇所に同じ式を書くと「片方だけ直して絵が 1 スライスずれる」が必ず起きるので、
// 地形 (M58d の terrain_common.hlsli) と同じ流儀で本体だけを切り出した。
// **common.hlsli には置けない** — あちらは「register 宣言を持たない」契約で、
// froxel は CB のフィールド名まで含めて共有したいわけではなく式だけを共有したいため。
//
// ★ここの関数はすべて **C++ の mye::froxel (src\Engine\Renderer\RenderTypes.h) と同一式**。
//   片方だけ直すと、RenderSelfTest は緑のままグリッドだけが静かにずれる。

#ifndef MYE_FROXEL_COMMON_HLSLI
#define MYE_FROXEL_COMMON_HLSLI

// C++ の mye::froxel::kGroupSize (RenderTypes.h) と一致検査される
// (tools\check_rules.ps1 規則 9)。XY だけをタイルにしているのは、注入も積分も
// 「同じ (x,y) の Z 列」を扱うため (積分は 1 スレッドが 1 列を手前から舐めるので Z を割れない)
#define MYE_FROXEL_GROUP 8

// スライス境界の view 深度 (指数分布)。slice = 0 → nearZ、slice = count → farZ。
// **C++ の froxel::SliceToViewDepth と同一式。**
// max() が要るのは、比が正だと分かっていても fxc が pow に X3571 を出すため
// (ビルド警告には出ず、実行時コンパイルのたびログへ流れる = M57b で踏んだ)
float FroxelSliceDepth(float slice, float sliceCount, float nearZ, float farZ)
{
    const float ratio = max(farZ / nearZ, 1e-4f);
    return nearZ * pow(ratio, slice / sliceCount);
}

// 上の逆関数。view 深度 → スライス座標 (小数)。範囲外もそのまま外挿して返す
// (クランプは呼ぶ側の責任 — グリッド外を最遠スライスへ丸めると空が濁る)。
// **C++ の froxel::ViewDepthToSlice と同一式**
float FroxelDepthToSlice(float depth, float sliceCount, float nearZ, float farZ)
{
    const float d = max(depth, 1e-6f);
    return sliceCount * log(d / nearZ) / log(max(farZ / nearZ, 1.0001f));
}

// セル (uv, view 深度) → view 空間座標。行ベクトル規約 (clip = view * proj、clip.w = view.z)
// の透視射影を逆に解いたもので、逆行列を掛けるより素直かつ深度の非線形性を経由しない。
// **正射影では成り立たない** (視錐台がスラブになる) ので、呼ぶ側が透視射影を保証すること
float3 FroxelViewPos(float2 uv, float viewZ, float invProj00, float invProj11)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return float3(ndc.x * viewZ * invProj00, ndc.y * viewZ * invProj11, viewZ);
}

// スライス 1 枚ぶんの透過率 T = e^{-σ_t·d} (Beer-Lambert)。
// **C++ の froxel::SliceTransmittance と同一式**
float FroxelSliceTransmittance(float sigmaT, float thickness)
{
    return exp(-max(sigmaT, 0.0f) * max(thickness, 0.0f));
}

// スライス 1 枚を均質と見なしたときの散乱の解析積分 ∫₀^d e^{-σ_t·s} ds = (1-e^{-σ_t·d})/σ_t。
// ★厚み d をそのまま掛けてはいけない — 1 スライス内の自己遮蔽が消え、濃い霧ほど
//   明るくなるという逆向きの絵になる (Hillaire 2015)。σ_t → 0 の極限はちょうど d。
// **C++ の froxel::IntegratedSliceScatter と同一式**
float FroxelIntegratedSliceScatter(float sigmaT, float thickness)
{
    const float s = max(sigmaT, 0.0f);
    const float d = max(thickness, 0.0f);
    if (s < 1e-5f) {
        return d;
    }
    return (1.0f - exp(-s * d)) / s;
}

#endif // MYE_FROXEL_COMMON_HLSLI
