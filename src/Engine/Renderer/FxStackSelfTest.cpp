/*----
 FxStackSelfTest.cpp  FxStackAsset のロード／保存回帰テスト (M78c)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/FxStackSelfTest.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <variant>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/FxStackAsset.h"
#include "Engine/Renderer/ProjectFxStackPolicy.h"

namespace mye {
namespace {

int g_failCount = 0;

// テスト判定マクロ
#define FX_CHECK(cond)                                                                 \
    do {                                                                               \
        if (cond) {                                                                    \
            MYE_LOG_INFO("  PASS: %s", #cond);                                        \
        } else {                                                                       \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);            \
            ++g_failCount;                                                             \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// テスト 1: 空 JSON → 空スタック ok=true
// ---------------------------------------------------------------------------
void TestEmptyJson()
{
    MYE_LOG_INFO("[selftest] FxStack: empty JSON -> empty stack");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_empty.fxstack.json";
    {
        FxStackAsset empty;
        FX_CHECK(SaveFxStack(tmp, empty));
    }
    {
        FxStackAsset loaded;
        std::string err;
        FX_CHECK(LoadFxStack(tmp, loaded, &err));
        FX_CHECK(loaded.passes.empty());
        FX_CHECK(loaded.version == 1);
    }
    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 2: Post パスの round-trip
// ---------------------------------------------------------------------------
void TestPostRoundTrip()
{
    MYE_LOG_INFO("[selftest] FxStack: Post pass round-trip");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_post.fxstack.json";

    // 書き出す
    FxStackAsset orig;
    orig.version = 1;
    {
        FxStackEntry e;
        e.kind      = FxStackKind::Post;
        e.shader    = "MyTint.post";
        e.enabled   = true;
        e.insertion = PostInsertionPoint::AfterTonemap;
        e.priority  = 200;
        e.properties["_Intensity"] = 0.75f;
        std::array<float, 4> tint = { 1.0f, 0.9f, 0.8f, 1.0f };
        e.properties["_Tint"] = tint;
        orig.passes.push_back(std::move(e));
    }
    FX_CHECK(SaveFxStack(tmp, orig));

    // 読み戻す
    FxStackAsset loaded;
    std::string err;
    FX_CHECK(LoadFxStack(tmp, loaded, &err));

    FX_CHECK(loaded.passes.size() == 1);
    if (loaded.passes.size() == 1) {
        const FxStackEntry& e = loaded.passes[0];
        FX_CHECK(e.kind == FxStackKind::Post);
        FX_CHECK(e.shader == "MyTint.post");
        FX_CHECK(e.enabled == true);
        FX_CHECK(e.insertion == PostInsertionPoint::AfterTonemap);
        FX_CHECK(e.priority == 200);

        // Float プロパティ
        auto it = e.properties.find("_Intensity");
        FX_CHECK(it != e.properties.end());
        if (it != e.properties.end()) {
            const bool isFloat = std::holds_alternative<float>(it->second);
            FX_CHECK(isFloat);
            if (isFloat) {
                const float v = std::get<float>(it->second);
                FX_CHECK(v > 0.74f && v < 0.76f); // 約 0.75
            }
        }

        // Float4 プロパティ
        auto it2 = e.properties.find("_Tint");
        FX_CHECK(it2 != e.properties.end());
        if (it2 != e.properties.end()) {
            using Vec4 = std::array<float, 4>;
            const bool isVec4 = std::holds_alternative<Vec4>(it2->second);
            FX_CHECK(isVec4);
        }
    }

    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 3: Compute パスのロード
// ---------------------------------------------------------------------------
void TestComputeEntry()
{
    MYE_LOG_INFO("[selftest] FxStack: Compute entry load");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_cs.fxstack.json";

    FxStackAsset orig;
    {
        FxStackEntry e;
        e.kind          = FxStackKind::Compute;
        e.shader        = "MySim.cs";
        e.enabled       = true;
        e.dispatchPoint = "BeforePost";
        e.priority      = 50;
        e.properties["_Steps"] = 2.0f;
        orig.passes.push_back(std::move(e));
    }
    FX_CHECK(SaveFxStack(tmp, orig));

    FxStackAsset loaded;
    FX_CHECK(LoadFxStack(tmp, loaded));
    FX_CHECK(loaded.passes.size() == 1);
    if (loaded.passes.size() == 1) {
        const FxStackEntry& e = loaded.passes[0];
        FX_CHECK(e.kind == FxStackKind::Compute);
        FX_CHECK(e.shader == "MySim.cs");
        FX_CHECK(e.dispatchPoint == "BeforePost");
        FX_CHECK(e.priority == 50);
    }

    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 4: InsertionToString / InsertionFromString 往復
// ---------------------------------------------------------------------------
void TestInsertionStringConversion()
{
    MYE_LOG_INFO("[selftest] FxStack: Insertion string conversion");

    FX_CHECK(std::string(InsertionToString(PostInsertionPoint::BeforeTonemap)) == "BeforeTonemap");
    FX_CHECK(std::string(InsertionToString(PostInsertionPoint::AfterTonemap))  == "AfterTonemap");
    FX_CHECK(InsertionFromString("BeforeTonemap") == PostInsertionPoint::BeforeTonemap);
    FX_CHECK(InsertionFromString("AfterTonemap")  == PostInsertionPoint::AfterTonemap);
    // 未知の文字列は BeforeTonemap にフォールバック
    FX_CHECK(InsertionFromString("Unknown") == PostInsertionPoint::BeforeTonemap);
}

// ---------------------------------------------------------------------------
// テスト 5: 壊れた JSON は ok=false
// ---------------------------------------------------------------------------
void TestBrokenJson()
{
    MYE_LOG_INFO("[selftest] FxStack: broken JSON -> false");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_bad.fxstack.json";

    // 不正 JSON を書く
    {
        std::ofstream f(tmp);
        f << "{ not json at all }";
    }

    FxStackAsset loaded;
    std::string err;
    const bool ok = LoadFxStack(tmp, loaded, &err);
    FX_CHECK(!ok);
    FX_CHECK(!err.empty());

    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 6: 複数パス (Post + Compute 混在)
// ---------------------------------------------------------------------------
void TestMixedPasses()
{
    MYE_LOG_INFO("[selftest] FxStack: mixed Post+Compute passes");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_mixed.fxstack.json";

    FxStackAsset orig;
    // Post パス
    {
        FxStackEntry e;
        e.kind      = FxStackKind::Post;
        e.shader    = "A.post";
        e.insertion = PostInsertionPoint::BeforeTonemap;
        e.priority  = 100;
        orig.passes.push_back(e);
    }
    // Compute パス
    {
        FxStackEntry e;
        e.kind          = FxStackKind::Compute;
        e.shader        = "B.cs";
        e.dispatchPoint = "BeforeTonemap";
        e.priority      = 50;
        orig.passes.push_back(e);
    }
    // disabled Post パス
    {
        FxStackEntry e;
        e.kind      = FxStackKind::Post;
        e.shader    = "C.post";
        e.enabled   = false;
        e.insertion = PostInsertionPoint::AfterTonemap;
        e.priority  = 200;
        orig.passes.push_back(e);
    }
    FX_CHECK(SaveFxStack(tmp, orig));

    FxStackAsset loaded;
    FX_CHECK(LoadFxStack(tmp, loaded));
    FX_CHECK(loaded.passes.size() == 3);
    if (loaded.passes.size() == 3) {
        FX_CHECK(loaded.passes[0].kind   == FxStackKind::Post);
        FX_CHECK(loaded.passes[0].shader == "A.post");
        FX_CHECK(loaded.passes[0].enabled == true);
        FX_CHECK(loaded.passes[1].kind   == FxStackKind::Compute);
        FX_CHECK(loaded.passes[1].shader == "B.cs");
        FX_CHECK(loaded.passes[2].enabled == false);
        FX_CHECK(loaded.passes[2].shader == "C.post");
    }

    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 7: Tex2D プロパティの round-trip (文字列値)
// ---------------------------------------------------------------------------
void TestTex2DRoundTrip()
{
    MYE_LOG_INFO("[selftest] FxStack: Tex2D property round-trip");

    namespace fs = std::filesystem;
    const std::wstring tmp = fs::temp_directory_path().wstring()
                             + L"\\mye_fxstack_test_tex2d.fxstack.json";

    FxStackAsset orig;
    {
        FxStackEntry e;
        e.kind      = FxStackKind::Post;
        e.shader    = "MyTex.post";
        e.insertion = PostInsertionPoint::BeforeTonemap;
        e.priority  = 100;
        // ビルトイン名
        e.properties["_Mask"] = std::string("white");
        // GUID hex 文字列
        e.properties["_LUT"]  = std::string("0102030405060708");
        orig.passes.push_back(std::move(e));
    }
    FX_CHECK(SaveFxStack(tmp, orig));

    FxStackAsset loaded;
    std::string err;
    FX_CHECK(LoadFxStack(tmp, loaded, &err));
    FX_CHECK(loaded.passes.size() == 1);
    if (loaded.passes.size() == 1) {
        const FxStackEntry& e = loaded.passes[0];

        auto it = e.properties.find("_Mask");
        FX_CHECK(it != e.properties.end());
        if (it != e.properties.end()) {
            const bool isStr = std::holds_alternative<std::string>(it->second);
            FX_CHECK(isStr);
            if (isStr) {
                FX_CHECK(std::get<std::string>(it->second) == "white");
            }
        }

        auto it2 = e.properties.find("_LUT");
        FX_CHECK(it2 != e.properties.end());
        if (it2 != e.properties.end()) {
            const bool isStr2 = std::holds_alternative<std::string>(it2->second);
            FX_CHECK(isStr2);
            if (isStr2) {
                FX_CHECK(std::get<std::string>(it2->second) == "0102030405060708");
            }
        }
    }

    { std::error_code ec_; fs::remove(tmp, ec_); }
}

// ---------------------------------------------------------------------------
// テスト 8: CameraOverride 時は fxstack (Post+Compute) を Resolve/Dispatch へ渡さない
// (M78 §4.1 / review-1 #1。RenderSystem は ShouldInjectProjectFxStack で両 Runner をガード)
// ---------------------------------------------------------------------------
void TestProjectFxStackInjectionPolicy()
{
    MYE_LOG_INFO("[selftest] FxStack: CameraOverride Post+Compute injection policy");

    FX_CHECK(ShouldInjectProjectFxStack(false));
    FX_CHECK(!ShouldInjectProjectFxStack(true));
}

// ---------------------------------------------------------------------------
// テスト 9: 実効 fxStack の決め方 (レビュー #7)。fxStack を持たないカメラへ切り替えたら null = パスを消す
// ---------------------------------------------------------------------------
void TestEffectiveFxStack()
{
    MYE_LOG_INFO("[selftest] FxStack: EffectiveFxStack");

    const AssetID fx{ 0x1234 };
    // 通常: Game View のカメラが解決できる fxStack を持つ
    FX_CHECK(EffectiveFxStack(false, true, fx, true) == fx);
    // Scene View (CameraOverride) は常に null
    FX_CHECK(EffectiveFxStack(true, true, fx, true).IsNull());
    // CameraPostFx の無いカメラ (カメラ無しも同じ扱い)
    FX_CHECK(EffectiveFxStack(false, false, fx, true).IsNull());
    // fxStack 未設定
    FX_CHECK(EffectiveFxStack(false, true, AssetID{}, false).IsNull());
    // GUID がパスに解決できない (アセット削除後など)
    FX_CHECK(EffectiveFxStack(false, true, fx, false).IsNull());
}

// ---------------------------------------------------------------------------
// テスト 10: fxstack.json を読み直す条件 (レビュー #6)。ID の切替かファイルの更新時だけ
// ---------------------------------------------------------------------------
void TestFxStackReloadDecision()
{
    MYE_LOG_INFO("[selftest] FxStack: NeedsFxStackReload");

    const AssetID a{ 0xA };
    const AssetID b{ 0xB };
    // 初回 (何も読んでいない)
    FX_CHECK(NeedsFxStackReload(AssetID{}, 0, a, 100));
    // 同じ ID・同じ更新時刻 → 読まない (毎フレームの同期読み込みをしない)
    FX_CHECK(!NeedsFxStackReload(a, 100, a, 100));
    // ファイルが書き換わった
    FX_CHECK(NeedsFxStackReload(a, 100, a, 101));
    // 別の fxStack に切り替わった
    FX_CHECK(NeedsFxStackReload(a, 100, b, 100));
    // 更新時刻が取れないファイルも、同じ状態のままなら読み直さない (失敗の WARN を毎フレーム出さない)
    FX_CHECK(!NeedsFxStackReload(a, 0, a, 0));
    // 実効 fxStack が null のときは読まない (パスを消す側の処理)
    FX_CHECK(!NeedsFxStackReload(a, 100, AssetID{}, 0));
}

} // namespace

// ---------------------------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------------------------
bool RunFxStackSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("=== FxStack SelfTest (M78c) ===");

    TestEmptyJson();
    TestPostRoundTrip();
    TestComputeEntry();
    TestInsertionStringConversion();
    TestBrokenJson();
    TestMixedPasses();
    TestTex2DRoundTrip();
    TestProjectFxStackInjectionPolicy();
    TestEffectiveFxStack();      // レビュー #7
    TestFxStackReloadDecision(); // レビュー #6

    if (g_failCount == 0) {
        MYE_LOG_INFO("=== FxStack SelfTest: ALL PASS ===");
    } else {
        MYE_LOG_ERROR("=== FxStack SelfTest: %d FAIL(s) ===", g_failCount);
    }
    return g_failCount == 0;
}

} // namespace mye
