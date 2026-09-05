// M46h: RT 反射。G-Buffer の可視点から GGX の可視法線分布 (VNDF) に沿って反射レイを
// 1 本撃ち、その方向の入射放射輝度をそのまま出力する (GI と同じ demodulated 形式)。
//
// 出力の次元は IBL のプリフィルタ済み放射輝度 (split-sum 第 1 項) と揃えてあるので、
// 合成側では IBL の `pre` をこの値へ差し替えて (F0*brdf.x + brdf.y) を掛けるだけでよい。
// 両者が同じ次元なので roughness による混色 (RtReflWeight) が段差を作らない。
//
// roughness > gRfMaxRoughness ではレイを撃たない — GGX ローブが広がるほど 1spp の
// 分散が跳ね上がる一方、プリフィルタ IBL との見た目の差は縮むため。合成側が
// 同じしきい値でフォールバックする (撃たなかった画素の値は使われない)。
//
// M67d: ReSTIR (時空間サンプル再利用) の初期 reservoir もここで作る。
// **gRsOn == 0 の経路は M67d 以前と同じ計算をして同じ値を書く** — 分岐は CB の
// スカラー 1 個で完全に uniform、reservoir 側の UAV (u1-u5) は C++ が張りもしない。
// golden `demo_render_rtrefl` (tol=0) がそのビット一致を機械証明している。
//
// M67e: 時間再利用。初期 reservoir を作った直後に「前フレームの同じ材質点の reservoir」
// (組 A = t11-t15) を 1 つだけ統合する。**別ディスパッチにしない** = 近傍を読まないので
// 同期が要らず、reservoir を SRV と UAV で同時に張らずに済む (ユーザー判断 U5)。
// 妥当性の判定は SVGF の蓄積 (rt_temporal) と**同じ関数** (rt_reproject.hlsli) —
// 2 か所に写経すると「片方だけ直して履歴条件がずれる」形で静かに壊れる。

#include "rt_common.hlsli"
// M67c: ReSTIR の数学。M67d からは実際に呼んでいる。
// ★rt_restir_common → rt_reproject の順で include すること — RtLuminance は
//   両方が `MYE_RT_LUMINANCE_DEFINED` ガードで定義していて、先に来た方が勝つ。
//   ReSTIR の重みは RtMath.h の CPU ミラーと同じ式 (rt_restir_common 側) で
//   評価しないと、selftest が固定した値と GPU の値がずれる
#include "rt_restir_common.hlsli"
#include "rt_reproject.hlsli"
// ReSTIR の CB (b3)。**宣言は rt_restir_cb.hlsli の 1 か所だけ** (spatial と共有)
#include "rt_restir_cb.hlsli"

cbuffer RtReflCB : register(b2)
{
    float2 gRfOutSize; // 反射バッファの解像度 (内部解像度)
    float2 gRfGbSize;  // G-Buffer の解像度 (フル)
    float3 gRfCameraPos;
    float gRfTMax;
    uint gRfFrameIndex;    // フレーム毎に乱数列をずらす (テンポラル蓄積で平均される)
    int gRfBounces;
    float gRfMaxRoughness; // これを超えたら撃たない (RtTypes.h が出所)
    float gRfEpsMin;       // レイ原点のオフセット (絶対下限)
    float gRfEpsRel;       // 同 (距離への比例係数)
    float3 gRfPad;
};

Texture2D gRfNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gRfPosition : register(t8); // GBuffer ワールド座標
Texture2D gRfMark : register(t9);     // GBuffer アルベド (a = ジオメトリ有りマーク)
Texture2D gRfMaterial : register(t10); // GBuffer マテリアル (r = metallic, g = roughness)
// M67d: 前フレームの reservoir (組 A)。M67e から temporal 統合が実際に読んでいる
Texture2D gRsPrevPos : register(t11);
Texture2D gRsPrevRad : register(t12);
Texture2D gRsPrevNrm : register(t13);
Texture2D gRsPrevGeom : register(t14);
Texture2D gRsPrevRpos : register(t15);
// M67e: GBuffer RT4 = 画面速度 (今 UV − 前 UV、フル解像度)。rt_temporal の t7 と同じもの。
// gRsUseVelocity == 0 のときは null が張られる (Load は 0 を返すが、そもそも読まない)。
// ★t16 は **ReSTIR が on のフレームでしか張られない** — off 経路は gRsOn == 0 で
//   ここへ来る前に return するので、張られていない SRV を読むことは無い
Texture2D<float2> gRsGbVelocity : register(t16);

RWTexture2D<float4> gRfOut : register(u0);
// M67d: 今フレームの reservoir (組 B)。**gRsOn == 0 のときは C++ 側が張らない**ので、
// 書き込みは黙って捨てられる — が、そもそも uniform 分岐で 1 回も実行されない
RWTexture2D<float4> gRsOutPos : register(u1);
RWTexture2D<float4> gRsOutRad : register(u2);
RWTexture2D<float4> gRsOutNrm : register(u3);
RWTexture2D<float4> gRsOutGeom : register(u4);
RWTexture2D<float4> gRsOutRpos : register(u5);

// 「この画素にサンプルは無い」を書く (ジオメトリ無し / roughness 超過)。
// M = 0 / cls = -1 なので、次段はこれを候補にしないし、デバッグ 12/14 は黒になる。
// geom.w = 0 は RtReprojectValid が必ず落とす値 (= 履歴としても使われない)
void RtRestirWriteEmpty(uint2 px)
{
    float4 pos, rad, nrm;
    RtReservoirPack(RtReservoirEmpty(), pos, rad, nrm);
    gRsOutPos[px] = pos;
    gRsOutRad[px] = rad;
    gRsOutNrm[px] = nrm;
    gRsOutGeom[px] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    gRsOutRpos[px] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gRfOutSize.x || tid.y >= (uint)gRfOutSize.y) {
        return;
    }
    // 内部解像度のピクセル中心を G-Buffer の座標へ写す (rt_gi.cs.hlsl と同じ写像)
    const float2 uv = (float2(tid.xy) + 0.5f) / gRfOutSize;
    const int3 gp = int3(int2(uv * gRfGbSize), 0);
    if (gRfMark.Load(gp).a < 0.5f) {
        gRfOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ジオメトリ無し (空)
        if (gRsOn != 0) {
            RtRestirWriteEmpty(tid.xy);
        }
        return;
    }
    const float roughness = gRfMaterial.Load(gp).g;
    if (roughness > gRfMaxRoughness) {
        gRfOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); // 合成側で IBL へフォールバック
        if (gRsOn != 0) {
            RtRestirWriteEmpty(tid.xy);
        }
        return;
    }
    const float3 N = normalize(gRfNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gRfPosition.Load(gp).xyz;
    const float3 V = normalize(gRfCameraPos - P);

    // GGX VNDF で half vector を 1 本引き、視線をそれで反射させる。
    // alpha = roughness² (common.hlsli::DistributionGGX と同じ規約)
    uint3 seed = uint3(tid.x, tid.y, gRfFrameIndex * 16u + 11u); // GI/影と別の乱数列
    const float alpha = roughness * roughness;
    float3 L;
    if (dot(N, V) <= 1e-4f) {
        // シルエット際で補間法線が視線の裏へ回った画素。VNDF の前提 (ve.z > 0) を
        // 満たさないので鏡面方向で代用する
        L = reflect(-V, N);
    } else {
        L = reflect(-V, RtGgxVndf(N, V, alpha, RtNextRand2(seed)));
    }
    if (dot(L, N) <= 0.0f) {
        // ローブが面の下へ抜けた (粗い面 + 斜め視線)。棄却して黒を返すと 1spp では
        // 黒斑になるので鏡面方向へ丸める (v1 の近似)
        L = reflect(-V, N);
    }

    // 原点の誤差は「ワールド座標の絶対値」と「カメラからの距離」の両方に比例して増える
    // (rt_shadow.cs.hlsl と同一式)
    const float dist = max(length(P - gRfCameraPos), length(P));
    const float eps = max(gRfEpsMin, gRfEpsRel * dist);
    // ミス時のスカイは lod 0 — 鏡面反射に映る空をぼかさない (GI は粗い mip のまま)。
    // envOnLastHit = 1: 映り込んだ面もラスタと同じ明るさ (直接光 + 環境項) にする。
    //
    // ★トレースは gRsOn に関わらず**この 1 回だけ**。off/on の分岐の中でそれぞれ
    //   RtTraceRadianceLod / RtTraceRadianceFirstHit を呼ぶと、BVH トラバーサルが
    //   2 度インライン展開されてスタック (indexable temp) が倍になり、fxc が
    //   X4714 (レジスタ超過、性能低下) を出す — **off 経路の性能まで落ちる** (実測)。
    //   RtTraceRadianceLod は RtTraceRadianceFirstHit を呼ぶだけのラッパなので、
    //   ここで直接呼んでも返る放射輝度はビット単位で同じ
    RtFirstHit fh;
    const float3 Ls = RtTraceRadianceFirstHit(P + N * eps, L, gRfTMax, max(gRfBounces, 1), seed,
                                              0.0f, 1.0f, fh);
    gRfOut[tid.xy] = float4(Ls, 1.0f); // 生の 1spp (デバッグ 10 が読む。M67d 以前と同一)
    if (gRsOn == 0) {
        return; // ---- M67d 以前と完全に同一 (以降は 1 命令も実行されない) ----
    }

    // ---- M67d: ReSTIR の初期 reservoir ----
    // 放射輝度は上と同じ 1 本のレイから取る (レイ数は 1 本も増えない)。違うのは
    // 「どこに当たったか」(fh) も一緒に持ち帰って reservoir に積むところだけ。
    //
    // ソース pdf = D_vis なので初期重みは p̂/p = lum(Ls) に約分される。
    // **lum = 0 (真っ黒) でも M = 1** — 「1 本撃った」事実は変わらないので、
    // RtReservoirUpdate を w = 0 のまま呼ぶ (spec §4.2「M を数える規則」)
    RtReservoir r = RtReservoirEmpty();
    float wSum = 0.0f;
    const float w = RtLuminance(Ls);
    RtReservoirUpdate(r, wSum, fh.pos, fh.nrm, Ls, fh.cls, /*mInc=*/1.0f, w, /*rnd=*/0.0f);

    // ---- M67e: 時間再利用 (前フレームの組 A から候補を 1 つ) ----
    // 「同じ材質点が前フレームのどこに写っていたか」を SVGF の蓄積とまったく同じ規約で
    // 探す (rt_reproject.hlsli)。当たらなければ候補ゼロ = M は 1 のまま = 1spp と同じ絵。
    //
    // ★候補が外れた経路では **M を 1 も足さない** (spec §4.2「M を数える規則」)。
    //   RtReservoirMerge が空 reservoir / J 範囲外 / w が 0・非有限を全部
    //   RtReservoirUpdate を呼ぶ前に落とすので、ここでは呼ぶだけでよい。
    //   幾何が食い違う (再投影が無効) 場合はそもそも Merge を呼ばない
    if (gRsHistValid != 0) {
        float2 prevUv;
        // 画面速度は G-Buffer と同解像度なので P/N と同じ gp で引く (rt_temporal と同じ)
        const float2 vel = gRsGbVelocity.Load(gp);
        if (RtHistoryUv(gRsUseVelocity, uv, vel, mul(float4(P, 1.0f), gRsPrevViewProj), prevUv)) {
            // reservoir は内部解像度なので履歴 UV も内部解像度で引く
            const int3 hp = int3(int2(prevUv * gRfOutSize), 0);
            const float4 geom = gRsPrevGeom.Load(hp);
            // ★深度は「**現**フレームの P を前カメラから測った距離」と比べる —
            //   rt_temporal.cs.hlsl と同じ近似 (画面速度は 2D なので前フレームの
            //   カメラ距離を復元できない)。geom.w == 0 = 未記録は必ず落ちる
            if (RtReprojectValid(length(P - gRsPrevCameraPos), geom.w, N, geom.xyz,
                                 gRsDepthThreshold, gRsNormalThreshold)) {
                const RtReservoir prev = RtReservoirUnpack(
                    gRsPrevPos.Load(hp), gRsPrevRad.Load(hp), gRsPrevNrm.Load(hp));
                // 前フレームの**受け側**ワールド座標 (ユーザー判断 U4 = 厳密な Jacobian)。
                // 静止した面では P_prev == P なので J = 1 ちょうど — 逆に言えば
                // rpos の配線が壊れると J が範囲外に落ちて M が 1 から伸びなくなる
                const float3 pPrev = gRsPrevRpos.Load(hp).xyz;
                // p̂ は**今フレームの V / N / α** で評価する (受け側が変わったぶんの
                // 重み付け直しが ReSTIR の本体)。方向は必ず xs からの復元
                const float pHatPrev =
                    RtRestirTargetPdf(prev.Ls, RtRestirSampleDir(prev, P), V, N, alpha);
                const float J = RtRestirJacobian(prev.xs, prev.ns, pPrev, P);
                // M 上限は**候補のクラス** (映っている物体) で決まる — 主役ほど短く積む
                RtReservoirMerge(r, wSum, prev, pHatPrev, RtRestirClassParams(prev.cls).z, J,
                                 gRsJacobianMax, RtNextRand2(seed).x);
            }
        }
    }

    // M67e: 書き戻す前に M を採用サンプルのクラス上限へ切り詰める。
    // wSum を同じ比率で縮めるので W も resolve の結果も変わらない — 変わるのは
    // 「次のフレームがこの reservoir をどれだけ重く扱うか」だけ (spec §4.2)
    RtRestirClampM(r, wSum, RtRestirClassParams(r.cls).z);

    // ★p̂ は**保存した xs から復元した方向**で評価する — 撃った L そのものではない。
    //   次段 (spatial) と次フレーム (temporal) は xs しか知らないので、そちらと同じ
    //   復元 (RtRestirSampleDir) をしておかないと p̂ の比が 1 にならず、
    //   再利用ゼロ (M=1) でも絵が現行から数 % ずれる (A5 の根拠)。
    //   ★**temporal の統合より後**に評価すること — 採用されたサンプルが履歴側に
    //     入れ替わっていることがあるので、r.Ls / r.xs を見てから W を作る
    const float pHat = RtRestirTargetPdf(r.Ls, RtRestirSampleDir(r, P), V, N, alpha);
    r.W = RtRestirWeight(wSum, r.M, pHat);

    float4 pos, rad, nrm;
    RtReservoirPack(r, pos, rad, nrm);
    gRsOutPos[tid.xy] = pos;
    gRsOutRad[tid.xy] = rad;
    gRsOutNrm[tid.xy] = nrm;
    // 受け側の情報 (RtHistory.geom と同レイアウト = 再投影の妥当性判定に流用できる)
    gRsOutGeom[tid.xy] = float4(N, length(P - gRfCameraPos));
    // 受け側のワールド座標。次フレームの temporal が Jacobian の P_from に使う (U4)
    gRsOutRpos[tid.xy] = float4(P, 0.0f);
}
