#include "Engine/Renderer/RenderSelfTest.h"

#include <cmath>
#include <cstring>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/PostFxMath.h"
#include "Engine/Renderer/PostProcess.h"
#include "Engine/Renderer/RenderTypes.h" // M57a: mye::froxel (グリッドの幾何)

using namespace DirectX;

namespace mye {
namespace {

int g_failCount = 0;

#define TEST_CHECK(cond)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            MYE_LOG_INFO("  PASS: %s", #cond);                              \
        } else {                                                            \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__); \
            ++g_failCount;                                                  \
        }                                                                   \
    } while (0)

// 平行移動 (+ 一様スケール) のワールド行列を作る (行ベクトル規約)
XMFLOAT4X4 MakeWorld(float x, float y, float z, float scale = 1.0f)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixScaling(scale, scale, scale) * XMMatrixTranslation(x, y, z));
    return m;
}

// CSM のカスケード分割 (M38d): 単調増加・境界一致・λ の両極
void TestCascadeSplits()
{
    MYE_LOG_INFO("[selftest] cascade splits (practical split)");
    float s[3] = {};
    ComputeCascadeSplits(0.1f, 60.0f, 3, 0.5f, s);
    TEST_CHECK(s[0] > 0.1f && s[0] < s[1] && s[1] < s[2]);
    TEST_CHECK(std::fabs(s[2] - 60.0f) < 1e-3f); // 最終カスケードの far = 影距離
    float lin[3] = {};
    ComputeCascadeSplits(0.1f, 60.0f, 3, 0.0f, lin); // λ=0 → 純線形
    TEST_CHECK(std::fabs(lin[0] - (0.1f + 59.9f / 3.0f)) < 1e-3f);
    float lg[3] = {};
    ComputeCascadeSplits(0.1f, 60.0f, 3, 1.0f, lg); // λ=1 → 純対数
    TEST_CHECK(lg[0] < lin[0]); // 対数分割は近距離に寄る
}

void TestFrustumCulling()
{
    MYE_LOG_INFO("[selftest] frustum culling (p-vertex)");

    // カメラは原点から +Z を見る LH ビュー + 遠近投影 (エンジン規約と同じ)
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1),
                                           XMVectorSet(0, 0, 1, 1), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 1.0f, 0.1f, 100.0f);
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, view * proj);
    const Frustum f = BuildFrustum(vp);

    const XMFLOAT3 unitMin = { -0.5f, -0.5f, -0.5f };
    const XMFLOAT3 unitMax = { 0.5f, 0.5f, 0.5f };

    // 正面 5m 先 → 見える
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 0, 5), unitMin, unitMax) == true);
    // カメラ後方 → 近平面(z>=0)で除外
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 0, -5), unitMin, unitMax) == false);
    // 遠平面(100m)より奥 → 除外
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 0, 200), unitMin, unitMax) == false);
    // 真横に大きく外れる → 除外
    TEST_CHECK(AabbInFrustum(f, MakeWorld(100, 0, 5), unitMin, unitMax) == false);
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 100, 5), unitMin, unitMax) == false);
    // 巨大スケールで視錐台を内包 → 見える (p-vertex がスケールを正しく扱う)
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 0, 5, 1000.0f), unitMin, unitMax) == true);
    // 近平面を跨ぐ (中心は手前だが箱が near を越える) → 落とさない
    TEST_CHECK(AabbInFrustum(f, MakeWorld(0, 0, 0.05f), unitMin, unitMax) == true);
}

// M32d: カメラ別ポスト効果のマージ (色収差 / ビネット / グレーディング)
void TestPostFxMerge()
{
    MYE_LOG_INFO("[selftest] postfx merge (M32d)");
    PostProcess::Settings base; // 既定
    CameraPostFxComponent comp;
    comp.chromAberration = 0.01f;
    comp.vignetteIntensity = 0.4f;
    comp.vignetteRadius = 0.6f;
    comp.saturation = 1.5f;
    comp.contrast = 1.2f;
    comp.colorFilter = { 1.0f, 0.8f, 0.6f, 1.0f };
    comp.godrayIntensity = 0.7f; // M43b
    comp.godrayDecay = 0.9f;
    comp.lutTexture = AssetID{ 123 }; // M44a
    comp.lutIntensity = 0.5f;
    comp.autoExposure = 1; // M44b
    comp.aeSpeed = 5.0f;
    comp.aeMin = 0.5f;
    comp.aeMax = 8.0f;
    comp.dofFocusDistance = 20.0f; // M44c
    comp.dofFocusRange = 8.0f;
    comp.dofMaxRadius = 12.0f;
    comp.motionBlurIntensity = 0.6f; // M44d
    comp.mbMaxPixels = 24.0f;
    comp.taaOn = 1; // M55d
    comp.taaFeedback = 0.75f;
    const PostProcess::Settings s = MergeCameraPostFx(base, comp);
    TEST_CHECK(s.chromAberration == 0.01f);
    TEST_CHECK(s.vignetteIntensity == 0.4f);
    TEST_CHECK(s.vignetteRadius == 0.6f);
    TEST_CHECK(s.saturation == 1.5f);
    TEST_CHECK(s.contrast == 1.2f);
    TEST_CHECK(s.colorFilter.y == 0.8f && s.colorFilter.z == 0.6f);
    TEST_CHECK(s.applyGamma == base.applyGamma); // applyGamma は base 維持
    TEST_CHECK(s.godrayIntensity == 0.7f && s.godrayDecay == 0.9f); // M43b
    TEST_CHECK(s.lutTexture.value == 123 && s.lutIntensity == 0.5f); // M44a
    TEST_CHECK(s.lutSRV == nullptr); // SRV はマージでは触らない (RenderSystem が解決)
    TEST_CHECK(s.autoExposure == 1 && s.aeSpeed == 5.0f && s.aeMin == 0.5f && s.aeMax == 8.0f);
    TEST_CHECK(s.dofFocusDistance == 20.0f && s.dofFocusRange == 8.0f
               && s.dofMaxRadius == 12.0f); // M44c
    TEST_CHECK(s.motionBlurIntensity == 0.6f && s.mbMaxPixels == 24.0f); // M44d
    TEST_CHECK(s.taaOn == 1 && s.taaFeedback == 0.75f);                  // M55d
    // 既定のコンポーネント (= シーンに置いただけ) では TAA は off のまま =
    // CameraPostFx を足しても絵が変わらない
    TEST_CHECK(MergeCameraPostFx(base, CameraPostFxComponent{}).taaOn == 0);
    // M44b: aeInstant は base 維持 (applyGamma と同じ Settings 専用フィールド)
    PostProcess::Settings instantBase;
    instantBase.aeInstant = true;
    TEST_CHECK(MergeCameraPostFx(instantBase, comp).aeInstant == true);

    // 既定コンポーネント = 無効 (従来の見た目)
    const PostProcess::Settings d = MergeCameraPostFx(base, CameraPostFxComponent{});
    TEST_CHECK(d.chromAberration == 0.0f && d.vignetteIntensity == 0.0f);
    TEST_CHECK(d.saturation == 1.0f && d.contrast == 1.0f);
    TEST_CHECK(d.godrayIntensity == 0.0f); // M43b: 既定 = off
    TEST_CHECK(d.lutIntensity == 0.0f && d.lutTexture.IsNull()); // M44a: 既定 = off
    TEST_CHECK(d.autoExposure == 0); // M44b: 既定 = off
    TEST_CHECK(d.dofMaxRadius == 0.0f); // M44c: 既定 = off
    TEST_CHECK(d.motionBlurIntensity == 0.0f); // M44d: 既定 = off
}

// メッシュインスタンシングの run 検出 (M38f): 同一 (material,mesh) の連続 2 件以上のみ、
// canInstance=false は run を分断、base はバッファ内オフセットとして単調に積み上がる
void TestInstanceRuns()
{
    MYE_LOG_INFO("[selftest] mesh instance runs (M38f)");

    auto makeItem = [](uint64_t mat, uint64_t mesh, float x) {
        RenderItem it;
        it.material.value = mat;
        it.mesh.value = mesh;
        it.world = MakeWorld(x, 0, 0);
        return it;
    };
    std::vector<MeshInstanceRun> runs;
    std::vector<XMFLOAT4X4> worlds;

    // 3 連続 = 1 run (count=3, base=0)、行列は項目順
    std::vector<RenderItem> a = { makeItem(1, 10, 0), makeItem(1, 10, 1), makeItem(1, 10, 2) };
    BuildInstanceRuns(a, { 1, 1, 1 }, runs, worlds);
    TEST_CHECK(runs.size() == 1 && runs[0].first == 0 && runs[0].count == 3 && runs[0].base == 0);
    TEST_CHECK(worlds.size() == 3 && worlds[1]._41 == 1.0f && worlds[2]._41 == 2.0f);

    // 単発は run にならない / mesh 違いは境界
    std::vector<RenderItem> b = { makeItem(1, 10, 0), makeItem(1, 20, 1), makeItem(1, 20, 2) };
    BuildInstanceRuns(b, { 1, 1, 1 }, runs, worlds);
    TEST_CHECK(runs.size() == 1 && runs[0].first == 1 && runs[0].count == 2 && runs[0].base == 0);

    // material 違いは同一 mesh でも境界
    std::vector<RenderItem> c = { makeItem(1, 10, 0), makeItem(2, 10, 1) };
    BuildInstanceRuns(c, { 1, 1 }, runs, worlds);
    TEST_CHECK(runs.empty() && worlds.empty());

    // canInstance=false (スキン等) は run を分断する
    std::vector<RenderItem> d = { makeItem(1, 10, 0), makeItem(1, 10, 1), makeItem(1, 10, 2) };
    BuildInstanceRuns(d, { 1, 0, 1 }, runs, worlds);
    TEST_CHECK(runs.empty());

    // 複数 run: base は先行 run の行列数だけ進む
    std::vector<RenderItem> e = { makeItem(1, 10, 0), makeItem(1, 10, 1), makeItem(2, 20, 2),
                                  makeItem(2, 20, 3), makeItem(2, 20, 4) };
    BuildInstanceRuns(e, { 1, 1, 1, 1, 1 }, runs, worlds);
    TEST_CHECK(runs.size() == 2 && runs[0].count == 2 && runs[1].first == 2 && runs[1].count == 3
               && runs[1].base == 2);
    TEST_CHECK(worlds.size() == 5 && worlds[2]._41 == 2.0f);
}

// M43a: ハイトフォグ / 太陽インスキャッタ (common.hlsli::ApplyFog のミラー検証)
void TestHeightFogInscatter()
{
    MYE_LOG_INFO("[selftest] height fog + sun inscatter (M43a)");
    // falloff=0 → 恒等 (従来とビット同一)
    TEST_CHECK(HeightFogEffectiveDistance(50.0f, 10.0f, 0.0f, 0.0f, 0.0f) == 50.0f);
    // 水平視線でカメラ高さ = 基準高さ → 等倍 (kd=0 の縮退分岐)
    TEST_CHECK(std::fabs(HeightFogEffectiveDistance(50.0f, 0.0f, 0.0f, 0.5f, 0.0f) - 50.0f)
               < 1e-3f);
    // 高所の水平視線 → e^{-k·camY} で減衰 (k=0.5, camY=10 → e^-5 ≈ 0.674%)
    const float high = HeightFogEffectiveDistance(50.0f, 10.0f, 10.0f, 0.5f, 0.0f);
    TEST_CHECK(high > 0.0f && high < 1.0f);
    // baseHeight を camY まで持ち上げると等倍に戻る (基準の意味)
    TEST_CHECK(std::fabs(HeightFogEffectiveDistance(50.0f, 10.0f, 10.0f, 0.5f, 10.0f) - 50.0f)
               < 1e-3f);
    // 積分は視線の向きに依らない: cam(0)→pos(10) と cam(10)→pos(0) で同じ実効距離
    const float up = HeightFogEffectiveDistance(50.0f, 0.0f, 10.0f, 0.5f, 0.0f);
    const float down = HeightFogEffectiveDistance(50.0f, 10.0f, 0.0f, 0.5f, 0.0f);
    TEST_CHECK(std::fabs(up - down) < 1e-2f);
    // 下向き (地表へ) は高所の水平より濃い
    TEST_CHECK(down > high);

    // インスキャッタ: 正対 → 1 / 直交 → 0 (sunDir は光の進行方向 = 太陽は逆側)
    const XMFLOAT3 sunDir = { 0.0f, 0.0f, -1.0f }; // 太陽は +Z 側
    TEST_CHECK(std::fabs(SunInscatterFactor({ 0.0f, 0.0f, 1.0f }, sunDir, 8.0f) - 1.0f) < 1e-4f);
    TEST_CHECK(SunInscatterFactor({ 1.0f, 0.0f, 0.0f }, sunDir, 8.0f) < 1e-4f);
    // 太陽を背にする (dot<0) → 0
    TEST_CHECK(SunInscatterFactor({ 0.0f, 0.0f, -1.0f }, sunDir, 8.0f) == 0.0f);
    // power が大きいほど 45° の係数が小さい (ピークが鋭い)
    const XMFLOAT3 diag = { 0.70710678f, 0.0f, 0.70710678f };
    TEST_CHECK(SunInscatterFactor(diag, sunDir, 8.0f) < SunInscatterFactor(diag, sunDir, 2.0f));
}

// M43b: 太陽のスクリーン位置 (ゴッドレイの放射中心)。カメラは原点から +Z (view=単位行列)
void TestSunScreenPos()
{
    MYE_LOG_INFO("[selftest] sun screen position (M43b)");
    XMFLOAT4X4 view, proj;
    XMStoreFloat4x4(&view, XMMatrixIdentity());
    XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 1.0f, 0.1f, 100.0f));
    float u = 0.0f, v = 0.0f;
    // 正面 (太陽が視線の先 = sunDir は -Z): 画面中央、フェード 1
    TEST_CHECK(ComputeSunScreenPos(view, proj, { 0.0f, 0.0f, -1.0f }, u, v) == 1.0f);
    TEST_CHECK(std::fabs(u - 0.5f) < 1e-4f && std::fabs(v - 0.5f) < 1e-4f);
    // 背面 (太陽がカメラの後ろ = sunDir は +Z): フェード 0
    TEST_CHECK(ComputeSunScreenPos(view, proj, { 0.0f, 0.0f, 1.0f }, u, v) == 0.0f);
    // 画面端の少し外 (右 35°、FOV 半角 30°): u>1 で部分フェード (0,1)
    const float f35 = ComputeSunScreenPos(
        view, proj, { -std::sin(XMConvertToRadians(35.0f)), 0.0f,
                      -std::cos(XMConvertToRadians(35.0f)) }, u, v);
    TEST_CHECK(u > 1.0f && f35 > 0.0f && f35 < 1.0f);
    // 大きく外 (右 60°): フェード 0
    TEST_CHECK(ComputeSunScreenPos(view, proj, { -0.866f, 0.0f, -0.5f }, u, v) == 0.0f);
    // 上方向 (太陽が真上 45°、FOV 内): v < 0.5 (スクリーン上半分)
    const float fup = ComputeSunScreenPos(view, proj, { 0.0f, -0.5f, -0.866f }, u, v);
    TEST_CHECK(fup > 0.0f && v < 0.5f);
}

// M44a: LUT ストリップの UV (postfx_tonemap.hlsl::SampleLutStrip のミラー検証)
void TestLutStripUv()
{
    MYE_LOG_INFO("[selftest] LUT strip UV (M44a)");
    float u0 = 0, u1 = 0, v = 0, f = 0;
    // 黒: スライス 0 のテクセル (0,0) 中心、補間 0
    LutStripUv(0.0f, 0.0f, 0.0f, u0, u1, v, f);
    TEST_CHECK(std::fabs(u0 - 0.5f / 256.0f) < 1e-6f && std::fabs(v - 0.5f / 16.0f) < 1e-6f
               && f == 0.0f);
    // 白: 最終スライス 15 のテクセル (15,15) 中心、u0==u1 (クランプ)
    LutStripUv(1.0f, 1.0f, 1.0f, u0, u1, v, f);
    TEST_CHECK(std::fabs(u0 - 255.5f / 256.0f) < 1e-6f && u0 == u1
               && std::fabs(v - 15.5f / 16.0f) < 1e-6f && f == 0.0f);
    // セル境界: b=0.5 はスライス 7/8 の中間 (frac=0.5)
    LutStripUv(0.5f, 0.5f, 0.5f, u0, u1, v, f);
    TEST_CHECK(std::fabs(f - 0.5f) < 1e-6f);
    TEST_CHECK(std::fabs(u0 - 120.0f / 256.0f) < 1e-6f && std::fabs(u1 - 136.0f / 256.0f) < 1e-6f);
    // 範囲外はクランプ (負/超過)
    LutStripUv(-1.0f, 2.0f, -0.5f, u0, u1, v, f);
    TEST_CHECK(std::fabs(u0 - 0.5f / 256.0f) < 1e-6f && std::fabs(v - 15.5f / 16.0f) < 1e-6f
               && f == 0.0f);
}

// M44b: 自動露出のヒストグラム量子化 (postfx_hist*.cs.hlsl のミラー検証)
void TestAutoExposureBins()
{
    MYE_LOG_INFO("[selftest] auto exposure histogram bins (M44b)");
    // ほぼ黒はレンジ外 bin 0 (平均から除外される側)
    TEST_CHECK(BinForLuminance(0.0f) == 0 && BinForLuminance(-1.0f) == 0);
    // レンジ両端: 2^-10 → bin 1 / 2^6 → bin 255 / 超過はクランプ
    TEST_CHECK(BinForLuminance(std::exp2(-10.0f)) == 1);
    TEST_CHECK(BinForLuminance(std::exp2(6.0f)) == 255);
    TEST_CHECK(BinForLuminance(1000.0f) == 255);
    // 単調性
    TEST_CHECK(BinForLuminance(0.1f) < BinForLuminance(0.5f)
               && BinForLuminance(0.5f) < BinForLuminance(2.0f));
    // 往復: bin 幅 (2^(16/254) ≈ ±4.5%) 以内で復元される
    const float lum = 1.0f;
    const float back = LumForBin(BinForLuminance(lum));
    TEST_CHECK(back > lum * 0.95f && back < lum * 1.05f);
    // 逆量子化の両端
    TEST_CHECK(std::fabs(LumForBin(1) - std::exp2(-10.0f)) < 1e-5f);
    TEST_CHECK(std::fabs(LumForBin(255) - std::exp2(6.0f)) < 1e-2f);
}

// M44c: 符号付き CoC (postfx_dof_prefilter/composite.hlsl のミラー検証)
void TestSignedCoC()
{
    MYE_LOG_INFO("[selftest] DoF signed CoC (M44c)");
    // 焦点面ちょうど → 0
    TEST_CHECK(SignedCoC(10.0f, 10.0f, 5.0f) == 0.0f);
    // 手前が負 / 奥が正 (半レンジで ±0.5)
    TEST_CHECK(std::fabs(SignedCoC(7.5f, 10.0f, 5.0f) + 0.5f) < 1e-6f);
    TEST_CHECK(std::fabs(SignedCoC(12.5f, 10.0f, 5.0f) - 0.5f) < 1e-6f);
    // レンジ超過は ±1 に clamp
    TEST_CHECK(SignedCoC(100.0f, 10.0f, 5.0f) == 1.0f);
    TEST_CHECK(SignedCoC(0.1f, 10.0f, 5.0f) == -1.0f);
    // range 0 ガード (ゼロ除算しない)
    TEST_CHECK(SignedCoC(11.0f, 10.0f, 0.0f) == 1.0f);
}

// M55a: 深度線形化 (common.hlsli::LinearizeDepth のミラー検証)。
// M55a 以前は同じ式が 5 つのシェーダにローカルコピーで散っていて CPU 側の検査も
// パーティクル文脈 (LinearizeParticleDepth の端点 2 点) しか無かった。共有版になったので
// 「実際の透視投影行列が吐く NDC 深度を戻せるか」まで踏み込んで固定する
void TestLinearizeDepth()
{
    MYE_LOG_INFO("[selftest] depth linearization (M55a)");
    // near=1/far=100: far-(far-near) が桁落ちしない組 (0.1/1000 だと端点が ~0.02% ずれる)
    const float n = 1.0f;
    const float f = 100.0f;
    // 端点: d=0 → near / d=1 → far
    TEST_CHECK(std::fabs(LinearizeDepth(0.0f, n, f) - n) < 1e-4f);
    TEST_CHECK(std::fabs(LinearizeDepth(1.0f, n, f) - f) < 1e-3f);
    // 実際の透視投影が吐く NDC 深度から元のビュー z を復元できること。
    // 行ベクトル規約: clip = mul(float4(0,0,z,1), proj)、d = clip.z / clip.w
    XMFLOAT4X4 proj;
    XMStoreFloat4x4(&proj,
                    XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f, n, f));
    for (const float viewZ : { 1.5f, 4.0f, 12.5f, 50.0f }) {
        const XMVECTOR clip =
            XMVector4Transform(XMVectorSet(0.0f, 0.0f, viewZ, 1.0f), XMLoadFloat4x4(&proj));
        const float d = XMVectorGetZ(clip) / XMVectorGetW(clip);
        TEST_CHECK(std::fabs(LinearizeDepth(d, n, f) - viewZ) < viewZ * 1e-4f);
    }
    // 単調増加 (深度が大きいほど遠い)
    TEST_CHECK(LinearizeDepth(0.25f, n, f) < LinearizeDepth(0.75f, n, f));
    // near==0 かつ d==1 のゼロ除算ガード (分母の 1e-4 クランプ)。HLSL 側と同じ形で入っている
    TEST_CHECK(std::isfinite(LinearizeDepth(1.0f, 0.0f, f)));
}

// M44d: 深度再投影 (postfx_motionblur.hlsl のミラー検証)
void TestReprojectUv()
{
    MYE_LOG_INFO("[selftest] motion blur reprojection (M44d)");
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    float pu = 0.0f, pv = 0.0f;
    // 恒等: カメラ不動 → prevUV == uv (速度 0)
    TEST_CHECK(ReprojectUv(identity, identity, 0.3f, 0.7f, 0.5f, pu, pv));
    TEST_CHECK(std::fabs(pu - 0.3f) < 1e-5f && std::fabs(pv - 0.7f) < 1e-5f);
    // 既知の平行移動: 前フレームがワールド +X に 0.2 ずれた投影 → prevU = u + 0.1
    // (NDC 幅 2 に対する +0.2 = UV では +0.1。y は v 反転規約で不変)
    XMFLOAT4X4 shifted;
    XMStoreFloat4x4(&shifted, XMMatrixTranslation(0.2f, 0.0f, 0.0f));
    TEST_CHECK(ReprojectUv(identity, shifted, 0.5f, 0.5f, 0.5f, pu, pv));
    TEST_CHECK(std::fabs(pu - 0.6f) < 1e-5f && std::fabs(pv - 0.5f) < 1e-5f);
    // 前フレームで背面 (透視 w<=0) → false (ブラーしない)
    XMFLOAT4X4 persp;
    XMStoreFloat4x4(&persp, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 1.0f,
                                                     0.1f, 100.0f));
    // 恒等 invViewProj で depth=-1 → ワールド z=-1 (カメラ背面) → 透視 w<0
    TEST_CHECK(!ReprojectUv(identity, persp, 0.5f, 0.5f, -1.0f, pu, pv));
}

// M55e: モーションブラーの速度源の選択 + クランプ (postfx_motionblur.hlsl のミラー検証)。
// **この関数が M55e の唯一の機械検査** — golden は motionBlurIntensity=0 (既定) しか
// 押さえていないので、on 側の挙動は絵からは確かめられない
void TestMotionBlurVelocity()
{
    MYE_LOG_INFO("[selftest] motion blur velocity source (M55e)");
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    XMFLOAT4X4 shifted; // 前フレームがワールド +X に 0.2 ずれた投影 = カメラが動いた状態
    XMStoreFloat4x4(&shifted, XMMatrixTranslation(0.2f, 0.0f, 0.0f));
    const float w = 960.0f;
    const float h = 540.0f;
    const float kNoClamp = 1.0e6f; // クランプを見たいケース以外は効かせない (px 長で効くため)
    float du = 0.0f, dv = 0.0f;

    // ① velocity 無し (Forward) → v1 と完全に同じ深度再投影。カメラ不動なら速度 0
    TEST_CHECK(motionblur::BlurVector(false, 0.0f, 0.0f, identity, identity, 0.3f, 0.7f, 0.5f,
                                      1.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(du == 0.0f && dv == 0.0f);
    // ② velocity 無し + カメラが動いた → uv - prevUv = -0.1 (prevU = u + 0.1)
    TEST_CHECK(motionblur::BlurVector(false, 0.0f, 0.0f, identity, shifted, 0.5f, 0.5f, 0.5f,
                                      1.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(std::fabs(du + 0.1f) < 1e-5f && std::fabs(dv) < 1e-5f);

    // ③ ★M55e の本題: velocity があるジオメトリ画素は **カメラが静止していても**
    //    バッファの値をそのまま使う (v1 ではここが 0 = 回る物体がブレなかった)
    TEST_CHECK(motionblur::BlurVector(true, 0.01f, -0.004f, identity, identity, 0.5f, 0.5f, 0.4f,
                                      1.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(std::fabs(du - 0.01f) < 1e-6f && std::fabs(dv + 0.004f) < 1e-6f);

    // ④ ★背景 (depth==1.0) は velocity バッファがあっても再投影へ落ちる。
    //    GBuffer を書いていない画素の RT4 は 0 のままなので、ここを分けないと
    //    「カメラを振っても空だけ止まる」= v1 より悪い絵になる
    TEST_CHECK(motionblur::BlurVector(true, 0.0f, 0.0f, identity, shifted, 0.5f, 0.5f, 1.0f,
                                      1.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(std::fabs(du + 0.1f) < 1e-5f && std::fabs(dv) < 1e-5f);

    // ⑤ intensity は速度に線形に効く (0 = 恒等)
    TEST_CHECK(motionblur::BlurVector(true, 0.01f, 0.0f, identity, identity, 0.5f, 0.5f, 0.4f,
                                      0.5f, kNoClamp, w, h, du, dv));
    TEST_CHECK(std::fabs(du - 0.005f) < 1e-6f);
    TEST_CHECK(motionblur::BlurVector(true, 0.01f, 0.0f, identity, identity, 0.5f, 0.5f, 0.4f,
                                      0.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(du == 0.0f && dv == 0.0f);

    // ⑥ クランプは px 長で効く: du=0.5 → 480px を maxPixels=16 へ切り詰める
    TEST_CHECK(motionblur::BlurVector(true, 0.5f, 0.0f, identity, identity, 0.5f, 0.5f, 0.4f,
                                      1.0f, 16.0f, w, h, du, dv));
    TEST_CHECK(std::fabs(du * w - 16.0f) < 1e-3f && dv == 0.0f);
    // 縦方向は h でスケールされる (UV 長でクランプしていたら 16/540 にならない)
    TEST_CHECK(motionblur::BlurVector(true, 0.0f, 0.5f, identity, identity, 0.5f, 0.5f, 0.4f,
                                      1.0f, 16.0f, w, h, du, dv));
    TEST_CHECK(std::fabs(dv * h - 16.0f) < 1e-3f && du == 0.0f);

    // ⑦ 前フレームで背面 = ブラーしない (velocity 側の経路には無い縮退)
    XMFLOAT4X4 persp;
    XMStoreFloat4x4(&persp, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 1.0f, 0.1f,
                                                     100.0f));
    TEST_CHECK(!motionblur::BlurVector(false, 0.0f, 0.0f, identity, persp, 0.5f, 0.5f, -1.0f,
                                       1.0f, kNoClamp, w, h, du, dv));
    TEST_CHECK(du == 0.0f && dv == 0.0f);
}

// 自己発光の G-Buffer 符号化 (M46i)。common.hlsli の EncodeEmissive/DecodeEmissive と対
void TestEmissiveEncoding()
{
    MYE_LOG_INFO("[selftest] emissive encode/decode (G-Buffer b channel)");
    const float kMax = static_cast<float>(kEmissiveMaxIntensity);

    // ★受け入れ基準の核: 発光なし → 符号化値も復号値も厳密に 0。
    //   ライトパスの加算項がちょうど 0 になるので M46i 以前とビット一致する
    TEST_CHECK(EncodeEmissive(0.0f) == 0.0f);
    TEST_CHECK(DecodeEmissive(0.0f) == 0.0f);
    TEST_CHECK(DecodeEmissive(EncodeEmissive(0.0f)) == 0.0f);

    // 往復 (量子化前の実数域では厳密に戻る値を選ぶ: kMax の 2 冪分の 1)
    for (float v : { 0.5f, 1.0f, 2.0f, 4.0f, kMax }) {
        TEST_CHECK(DecodeEmissive(EncodeEmissive(v)) == v);
    }

    // 上限で飽和し、それを超えても 1.0 (= kMax) で頭打ち
    TEST_CHECK(EncodeEmissive(kMax) == 1.0f);
    TEST_CHECK(EncodeEmissive(kMax * 2.0f) == 1.0f);
    TEST_CHECK(DecodeEmissive(EncodeEmissive(kMax * 100.0f)) == kMax);

    // 負値は 0 へクランプ (saturate 相当)。マテリアルに負の強度が入っても黒くならない
    TEST_CHECK(EncodeEmissive(-1.0f) == 0.0f);

    // 単調増加 + 8bit UNORM に落としても順序が保たれる
    float prev = -1.0f;
    for (int i = 0; i <= 16; ++i) {
        const float e = EncodeEmissive(kMax * static_cast<float>(i) / 16.0f);
        TEST_CHECK(e > prev);
        prev = e;
    }

    // R8G8B8A8_UNORM の量子化誤差は 1 段 = kMax/255 未満に収まる
    const float quantStep = kMax / 255.0f;
    for (float v : { 0.1f, 1.3f, 3.7f, 6.0f }) {
        const float roundTrip =
            DecodeEmissive(std::round(EncodeEmissive(v) * 255.0f) / 255.0f);
        TEST_CHECK(std::fabs(roundTrip - v) <= quantStep * 0.5f + 1e-5f);
    }
}

// M55b: TAA 用カメラジッタ。ここで守りたい性質は 4 つ —
//  ① 列が frame index の純関数 (実時間も rand() も入らない = 決定的撮影で再現する)
//  ② 振幅 0 で射影行列が 1 ビットも変わらない (既定の絵が動かない受入基準そのもの)
//  ③ NDC オフセットが「指定したサブピクセル量」ちょうどになる (透視 / 正射影の両方)
//  ④ 1 周期の平均が原点付近 (偏ると TAA の収束先が本来の絵からずれる)
void TestCameraJitter()
{
    using namespace camerajitter;

    // ① radical inverse の既知値 (van der Corput)
    TEST_CHECK(RadicalInverse(1, 2) == 0.5f);
    TEST_CHECK(RadicalInverse(2, 2) == 0.25f);
    TEST_CHECK(RadicalInverse(3, 2) == 0.75f);
    TEST_CHECK(RadicalInverse(0, 2) == 0.0f);
    TEST_CHECK(std::fabs(RadicalInverse(1, 3) - 1.0f / 3.0f) < 1e-6f);
    TEST_CHECK(std::fabs(RadicalInverse(2, 3) - 2.0f / 3.0f) < 1e-6f);

    // ① 純関数 = 同じ frame index なら何度呼んでも同じ / 周期 kSequenceLength で厳密に一巡
    float sx = 0.0f, sy = 0.0f, tx = 0.0f, ty = 0.0f;
    Sample(3, sx, sy);
    Sample(3, tx, ty);
    TEST_CHECK(sx == tx && sy == ty);
    Sample(3 + kSequenceLength, tx, ty);
    TEST_CHECK(sx == tx && sy == ty);

    // ④ 全サンプルが [-0.5,0.5] に収まり、原点ちょうど (= 揺れないフレーム) が無く、
    //    1 周期の平均が原点近傍
    float sumX = 0.0f, sumY = 0.0f;
    for (uint32_t i = 0; i < kSequenceLength; ++i) {
        float jx = 0.0f, jy = 0.0f;
        Sample(i, jx, jy);
        TEST_CHECK(jx >= -0.5f && jx <= 0.5f && jy >= -0.5f && jy <= 0.5f);
        TEST_CHECK(jx != 0.0f || jy != 0.0f);
        sumX += jx;
        sumY += jy;
    }
    TEST_CHECK(std::fabs(sumX / kSequenceLength) < 0.1f);
    TEST_CHECK(std::fabs(sumY / kSequenceLength) < 0.1f);

    // ピクセル → NDC。y は符号反転 (NDC は上向き、ピクセルは下向き)。
    // 幅/高さ 0 は 0 を返す (ヘッドレスや未リサイズのビューでゼロ除算しない)
    float nx = 0.0f, ny = 0.0f;
    PixelsToNdc(0.5f, 0.5f, 960, 540, nx, ny);
    TEST_CHECK(std::fabs(nx - 1.0f / 960.0f) < 1e-7f);
    TEST_CHECK(std::fabs(ny + 1.0f / 540.0f) < 1e-7f);
    PixelsToNdc(0.5f, 0.5f, 0, 0, nx, ny);
    TEST_CHECK(nx == 0.0f && ny == 0.0f);

    XMFLOAT4X4 persp;
    XMStoreFloat4x4(&persp, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f,
                                                     0.1f, 1000.0f));
    // ② 振幅 0 = ビット同一 (memcmp。「ほぼ同じ」では受入基準にならない)
    const XMFLOAT4X4 zero = ApplyToProj(persp, 0.0f, 0.0f);
    TEST_CHECK(std::memcmp(&zero, &persp, sizeof(XMFLOAT4X4)) == 0);

    // ③ 透視: view 空間の任意の点の NDC が、深度に依らず指定量ちょうどずれる
    const XMFLOAT4X4 jit = ApplyToProj(persp, 0.02f, -0.03f);
    for (float z : { 1.0f, 10.0f, 250.0f }) {
        const XMVECTOR pv = XMVectorSet(3.0f, -2.0f, z, 1.0f);
        const XMVECTOR a = XMVector4Transform(pv, XMLoadFloat4x4(&persp));
        const XMVECTOR b = XMVector4Transform(pv, XMLoadFloat4x4(&jit));
        const float aw = XMVectorGetW(a);
        const float bw = XMVectorGetW(b);
        TEST_CHECK(aw == bw); // w (= viewZ) は不変 = 深度は 1 ビットも動かない
        TEST_CHECK(std::fabs((XMVectorGetX(b) / bw - XMVectorGetX(a) / aw) - 0.02f) < 1e-5f);
        TEST_CHECK(std::fabs((XMVectorGetY(b) / bw - XMVectorGetY(a) / aw) + 0.03f) < 1e-5f);
    }

    // ③ 正射影 (エディタの Ortho ビュー): w=1 なので平行移動は _41/_42 側に載る。
    // _31/_32 に足すと view z に比例した歪みになるので、経路を間違えるとここで落ちる
    XMFLOAT4X4 ortho;
    XMStoreFloat4x4(&ortho, XMMatrixOrthographicLH(19.2f, 10.8f, 0.1f, 1000.0f));
    const XMFLOAT4X4 orthoJit = ApplyToProj(ortho, 0.02f, -0.03f);
    for (float z : { 1.0f, 10.0f, 250.0f }) {
        const XMVECTOR pv = XMVectorSet(3.0f, -2.0f, z, 1.0f);
        const XMVECTOR a = XMVector4Transform(pv, XMLoadFloat4x4(&ortho));
        const XMVECTOR b = XMVector4Transform(pv, XMLoadFloat4x4(&orthoJit));
        TEST_CHECK(std::fabs((XMVectorGetX(b) - XMVectorGetX(a)) - 0.02f) < 1e-5f);
        TEST_CHECK(std::fabs((XMVectorGetY(b) - XMVectorGetY(a)) + 0.03f) < 1e-5f);
        TEST_CHECK(XMVectorGetZ(a) == XMVectorGetZ(b));
    }
}

// M55c: 画面速度 (common.hlsli::ComputeVelocityUv のミラー検証)。
// velocity を読む消費者は M55d 以降まで居ない = **ピクセル回帰では 1 ミリも被覆できない**
// ので、式の正しさはここで固定するしかない
void TestVelocityUv()
{
    MYE_LOG_INFO("[selftest] velocity buffer (M55c)");
    XMFLOAT4X4 proj;
    XMStoreFloat4x4(&proj,
                    XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f));
    XMFLOAT4X4 viewM;
    XMStoreFloat4x4(&viewM, XMMatrixIdentity()); // カメラは原点・+Z 向き
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, XMLoadFloat4x4(&viewM) * XMLoadFloat4x4(&proj));

    const XMFLOAT3 p = { 1.0f, 0.5f, 8.0f };
    float u = 1.0f;
    float v = 1.0f;

    // ① 静止 (カメラも物体も動かない) → 厳密に 0。これが「既定で絵が変わらない」の根拠
    TEST_CHECK(velocity::FromWorld(vp, vp, p, p, 0.0f, 0.0f, u, v));
    TEST_CHECK(u == 0.0f && v == 0.0f);

    // ② 物体が +X へ動いた → velocity は「今 UV − 前 UV」= 正の u。
    //    大きさは UV 差そのものと一致すること (符号規約 prevUv = uv - velocity の担保)
    const XMFLOAT3 prevPos = { 0.5f, 0.5f, 8.0f };
    TEST_CHECK(velocity::FromWorld(vp, vp, p, prevPos, 0.0f, 0.0f, u, v));
    TEST_CHECK(u > 0.0f);
    {
        auto uvOf = [&](const XMFLOAT3& w) {
            const XMVECTOR c =
                XMVector4Transform(XMVectorSet(w.x, w.y, w.z, 1.0f), XMLoadFloat4x4(&vp));
            const float cw = XMVectorGetW(c);
            return XMFLOAT2{ XMVectorGetX(c) / cw * 0.5f + 0.5f,
                             0.5f - XMVectorGetY(c) / cw * 0.5f };
        };
        const XMFLOAT2 cur = uvOf(p);
        const XMFLOAT2 old = uvOf(prevPos);
        TEST_CHECK(std::fabs((cur.x - old.x) - u) < 1e-6f);
        TEST_CHECK(std::fabs((cur.y - old.y) - v) < 1e-6f);
    }

    // ③ ★核心: **ジッタを載せても velocity は変わらない**。
    //    今フレームだけジッタ込みの proj でラスタライズされ、prevViewProj は非ジッタ —
    //    引き戻しを忘れると静止物が毎フレーム半ピクセル動いて TAA が履歴を外す
    const float jx = 0.0031f;
    const float jy = -0.0047f;
    const XMFLOAT4X4 projJit = camerajitter::ApplyToProj(proj, jx, jy);
    XMFLOAT4X4 vpJit;
    XMStoreFloat4x4(&vpJit, XMLoadFloat4x4(&viewM) * XMLoadFloat4x4(&projJit));
    float uj = 0.0f;
    float vj = 0.0f;
    TEST_CHECK(velocity::FromWorld(vpJit, vp, p, p, jx, jy, uj, vj));
    TEST_CHECK(std::fabs(uj) < 1e-6f && std::fabs(vj) < 1e-6f);
    TEST_CHECK(velocity::FromWorld(vpJit, vp, p, prevPos, jx, jy, uj, vj));
    TEST_CHECK(std::fabs(uj - u) < 1e-6f && std::fabs(vj - v) < 1e-6f);
    // 引き戻しを忘れた場合はちょうどジッタ分ずれる (この試験に歯があることの確認)
    float uBad = 0.0f;
    float vBad = 0.0f;
    TEST_CHECK(velocity::FromWorld(vpJit, vp, p, p, 0.0f, 0.0f, uBad, vBad));
    TEST_CHECK(std::fabs(uBad - jx * 0.5f) < 1e-6f);

    // ④ カメラ背面 (w<=0) は false + 速度 0 (消費側はカメラ再投影のみへ縮退)
    const XMFLOAT3 behind = { 0.0f, 0.0f, -5.0f };
    TEST_CHECK(!velocity::FromWorld(vp, vp, behind, behind, 0.0f, 0.0f, u, v));
    TEST_CHECK(u == 0.0f && v == 0.0f);
}

// M55c: 「前フレームに実際に描いた world 行列」ストア。
// ★★決定的撮影モードでは interpAlpha == 1.0 なので「前 tick の行列で代用する」誤りは
// golden に一切現れない。だから通番の連続性判定はここで機械検査するしかない
void TestPrevRenderWorldStore()
{
    MYE_LOG_INFO("[selftest] prev-render world store (M55c)");
    PrevRenderWorldStore s;
    const EntityID a = { 3, 0 };
    const EntityID b = { 7, 0 };
    XMFLOAT4X4 m0;
    XMStoreFloat4x4(&m0, XMMatrixTranslation(1.0f, 0.0f, 0.0f));
    XMFLOAT4X4 m1;
    XMStoreFloat4x4(&m1, XMMatrixTranslation(2.0f, 0.0f, 0.0f));

    // ① 初フレーム: 履歴なし
    TEST_CHECK(!s.Begin(0, 960, 540));
    TEST_CHECK(s.Lookup(a) == nullptr);
    s.Record(a, m0);
    s.Record(b, m0);

    // ② 次フレーム: 通番が 1 つ違い + 同サイズ → 前フレームの行列が引ける
    TEST_CHECK(s.Begin(1, 960, 540));
    const XMFLOAT4X4* got = s.Lookup(a);
    TEST_CHECK(got != nullptr && std::memcmp(got, &m0, sizeof(XMFLOAT4X4)) == 0);
    s.Record(a, m1);
    // b は今フレーム描かれなかった (Record しない) — 次フレームで拾われないこと

    // ③ さらに次: a は m1、b は「前フレームに描かれていない」ので null
    TEST_CHECK(s.Begin(2, 960, 540));
    got = s.Lookup(a);
    TEST_CHECK(got != nullptr && std::memcmp(got, &m1, sizeof(XMFLOAT4X4)) == 0);
    TEST_CHECK(s.Lookup(b) == nullptr);
    s.Record(a, m1);

    // ④ 通番が飛んだ (別ビューを挟んだ / このビューが 1 フレーム描かれなかった) → 破棄
    TEST_CHECK(!s.Begin(5, 960, 540));
    TEST_CHECK(s.Lookup(a) == nullptr);
    s.Record(a, m0);

    // ⑤ リサイズ → 破棄 (再投影の前提が崩れる)
    TEST_CHECK(!s.Begin(6, 1280, 720));
    TEST_CHECK(s.Lookup(a) == nullptr);
    s.Record(a, m0);

    // ⑥ index 再利用 (generation 違い) は別エンティティとして扱う
    TEST_CHECK(s.Begin(7, 1280, 720));
    TEST_CHECK(s.Lookup(a) != nullptr);
    const EntityID reused = { 3, 1 };
    TEST_CHECK(s.Lookup(reused) == nullptr);
}

// M55d: TAA の解決式 (postfx_taa.hlsl::PSMain のミラー検証)。
// ★golden (demo_render_taa) は「TAA を on にした 1 枚」しか押さえない = **恒等の側**
//   (履歴が無い / 画面外 / feedback=0 で cur がビット単位でそのまま出ること) は
//   絵からは確かめられない。既定 off の受け入れ基準を支えているのはここ
void TestTaaResolve()
{
    MYE_LOG_INFO("[selftest] TAA resolve (M55d)");
    const XMFLOAT3 cur = { 0.40f, 0.50f, 0.60f };
    const XMFLOAT3 hist = { 0.80f, 0.10f, 0.60f };
    // 近傍の箱は履歴を丸ごと含む広さ (クランプが効かない条件) にしておく
    const XMFLOAT3 wideMin = { 0.0f, 0.0f, 0.0f };
    const XMFLOAT3 wideMax = { 1.0f, 1.0f, 1.0f };

    auto same = [](const XMFLOAT3& a, const XMFLOAT3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z; // ビット同一を要求する
    };

    // ① 履歴なし (初回フレーム / 通番が飛んだ / リサイズ直後) → cur がそのまま出る。
    //    ★shot_verify は --frames 6 --shot-frame 3 = 撮影時点で履歴 3 枚しかない。
    //    「収束後の絵」前提の実装だとここが崩れる
    TEST_CHECK(same(taa::Resolve(cur, hist, wideMin, wideMax, 0.5f, 0.5f, false, 0.9f), cur));

    // ② 履歴 UV が画面外 → 前フレームにその画素は無いので cur。境界は [0,1) の半開区間
    TEST_CHECK(same(taa::Resolve(cur, hist, wideMin, wideMax, -0.001f, 0.5f, true, 0.9f), cur));
    TEST_CHECK(same(taa::Resolve(cur, hist, wideMin, wideMax, 0.5f, 1.0f, true, 0.9f), cur));
    TEST_CHECK(taa::HistoryUvValid(0.0f, 0.0f) && !taa::HistoryUvValid(1.0f, 0.0f));

    // ③ feedback=0 → 履歴が有効でも cur がそのまま (A/B 比較の恒等点)
    TEST_CHECK(same(taa::Resolve(cur, hist, wideMin, wideMax, 0.5f, 0.5f, true, 0.0f), cur));

    // ④ 通常経路: cur と履歴の線形補間。feedback=0.5 なら中点
    {
        const XMFLOAT3 r = taa::Resolve(cur, hist, wideMin, wideMax, 0.5f, 0.5f, true, 0.5f);
        TEST_CHECK(std::fabs(r.x - 0.60f) < 1e-6f);
        TEST_CHECK(std::fabs(r.y - 0.30f) < 1e-6f);
        TEST_CHECK(std::fabs(r.z - 0.60f) < 1e-6f); // 同値の成分は動かない
    }

    // ⑤ 近傍クランプ = ゴースト抑制の本体。履歴が今フレームの近傍の箱から外れていたら
    //    箱の面まで引き寄せてから混ぜる (遮蔽が解けた画素で前の像が残らない)
    {
        const XMFLOAT3 nmin = { 0.35f, 0.45f, 0.55f };
        const XMFLOAT3 nmax = { 0.45f, 0.55f, 0.65f };
        const XMFLOAT3 c = taa::ClampToNeighborhood(hist, nmin, nmax);
        TEST_CHECK(c.x == 0.45f && c.y == 0.45f && c.z == 0.60f);
        const XMFLOAT3 r = taa::Resolve(cur, hist, nmin, nmax, 0.5f, 0.5f, true, 1.0f);
        TEST_CHECK(same(r, c)); // feedback=1 ならクランプ後の履歴そのもの
    }

    // ⑥ 収束済みの静止画 (cur == hist、速度 0) は何フレーム回しても値が動かない。
    //    これが崩れると「止まっているのに絵がじりじり変わる」= 決定的撮影が成立しない
    {
        XMFLOAT3 acc = cur;
        for (int i = 0; i < 8; ++i) {
            acc = taa::Resolve(cur, acc, cur, cur, 0.5f, 0.5f, true, 0.9f);
        }
        TEST_CHECK(same(acc, cur));
    }
}

// M57a: フロクセルグリッドの幾何 (RenderTypes.h の mye::froxel)。
// GPU を起こさずに検査できるのはここまで — 3D UAV が本当に書けるかは
// `Editor.exe --froxel-probe` (実デバイスが要る) 側の担当
void TestFroxelGrid()
{
    MYE_LOG_INFO("[selftest] froxel grid geometry");

    // ① ディスパッチのグループ数は切り上げ。**90 は 8 で割り切れない**ので、
    //    切り捨てだと最後の 2 行が永久に書かれない (= 前フレームの残骸を積分する)
    TEST_CHECK(froxel::DispatchGroups(160, 8) == 20);
    TEST_CHECK(froxel::DispatchGroups(90, 8) == 12); // 11.25 → 12
    TEST_CHECK(froxel::DispatchGroups(1, 8) == 1);
    TEST_CHECK(froxel::DispatchGroups(0, 8) == 0); // 空グリッドは空振り
    // グループ数 x スレッド数がグリッドを必ず覆う (シェーダ側の範囲外判定の前提)
    TEST_CHECK(froxel::DispatchGroups(froxel::kGridY, froxel::kGroupSize) * froxel::kGroupSize
               >= froxel::kGridY);

    // ② スライス境界は near から far まで単調増加し、両端はぴったり一致する
    const int slices = froxel::kGridZ;
    const float nearZ = 0.1f;
    const float farZ = 200.0f;
    TEST_CHECK(std::fabs(froxel::SliceToViewDepth(0.0f, slices, nearZ, farZ) - nearZ) < 1e-4f);
    TEST_CHECK(std::fabs(froxel::SliceToViewDepth(static_cast<float>(slices), slices, nearZ, farZ)
                         - farZ)
               < 1e-2f);
    bool monotonic = true;
    for (int s = 1; s <= slices; ++s) {
        const float prev = froxel::SliceToViewDepth(static_cast<float>(s - 1), slices, nearZ, farZ);
        const float cur = froxel::SliceToViewDepth(static_cast<float>(s), slices, nearZ, farZ);
        monotonic = monotonic && (cur > prev);
    }
    TEST_CHECK(monotonic);

    // ③ 指数分布 = 手前のスライスのほうが薄い。等間隔にすると近景が 1 枚に潰れて縞が出る
    const float firstThickness = froxel::SliceToViewDepth(1.0f, slices, nearZ, farZ)
        - froxel::SliceToViewDepth(0.0f, slices, nearZ, farZ);
    const float lastThickness
        = froxel::SliceToViewDepth(static_cast<float>(slices), slices, nearZ, farZ)
        - froxel::SliceToViewDepth(static_cast<float>(slices - 1), slices, nearZ, farZ);
    TEST_CHECK(firstThickness < lastThickness);

    // ④ 深度 → スライス → 深度の往復。M57c の再投影がこの逆関数に乗るので、
    //    片方だけ式を直したときに気付ける形にしておく
    bool roundTrip = true;
    for (int s = 0; s <= slices; ++s) {
        const float d = froxel::SliceToViewDepth(static_cast<float>(s), slices, nearZ, farZ);
        const float back = froxel::ViewDepthToSlice(d, slices, nearZ, farZ);
        roundTrip = roundTrip && (std::fabs(back - static_cast<float>(s)) < 1e-2f);
    }
    TEST_CHECK(roundTrip);

    // ⑤ 退化した入力でも NaN を出さない (nearZ=0 は log の発散点。CameraPostFx から
    //    0 が来る経路は無いが、ここで NaN を作ると積分結果が丸ごと消える)
    const float degenerate = froxel::SliceToViewDepth(0.5f, slices, 0.0f, 0.0f);
    TEST_CHECK(std::isfinite(degenerate) && degenerate > 0.0f);
    TEST_CHECK(std::isfinite(froxel::ViewDepthToSlice(0.0f, slices, 0.0f, 0.0f)));
}

} // namespace

bool RunRenderSelfTest()
{
    g_failCount = 0;
    TestFrustumCulling();
    TestPostFxMerge();
    TestCascadeSplits();
    TestInstanceRuns();
    TestHeightFogInscatter();
    TestSunScreenPos();
    TestLutStripUv();
    TestAutoExposureBins();
    TestSignedCoC();
    TestLinearizeDepth();
    TestReprojectUv();
    TestEmissiveEncoding();
    TestCameraJitter();
    TestVelocityUv();
    TestPrevRenderWorldStore();
    TestTaaResolve();
    TestMotionBlurVelocity();
    TestFroxelGrid(); // M57a
    if (g_failCount == 0) {
        MYE_LOG_INFO("[selftest] render: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("[selftest] render: %d FAILED", g_failCount);
    return false;
}

} // namespace mye
