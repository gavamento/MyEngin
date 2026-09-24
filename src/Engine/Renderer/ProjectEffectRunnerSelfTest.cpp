/*----
 ProjectEffectRunnerSelfTest.cpp  ProjectEffectRunner の回帰テスト (M78b)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectEffectRunnerSelfTest.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ProjectEffectRunner.h"
#include "Engine/Renderer/RenderTexture.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye {
namespace {

int g_failCount = 0;

// テスト判定マクロ
#define RUN_CHECK(cond)                                                               \
    do {                                                                              \
        if (cond) {                                                                   \
            MYE_LOG_INFO("  PASS: %s", #cond);                                       \
        } else {                                                                      \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);           \
            ++g_failCount;                                                            \
        }                                                                             \
    } while (0)

// ---------------------------------------------------------------------------
// テスト 1: 空スタックで HasPasses が false を返す
// ---------------------------------------------------------------------------
void TestEmptyStack()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 空スタック");

    ProjectEffectRunner runner;

    // パスが 0 件のとき HasPasses は false
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::AfterTonemap));

    // パスを追加して ClearPasses 後も false
    ProjectPostPassDesc desc;
    desc.shaderName = "TestShader.post";
    desc.insertion  = PostInsertionPoint::BeforeTonemap;
    runner.AddPass(desc);
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));

    runner.ClearPasses();
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));
}

// ---------------------------------------------------------------------------
// テスト 2: 挿入点フィルタリング
// ---------------------------------------------------------------------------
void TestInsertionFilter()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 挿入点フィルタリング");

    ProjectEffectRunner runner;

    // BeforeTonemap パスのみ登録
    {
        ProjectPostPassDesc d;
        d.shaderName = "A.post";
        d.insertion  = PostInsertionPoint::BeforeTonemap;
        runner.AddPass(d);
    }

    // BeforeTonemap は有効、AfterTonemap は無効
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::AfterTonemap));

    // AfterTonemap パスを追加
    {
        ProjectPostPassDesc d;
        d.shaderName = "B.post";
        d.insertion  = PostInsertionPoint::AfterTonemap;
        runner.AddPass(d);
    }

    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::AfterTonemap));
}

// ---------------------------------------------------------------------------
// テスト 3: enabled=false のパスは HasPasses から除外される
// ---------------------------------------------------------------------------
void TestDisabledPass()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: disabled パスの除外");

    ProjectEffectRunner runner;

    ProjectPostPassDesc desc;
    desc.shaderName = "C.post";
    desc.insertion  = PostInsertionPoint::BeforeTonemap;
    desc.enabled    = false;
    runner.AddPass(desc);

    // enabled=false のパスは HasPasses で見えない
    RUN_CHECK(!runner.HasPasses(PostInsertionPoint::BeforeTonemap));

    // enabled=true のパスを追加すると見える
    desc.enabled = true;
    desc.shaderName = "D.post";
    runner.AddPass(desc);
    RUN_CHECK(runner.HasPasses(PostInsertionPoint::BeforeTonemap));
}

// ---------------------------------------------------------------------------
// テスト 4: Priority 昇順安定ソート — CollectSortedPasses で実装本体を観測
// ---------------------------------------------------------------------------
void TestPrioritySort()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: Priority 昇順安定ソート (実装本体)");

    // Z(200) → A(100) → M(100) の順で登録する
    // 期待実行順: A(100) → M(100) → Z(200) (昇順安定ソート)
    ProjectEffectRunner runner;

    ProjectPostPassDesc dZ;
    dZ.shaderName = "Z.post";
    dZ.priority   = 200;
    dZ.insertion  = PostInsertionPoint::BeforeTonemap;

    ProjectPostPassDesc dA;
    dA.shaderName = "A.post";
    dA.priority   = 100;
    dA.insertion  = PostInsertionPoint::BeforeTonemap;

    ProjectPostPassDesc dM;
    dM.shaderName = "M.post";
    dM.priority   = 100; // A と同値 → 登録順 (A→M) を維持する安定ソートを確認
    dM.insertion  = PostInsertionPoint::BeforeTonemap;

    runner.AddPass(dZ);
    runner.AddPass(dA);
    runner.AddPass(dM);

    // CollectSortedPasses で RunPasses が使うソート結果を直接観測する
    auto sorted = runner.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);

    RUN_CHECK(sorted.size() == 3);
    if (sorted.size() == 3)
    {
        RUN_CHECK(sorted[0]->shaderName == "A.post"); // priority 100 (先に登録)
        RUN_CHECK(sorted[1]->shaderName == "M.post"); // priority 100 (後に登録)
        RUN_CHECK(sorted[2]->shaderName == "Z.post"); // priority 200
    }

    // AfterTonemap には 1 件も入っていない (挿入点フィルタも CollectSortedPasses で確認)
    auto sortedAfter = runner.CollectSortedPasses(PostInsertionPoint::AfterTonemap);
    RUN_CHECK(sortedAfter.empty());

    // 同値が逆順 (M→A) で登録された場合も安定ソートで元の順を維持
    ProjectEffectRunner runner2;
    ProjectPostPassDesc dM2 = dM; dM2.shaderName = "M2.post";
    ProjectPostPassDesc dA2 = dA; dA2.shaderName = "A2.post";
    runner2.AddPass(dM2); // 先に登録
    runner2.AddPass(dA2); // 後に登録

    auto sorted2 = runner2.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);
    RUN_CHECK(sorted2.size() == 2);
    if (sorted2.size() == 2)
    {
        RUN_CHECK(sorted2[0]->shaderName == "M2.post"); // 先に登録 → 安定ソートで先頭維持
        RUN_CHECK(sorted2[1]->shaderName == "A2.post");
    }
}

// ---------------------------------------------------------------------------
// テスト 5: ハード上限 (8 件超は切り捨て)
// ---------------------------------------------------------------------------
void TestHardLimit()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: ハード上限 %d",
                 ProjectEffectRunner::kMaxPostPasses);

    ProjectEffectRunner runner;

    const int over = ProjectEffectRunner::kMaxPostPasses + 2;
    std::vector<ProjectPostPassDesc> descs;
    descs.reserve(static_cast<size_t>(over));
    for (int i = 0; i < over; ++i)
    {
        ProjectPostPassDesc d;
        d.shaderName = "Shader" + std::to_string(i) + ".post";
        d.insertion  = PostInsertionPoint::BeforeTonemap;
        d.enabled    = true;
        descs.push_back(std::move(d));
    }

    runner.SetPasses(descs);

    auto sorted = runner.CollectSortedPasses(PostInsertionPoint::BeforeTonemap);
    RUN_CHECK(sorted.size() == static_cast<size_t>(ProjectEffectRunner::kMaxPostPasses));

    const std::string overflowName = "Shader" + std::to_string(over - 1) + ".post";
    const bool hasOverflow = std::any_of(sorted.begin(), sorted.end(),
        [&](const ProjectPostPassDesc* p) { return p->shaderName == overflowName; });
    RUN_CHECK(!hasOverflow);
}

// ---------------------------------------------------------------------------
// テスト 6: AfterTonemap の挿入点定数値
// ---------------------------------------------------------------------------
void TestInsertionPointConstants()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: 挿入点定数値");

    // spec §4.1 に基づく定数値 (0/1) を確認する
    // (シリアライズ・比較の安定性に関わるため)
    // volatile で定数畳み込みを防ぎ C4127 を回避する
    volatile auto vBefore = static_cast<int32_t>(PostInsertionPoint::BeforeTonemap);
    volatile auto vAfter  = static_cast<int32_t>(PostInsertionPoint::AfterTonemap);
    RUN_CHECK(vBefore == 0);
    RUN_CHECK(vAfter  == 1);
}

// ---------------------------------------------------------------------------
// テスト 7 (レビュー #4、WARP 実描画): assets 内の任意フォルダに置いたポストの Properties が
// 実行時に効くこと、ホットリロード (再コンパイル) でスキーマと CB が作り直されること。
// 修正前はソースを ShaderDirs 直下からしか探さず空スキーマ → b1 未バインドで真っ黒、
// 再コンパイル後も古いスキーマのまま だった
// ---------------------------------------------------------------------------
void TestNestedPostSchemaAndHotReload()
{
    MYE_LOG_INFO("[selftest] ProjectEffectRunner: assets 内の入れ子フォルダのポストとホットリロード");
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_post_runner_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"shaders", ec);
    fs::create_directories(root / L"fx" / L"deep", ec);
    const fs::path postPath = root / L"fx" / L"deep" / L"RunnerTint.post.hlsl";
    auto writePost = [&](const char* tint) {
        std::ofstream f(postPath, std::ios::binary | std::ios::trunc);
        f << "/*@MyEngineProperties\n_Tint (\"Tint\", Color) = " << tint << "\n@*/\n"
          << "#include \"ProjectPostCommon.hlsli\"\n"
          << "cbuffer MyEnginePerEffect : register(b1) { float4 _Tint; };\n"
          << "float4 PSMain(ProjectPostVSOut i) : SV_Target { return _Tint; }\n";
    };
    writePost("(0, 1, 0, 1)");

    GraphicsDevice device;
    const std::wstring engineShaderDir = FindEngineShaderDir();
    RUN_CHECK(!engineShaderDir.empty() && device.Init(true));
    if (engineShaderDir.empty()) {
        return;
    }
    ShaderManager shaders;
    RUN_CHECK(shaders.Init(device, { (root / L"shaders").wstring(), engineShaderDir }));
    shaders.SetAssetsRoot(root.wstring());
    shaders.RebuildProjectShaderIndex();

    constexpr int kSize = 4;
    RenderTexture pingA;
    RenderTexture pingB;
    RenderTexture dst;
    const bool rtOk = pingA.Create(device, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, false)
        && pingB.Create(device, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, false)
        && dst.Create(device, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    RUN_CHECK(rtOk);
    ID3D11Device* dev = device.Device();
    ID3D11DeviceContext* dc = device.Context();
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthOff;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendOff;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp;
    const bool stOk = SUCCEEDED(dev->CreateDepthStencilState(&dsd, depthOff.GetAddressOf()))
        && SUCCEEDED(dev->CreateBlendState(&bd, blendOff.GetAddressOf()))
        && SUCCEEDED(dev->CreateRasterizerState(&rd, raster.GetAddressOf()))
        && SUCCEEDED(dev->CreateSamplerState(&sd, linearClamp.GetAddressOf()));
    RUN_CHECK(stOk);
    if (!rtOk || !stOk) {
        return;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kSize;
    td.Height = kSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    RUN_CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, staging.GetAddressOf())));

    ProjectEffectRunner runner;
    ProjectPostPassDesc d;
    d.shaderName = "RunnerTint.post";
    d.insertion = PostInsertionPoint::AfterTonemap;
    runner.SetPasses({ d });
    const AssetID magenta = shaders.Load("project_post_magenta");

    auto runAndRead = [&]() {
        runner.RunPasses(PostInsertionPoint::AfterTonemap, device, shaders, pingA.SRV(), pingA, pingB,
                         dst.RTV(), nullptr, kSize, kSize, depthOff.Get(), blendOff.Get(), raster.Get(),
                         linearClamp.Get(), magenta);
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        dst.RTV()->GetResource(res.GetAddressOf());
        dc->CopyResource(staging.Get(), res.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        std::array<uint8_t, 4> px = { 0, 0, 0, 0 };
        if (SUCCEEDED(dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            std::memcpy(px.data(), mapped.pData, 4);
            dc->Unmap(staging.Get(), 0);
        }
        return px;
    };

    const std::array<uint8_t, 4> first = runAndRead();
    MYE_LOG_INFO("    first pass pixel = (%d,%d,%d,%d)", first[0], first[1], first[2], first[3]);
    RUN_CHECK(first[0] < 8 && first[1] > 247 && first[2] < 8); // Properties 既定の緑が b1 から読めている

    // ファイルを書き換えて再コンパイル (エディタのホットリロードと同じ差し替え)
    writePost("(1, 0, 0, 1)");
    const AssetID tintId = shaders.Load("RunnerTint.post");
    RUN_CHECK(shaders.Recompile(tintId));
    const std::array<uint8_t, 4> reloaded = runAndRead();
    MYE_LOG_INFO("    reloaded pass pixel = (%d,%d,%d,%d)", reloaded[0], reloaded[1], reloaded[2], reloaded[3]);
    RUN_CHECK(reloaded[0] > 247 && reloaded[1] < 8 && reloaded[2] < 8); // 新しい既定 (赤) に作り直された

    fs::remove_all(root, ec);
}

} // namespace

// ---------------------------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------------------------
bool RunProjectEffectRunnerSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("=== ProjectEffectRunner SelfTest (M78b) ===");

    TestEmptyStack();
    TestInsertionFilter();
    TestDisabledPass();
    TestPrioritySort();
    TestHardLimit();
    TestInsertionPointConstants();
    TestNestedPostSchemaAndHotReload(); // レビュー #4

    if (g_failCount == 0)
    {
        MYE_LOG_INFO("=== ProjectEffectRunner SelfTest: ALL PASS ===");
    }
    else
    {
        MYE_LOG_ERROR("=== ProjectEffectRunner SelfTest: %d FAIL(s) ===", g_failCount);
    }
    return g_failCount == 0;
}

} // namespace mye
