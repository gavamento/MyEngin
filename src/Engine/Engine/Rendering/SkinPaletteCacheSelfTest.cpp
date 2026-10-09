//====================================================================================
//                          SkinPaletteCacheSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          パレットのキャッシュと URO (アニメ間引き) の判定の回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/SkinPaletteCacheSelfTest.h"

#include <cmath>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Rendering/SkinPaletteCache.h"
#include "Engine/Renderer/Mesh/MeshLod.h"

using namespace DirectX;

namespace mye {
namespace {

int g_fails = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", what);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", what);
        ++g_fails;
    }
}

SkinPaletteCache::Request MakeRequest(const SkinnedMeshComponent& sm, uint32_t entityIndex, uint64_t tick,
                                      uint32_t interval)
{
    SkinPaletteCache::Request req;
    req.entity = EntityID{ entityIndex, 1 };
    req.model = AssetID{ 77 };
    req.sm = &sm;
    req.alpha = 1.0f;
    req.simTick = tick;
    req.interval = interval;
    return req;
}

void TestUroFunctions()
{
    MYE_LOG_INFO("-- URO の間隔と更新 tick --");
    Check(UroUpdateInterval(0.5f) == 1 && UroUpdateInterval(0.05f) == 1, "画面の 5% 以上は毎 tick");
    Check(UroUpdateInterval(0.049f) == 2 && UroUpdateInterval(0.02f) == 2, "2% 以上 5% 未満は 2 tick");
    Check(UroUpdateInterval(0.019f) == 4 && UroUpdateInterval(0.008f) == 4, "0.8% 以上 2% 未満は 4 tick");
    Check(UroUpdateInterval(0.0079f) == 8 && UroUpdateInterval(0.0f) == 8, "それ未満は 8 tick");

    // どのエンティティも interval tick に 1 回だけ更新 tick が来る
    bool onePerWindow = true;
    for (uint32_t index = 0; index < 8; ++index) {
        int updates = 0;
        for (uint64_t tick = 100; tick < 100 + 8; ++tick) {
            updates += UroIsUpdateTick(tick, index, 4) ? 1 : 0;
        }
        onePerWindow = onePerWindow && updates == 2;
    }
    Check(onePerWindow, "8 tick の間に interval 4 の更新 tick がちょうど 2 回来る (全エンティティ)");
    Check(UroIsUpdateTick(123, 5, 1), "interval 1 は毎 tick が更新 tick");
    // 窓の先頭 = 直近の更新 tick。更新 tick の判定と一致する (新しい位相でも同じ)
    bool windowConsistent = true;
    for (uint32_t index : { 0u, 1u, 20u, 41u, 380u }) {
        for (uint32_t interval : { 2u, 4u, 8u }) {
            for (uint64_t tick = 0; tick < 40; ++tick) {
                const int64_t w = UroWindowStart(tick, index, interval);
                const int64_t age = static_cast<int64_t>(tick) - w;
                windowConsistent = windowConsistent && age >= 0 && age < static_cast<int64_t>(interval)
                                   && UroIsUpdateTick(static_cast<uint64_t>(w + static_cast<int64_t>(interval) * 100),
                                                      index, interval)
                                   && (UroIsUpdateTick(tick, index, interval) == (age == 0));
            }
        }
    }
    Check(windowConsistent, "窓の先頭 tick は直近の更新 tick (更新 tick のとき age 0、窓の長さは interval 未満)");
    bool negativeWindow = false;
    for (uint32_t index = 0; index < 16; ++index) {
        negativeWindow = negativeWindow || UroWindowStart(0, index, 8) < 0;
    }
    Check(negativeWindow, "tick が小さいとき窓の先頭は負になりうる (符号なしでアンダーフローしない)");

    // 20 刻みの entity.index (1 体 20 エンティティを作るローダ) でも、更新 tick が 1 つの位相に偏らない
    for (uint32_t interval : { 2u, 4u }) {
        int perPhase[4] = {};
        for (uint32_t k = 0; k < 20; ++k) {
            for (uint64_t tick = 0; tick < interval; ++tick) {
                if (UroIsUpdateTick(tick, k * 20, interval)) {
                    ++perPhase[tick];
                }
            }
        }
        bool spread = true;
        for (uint32_t p = 0; p < interval; ++p) {
            spread = spread && perPhase[p] >= 2; // 20 体の 1 割以上
        }
        Check(spread, interval == 2 ? "20 刻みの 20 体: interval 2 の更新が両方の位相に 1 割以上ずつ"
                                    : "20 刻みの 20 体: interval 4 の更新が 4 つの位相に 1 割以上ずつ");
    }

    SkinnedMeshComponent plain;
    Check(UroEligible(plain, false), "ふつうのスキンは間引ける");
    Check(!UroEligible(plain, true), "ラグドール作動中は間引かない");
    SkinnedMeshComponent fading;
    fading.fadeTotal = 10;
    fading.fadeElapsed = 3;
    Check(!UroEligible(fading, false), "クロスフェード中は間引かない");
    fading.fadeElapsed = 10;
    Check(UroEligible(fading, false), "終わったフェードは間引ける");
    SkinnedMeshComponent twoLayers;
    twoLayers.poseLayerCount = 2;
    SkinnedMeshComponent oneLayer;
    oneLayer.poseLayerCount = 1;
    Check(!UroEligible(twoLayers, false) && UroEligible(oneLayer, false), "層を 2 つ以上混ぜている間は間引かない");
}

void TestCacheKey()
{
    MYE_LOG_INFO("-- パレットのキャッシュの鍵 --");
    SkinPaletteCache cache;
    cache.BeginFrame();
    SkinnedMeshComponent sm;
    sm.timeTicks = 5;

    SkinPaletteCache::Request req = MakeRequest(sm, 3, 10, 1);
    Check(cache.Find(req) == nullptr, "空のキャッシュは当たらない");
    SkinPaletteCache::Entry* entry = cache.Claim(req);
    entry->palette.assign(4, XMFLOAT4X4{});
    Check(cache.Find(req) == entry, "同じ入力の 2 回目 (別ビュー) は同じエントリに当たる");

    SkinnedMeshComponent moved = sm;
    moved.timeTicks = 6;
    Check(cache.Find(MakeRequest(moved, 3, 10, 1)) == nullptr, "ポーズ入力 (時刻) が変わったら必ず作り直す");
    SkinnedMeshComponent otherClip = sm;
    otherClip.clip = 2;
    Check(cache.Find(MakeRequest(otherClip, 3, 10, 1)) == nullptr, "クリップが変わったら作り直す");

    SkinPaletteCache::Request alphaReq = req;
    alphaReq.alpha = 0.5f;
    Check(cache.Find(alphaReq) == nullptr, "補間 alpha が変わったら作り直す");
    SkinPaletteCache::Request modelReq = req;
    modelReq.model = AssetID{ 78 };
    Check(cache.Find(modelReq) == nullptr, "モデルが変わったら作り直す");
    SkinPaletteCache::Request genReq = req;
    genReq.entity.generation = 2;
    Check(cache.Find(genReq) == nullptr, "同じ index でも世代が違う (別のエンティティ) なら当たらない");
    SkinPaletteCache::Request uncacheable = req;
    uncacheable.cacheable = false;
    Check(cache.Find(uncacheable) == nullptr, "ラグドール (ECS のポーズ入力だけで決まらない) は再利用しない");

    // SamePoseInputs が見ない描画補間の入力
    SkinnedMeshComponent a;
    a.poseLayerCount = 1;
    a.poseLayers[0] = { 4, 256, 65536, 200, 56 };
    SkinnedMeshComponent b = a;
    Check(SameRenderPoseInputs(a, b), "同じ入力は等価");
    b.poseLayers[0].prevTimeQ = 100;
    Check(SamePoseInputs(a, b) && !SameRenderPoseInputs(a, b), "描画補間の前の時刻だけが違っても作り直す");
    b = a;
    b.poseLayers[0].stepQ = 0;
    Check(!SameRenderPoseInputs(a, b), "描画補間の進みだけが違っても作り直す");
    b = a;
    b.poseLayers[1].prevTimeQ = 999;
    Check(SameRenderPoseInputs(a, b), "使っていない層の中身では割れない");

    cache.Clear();
    Check(cache.Find(req) == nullptr && cache.LiveCount() == 0, "捨てた後は当たらない");
}

void TestCacheUro()
{
    MYE_LOG_INFO("-- URO の窓の中の再利用 --");
    SkinPaletteCache cache;
    cache.BeginFrame();
    SkinnedMeshComponent sm;
    const uint32_t entityIndex = 1;
    const uint32_t interval = 4;
    // 更新 tick T を探す。窓は T, T+1, T+2, T+3
    uint64_t T = 20;
    while (!UroIsUpdateTick(T, entityIndex, interval)) {
        ++T;
    }
    sm.timeTicks = static_cast<int32_t>(T);
    SkinPaletteCache::Entry* entry = cache.Claim(MakeRequest(sm, entityIndex, T, interval));

    bool reusedInWindow = true;
    for (uint64_t tick = T + 1; tick <= T + 3; ++tick) {
        sm.timeTicks = static_cast<int32_t>(tick); // sim が進んでポーズ入力は変わっている
        reusedInWindow = reusedInWindow && cache.Find(MakeRequest(sm, entityIndex, tick, interval)) == entry;
    }
    Check(reusedInWindow, "窓の途中の tick (T+1..T+3) はポーズ入力が変わっていても同じパレットを使う");
    sm.timeTicks = static_cast<int32_t>(T + 4);
    Check(cache.Find(MakeRequest(sm, entityIndex, T + 4, interval)) == nullptr, "次の更新 tick (T+4) は作り直す");
    cache.Claim(MakeRequest(sm, entityIndex, T + 4, interval));
    Check(cache.Find(MakeRequest(sm, entityIndex, T + 4, interval)) != nullptr,
          "更新 tick に作ったものは同じ tick の別ビューに使われる (1 フレーム 1 回)");

    // 窓の途中で初めて見えた (画面外から入った): その時点のポーズで作り、窓の残りで使う
    SkinPaletteCache lateCache;
    lateCache.BeginFrame();
    sm.timeTicks = static_cast<int32_t>(T + 2);
    SkinPaletteCache::Entry* late = lateCache.Claim(MakeRequest(sm, entityIndex, T + 2, interval));
    sm.timeTicks = static_cast<int32_t>(T + 3);
    Check(lateCache.Find(MakeRequest(sm, entityIndex, T + 3, interval)) == late,
          "窓の途中で作ったパレットも、その窓の残りの tick で使う");
    sm.timeTicks = static_cast<int32_t>(T + 5);
    Check(lateCache.Find(MakeRequest(sm, entityIndex, T + 5, interval)) == nullptr, "窓が変われば作り直す");

    // tick が進まない (編集中) のにポーズが変わったら、間引かずに作り直す
    SkinPaletteCache editCache;
    editCache.BeginFrame();
    sm.timeTicks = static_cast<int32_t>(T + 2);
    editCache.Claim(MakeRequest(sm, entityIndex, T + 2, interval));
    sm.timeTicks = 9999;
    Check(editCache.Find(MakeRequest(sm, entityIndex, T + 2, interval)) == nullptr,
          "同じ tick のうちにポーズが変わったら間引かない (tick が止まる編集中の反映)");

    // 間引きを切った (interval 1) ビューは窓のパレットを使わない
    SkinPaletteCache noUro;
    noUro.BeginFrame();
    sm.timeTicks = static_cast<int32_t>(T);
    noUro.Claim(MakeRequest(sm, entityIndex, T, interval));
    sm.timeTicks = static_cast<int32_t>(T + 1);
    Check(noUro.Find(MakeRequest(sm, entityIndex, T + 1, 1)) == nullptr, "間引かないリクエストは古いポーズのパレットを使わない");
}
void TestCacheSweep()
{
    MYE_LOG_INFO("-- 古いエントリの掃除 --");
    SkinPaletteCache cache;
    SkinnedMeshComponent sm;
    cache.BeginFrame();
    cache.Claim(MakeRequest(sm, 1, 0, 1));
    cache.Claim(MakeRequest(sm, 2, 0, 1));
    Check(cache.LiveCount() == 2, "確保した数が数えられる");
    for (int i = 0; i < 1100; ++i) {
        cache.BeginFrame();
        cache.Find(MakeRequest(sm, 2, 0, 1)); // エンティティ 2 だけ使い続ける
    }
    Check(cache.LiveCount() == 1 && cache.Find(MakeRequest(sm, 2, 0, 1)) != nullptr
              && cache.Find(MakeRequest(sm, 1, 0, 1)) == nullptr,
          "使われなくなったエントリだけが手放される");
}

void TestScreenSizeAndLodHistory()
{
    MYE_LOG_INFO("-- 画面での大きさと LOD の履歴 --");
    const XMFLOAT3 lo{ -1.0f, -1.0f, 9.0f };
    const XMFLOAT3 hi{ 1.0f, 1.0f, 11.0f };
    const XMFLOAT3 cam{ 0.0f, 0.0f, 0.0f };
    const float expected = LodScreenSize(0.5f * std::sqrt(12.0f), 10.0f, 1.7f, false);
    Check(std::fabs(BoxScreenSize(lo, hi, cam, 1.7f, false) - expected) < 1e-6f,
          "BoxScreenSize は外接球の半径と中心までの距離から LodScreenSize を求める");

    // 段の境目のヒステリシスの帯: 履歴ありは前の段にとどまり、履歴なしは距離だけで決まる
    std::vector<MeshLodLevel> lods(2);
    lods[1].screenSize = 0.2f;
    const LodSelectParams params{ 1.0f, -1 };
    const float inBand = 0.19f; // 0.2 を下回るが、ヒステリシス (10%) の帯の中
    LodHistory history;
    history.Set(EntityID{ 4, 1 }, 0);
    Check(SelectLod(lods, inBand, history.Get(EntityID{ 4, 1 }), params) == 0, "履歴ありは帯の中で前の段 (0) にとどまる");
    history.Clear();
    Check(history.Get(EntityID{ 4, 1 }) == -1, "履歴を捨てると履歴なし (-1) に戻る");
    Check(SelectLod(lods, inBand, history.Get(EntityID{ 4, 1 }), params) == 1,
          "捨てた直後の段は距離だけで決まる (帯の中でも粗い段 1)");
    history.Set(EntityID{ 4, 1 }, 1);
    Check(history.Get(EntityID{ 4, 2 }) == -1, "世代が違う (別のエンティティ) 履歴は読まない");
}

} // namespace

bool RunSkinPaletteCacheSelfTest()
{
    MYE_LOG_INFO("==== Skin palette cache self test ====");
    g_fails = 0;
    TestUroFunctions();
    TestCacheKey();
    TestCacheUro();
    TestCacheSweep();
    TestScreenSizeAndLodHistory();
    MYE_LOG_INFO("==== Skin palette cache self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
