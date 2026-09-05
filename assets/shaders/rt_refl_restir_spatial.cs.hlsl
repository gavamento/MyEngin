// M67d: ReSTIR 反射の 2 パス目 (空間再利用 + resolve)。
//
// 入力  = rt_refl.cs.hlsl が書いた reservoir の組 B (t11-t15) と G-Buffer (t7-t10)
// 出力  = 解決した反射放射輝度 (u0 = reflRestirRt_、後段の SVGF がこれを食う) と、
//         次フレームへ持ち越す reservoir の組 A (u1-u5)
//
// ★読む組と書く組が常に別テクスチャなのは意図的 (ping-pong を flip しない) —
//   同じテクスチャを SRV と UAV で同時に張れない D3D11 の制約を、typed UAV load
//   (= フォーマット制限が厳しい) を使わずに回避するため。spec §4.2 / ユーザー判断 U5。
//
// **M67d ではタップ 0** = 自画素の reservoir を 1 つ統合して resolve するだけ。
// それでも「reservoir に詰めて → 読み直して → 解決した」絵が現行とビット一致することが、
// 配管が正しいことの証拠になる (受け入れ条件 A5)。空間タップは M67f。

#include "rt_common.hlsli"
#include "rt_restir_common.hlsli"
#include "rt_reproject.hlsli"
#include "rt_restir_cb.hlsli"

Texture2D gRsGbNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gRsGbPosition : register(t8); // GBuffer ワールド座標
// GBuffer アルベド。**このパスは読まない** (「ジオメトリ無し」は reservoir の M = 0 で
// 既に分かる) が、C++ 側は rt_refl と同じ t7-t10 の 4 枚をまとめて張るので宣言だけ置く
Texture2D gRsGbMark : register(t9);
Texture2D gRsGbMaterial : register(t10); // GBuffer マテリアル (r = metallic, g = roughness)
// 組 B (rt_refl がこのフレームに書いたもの)
Texture2D gRsInPos : register(t11);
Texture2D gRsInRad : register(t12);
Texture2D gRsInNrm : register(t13);
Texture2D gRsInGeom : register(t14);
Texture2D gRsInRpos : register(t15);

RWTexture2D<float4> gRsOut : register(u0); // 解決した反射放射輝度 (rgb) + 有効マーク (a)
// 組 A (次フレームの rt_refl が temporal 候補として読む)
RWTexture2D<float4> gRsOutPos : register(u1);
RWTexture2D<float4> gRsOutRad : register(u2);
RWTexture2D<float4> gRsOutNrm : register(u3);
RWTexture2D<float4> gRsOutGeom : register(u4);
RWTexture2D<float4> gRsOutRpos : register(u5);

// 「この画素にサンプルは無い」を書く。rt_refl の同名関数と同じ規約
// (M = 0 / cls = -1 / geom.w = 0 = 再投影の妥当性判定が必ず落とす値)
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
    if (tid.x >= (uint)gRsOutSize.x || tid.y >= (uint)gRsOutSize.y) {
        return;
    }
    const int3 sp = int3(int2(tid.xy), 0);
    const RtReservoir center =
        RtReservoirUnpack(gRsInPos.Load(sp), gRsInRad.Load(sp), gRsInNrm.Load(sp));
    if (!(center.M > 0.0f)) {
        // ジオメトリ無し / roughness 超過。rt_refl の同じ画素と同じ「空」を書く
        gRsOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        RtRestirWriteEmpty(tid.xy);
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

    // ---- M67f: ここに近傍タップ (Vogel 螺旋 × gRsSpatialOn × クラス別の半径/タップ数) が入る。
    //      [loop] の静的上限は MYE_RT_RESTIR_MAX_TAPS ----

    // 書き戻し前に M をクラスの上限へ切り詰める (W は変わらない = 絵は変わらない)
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
    //   ★この画素の reservoir は M = 0 のまま書き戻す — 採点できなかったサンプルを
    //     次フレームに再利用させないため。デバッグ 12 では赤 (M=0) として見える
    const float3 radiance = (r.M > 0.0f) ? RtRestirResolve(r, wSum) : center.Ls;
    gRsOut[tid.xy] = float4(radiance, 1.0f);

    // ★M67e: **書き戻す前に W を作り直す** — テクスチャに載るのは wSum ではなく
    //   W = wSum / (M · p̂(y)) なので (spec §4.2 の保存表)、ここで入れ忘れると
    //   組 A の pos.w が RtReservoirEmpty() の 0 のまま出ていく。M67d では誰も
    //   読まなかったので無害だったが、M67e の temporal はこの W を
    //   `w = p̂ · W · M · J` に掛ける = **全候補の重みが 0 になり M が永久に 1 のまま**
    //   になる (実測: この 2 行が無いとデバッグ 12 が frame 3 / 40 / 80 で同一画像)。
    //   p̂ は rt_refl の初期 reservoir とまったく同じ式・同じ方向の復元で評価する
    const float pSel = RtRestirTargetPdf(r.Ls, RtRestirSampleDir(r, P), V, N, alpha);
    r.W = RtRestirWeight(wSum, r.M, pSel);

    float4 pos, rad, nrm;
    RtReservoirPack(r, pos, rad, nrm);
    gRsOutPos[tid.xy] = pos;
    gRsOutRad[tid.xy] = rad;
    gRsOutNrm[tid.xy] = nrm;
    gRsOutGeom[tid.xy] = float4(N, length(P - gRsCameraPos));
    // ★**統合後の reservoir の受け側はこの画素** — B から写すのではなく
    //   G-Buffer の P を書く (M67d はタップ 0 なので同値だが、M67f で意味が分かれる)
    gRsOutRpos[tid.xy] = float4(P, 0.0f);
}
