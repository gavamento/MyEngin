/*----
 ComputeAbiSelfTest.cpp  Compute ABI v21 の回帰テスト (M78e)
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ComputeAbiSelfTest.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Renderer/ComputeAbiRunner.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/GraphicsDevice.h"
#include "Engine/Renderer/ShaderManager.h"
#include "Shared/EngineAPI.h"

namespace mye {
namespace {

int g_fails = 0;

void Check(bool cond, const char* msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg);
        ++g_fails;
    }
}

const char* kShader = "abi_fill.cs";

const char* kHlsl =
    "cbuffer Params : register(b0)\n"
    "{\n"
    "    float gScale;\n"
    "    float3 _pad;\n"
    "    float4 gTint;\n"
    "};\n"
    "StructuredBuffer<float4> gIn : register(t0);\n"
    "Texture2D gTex : register(t1);\n"
    "RWStructuredBuffer<float4> gOut : register(u0);\n"
    "[numthreads(64, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    float4 src = gIn[id.x];\n"
    "    float4 tex = gTex.Load(int3(0, 0, 0));\n"
    "    gOut[id.x] = src * gScale + gTint * tex.r;\n"
    "}\n";

} // namespace

bool RunComputeAbiSelfTest()
{
    MYE_LOG_INFO("[selftest] Compute ABI v21");
    g_fails = 0;

    GraphicsDevice device;
    if (!device.Init(true)) {
        MYE_LOG_ERROR("  FAIL: WARP device init");
        return false;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / L"mye_compute_abi_selftest";
    fs::create_directories(dir, ec);
    const fs::path hlslPath = dir / L"abi_fill.cs.hlsl";
    {
        std::ofstream out(hlslPath, std::ios::binary);
        out << kHlsl;
    }

    ShaderManager shaders;
    Check(shaders.Init(device, { dir.wstring() }), "shader manager init");
    TextureLibrary textures;
    textures.Init(device);

    ComputeAbiRunner runner;
    ScriptApiContext apiCtx;
    apiCtx.computeAbi = &runner;
    apiCtx.graphicsDevice = &device;
    apiCtx.shaderManager = &shaders;
    apiCtx.textureLibrary = &textures;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);

    Check(api.version == 21u, "api version is 21");
    Check(api.CreateComputeBuffer && api.ReleaseComputeBuffer && api.SetComputeBuffer
              && api.SetComputeFloat && api.SetComputeFloat4 && api.SetComputeTextureFromAsset
              && api.DispatchCompute,
          "seven compute slots are non-null");

    const uint32_t flags = MYE_COMPUTE_BUFFER_STRUCTURED | MYE_COMPUTE_BUFFER_UAV;
    const uint64_t outBuf = api.CreateComputeBuffer(api.engine, 64, 16, flags);
    const uint64_t inBuf = api.CreateComputeBuffer(api.engine, 64, 16, MYE_COMPUTE_BUFFER_STRUCTURED);
    Check(outBuf != 0 && inBuf != 0, "CreateComputeBuffer returns a handle");

    Check(api.SetComputeBuffer(api.engine, kShader, "gOut", outBuf) == 1, "SetComputeBuffer UAV name");
    Check(api.SetComputeBuffer(api.engine, kShader, "gIn", inBuf) == 1, "SetComputeBuffer SRV name");
    Check(api.SetComputeBuffer(api.engine, kShader, "nope", outBuf) == 0, "unknown buffer name returns 0");
    Check(api.SetComputeFloat(api.engine, kShader, "gScale", 2.0f) == 1, "SetComputeFloat known name");
    Check(api.SetComputeFloat4(api.engine, kShader, "gTint", 1, 0, 0, 1) == 1, "SetComputeFloat4 known name");
    Check(api.SetComputeFloat(api.engine, kShader, "missing", 1.0f) == 0, "unknown float name returns 0");
    Check(api.SetComputeTextureFromAsset(api.engine, kShader, "gTex", HashStr("white")) == 1,
          "builtin white key binds");
    Check(api.SetComputeTextureFromAsset(api.engine, kShader, "gTex", HashStr("builtin://white")) == 1,
          "builtin://white key binds");
    Check(api.SetComputeTextureFromAsset(api.engine, kShader, "gTex", 0) == 0, "assetId 0 returns 0");
    Check(api.SetComputeTextureFromAsset(api.engine, kShader, "gTex", 1) == 0,
          "unresolved asset id returns 0");
    Check(api.SetComputeTextureFromAsset(api.engine, kShader, "nope", HashStr("white")) == 0,
          "unknown texture name returns 0");

    Check(api.DispatchCompute(api.engine, kShader, 1, 1, 1) == 1, "DispatchCompute succeeds");
    Check(api.DispatchCompute(api.engine, "no_such_shader.cs", 1, 1, 1) == 0,
          "unknown shader dispatch returns 0");
    Check(api.DispatchCompute(api.engine, nullptr, 1, 1, 1) == 0, "null shader dispatch returns 0");

    api.ReleaseComputeBuffer(api.engine, outBuf);
    api.ReleaseComputeBuffer(api.engine, outBuf); // 二重解放
    Check(api.SetComputeBuffer(api.engine, kShader, "gOut", outBuf) == 0,
          "use-after-free handle is rejected");
    const uint64_t reused = api.CreateComputeBuffer(api.engine, 64, 16, flags);
    Check(reused != 0 && reused != outBuf, "reissued handle has a new generation");
    Check(api.SetComputeBuffer(api.engine, kShader, "gOut", outBuf) == 0,
          "stale handle stays invalid after slot reuse");
    Check(api.SetComputeBuffer(api.engine, kShader, "gOut", reused) == 1, "new handle binds");

    // 上限: 生存 64 本で次は 0 (inBuf + reused が既に 2 本)
    uint64_t extra[64] = {};
    int live = 2;
    for (int i = 0; i < 64 && live < ComputeAbiRunner::kMaxAbiBuffers; ++i) {
        extra[i] = api.CreateComputeBuffer(api.engine, 4, 16, MYE_COMPUTE_BUFFER_STRUCTURED);
        if (extra[i] != 0) {
            ++live;
        }
    }
    Check(live == ComputeAbiRunner::kMaxAbiBuffers, "buffer cap is reachable");
    Check(api.CreateComputeBuffer(api.engine, 4, 16, MYE_COMPUTE_BUFFER_STRUCTURED) == 0,
          "over-cap create returns 0");

    runner.Shutdown();
    Check(api.SetComputeBuffer(api.engine, kShader, "gIn", inBuf) == 0,
          "handles die on Shutdown (scene transition)");
    Check(api.CreateComputeBuffer(api.engine, 4, 16, MYE_COMPUTE_BUFFER_STRUCTURED) != 0,
          "create works again after Shutdown");

    runner.Shutdown();
    device.Shutdown();
    fs::remove_all(dir, ec);

    if (g_fails != 0) {
        MYE_LOG_ERROR("[selftest] Compute ABI v21: %d failure(s)", g_fails);
        return false;
    }
    MYE_LOG_INFO("[selftest] Compute ABI v21: PASS");
    return true;
}

} // namespace mye
