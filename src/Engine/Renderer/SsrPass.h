#pragma once
#include <cmath>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/GpuTimer.h"
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

class GraphicsDevice;
class ShaderManager;

// SSR = スクリーンスペース反射 (M56d)。HZB (M56c) の min-Z ピラミッドを階層的に辿り、
// **既に画面に出ている色**を反射として拾って加算する。
//
// ★このパスが足すのは「反射そのもの」ではなく **IBL スペキュラとの差分**。
//   ライトパスは環境スペキュラを `iblPrefiltered * ao * (F0*brdf.x + brdf.y)` として
//   既に足しているので、素の反射を上乗せすると同じ光を二重に数えてしまう。
//   `(反射 - IBL スペキュラ) * 環境BRDF * 重み` を加算すると、結果は
//   `ApplyLightingHybrid` が RT 反射に対してやっている「同次元の放射輝度を lerp で
//   差し替える」式とちょうど一致する — 重み 0 で厳密に 0 が足される (= 恒等) のも同じ理由。
//
// ★**読むのはライトパス (+ スカイボックス) が書き終えた RT そのもの**なので、
//   SRV 専用のコピーを 1 枚取ってから読む。同一リソースを RTV と SRV に同時に張ることは
//   できず、外して別 RT へ書いてから合成し直すと WARP でフルスクリーンパスが 1 本増える。
//   コピーは M56b が RT1 (法線) に対してやったのと同じ手 (gbNormalCopy_)。
//
// ★交差判定に GBuffer RT2 (ワールド座標、R16G16B16A16F) を**使わない**。原点から離れると
//   16bit 浮動小数の刻みがワールド単位で粗くなり、鏡面の像がガタつく。深度 (= HZB の mip 0)
//   から逆投影する。逆投影に使う行列は**ジッタ込みの view*proj** — 深度バッファを
//   ラスタライズした行列と同じものでないと、復元位置が半ピクセル分ずれる。
class SsrPass {
public:
    // 1 フレーム分の入力。DeferredPath が自分の GBuffer / HZB / IBL から埋める
    struct Inputs {
        ID3D11ShaderResourceView* hzb = nullptr;      // 全段を覆う SRV (mip 0 = 深度)
        int hzbMipCount = 0;
        ID3D11ShaderResourceView* gbAlbedo = nullptr;  // a<0.5 = ジオメトリ無し
        ID3D11ShaderResourceView* gbNormal = nullptr;  // ワールド法線 *0.5+0.5
        ID3D11ShaderResourceView* gbMaterial = nullptr; // r=metallic g=roughness
        ID3D11ShaderResourceView* ssao = nullptr;      // 半解像度 AO (null = 遮蔽なし)
        ID3D11SamplerState* linearClamp = nullptr;     // s0 (新設しない = 光パスの iblSampler_)
    };

    bool Init(GraphicsDevice& device, ShaderManager& shaders);
    void Shutdown();

    // view.rtv へ反射の差分を加算合成する。呼び出し側は
    //   ・RTV / DSV を外しておくこと (シーン色をコピーするため)
    //   ・ビューポートを view の実寸に設定しておくこと
    //   ・戻った後の RTV / 深度ステート / ブレンドステートを自分で戻すこと
    // 戻り値 false = 何もしなかった (シェーダ未コンパイル / HZB 無し / 確保失敗)
    bool Render(GraphicsDevice& device, ShaderManager& shaders, const RenderView& view,
                const Inputs& in);

    // 直近の Render の GPU 時間 [ms] (ProfilerWindow 表示用)。走らせていないフレームは前の値
    float GpuMs() const { return timer_.Milliseconds(); }

private:
    // view.rtv の中身を SRV 専用テクスチャへ複製する (無ければ作る / 寸法が変わったら作り直す)
    bool EnsureSceneCopy(GraphicsDevice& device, ID3D11RenderTargetView* rtv);

    AssetID shader_ = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendAdd_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneCopy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneCopySrv_;
    GpuTimer timer_;
    int copyW_ = 0;
    int copyH_ = 0;
    DXGI_FORMAT copyFormat_ = DXGI_FORMAT_UNKNOWN;
};

// ---- 以下は純関数 + 共有定数 (GPU も device も要らない)。SsrSelfTest が直接検査する ----

// 光線 1 本あたりの最大反復回数。**HLSL 側の MYE_SSR_MAX_STEPS と必ず一致させること** —
// 食い違っても絵は出る (反射が途中で切れるだけ) ので、tools\check_rules.ps1 の規則 9 が
// 一致を静的に検査する。階層 Z で大股に進むので 64 あれば 960x540 の対角を十分に渡れる
constexpr int kSsrMaxSteps = 64;

// 光線のワールド長 [world]。--render-demo の床は 200 角だが、反射で意味があるのは
// せいぜい柱 (高さ 7 まで) が映り込む範囲。CSM の kShadowMaxDist=60 と同じ桁に合わせてある
constexpr float kSsrMaxDistance = 60.0f;

// 交差とみなす「面の厚み」[world]。粗い段で薄い物体を飛び越したとき、光線は面の裏側に
// 出てしまう — その差がこの値を超えたら「裏を通過した」と見なして探索を続ける。
// 大きすぎると物体の裏の面に偽の反射が付き、小さすぎると細い柱の反射が欠ける
constexpr float kSsrThickness = 1.0f;

// IBL へフェードし始める roughness を maxRoughness の何倍に置くか。
// 既定の maxRoughness=0.6 でちょうど kRtReflFadeStart=0.4 / kRtReflMaxRoughness=0.6 =
// **RT 反射 (M46h) と同一のフェード**になる比率。段差を作らないための約束
constexpr float kSsrFadeStartRatio = 2.0f / 3.0f;

// 画面端のフェード幅 (UV 比)。ここを 0 にすると、カメラを動かしたとき画面外へ出た情報が
// 反射から瞬間的に消える (画面の縁に沿って反射がちぎれる) のがそのまま見える
constexpr float kSsrEdgeFade = 0.12f;

// 1 反復で最低限進む画素数。セル境界までの距離が 0 に潰れた (境界のちょうど上に居る /
// 軸に平行) ときでも必ず前進させるための下限で、**これが無いと光線が同じ場所で
// 反復を使い切る**。0.5 = 半画素
constexpr float kSsrMinPixelStep = 0.5f;

// 受け面から光線を離す量 (ビュー深度に対する比)。0 だと自分自身の深度に当たって
// 画面全体が「自分の色」で埋まる。深度に比例させるのは、遠い面ほど 1 画素が広く、
// 固定量では足りなくなるため
constexpr float kSsrNormalBiasRel = 0.002f;

// これ以上の深度は「ジオメトリ無し (空)」とみなす。深度クリア値 1.0 との比較を
// 厳密等号でやると、far 面ちょうどの画素で偽の交差が出る
constexpr float kSsrSkyDepth = 0.99999f;

// HLSL の smoothstep と同一 (CPU ミラーを 1 箇所に閉じ込めるためのローカル実装)
inline float SsrSmoothstep(float edge0, float edge1, float x)
{
    const float d = edge1 - edge0;
    float t = (d > 1e-8f || d < -1e-8f) ? (x - edge0) / d : ((x < edge0) ? 0.0f : 1.0f);
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// 反射を IBL スペキュラへ戻す重み (1 = SSR 100% / 0 = IBL 100%)。
// `RtReflWeight(roughness, max*kSsrFadeStartRatio, max)` と同じ式で、maxRoughness を
// 1 本のスライダで動かせるようにフェード開始を比率で導いている。
// **maxRoughness 以上でちょうど 0** を返すのが「粗い面には 1 ビットも足さない」の根拠
inline float SsrReflWeight(float roughness, float maxRoughness)
{
    const float maxR = (maxRoughness > 0.0f) ? maxRoughness : 0.0f;
    return 1.0f - SsrSmoothstep(maxR * kSsrFadeStartRatio, maxR, roughness);
}

// 画面端のフェード。UV が [0,1] の外なら 0、端から width 以内で線形に立ち上がる。
// **反射先が画面の外に出た瞬間に情報が消える**のを隠すための唯一の仕掛け
inline float SsrEdgeFade(float u, float v, float width)
{
    if (width <= 0.0f) {
        return (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) ? 1.0f : 0.0f;
    }
    const float du = (u < 1.0f - u) ? u : (1.0f - u);
    const float dv = (v < 1.0f - v) ? v : (1.0f - v);
    const float d = (du < dv) ? du : dv;
    const float t = d / width;
    return (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
}

// 画面座標 (px, py) から、辺 cellSize px のセルの**外**へ出るのに必要な光線パラメータの増分。
// 光線は p = 原点 + 方向 * t で、(dx, dy) は t が 1 進んだときの画素移動量。
//
// ★**階層 Z トレースが静かに固まるのはここ**。セル境界のちょうど上に居る場合や、
//   軸に平行な光線 (dx==0) では境界までの距離が 0 / 無限大になり、素朴に書くと
//   増分 0 = 同じ場所で反復を使い切る (絵は「反射が出ない」だけなので原因を追えない)。
//   ・軸に平行な成分は候補から外す (無限大扱い)
//   ・境界のわずかに先へ着地させる (min 側に minAdvance の半分を上乗せ)
//   ・**戻り値は必ず minAdvance 以上** — これが唯一の前進保証
//   NaN も `>` 比較が偽になることで minAdvance に落ちる (比較の向きを変えないこと)。
//   **HLSL ミラー: ssr_trace.hlsl の SsrCellAdvance — 変更時は両方更新** (SsrSelfTest が検証)
inline float SsrCellAdvance(float px, float py, float dx, float dy, float cellSize,
                            float minAdvance)
{
    const float big = 1.0e30f;
    float tx = big;
    float ty = big;
    if (dx > 1e-8f || dx < -1e-8f) {
        const float plane = (std::floor(px / cellSize) + ((dx > 0.0f) ? 1.0f : 0.0f)) * cellSize;
        tx = (plane - px) / dx;
    }
    if (dy > 1e-8f || dy < -1e-8f) {
        const float plane = (std::floor(py / cellSize) + ((dy > 0.0f) ? 1.0f : 0.0f)) * cellSize;
        ty = (plane - py) / dy;
    }
    const float adv = ((tx < ty) ? tx : ty) + 0.5f * minAdvance;
    return (adv > minAdvance) ? adv : minAdvance;
}

} // namespace mye
