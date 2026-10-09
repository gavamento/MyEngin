//====================================================================================
//                          ShadowCullSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          CSM のカスケード別キャスター判定の回帰テストの実装
//====================================================================================
#include "Engine/Engine/Rendering/ShadowCullSelfTest.h"

#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Diagnostics/Profiler.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Passes/ShadowPass.h"
#include "Engine/Renderer/Pipeline/FrustumCull.h"
#include "Engine/Renderer/Pipeline/RenderTypes.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

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

// 真上から見下ろす直交ライト: xy は [-10,10]、ライト空間の z = 100 - y、近平面 50 (y=50)、遠平面 150 (y=-50)
Frustum MakeTopDownCascade()
{
    const XMMATRIX view = XMMatrixLookToLH(XMVectorSet(0, 100, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0));
    const XMMATRIX proj = XMMatrixOrthographicOffCenterLH(-10.0f, 10.0f, -10.0f, 10.0f, 50.0f, 150.0f);
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, view * proj);
    return BuildFrustum(vp);
}

XMFLOAT3 P(float x, float y, float z) { return { x, y, z }; }

void TestMask()
{
    MYE_LOG_INFO("-- カスケード判定: 近平面を除く 5 面 --");
    const Frustum f = MakeTopDownCascade();
    const Frustum frusta[2] = { f, f };
    const auto mask = [&](XMFLOAT3 lo, XMFLOAT3 hi) { return CascadeCasterMask(frusta, 2, true, lo, hi); };

    Check(mask(P(-1, 10, -1), P(1, 12, 1)) == 3, "ボリュームの中の箱は全カスケードに入る");
    Check(!WorldAabbInFrustum(f, P(-1, 140, -1), P(1, 142, 1)), "(対照) 近平面を含む 6 面の判定は光源側の箱を落とす");
    Check(mask(P(-1, 140, -1), P(1, 142, 1)) == 3, "近平面より光源側 (遠く) の箱は残る");
    Check(mask(P(-1, 40, -1), P(1, 60, 1)) == 3, "近平面をまたぐ箱は残る");
    Check(mask(P(100, 10, -1), P(102, 12, 1)) == 0, "xy の外の箱は落ちる");
    Check(mask(P(-1, 10, 100), P(1, 12, 102)) == 0, "もう一方の xy の外の箱も落ちる");
    Check(mask(P(-1, -100, -1), P(1, -90, 1)) == 0, "遠平面より奥の箱は落ちる");
    Check(CascadeCasterMask(frusta, 2, false, P(500, 500, 500), P(501, 501, 501)) == 3, "箱を持たない物は全カスケードに入る");

    // 一方のカスケードだけに入る箱
    const XMMATRIX view = XMMatrixLookToLH(XMVectorSet(0, 100, 0, 1), XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0));
    XMFLOAT4X4 vpWide;
    XMStoreFloat4x4(&vpWide, view * XMMatrixOrthographicOffCenterLH(-200.0f, 200.0f, -200.0f, 200.0f, 50.0f, 150.0f));
    const Frustum mixed[2] = { f, BuildFrustum(vpWide) };
    Check(CascadeCasterMask(mixed, 2, true, P(100, 10, -1), P(102, 12, 1)) == 2, "広いカスケードだけに入る箱は bit 1 だけ立つ");

    // 画面外のスキンのパレット評価: どのカスケードにも入らない物は 0、入る物だけ評価する
    int evaluated = 0;
    const XMFLOAT3 boxes[3][2] = { { P(100, 10, 100), P(103, 14, 103) },  // どこにも入らない
                                   { P(300, 10, 0), P(303, 14, 3) },      // どこにも入らない
                                   { P(-1, 140, -1), P(1, 150, 1) } };    // 光源側に離れているが影は落ちる
    for (const auto& b : boxes) {
        evaluated += OffscreenCasterNeedsPalette(CascadeCasterMask(frusta, 2, true, b[0], b[1])) ? 1 : 0;
    }
    Check(evaluated == 1, "画面外のスキンは、どのカスケードにも入らなければパレットを評価しない (入る 1 体だけ評価)");
}

void TestShadowPassRuns()
{
    MYE_LOG_INFO("-- ShadowPass: カスケード別のキューの run 数と描画数 --");
    GraphicsDevice device;
    if (!device.Init(true)) {
        Check(false, "WARP device init");
        return;
    }
    const std::wstring shaderDir = FindEngineShaderDir();
    ShaderManager shaders;
    if (shaderDir.empty() || !shaders.Init(device, { shaderDir })) {
        Check(false, "shader manager init");
        return;
    }
    RenderResources resources;
    resources.Init(device);
    ShadowPass pass;
    Check(pass.Init(device, shaders, 256), "ShadowPass::Init");

    Material material;
    const AssetID mat = resources.materials.Register("shadowcull_mat", material);
    const AssetID cube = resources.meshes.Cube();
    const AssetID sphere = resources.meshes.Sphere();
    const auto item = [&](AssetID mesh, uint32_t index) {
        RenderItem it;
        it.mesh = mesh;
        it.material = mat;
        it.entity = EntityID{ index, 1 };
        XMStoreFloat4x4(&it.world, XMMatrixTranslation(static_cast<float>(index), 0.0f, 0.0f));
        return it;
    };

    RenderQueue queues[ShadowPass::kCascades];
    for (uint32_t i = 0; i < 12; ++i) {
        queues[0].opaque.push_back(item((i & 1) ? sphere : cube, i + 1)); // 交互 = 並べ替え前は run が組めない
    }
    for (uint32_t i = 0; i < 3; ++i) {
        queues[1].opaque.push_back(item(cube, 100 + i));
    }
    // カスケード 2 は空
    XMFLOAT4X4 vps[ShadowPass::kCascades];
    for (XMFLOAT4X4& m : vps) {
        XMStoreFloat4x4(&m, XMMatrixIdentity());
    }
    const RenderQueue* ptrs[ShadowPass::kCascades] = { &queues[0], &queues[1], &queues[2] };

    prof::BeginFrame();
    pass.Render(device, shaders, ptrs, resources, vps, ShadowPass::kCascades, 0, true, nullptr);
    prof::RenderStats unsorted = prof::GetRenderStats();
    Check(unsorted.shadowCascadeDraws[0] == 12 && unsorted.shadowCascadeDraws[1] == 1 && unsorted.shadowCascadeDraws[2] == 0,
          "並べ替え前: 交互のキューは 12 draw、同じメッシュ 3 個は 1 run = 1 draw、空のカスケードは 0 draw");

    queues[0].Sort();
    prof::BeginFrame();
    pass.Render(device, shaders, ptrs, resources, vps, ShadowPass::kCascades, 0, true, nullptr);
    const prof::RenderStats sorted = prof::GetRenderStats();
    Check(sorted.shadowCascadeDraws[0] == 2 && sorted.shadowCascadeDraws[1] == 1 && sorted.shadowCascadeDraws[2] == 0,
          "並べ替え後: カスケード 0 は 2 run = 2 draw (カスケードごとに run を組む)");
    Check(sorted.shadowDrawCalls == 3, "全カスケードの draw の和は 3");
    prof::BeginFrame();
}

} // namespace

bool RunShadowCullSelfTest()
{
    MYE_LOG_INFO("==== Shadow cull self test ====");
    g_fails = 0;
    TestMask();
    TestShadowPassRuns();
    MYE_LOG_INFO("==== Shadow cull self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
