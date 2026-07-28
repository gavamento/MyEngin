#include "Engine/Renderer/RenderSelfTest.h"

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/MeshInstancing.h"
#include "Engine/Renderer/PostFxMath.h"
#include "Engine/Renderer/PostProcess.h"

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
    TestReprojectUv();
    if (g_failCount == 0) {
        MYE_LOG_INFO("[selftest] render: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("[selftest] render: %d FAILED", g_failCount);
    return false;
}

} // namespace mye
