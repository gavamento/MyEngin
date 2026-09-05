// M67d: ReSTIR 反射の 2 パス目 (空間再利用 + resolve)。
//
// 入力  = rt_refl.cs.hlsl が今フレームに書いた reservoir (t11-t15) と G-Buffer (t7-t10)
// 出力  = 解決した反射放射輝度 (u0 = reflRestirRt_、後段の SVGF がこれを食う) **だけ**
//
// ★M67f: **reservoir を書き戻さない**。時間再利用の履歴は rt_refl (temporal) の出力
//   そのもので、spatial の結果は「今フレームの絵」にしか使わない。書き戻していた初版は
//   近傍の履歴が自画素の履歴に混ざり、(a) M の重いクラス (Prop) のサンプルが 1 フレーム
//   あたり半径ぶんずつ拡散して 40 フレームで画面の 94% を占拠 (実測 9988 → 40432 px、
//   平均輝度 +8.4%)、(b) 採用サンプルの乗り換えがフリッカーになる、という壊れ方をした。
//   断てば「Hero のサンプルは radius[Hero] より遠くへ運ばれない」がフレームを跨いでも
//   成り立つ (spec §4.2 の保存の項)。
//   組の入れ替え (ping-pong の flip) は RtPasses が持つ — このシェーダは
//   「今フレームの reservoir を読んで絵を作る」だけの純粋な消費者になった。
//
// 読む面と書く面が常に別テクスチャなのは変わらない (typed UAV load を避ける。U5)。

#include "rt_common.hlsli"
#include "rt_restir_common.hlsli"
#include "rt_reproject.hlsli"
#include "rt_restir_cb.hlsli"

Texture2D gRsGbNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gRsGbPosition : register(t8); // GBuffer ワールド座標
// t9 (GBuffer アルベド = ジオメトリ有りマーク) は C++ が rt_refl と同じ 4 枚まとめて
// 張るが、このパスは**宣言もしない** — 「ジオメトリ無し」は reservoir の M = 0 で
// 既に分かるので読む用が無い。読まない SRV を宣言だけ残すと死コードになる
Texture2D gRsGbMaterial : register(t10); // GBuffer マテリアル (r = metallic, g = roughness)
// 今フレームの reservoir (rt_refl が初期化 + temporal 統合まで済ませたもの)
Texture2D gRsInPos : register(t11);
Texture2D gRsInRad : register(t12);
Texture2D gRsInNrm : register(t13);
Texture2D gRsInGeom : register(t14);
Texture2D gRsInRpos : register(t15);

// 解決した反射放射輝度 (rgb) + 有効マーク (a)。**このパスの出力はこれだけ** (M67f)
RWTexture2D<float4> gRsOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gRsOutSize.x || tid.y >= (uint)gRsOutSize.y) {
        return;
    }
    const int3 sp = int3(int2(tid.xy), 0);
    const RtReservoir center =
        RtReservoirUnpack(gRsInPos.Load(sp), gRsInRad.Load(sp), gRsInNrm.Load(sp));
    if (!(center.M > 0.0f)) {
        // ジオメトリ無し / roughness 超過。rt_refl の同じ画素と同じ「空」を出す
        gRsOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 内部解像度のピクセル中心を G-Buffer の座標へ写す (rt_refl.cs.hlsl と同じ写像)
    const float2 uv = (float2(tid.xy) + 0.5f) / gRsOutSize;
    const int3 gp = int3(int2(uv * gRsGbSize), 0);
    const float3 N = normalize(gRsGbNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gRsGbPosition.Load(gp).xyz;
    const float3 V = normalize(gRsCameraPos - P);
    const float roughness = gRsGbMaterial.Load(gp).g;
    const float alpha = roughness * roughness;

    // ---- 自画素の reservoir を統合 (受け側が同じ画素なので J = 1) ----
    // 最初の候補なので採用確率は w/wSum = 1 = 乱数によらず必ず採られる → rnd = 0
    RtReservoir r = RtReservoirEmpty();
    float wSum = 0.0f;
    const float3 lc = RtRestirSampleDir(center, P);
    const float pc = RtRestirTargetPdf(center.Ls, lc, V, N, alpha);
    RtReservoirMerge(r, wSum, center, pc, RtRestirClassParams(center.cls).z, /*J=*/1.0f,
                     gRsJacobianMax, /*rnd=*/0.0f);

    // ---- M67f: 近傍タップ (Vogel 螺旋 × クラス別の半径 / タップ数) ----
    // 半径とタップ数は**中心画素の reservoir が持っているクラス** (= この画素の反射像に
    // 今映っている物体) で決める。「主役が映っている画素は借りる範囲を狭くする」が
    // ReflectionClass の狙いなので、受け側 (鏡そのもの) の材質では決めない。
    //
    // ★上限は必ず MYE_RT_RESTIR_MAX_TAPS (= C++ の kRtRestirMaxTaps と規則 9 で照合)。
    //   ここを生の数字にすると照合が形だけになる。[unroll] にしないのは、タップ数が
    //   gRsClass 由来の動的値だから (fxc が展開に失敗しうる)
    // ★半径は**受け側の α に比例して縮める** (M67f)。p̂ のローブ幅は α に比例するので、
    //   滑らかな面で表の半径をそのまま使うと「ローブの外のタップ」が増えるだけで、
    //   暗化とフリッカーしか生まない (round 1 実測: 粗さ 0.10 で -5.2% / フリッカー 5 倍)。
    //   実効半径が 1 px 未満なら**タップ 0** = 鏡面では spatial が自然に切れる
    const float4 centerParams = RtRestirClassParams(center.cls);
    const float radiusScale = RtRestirRadiusScale(alpha, gRsRadiusAlphaRef);
    const float radiusEff = centerParams.x * radiusScale;
    const int tapCount = (gRsSpatialOn != 0 && radiusEff >= 1.0f)
        ? (int)clamp(centerParams.y, 0.0f, (float)MYE_RT_RESTIR_MAX_TAPS)
        : 0;
    // タップの回転角。**画素ごとに違うがフレームでは回さない** — 全画素同じだと螺旋の
    // 偏りが格子模様として絵に焼き付くが、フレームで回すと候補集合が毎フレーム
    // 入れ替わって採用サンプルの乗り換えがそのままフリッカーになる (round 1 実測 2 倍)。
    // 書き戻しを断ったのでフレーム間の脱相関は要らない (spec §4.3)
    uint3 seed = uint3(tid.x, tid.y, (uint)MYE_RT_RESTIR_TAP_SEED);
    const float rot = RtNextRand2(seed).x * 6.28318531f;
    const float centerDist = length(P - gRsCameraPos);
    [loop]
    for (int i = 0; i < MYE_RT_RESTIR_MAX_TAPS; ++i) {
        if (i >= tapCount) {
            break;
        }
        const float2 tapOff = RtRestirVogelTap(i, tapCount, radiusEff, rot);
        const int2 tp = int2(tid.xy) + int2(round(tapOff));
        if (tp.x < 0 || tp.y < 0 || tp.x >= (int)gRsOutSize.x || tp.y >= (int)gRsOutSize.y) {
            continue; // 画面外
        }
        if (tp.x == (int)tid.x && tp.y == (int)tid.y) {
            continue; // 半径が 1px 未満に丸まった = 自画素。二重に数えない
        }
        const int3 sn = int3(tp, 0);
        // ★**受け側の幾何一致を先に見る** — 別の面 (奥の壁・向きの違う面) の画素から
        //   借りると、Jacobian では補正しきれない不連続がにじみとして出る。
        //   判定は SVGF の再投影とまったく同じ関数 (rt_reproject.hlsli) で、
        //   「前フレームの同じ点か」を「隣の画素は同じ面か」に読み替えて使う。
        //   geom.w == 0 (= rt_refl が空を書いた画素) は必ず落ちる
        const float4 geomN = gRsInGeom.Load(sn);
        if (!RtReprojectValid(centerDist, geomN.w, N, geomN.xyz, gRsDepthThreshold,
                              gRsNormalThreshold)) {
            continue;
        }
        const RtReservoir cand =
            RtReservoirUnpack(gRsInPos.Load(sn), gRsInRad.Load(sn), gRsInNrm.Load(sn));
        // ★半径のもう一段の縛りは**候補のクラス** — 中心が Prop (半径 12px) でも、
        //   その円板の中に Hero (半径 2px) が映っている画素があれば 2px より遠くへは
        //   運ばない。クラスの境界で「主役が急に遠くから借りられる」を防ぐ仕掛け
        //   (spec §4.3)。距離は丸めた後の実際の画素差で測る (実際に運ぶ距離だから)。
        //   ★候補側の半径にも同じ α 係数を掛ける — 中心と候補で尺度が違うと
        //     「中心の実効半径では届く距離なのに候補の生半径で弾かれる」がまだらに起きる
        const float4 candParams = RtRestirClassParams(cand.cls);
        const float2 realOff = float2(tp - int2(tid.xy));
        if (!(length(realOff) <= candParams.x * radiusScale)) {
            continue;
        }
        // 受け側 (自画素) から見たサンプル方向。半球の外は p̂ = 0 になるので
        // Merge でも落ちるが、可視レイを撃つ前にここで落とす (無駄なレイを減らす)
        const float3 candL = RtRestirSampleDir(cand, P);
        if (!(dot(candL, N) > 0.0f)) {
            continue;
        }
        // 受け側が候補の画素から自画素へ移ったぶんの立体角の伸縮。
        // 候補の受け側ワールド座標は組 B の rpos (rt_refl がその画素の G-Buffer P を
        // そのまま書いたもの) から取る — G-Buffer を引き直すより一致が保証される
        const float3 candP = gRsInRpos.Load(sn).xyz;
        const float J = RtRestirJacobian(cand.xs, cand.ns, candP, P);
        if (!(J >= 1.0f / gRsJacobianMax && J <= gRsJacobianMax)) {
            continue; // 幾何が違いすぎる (Merge も同じ判定で落とすが、レイの前に抜ける)
        }
        // ★可視レイ (既定 off)。「隣の画素から見えていたヒット点が、自画素からも
        //   見えるか」を実際に撃って確かめる。off のときの光漏れ (壁の裏の明るさが
        //   にじむ) が v1 の既知バイアスで、これを on にすると消える代わりに
        //   タップ数ぶんのレイが増える (spec §7)
        if (gRsVisRay != 0) {
            const bool sky = !(dot(cand.ns, cand.ns) > 0.0f);
            // スカイは xs が方向 = 距離が無い。遮蔽物が 1 つでもあれば棄却でよいので
            // 実質無限の tMax で撃つ (rt_common の太陽影と同じ 1e16)
            const float tMax = sky ? 1e16f : (length(cand.xs - P) - 2.0f * gRtRayEps);
            if (tMax > gRtRayEps && RtTraceAnyHit(P + N * gRtRayEps, candL, tMax)) {
                continue;
            }
        }
        // p̂ は**今フレームの受け側の V / N / α**で評価し直す (= 重み付けの本体)。
        // M 上限は候補のクラス。ここまで来た候補だけが M に数えられる
        const float candPHat = RtRestirTargetPdf(cand.Ls, candL, V, N, alpha);
        RtReservoirMerge(r, wSum, cand, candPHat, candParams.z, J, gRsJacobianMax,
                         RtNextRand2(seed).x);
    }

    // M をクラスの上限へ切り詰める (wSum も同じ比率で縮むので resolve の結果は不変)。
    // ★書き戻しは無くなったが**残す** — spec §4.2 の「統合後は cls_sel の上限まで」を
    //   1 か所でも崩すと、CPU ミラーの往復 selftest と GPU の M が食い違う
    RtRestirClampM(r, wSum, RtRestirClassParams(r.cls).z);
    // ★候補を 1 つも採れなかった画素は **1spp をそのまま通す** (0 にしない)。
    //   補間法線が視線の裏へ回った画素 (現行コードが「鏡面方向で代用する」と書いている
    //   シルエット際) では VNDF の pdf が定義できず p̂ = 0 → W = 0 になり、ReSTIR の
    //   推定量はその画素を**永久に**黒くする (時間・空間再利用を足しても受け側の p̂ が
    //   0 なので全候補が落ちる)。--render-demo の frame 3 で実測 950 テクセル。
    //   現行の 1spp はそこにも値を出しているので、**置き換えられない画素は置き換えない**
    //   = 「ReSTIR on が off より悪くなることはない」を不変量にする。
    //   center.Ls は rt_refl が u0 (reflRt_) へ書いた 1spp と**同じ fp16 の値**なので、
    //   これで再利用ゼロのときに現行とビット一致する (受け入れ条件 A5)。
    //   ★M67f: reservoir は書き戻さないので、この画素が「採点できなかった」ことは
    //     次フレームには伝わらない — 伝える必要も無い (履歴は rt_refl の出力が持つ)
    const float3 radiance = (r.M > 0.0f) ? RtRestirResolve(r, wSum) : center.Ls;
    gRsOut[tid.xy] = float4(radiance, 1.0f);
}
