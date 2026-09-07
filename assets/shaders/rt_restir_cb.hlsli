// M67d: ReSTIR 反射の**配管側の共有部品** — 定数バッファ (b3) の唯一の宣言と、
// それを読む小さなヘルパ。rt_refl.cs.hlsl と rt_refl_restir_spatial.cs.hlsl の 2 本が
// 同じものを読むので、宣言を写経すると「片方だけ並びを直して CB が丸ごとずれる」=
// 絵は出るが再利用パラメータが全部でたらめ、という静かな壊れ方をする
// (M58d の terrain_common.hlsli と同じ理由)。
//
// 数式そのもの (reservoir の統合 / pdf / Jacobian) は rt_restir_common.hlsli 側 —
// あちらは RtMath.h の 1:1 ミラーなので、CB を知っている関数を混ぜてはいけない。
//
// C++ 側の出所は RtPasses.cpp の `struct RtRestirCB` (static_assert でサイズを固定)。
// 配列長 gRsClass[MYE_RT_REFL_CLASS_COUNT] は tools/check_rules.ps1 の規則 9 が
// C++ の kRtReflClassCount と照合する。

#ifndef MYE_RT_RESTIR_CB_INCLUDED
#define MYE_RT_RESTIR_CB_INCLUDED

#ifndef MYE_RT_RESTIR_COMMON_INCLUDED
#error "rt_restir_cb.hlsli は rt_common.hlsli → rt_restir_common.hlsli の後に include すること"
#endif

cbuffer RtRestirCB : register(b3)
{
    float4x4 gRsPrevViewProj; // 前フレームの viewProj (転置済み、mul(row, M) 規約)
    float3 gRsPrevCameraPos;
    int gRsOn;             // 0 = ReSTIR を使わない (rt_refl は M67d 以前と同一経路)
    float3 gRsCameraPos;   // 今フレームのカメラ位置 (V の出所)
    int gRsSpatialOn;      // 空間再利用 (M67f)
    float2 gRsOutSize;     // reservoir の解像度 (= 反射バッファの内部解像度)
    float2 gRsGbSize;      // G-Buffer の解像度 (フル)
    int gRsVisRay;         // 候補の可視レイ (M67f)
    int gRsHistValid;      // 0 = 前フレームの reservoir が無い (初回 / リサイズ / 不連続)
    int gRsUseVelocity;    // 1 = 履歴 UV を GBuffer RT4 (画面速度) から作る (M67e)
    int gRsClassOverride;  // -1 = off (M67f)
    // M67h: 旧 gRsFrameIndex の枠。**フレーム番号は ReSTIR には二度と混ぜない** —
    // M67f でタップ回転のフレーム項を外した (回すと候補集合が毎フレーム入れ替わり、
    // 乗り換えがそのままフリッカーになる = 実測 2 倍)。理由の本文は
    // rt_restir_common.hlsli の MYE_RT_RESTIR_TAP_SEED / C++ の kRtRestirTapSeed。
    // 枠を残しているのは C++ の 240 B / gRsClass の offsetof 160 を動かさないため
    uint gRsPad1;
    float gRsDepthThreshold;  // 再投影の妥当性 (kRtTemporal* の流用)
    float gRsNormalThreshold;
    float gRsJacobianMax;  // これと逆数の外の J は候補ごと棄却 (kRtRestirJacobianMax)
    float gRsRadiusAlphaRef; // 半径を α で縮める基準 (M67f。kRtRestirRadiusAlphaRef)
    // ★明示パディング。float4 配列は 16 バイト境界からしか始まらないので、ここを
    //   省くと C++ 側の offsetof と HLSL の実配置が 12 バイトずれて表が丸ごと化ける
    float3 gRsPad0;
    // クラス別の再利用パラメータ (x = 半径 px / y = タップ数 / z = M 上限 / w = 予備)。
    // ★ここは C++ の offsetof(RtRestirCB, classTable) == 160 と一致している
    //   (RtPasses.cpp の static_assert が門番)。**前に float を足すときは両方**直すこと —
    //   片方だけ動かすと表が丸ごとずれ、絵は出るが再利用パラメータが全部でたらめになる
    float4 gRsClass[MYE_RT_REFL_CLASS_COUNT];
};

// M67f: クラス上書き (--rt-class-override / チューニング UI) を適用したクラス。
// **上書きの本体は CPU 側** (RtScene::Update が RtInstance.reflectionClass を書き換える)
// なので、今フレームに撃ったサンプルはここを通らなくても既に N になっている。
// ここが効くのは**上書きを切り替えた瞬間に残っている古い reservoir** —
// 時間再利用の履歴は最大 32 フレーム分生き残るので、シェーダ側でも一度潰さないと
// 「スライダを動かしてから効き始めるまで数十フレーム掛かる」= UI が壊れて見える。
// ★**空 reservoir の -1 は上書きしない** (センチネルを潰すとデバッグ 14 で
//   「何も入っていない画素」が上書き先のクラス色で塗り潰される)
int RtRestirEffectiveClass(int cls)
{
    return (gRsClassOverride >= 0 && cls >= 0) ? gRsClassOverride : cls;
}

// クラス表の安全な引き方。**空 reservoir の cls = -1 を 0 (Hero) へ丸めない** —
// 範囲外は中立クラス (Default) を返す (spec §4.1 と同じ規約)。
// cls = -1 の候補は重み 0 で必ず外れるので値そのものは絵に出ないが、
// gRsClass[-1] は**配列の外**なので読み方だけは正しくしておく。
// クラス上書きはここに畳んである = **表を引く全経路が自動的に上書きに従う**
float4 RtRestirClassParams(int cls)
{
    const int e = RtRestirEffectiveClass(cls);
    const int c = (e >= 0 && e < MYE_RT_REFL_CLASS_COUNT) ? e : (MYE_RT_REFL_CLASS_COUNT - 1);
    return gRsClass[c];
}

// reservoir のサンプル方向を受け側 P から復元する。
// **スカイ (ns = 0) は xs が方向ベクトルそのもの** (spec §4.2)。
// ★rt_refl (初期 reservoir の p̂ 評価) と spatial (統合時の p̂ 評価) が
//   **同一式**でなければならない — ここが 1 文字でもずれると、再利用ゼロ (M = 1) でも
//   p̂ の比が 1 にならず絵が現行からずれる (受け入れ条件 A5 の根拠)。
//   撃った L ではなく「保存した xs から復元した方向」を使うのは、レイ原点を
//   法線方向へ eps ずらしているぶん両者が数 % 違う pdf を与えるため
float3 RtRestirSampleDir(RtReservoir r, float3 P)
{
    return (dot(r.ns, r.ns) > 0.0f) ? normalize(r.xs - P) : r.xs;
}

#endif // MYE_RT_RESTIR_CB_INCLUDED
