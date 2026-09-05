// M67: ReSTIR 反射 (時空間サンプル再利用) の数学。reservoir の表現・統合・解決だけを持ち、
// テクスチャも定数バッファも触らない (配管は rt_refl.cs.hlsl / rt_refl_restir_spatial.cs.hlsl)。
// C++ 側 src/Engine/Renderer/RayTracing/RtMath.h の同名関数の写しで、
// **式は 1 文字も変えずに両方更新すること** (selftest が CPU 側だけを固定できる根拠)。
//
// 推定対象は現行 (M46h) と同じ「VNDF 方向の入射放射輝度の期待値」なので出力の次元は
// 変わらない = 合成側は不変。M = 1 (再利用なし) で RtRestirResolve が Ls をそのまま返すのが
// 「ReSTIR off = 現行とビット一致」の数学的な根拠。
//
// ★`!(x > 0)` の形で書いてある判定は CPU 側 (/fp:precise) では NaN も弾くが、
//   fxc は既定で IEEE 厳密ではない (D3DCOMPILE_IEEE_STRICTNESS を渡していない) ので
//   GPU 側で NaN が抜ける可能性は残る — **NaN を作らないのは呼び出し側の責任**。

#ifndef MYE_RT_RESTIR_COMMON_INCLUDED
#define MYE_RT_RESTIR_COMMON_INCLUDED

#ifndef MYE_RT_COMMON_INCLUDED
#error "rt_restir_common.hlsli は rt_common.hlsli を include した後に include すること"
#endif

// 空間再利用のタップ数の上限 (= [loop] の静的上限)。クラス表の taps はこれ以下。
// C++ の kRtRestirMaxTaps と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_RESTIR_MAX_TAPS 8

// 空間タップの回転角ハッシュの第 3 成分。**フレーム番号を混ぜない** (spec §4.3)。
// C++ の kRtRestirTapSeed と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_RESTIR_TAP_SEED 23

// target function を評価するときの alpha の下限。C++ の kRtRestirAlphaMin と同値。
// ★規則 9 は整数しか比べられないのでここだけは目視同期 — 変えたら両方直すこと
#define MYE_RT_RESTIR_ALPHA_MIN 1e-3f

// 輝度 (Rec.709)。rt_temporal / rt_variance / rt_atrous が各自持っている同名関数と同一式で、
// 同じ翻訳単位に 2 つ来ると再定義エラーになるのでガードで包む
// (共通ヘッダへ括り出すのは M67 の範囲外。括り出す側も同じガードを使うこと)
#ifndef MYE_RT_LUMINANCE_DEFINED
#define MYE_RT_LUMINANCE_DEFINED
float RtLuminance(float3 c)
{
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}
#endif

// reservoir が保持する 1 サンプルと統計。3 枚のテクスチャに詰めて運ぶ (RtReservoirPack)。
// 受け側の情報 (geom / rpos) は reservoir ではなく別テクスチャで持つ —
// 「どこから借りたか」は再利用の判定にしか使わず、サンプルそのものではないため
struct RtReservoir {
    float3 xs;  // first hit のワールド座標 (スカイは方向ベクトル)
    float W;    // = wSum / (M * pHat(y))
    float3 Ls;  // ヒット点から出た放射輝度 (バウンス込み)
    float M;    // 統合したサンプル数 (0 = 空)
    float3 ns;  // ヒット点の法線 (**ゼロ = スカイのセンチネル**)
    int cls;    // ReflectionClass (空 = -1 = 範囲外 = 表示は黒)
};

// 空の reservoir。cls は **-1 (範囲外)** — 4 (Default) にすると
// デバッグ表示で「何も入っていない画素」と「クラス 4 の物体」が同じ灰色になる
RtReservoir RtReservoirEmpty()
{
    RtReservoir r;
    r.xs = float3(0.0f, 0.0f, 0.0f);
    r.W = 0.0f;
    r.Ls = float3(0.0f, 0.0f, 0.0f);
    r.M = 0.0f;
    r.ns = float3(0.0f, 0.0f, 0.0f);
    r.cls = -1;
    return r;
}

// テクスチャ 3 枚への詰め方 (spec §4.2 の表)。
//   pos = R32G32B32A32 (xs は fp32 が要る — 半精度だと遠景で Jacobian の d² が狂う)
//   rad = R16G16B16A16 (M は 32 までなので半精度で厳密)
//   nrm = R16G16B16A16 (cls は小さい整数なので半精度で厳密)
void RtReservoirPack(RtReservoir r, out float4 pos, out float4 rad, out float4 nrm)
{
    pos = float4(r.xs, r.W);
    rad = float4(r.Ls, r.M);
    nrm = float4(r.ns, (float)r.cls);
}

RtReservoir RtReservoirUnpack(float4 pos, float4 rad, float4 nrm)
{
    RtReservoir r;
    r.xs = pos.xyz;
    r.W = pos.w;
    r.Ls = rad.rgb;
    r.M = rad.w;
    r.ns = nrm.xyz;
    r.cls = (int)round(nrm.w); // 負のセンチネル (-1) を切り捨てで 0 にしないため round
    return r;
}

// GGX VNDF サンプリング (RtGgxVndf) の pdf。**立体角 (方向 l) に対する密度**で、
// Heitz 2018 の可視法線分布 D_vis(h) に半ベクトル → 反射方向のヤコビアン
// 1/(4 (v·h)) を掛けたもの: G1(v) * D(h) / (4 (n·v))。
//   n = 面法線 / v = 面 → カメラ / l = 面 → 反射先 / alpha = roughness²
// ★半球で積分すると 1 ではなく「1 − 下半球へ抜けた分」になる (alpha=0.36 で約 0.885) —
//   VNDF は半ベクトル側で正規化されており、反射後に地平線の下へ回った分は
//   pdf の定義域から外れるため。**RtMath.h の RtGgxVndfPdf と同一式**
float RtGgxVndfPdf(float3 n, float3 v, float3 l, float alpha)
{
    const float kPi = 3.14159265358979f;
    const float a = max(alpha, MYE_RT_RESTIR_ALPHA_MIN);
    const float ndotv = dot(n, v);
    const float ndotl = dot(n, l);
    if (ndotl <= 0.0f || ndotv <= 1e-6f) {
        return 0.0f; // 面の裏 / 視線が面と平行 (G1/(4 n·v) が 0/0 になる)
    }
    const float3 hv = v + l;
    const float hlen = length(hv);
    if (hlen <= 1e-8f) {
        return 0.0f; // v と l が正反対 (半ベクトルが定義できない)
    }
    const float ndoth = saturate(dot(n, hv) / hlen);
    const float a2 = a * a;
    const float dd = ndoth * ndoth * (a2 - 1.0f) + 1.0f;
    const float ggxD = a2 / (kPi * dd * dd);
    const float g1 = 2.0f * ndotv / (ndotv + sqrt(a2 + (1.0f - a2) * ndotv * ndotv));
    return g1 * ggxD / (4.0f * ndotv);
}

// ReSTIR の target function p̂_q(y)。「この受け側画素にとってこのサンプルがどれだけ
// 効くか」を 1 本のスカラーで表す = 再利用の重み付けの基準。
// 輝度 × VNDF pdf にしてあるので、初期サンプル (ソース pdf = VNDF) では
// w = p̂/p = lum(Ls) に約分される = M=1 で現行と一致する形になる。
// **RtMath.h の RtRestirTargetPdf と同一式**
float RtRestirTargetPdf(float3 Ls, float3 L, float3 V, float3 N, float alpha)
{
    const float lum = RtLuminance(Ls);
    if (lum <= 0.0f) {
        return 0.0f; // 真っ黒なサンプルは誰の役にも立たない (借りても絵が変わらない)
    }
    return lum * RtGgxVndfPdf(N, V, L, alpha);
}

// unbiased contribution weight W = wSum / (M * p̂(y))。**テクスチャへ書き戻すのはこの W**
// (wSum ではなく) — 統合の式 (RtReservoirMerge) が教科書形 `p̂ * W * M * J` のままになる。
// **RtMath.h の RtRestirWeight と同一式**
float RtRestirWeight(float wSum, float M, float pHat)
{
    if (M <= 0.0f || pHat <= 0.0f) {
        return 0.0f;
    }
    return wSum / (M * pHat);
}

// streaming RIS の 1 手。重み w の候補を確率 w/wSum で採用し、M は**重みに関わらず**進める。
// ★ここは**自画素のサンプル専用**の入口 — 真っ黒 (lum = 0 → w = 0) でも「1 本撃った」事実は
//   変わらないので M = 1 になる。**ここに w > 0 のゲートを足さないこと**
//   (足すと黒い画素の M が 0 に落ち、resolve が 0 を返し続ける)。
//   借りてきた候補を「候補から外す」判定は RtReservoirMerge の仕事。
// rnd は [0,1)。**RtMath.h の RtReservoirUpdate と同一式**
bool RtReservoirUpdate(inout RtReservoir r, inout float wSum, float3 xs, float3 ns, float3 Ls,
                       int cls, float mInc, float w, float rnd)
{
    r.M += mInc;
    if (!(w > 0.0f)) {
        return false; // 0 と NaN をまとめて弾く (NaN を足すと wSum が二度と戻らない)
    }
    wSum += w;
    if (rnd < w / wSum) {
        r.xs = xs;
        r.ns = ns;
        r.Ls = Ls;
        r.cls = cls;
        return true;
    }
    return false;
}

// 候補 reservoir を 1 つ統合する (temporal / spatial 共通)。
//   w = p̂_q(y') * W' * min(M', mCap) * J
// mCap はクラス別の M 上限 (kRtReflClassTable)、J は受け側が変わったぶんの Jacobian、
// jMax は棄却の閾値 (CB の gRsJacobianMax。関数内の定数にすると temporal だけ緩められない)。
// ★**候補から外したものは M にも数えない** (spec §4.2「M を数える規則」)。外すのは
//   空 reservoir / J が範囲外 / **有効重み w が 0 または非有限** (p̂=0 = ローブ外・受け側の
//   半球外・真っ黒、W'=0) の 3 経路。教科書の biased 変種は p̂=0 でも M を足すが、それだと
//   W = wSum/(M·p̂) が縮んで**暗化**する — 粗さ 0.10 の鏡面 (α=0.01、ローブ幅 ≈ 1°) では
//   半径 8px の候補の大半が p̂ ≈ 0 なので、鏡面パッチが目に見えて暗くなる。数えない側の
//   偏りは「わずかに明るい / 分散が減らない」= 鏡面のディテールを守る向き。
//   **自画素の初期サンプルは別扱い** (RtReservoirUpdate を直接呼ぶので w=0 でも M=1)。
// **RtMath.h の RtReservoirMerge と同一式**
bool RtReservoirMerge(inout RtReservoir r, inout float wSum, RtReservoir cand, float pHatAtQ,
                      float mCap, float J, float jMax, float rnd)
{
    if (!(cand.M > 0.0f)) {
        return false; // 空 reservoir は候補にならない
    }
    if (!(J >= 1.0f / jMax && J <= jMax)) {
        return false; // NaN もここで落ちる (比較が両方 false になる)
    }
    const float mInc = min(cand.M, mCap);
    const float w = pHatAtQ * cand.W * mInc * J;
    // 有効重みが 0 / 非有限なら候補から外す (M も wSum も動かさない)。
    // ★`isfinite()` ではなく上限との比較で書く — fxc は /Gis 抜きだと
    //   「値が無限になることは無い」前提で isfinite() を消しうる (警告 X3577) ので、
    //   **GPU でも実際に実行される形**にそろえる。NaN も両方の比較に落ちる
    const float kWeightMax = 1e30f;
    if (!(w > 0.0f) || !(w < kWeightMax)) {
        return false;
    }
    return RtReservoirUpdate(r, wSum, cand.xs, cand.ns, cand.Ls, cand.cls, mInc, w, rnd);
}

// 受け側が P_from から P_to へ変わったときの立体角の伸縮 (Jacobian)。
//   J = (cosθ_to / cosθ_from) * (d_from² / d_to²)、d = |xs − P|、cosθ = |ns·(P−xs)/d|
// temporal (P_from = 前フレームの受け側) と spatial (P_from = 近傍画素) で共通。
// スカイ (ns = 0) は無限遠の方向サンプルなので伸縮しない = 1。
// **RtMath.h の RtRestirJacobian と同一式**
float RtRestirJacobian(float3 xs, float3 ns, float3 pFrom, float3 pTo)
{
    const float kEps = 1e-6f;
    if (dot(ns, ns) <= 0.0f) {
        return 1.0f; // スカイのセンチネル
    }
    const float3 df = pFrom - xs;
    const float3 dt = pTo - xs;
    const float lenF = length(df);
    const float lenT = length(dt);
    if (lenF <= kEps || lenT <= kEps) {
        return 1.0f; // 受け側がヒット点に重なった (退化。棄却せず素通しする)
    }
    const float cosF = max(abs(dot(ns, df)) / lenF, kEps); // 0 除算を避ける
    const float cosT = abs(dot(ns, dt)) / lenT;
    return (cosT / cosF) * ((lenF * lenF) / (lenT * lenT));
}

// 書き戻し前に M をクラスの上限へ切り詰める。wSum を同じ比率で縮めるので
// **W = wSum/(M·p̂) は変わらない = 出力される絵は変わらない**。
// 変わるのは「次のフレームがこの reservoir をどれだけ重く扱うか」だけ。
// **RtMath.h の RtRestirClampM と同一式**
void RtRestirClampM(inout RtReservoir r, inout float wSum, float mCap)
{
    if (r.M > mCap && r.M > 0.0f) {
        wSum *= mCap / r.M;
        r.M = mCap;
    }
}

// M67f: 空間再利用の半径を受け側の α で縮める係数 (0〜1)。実効半径 = radius[cls] * これ。
// p̂ (VNDF pdf) のローブ幅は α に比例するので、滑らかな面ほど円板を小さくしないと
// 「ローブの外のタップが増えるだけ」になり、暗化とフリッカーを増やす。
// α <= 0 / ref <= 0 / NaN は 0 (= タップしない) — **`isfinite` を使わずに比較で落とす**
// (fxc は /Gis 抜きだと isfinite を最適化除去しうる。spec §4.5)。
// **RtMath.h の RtRestirRadiusScale と同一式**
float RtRestirRadiusScale(float alpha, float ref)
{
    if (!(alpha > 0.0f) || !(ref > 0.0f)) {
        return 0.0f;
    }
    return min(1.0f, alpha / ref);
}

// M67f: 空間再利用のタップ位置 (半径 radius の円板上の Vogel 螺旋、i 番目 / 全 count 点)。
//   r = radius * sqrt((i + 0.5) / count)  … 面積が均等になる半径の配り方
//   θ = i * 黄金角 + rotation             … 隣り合う点が同じ方角に並ばない回し方
// **rotation を画素ごとに変える**のが要で、全画素が同じ配置だとタップの偏りが
// 「格子状のまだら」として絵に固定される (時間再利用と違い空間再利用は平均されない)。
// count = 0 で呼ばれても 0 除算しないよう max(1) を噛ませる (呼ぶ側がループを回さない
// のが正だが、ここが落ちると画面全体が NaN になるので二重に守る)。
// **RtMath.h の RtRestirVogelTap と同一式**
float2 RtRestirVogelTap(int i, int count, float radius, float rotation)
{
    const float kGoldenAngle = 2.39996323f; // π(3 − √5)
    const float r = radius * sqrt((float(i) + 0.5f) / max((float)count, 1.0f));
    const float a = (float)i * kGoldenAngle + rotation;
    return float2(r * cos(a), r * sin(a));
}

// reservoir → 出力放射輝度。out = Ls * wSum / (M * lum(Ls))。
// ★**Ls を先に掛けない** — scale を先に求めることで M=1 (wSum = lum) のとき
//   scale が厳密に 1.0f になり、Ls がビット単位でそのまま出る。
//   (Ls*wSum)/(M*lum) の順だと丸めが 2 回入って 1 ulp ずれうる。
// **RtMath.h の RtRestirResolve と同一式**
float3 RtRestirResolve(RtReservoir r, float wSum)
{
    if (!(r.M > 0.0f)) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float lum = RtLuminance(r.Ls);
    if (!(lum > 0.0f)) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float scale = wSum / (r.M * lum);
    return r.Ls * scale;
}

#endif // MYE_RT_RESTIR_COMMON_INCLUDED
