#pragma once
#include <cstdint>

#include <DirectXMath.h>

// ハイブリッド・パストレーシング (M46) の GPU データレイアウト。
// HLSL 側 assets/shaders/rt_common.hlsli の同名構造体とバイト単位で一致させること
// (変更時は両方更新)。全て 16 バイト境界に揃える。
namespace mye {

// トラバーサルスタックの深さ。TLAS/BLAS で同じ値を使う。
// HLSL の MYE_RT_STACK_DEPTH と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtStackDepth = 32;

// 1 レイあたりのノード訪問上限 (TDR 保険)。超えたら miss 扱いで打ち切る。
// HLSL の MYE_RT_MAX_VISIT と一致検査される (規則 9)
constexpr int kRtMaxVisit = 512;

// TLAS の葉あたりインスタンス数 (BLAS 側は MeshColliderLibrary の kLeafTris=8 に従う)
constexpr int kRtTlasLeafSize = 2;

// ---- M46d: テンポラル蓄積 ----

// 履歴長の上限。移動平均の重み下限 = 1/この値 (32 → 約 3% で追従が止まらない)。
// HLSL の MYE_RT_TEMPORAL_MAX_HISTORY と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtTemporalMaxHistory = 32;

// 再投影の妥当性しきい値。CB 経由で HLSL へ渡す (= C++ 側が唯一の出所)。
// 深度はカメラ距離の相対差、法線は cos。どちらかを外れたら履歴を捨てて 1spp に戻す
constexpr float kRtTemporalDepthThreshold = 0.05f;
constexpr float kRtTemporalNormalThreshold = 0.9f;

// ---- M46e: SVGF 空間フィルタ (分散推定 + エッジ停止 A-Trous) ----

// A-Trous のカーネル半径 (5x5 = B3 スプライン)。分散の空間フォールバックも同じ半径。
// HLSL の MYE_RT_ATROUS_RADIUS と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtAtrousRadius = 2;

// A-Trous の反復回数。刻み幅を 1,2,4 と倍化しながら掛ける (GI の推奨は 3)
constexpr int kRtAtrousIterations = 3;

// テンポラルモーメントから分散を取るのに必要な履歴長。
// これ未満は 5x5 の空間推定に落とす (蓄積が浅いと μ,μ² が信用できない)
constexpr float kRtVarianceHistoryMin = 4.0f;

// エッジ停止関数のパラメータ (C++ が唯一の出所 → CB で HLSL へ渡す)。
//   depth  = タップ 1 画素あたりに許す相対深度差 (真の深度勾配を持たないための近似)
//   normal = cos の指数 (大きいほど法線の違いに厳しい)
//   luma   = 推定標準偏差の何倍までを「ノイズ」として均すか
constexpr float kRtAtrousSigmaDepth = 0.02f;
constexpr float kRtAtrousSigmaNormal = 64.0f;
constexpr float kRtAtrousSigmaLuma = 4.0f;

// ---- M46g: RT 影 (太陽コーンサンプル) ----

// 太陽の見かけ半径 (度)。実測値 (視直径 0.53°) の半分。
// 大きいほど半影が広がるが、1spp のノイズも同じだけ増える
constexpr float kRtShadowSunAngleDeg = 0.265f;

// G-Buffer の可視点から二次光線を撃つときの原点オフセット (自己交差回避)。
// ワールド座標が半精度 (R16G16B16A16_FLOAT = 相対誤差 ~5e-4) なので、定数だけでは
// 遠景でアクネが出る。実効値 = max(絶対下限, 相対係数 * 距離)。
// **影 (M46g) と反射 (M46h) で共有** — 面の自己交差はレイの種類によらないため
constexpr float kRtSurfaceEpsMin = 1e-3f;
constexpr float kRtSurfaceEpsRel = 1e-3f;

// 影の空間フィルタ (SVGF のスカラー軽量版) の反復回数。刻み幅は 1 から倍化。
// GI (3 回) より弱いのは、太陽コーンが狭く 1spp のノイズが半影に限られるため
constexpr int kRtShadowFilterIterations = 1;

// ---- M46h: RT 反射 (GGX VNDF 1 本 + IBL フォールバック) ----

// これを超える roughness ではレイを撃たず IBL スペキュラへ完全に委ねる。
// GGX ローブが広がるほど 1spp の分散が跳ね上がる一方、プリフィルタ IBL との
// 見た目の差は縮むので、コストを払う意味が無くなる境界
constexpr float kRtReflMaxRoughness = 0.6f;

// IBL へのフェード開始 roughness。ここから kRtReflMaxRoughness まで smoothstep で
// 混ぜる (段差を作らないため。両者は同じ次元の入射放射輝度なので混色して良い)
constexpr float kRtReflFadeStart = 0.4f;

// 反射のテンポラル履歴長の上限。GI (32) より短いのは、鏡面ほど反射像が
// カメラ運動で大きく動くため — 長く積むとラグ (引きずり) として見える。
// 実効値は HLSL の MYE_RT_TEMPORAL_MAX_HISTORY とのより小さい方
constexpr float kRtReflMaxHistory = 8.0f;

// 反射の A-Trous 反復回数。GI (3 回) より少ないのは、反射像は「本物のディテール」を
// 持つので広く均すと像そのものが溶けるため
constexpr int kRtReflAtrousIterations = 2;

// 反射の輝度エッジ停止 σ。GI (4.0) より厳しくして反射像のディテールを残す
// (分散が高い = ノイズのときだけ均し、収束したら像を保つ)
constexpr float kRtReflSigmaLuma = 1.0f;

// ---- M67: ReflectionClass (反射に「映る側」の品質クラス) ----

// 反射に映る物体の重要度。**受け側 (映す面) ではなくヒット側 (映る物体) に付く**ので、
// 値の出所は Material、GPU へは RtInstance で運び、HLSL はヒット点から引く。
// ReSTIR の再利用 (空間半径 / タップ数 / M 上限) をクラスごとに変えて
// 「主役は保守的に = にじませずゴーストさせず、小物は積極的に再利用」を作る。
// 番号は .mat.json にそのまま整数で載る = **既存の値の意味を変えない** (追加は末尾へ)。
constexpr int kRtReflClassHero = 0;      // 主役 (プレイヤー / ボス)
constexpr int kRtReflClassCharacter = 1; // 人型・敵
constexpr int kRtReflClassVehicle = 2;   // 乗り物
constexpr int kRtReflClassProp = 3;      // 小物
// 中立クラス。**欠損・範囲外はクランプせずここへ落とす** — -1 を 0 (Hero) に丸めると
// 打ち間違いが「最も保守的で最も重いクラス」に化けて静かにコストだけ増える
constexpr int kRtReflClassDefault = 4;
// HLSL の MYE_RT_REFL_CLASS_COUNT と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtReflClassCount = 5;

// ---- M67: ReSTIR 反射 (時空間サンプル再利用) の数学定数 ----
// 数式は assets/shaders/rt_restir_common.hlsli と RtMath.h の 2 か所に**同じ式**で置き、
// RtSelfTest.cpp の TestRestir が CPU 側を固定する。**変更時は必ず両方を同時に直すこと**。

// 空間再利用のタップ数の上限 (= ループの静的上限。クラス表の taps はこれ以下)。
// HLSL の MYE_RT_RESTIR_MAX_TAPS と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtRestirMaxTaps = 8;

// reservoir が持てる M (統合済みサンプル数) の上限。**半精度テクスチャの w に載せる**ので
// 32 までは整数が厳密に表現できる = 書き戻しで値が動かない。クラス表の mCap もこれ以下
constexpr float kRtRestirMaxM = 32.0f;

// target function を評価するときの alpha の下限。alpha = 0 (完全鏡面) の GGX は
// デルタ分布 = pdf が発散するので、ReSTIR の重み比が 0/0 になる。
// **サンプリング側 (RtGgxVndf) は alpha=0 でも鏡面方向へ退化して破綻しない**ため
// クランプするのは pdf の評価だけ。HLSL の MYE_RT_RESTIR_ALPHA_MIN と同値 (規則 9 は
// 整数しか比べられないので、ここだけは目視同期 — 変えたら両方直す)
constexpr float kRtRestirAlphaMin = 1e-3f;

// M67f: 空間再利用の半径を「受け側の α」で縮める基準値。実効半径 =
// radius[cls] * min(1, α / この値)。**滑らかな面ほど円板を小さくする**のが目的。
// p̂ (VNDF pdf) のローブ幅は α に比例するので、α の小さい面ではローブの外のタップが
// 増えるだけ = 候補が p̂ ≈ 0 で全部落ちるか、たまに通った 1 本が重みを独占して荒れる
// (sub-06 round 1 実測: 粗さ 0.10 の鏡面で一様 Prop -5.2% / 一様 Hero -1.9% の暗化と
// フリッカー 5〜7 倍)。基準は「レイを撃つ上限の粗さ」= kRtReflMaxRoughness² なので、
// その粗さでだけ表の半径が等倍になる。CB (gRsRadiusAlphaRef) 経由で HLSL へ渡す
// = C++ が唯一の出所 (チューニング UI が実行中に書き換える)
constexpr float kRtRestirRadiusAlphaRef = kRtReflMaxRoughness * kRtReflMaxRoughness;

// M67f: 空間タップの回転角を決めるハッシュの第 3 成分。**フレーム番号を混ぜない** —
// spatial は reservoir を書き戻さない (履歴は temporal の出力だけ) ので、フレーム間で
// タップ集合を回して脱相関させる必要が無い。回すと候補集合が毎フレーム入れ替わり、
// 採用サンプルの乗り換えがそのままフリッカーになる (sub-06 round 1 実測: 回すと 2 倍)。
// HLSL の MYE_RT_RESTIR_TAP_SEED と一致検査される (tools/check_rules.ps1 規則 9)
constexpr int kRtRestirTapSeed = 23;

// 再利用時の Jacobian の許容範囲 (この逆数〜この値の外は候補ごと棄却する)。
// 幾何が違いすぎる候補を「重みを補正して使う」と、補正係数そのものが分散源になって
// firefly になる。既定値は CB (gRsJacobianMax) 経由で HLSL へ渡す = C++ が唯一の出所
constexpr float kRtRestirJacobianMax = 10.0f;

// BVH ノード (BLAS / TLAS 共通)。
//   内部ノード: left/right = 子ノードの絶対 index (どちらも >= 0)
//   葉:         left = -(start + 1) で負、right = 個数
//               BLAS の葉は三角形の連続範囲、TLAS の葉はインスタンスの連続範囲を指す
struct RtBvhNode {
    DirectX::XMFLOAT3 aabbMin = { 0, 0, 0 };
    int32_t left = -1;
    DirectX::XMFLOAT3 aabbMax = { 0, 0, 0 };
    int32_t right = 0;
};
static_assert(sizeof(RtBvhNode) == 32, "HLSL RtBvhNode と一致させること");

// 三角形 (ローカル空間、Möller-Trumbore 用に辺を前計算済み)
struct RtTri {
    DirectX::XMFLOAT3 p0 = { 0, 0, 0 };
    float pad0 = 0.0f;
    DirectX::XMFLOAT3 e1 = { 0, 0, 0 }; // p1 - p0
    float pad1 = 0.0f;
    DirectX::XMFLOAT3 e2 = { 0, 0, 0 }; // p2 - p0
    float pad2 = 0.0f;
};
static_assert(sizeof(RtTri) == 48, "HLSL RtTri と一致させること");

// 三角形の頂点属性 (最近ヒットが確定してからしか読まないので別バッファに分ける)
struct RtTriAttr {
    DirectX::XMFLOAT4 n0u0 = { 0, 1, 0, 0 }; // xyz = 法線 0, w = u0
    DirectX::XMFLOAT4 n1v0 = { 0, 1, 0, 0 }; // xyz = 法線 1, w = v0
    DirectX::XMFLOAT4 n2u1 = { 0, 1, 0, 0 }; // xyz = 法線 2, w = u1
    DirectX::XMFLOAT4 uvRest = { 0, 0, 0, 0 }; // x = v1, y = u2, z = v2, w = 未使用
};
static_assert(sizeof(RtTriAttr) == 64, "HLSL RtTriAttr と一致させること");

// インスタンス。行列は worldToLocal のみ持つ (行ベクトル規約の 4x3)。
// レイをローカルへ移すとき方向ベクトルを正規化しないので、求まる t は
// ワールド空間のパラメータのまま = インスタンス間で t を直接比較できる。
// 法線をワールドへ戻すときは mul(float3x3(invRow0..2), nLocal) (= nLocal * transpose)
struct RtInstance {
    DirectX::XMFLOAT4 invRow0 = { 1, 0, 0, 0 }; // xyz = worldToLocal の行 0
    DirectX::XMFLOAT4 invRow1 = { 0, 1, 0, 0 };
    DirectX::XMFLOAT4 invRow2 = { 0, 0, 1, 0 };
    DirectX::XMFLOAT4 invRow3 = { 0, 0, 0, 0 }; // xyz = 平行移動成分
    int32_t blasRoot = 0;      // 連結ノード配列における BLAS のルート index
    int32_t materialIndex = 0; // マテリアル配列の index
    // M67: 反射に映る側の品質クラス (kRtReflClass*)。旧 pad0 の枠をそのまま意味付けした
    // ものなのでレイアウトは不変 (static_assert 80 が動かない)。**コメントではなく名前で
    // 縛る** — pad は「誰も読まない」が前提の名前で、読み始めた瞬間に嘘になる
    int32_t reflectionClass = kRtReflClassDefault;
    int32_t pad1 = 0;
};
static_assert(sizeof(RtInstance) == 80, "HLSL RtInstance と一致させること");

// ヒット点のシェーディングに使うマテリアル定数。
// baseColor はリニア (SrgbToLinear 済み)。emissive は M46i まで 0
struct RtMaterial {
    DirectX::XMFLOAT3 baseColor = { 1, 1, 1 };
    float metallic = 0.0f;
    DirectX::XMFLOAT3 emissive = { 0, 0, 0 };
    float roughness = 0.5f;
};
static_assert(sizeof(RtMaterial) == 32, "HLSL RtMaterial と一致させること");

// ---- M67: ReSTIR の再利用パラメータ (GPU バッファのレイアウトではなく、
//      定数バッファと RenderView へ運ぶ POD。定数は上の kRtRestir* 節を参照) ----

// クラスごとの再利用の強さ。**CB の float4 配列 (gRsClass) にそのまま載る**ので
// 16 バイト固定。taps / mCap を float で持つのは HLSL 側の float4 と型を揃えるため
// (int で持つと CB の詰め方が言語間でずれる)
struct RtReflClassParams {
    float radiusPx = 0.0f; // 空間再利用の探索半径 (**内部解像度の画素**)
    float taps = 0.0f;     // 1 画素あたりのタップ数 (0 = 空間再利用しない)
    float mCap = 0.0f;     // 候補として取り込める M の上限 (= 履歴の長さの上限)
    float pad = 0.0f;      // float4 の余り (HLSL 側は .w を読まない)
};
static_assert(sizeof(RtReflClassParams) == 16, "HLSL の gRsClass (float4) と一致させること");

// クラス別の既定値。**向きが元計画の初版と逆で「Hero ほど数字が小さい = 保守的」**。
// 反射に映る主役を遠くの画素から借りると、輪郭がにじみ (空間)、動いたときに
// 残像として引きずる (時間) — 主役ほどそれが目立つので、主役の再利用を絞る。
// 逆に小物は多少にじんでも気付かれないので、思い切って借りてノイズを消す。
// **ここが唯一の出所** (UI のスライダも「既定に戻す」でこの表へ戻る)。
// ★M67h (S5) で **2 軸 × 5 条件 (64 run) を測った**。結論は「**4 行は規則どおり据え置き、
//   Hero の mCap だけユーザー判断で 8 → 16**」。数値と領域の取り方は ADR-016 の
//   「S5 の結論」節。**同じ測定をやり直す前に読むこと**。
//   - 据え置いた 4 行の根拠: mCap を上げるとフリッカーは必ず減る (片側の軸) が、動く反射像の
//     追従が同時に落ちる。規則「フリッカー 20% 以上改善 かつ 追従の低下 0.05 未満」を
//     満たす段差が 1 つも無かった (24→32 は軸 A が 14.0% / 9.6% で 20% にすら届かない)。
//   - **Hero だけは規則も不成立だった** — 追従の低下は規則の指標で +0.083 / +0.056 / +0.053 /
//     +0.042 (4 標本中 3 つが閾値 0.05 超)、動きを分離した対照では 0.010 (= 分解能内)。
//     つまり「遅れない」ではなく「我々の道具では判定できない」。**2026-09-08 にユーザーが
//     フリッカー 35.5〜38.9% の改善 (Ro 3.789 → 2.317 / R 4.343 → 2.802) を採り、
//     決着しなかった遅れのリスクを引き受けて 16 を選んだ**。8 へ戻すなら上の数値がそのまま根拠。
//   - ★**16 にしたことで Hero == Character == Default になった**。spatial が既定 off の間、
//     クラスが選ぶのは mCap だけ (radiusPx / taps はシェーダで 1 タップも使われない) =
//     **出荷構成ではこの 3 クラスが同挙動**。Hero をこれ以上上げると「主役が中立より積極的」に
//     なるので、`RtSelfTest` の `hero.mCap <= 他 4 クラスの最小` で機械的に止めてある
constexpr RtReflClassParams kRtReflClassTable[kRtReflClassCount] = {
    { 2.0f, 2.0f, 16.0f, 0.0f },  // 0 Hero      = 最も保守的 (mCap は M67h でユーザー判断により 8 → 16)
    { 4.0f, 4.0f, 16.0f, 0.0f },  // 1 Character
    { 6.0f, 6.0f, 24.0f, 0.0f },  // 2 Vehicle
    { 12.0f, 8.0f, 32.0f, 0.0f }, // 3 Prop      = 最も積極的
    { 8.0f, 4.0f, 16.0f, 0.0f },  // 4 Default   = 中立 (欠損・範囲外はここへ落ちる)
};
static_assert(kRtReflClassCount == 5,
              "段数を変えたら kRtReflClassTable と RtReflRestirParams の既定も直すこと");

// ReSTIR の実行時パラメータ一式 (RenderView に載り、CB へ写される)。
// **非永続** — チューニング UI (M67f) が実行中に書き換えるだけでプロジェクトには保存しない。
// 既定は上の定数表と M46h の SVGF 設定そのもの = 「既定のまま on にしたら元計画の表で動く」
struct RtReflRestirParams {
    RtReflClassParams classTable[kRtReflClassCount] = {
        kRtReflClassTable[0], kRtReflClassTable[1], kRtReflClassTable[2],
        kRtReflClassTable[3], kRtReflClassTable[4],
    };
    float svgfHistory = kRtReflMaxHistory;              // ReSTIR 後段の SVGF 履歴長
    int atrousIterations = kRtReflAtrousIterations;     // 同 A-Trous 反復回数
    float radiusAlphaRef = kRtRestirRadiusAlphaRef;     // 半径を α で縮める基準 (M67f)
    // 空間再利用の既定。**sub-06 round 2 の計測で 0 に決めた** (spec §7 U7 の規則:
    // 目標帯で temporal 単独より改善すれば on、しなければ off)。実測 (音響デモの床、
    // 粗さ 0.5、--rt-debug 11 = 反射レーンだけ、frame 120/121 のフリッカー):
    //   off 2.943 → temporal 単独 0.181 → spatial on 0.255
    // temporal 単独が最良で、spatial を足すと 1.4 倍に戻る。MIS 重みを持たない
    // biased 合成では近傍の p̂ 比がそのまま重みの分散になるため (unbiased 化は M67 の
    // スコープ外 = spec §3)。**ノブ (UI) と CLI は残す** — 粗い面が主役のシーンでは
    // 効く可能性があり、S5 / M67h で再評価できるようにしておく
    int spatial = 0;                                    // 空間再利用 (0 = temporal のみ)
    int visRay = 0;                                     // 候補の可視レイ (既定 off = 光漏れ許容)
    int classOverride = -1;                             // 全インスタンスのクラス強制 (-1 = off)
};

} // namespace mye
