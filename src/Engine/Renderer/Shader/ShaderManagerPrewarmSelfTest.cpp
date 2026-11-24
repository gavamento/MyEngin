//====================================================================================
//                          ShaderManagerPrewarmSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     11/24/2026
//                                          シェーダーキャッシュのあたため (並列コンパイル) の自己テスト
//====================================================================================
#include "Engine/Renderer/Shader/ShaderManagerPrewarmSelfTest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <process.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"
#include "Engine/Renderer/Shader/ShaderManager.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

constexpr char kProgramSource[] = R"(
struct VSIn { float3 pos : POSITION; };
float4 VSMain(VSIn v) : SV_Position { return float4(v.pos * SCALE, 1.0f); }
float4 PSMain() : SV_Target { return float4(SCALE, 0.0f, 0.0f, 1.0f); }
)";

constexpr char kComputeSource[] = R"(
RWTexture2D<float4> gOut : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { gOut[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f); }
)";

// 入口関数を持たない (他から取り込まれるだけの) ファイル
constexpr char kIncludeOnlySource[] = R"(
float Half(float x) { return x * 0.5f; }
)";

// 入口関数の名前はあるが、コンパイルできない
constexpr char kBrokenSource[] = R"(
float4 VSMain() : SV_Position { return undefined_symbol; }
float4 PSMain() : SV_Target { return float4(0.0f, 0.0f, 0.0f, 1.0f); }
)";

void WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ディレクトリ直下のキャッシュファイル名 (昇順)
std::vector<std::wstring> CacheFiles(const fs::path& dir)
{
    std::vector<std::wstring> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == L".shc") {
            names.push_back(entry.path().filename().wstring());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

bool RunShaderManagerPrewarmSelfTest()
{
    MYE_LOG_INFO("==== ShaderManager prewarm self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path tempRoot =
        fs::temp_directory_path(ec) / (L"mye_shader_prewarm_selftest_" + std::to_wstring(_getpid()));
    fs::remove_all(tempRoot, ec);
    const fs::path shaderDir = tempRoot / L"shaders";
    const fs::path warmCache = tempRoot / L"cache_warm";
    const fs::path plainCache = tempRoot / L"cache_plain";
    const fs::path offCache = tempRoot / L"cache_off";
    for (const fs::path& dir : { shaderDir, warmCache, plainCache, offCache }) {
        fs::create_directories(dir, ec);
    }

    const std::string define1 = "#define SCALE 1.0f\n";
    const std::string define2 = "#define SCALE 2.0f\n";
    WriteText(shaderDir / L"good_a.hlsl", define1 + kProgramSource);
    WriteText(shaderDir / L"good_b.hlsl", define2 + kProgramSource);
    WriteText(shaderDir / L"comp.cs.hlsl", kComputeSource);
    WriteText(shaderDir / L"include_only.hlsl", kIncludeOnlySource);
    WriteText(shaderDir / L"broken.hlsl", kBrokenSource);
    WriteText(shaderDir / L"skip.kernel.hlsl", kComputeSource);
    constexpr int kGoodCount = 3;      // good_a / good_b / comp.cs
    constexpr int kCandidateCount = 4; // 上の 3 本 + broken (入口関数の名前があるので試す)

    // ---- (1) デバイスなし (Init 前) は何もしない ----
    {
        ShaderManager uninitialized;
        uninitialized.SetCacheDir(offCache.wstring(), true);
        const ShaderManager::PrewarmStats stats = uninitialized.PrewarmCache();
        check(stats.candidates == 0 && stats.compiled == 0 && CacheFiles(offCache).empty(),
              "no device: prewarm does nothing before Init");
    }

    GraphicsDevice device;
    if (!device.Init(true)) {
        check(false, "WARP device creation for the shader prewarm test");
        fs::remove_all(tempRoot, ec);
        MYE_LOG_ERROR("==== ShaderManager prewarm self test: %d FAILED ====", failCount);
        return false;
    }
    const std::vector<std::wstring> dirs{ shaderDir.wstring() };

    // ---- (2) キャッシュ無効なら何もしない ----
    {
        ShaderManager disabled;
        disabled.SetCacheDir(offCache.wstring(), false);
        check(disabled.Init(device, dirs), "shader manager init (cache disabled)");
        const ShaderManager::PrewarmStats stats = disabled.PrewarmCache();
        check(stats.candidates == 0 && stats.compiled == 0 && CacheFiles(offCache).empty(),
              "cache disabled: prewarm does nothing");
    }

    // ---- (3) あたため → 後の Load は全て命中する ----
    {
        ShaderManager warm;
        warm.SetCacheDir(warmCache.wstring(), true);
        check(warm.Init(device, dirs), "shader manager init (prewarm)");
        const ShaderManager::PrewarmStats first = warm.PrewarmCache();
        check(first.candidates == kCandidateCount,
              "prewarm: programs with entry points are tried; include-only and .kernel files are skipped");
        check(first.compiled == kGoodCount && first.threads >= 1,
              "prewarm: every healthy shader is compiled, and the broken one does not stop the others");
        check(static_cast<int>(CacheFiles(warmCache).size()) == kGoodCount,
              "prewarm: one cache file per healthy shader, none for the broken one");

        const int missesBefore = warm.CacheMisses();
        const int hitsBefore = warm.CacheHits();
        const AssetID a = warm.Load("good_a");
        const AssetID b = warm.Load("good_b");
        const AssetID c = warm.LoadCompute("comp.cs");
        const ShaderProgram* pa = warm.Get(a);
        const ShaderProgram* pb = warm.Get(b);
        const ShaderProgram* pc = warm.Get(c);
        check(pa != nullptr && pa->valid && pb != nullptr && pb->valid && pc != nullptr && pc->valid,
              "after prewarm: Load / LoadCompute return valid programs");
        check(warm.CacheHits() == hitsBefore + kGoodCount && warm.CacheMisses() == missesBefore,
              "after prewarm: every Load is a cache hit (no compile on the calling thread)");

        const AssetID broken = warm.Load("broken");
        const ShaderProgram* pbroken = warm.Get(broken);
        check(pbroken != nullptr && !pbroken->valid,
              "after prewarm: a broken shader still fails in Load, which reports it");

        const ShaderManager::PrewarmStats second = warm.PrewarmCache();
        check(second.compiled == 0 && static_cast<int>(CacheFiles(warmCache).size()) == kGoodCount,
              "prewarm again: nothing is compiled when the cache files already exist");
    }

    // ---- (4) あたためなしで作ったキャッシュとバイト一致する ----
    {
        ShaderManager plain;
        plain.SetCacheDir(plainCache.wstring(), true);
        check(plain.Init(device, dirs), "shader manager init (no prewarm)");
        plain.Load("good_a");
        plain.Load("good_b");
        plain.LoadCompute("comp.cs");
        const std::vector<std::wstring> warmFiles = CacheFiles(warmCache);
        const std::vector<std::wstring> plainFiles = CacheFiles(plainCache);
        bool same = warmFiles == plainFiles && static_cast<int>(plainFiles.size()) == kGoodCount;
        for (size_t i = 0; same && i < plainFiles.size(); ++i) {
            same = ReadBytes(warmCache / warmFiles[i]) == ReadBytes(plainCache / plainFiles[i]);
        }
        check(same, "prewarm writes byte-identical cache files to a plain Load");
    }

    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== ShaderManager prewarm self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== ShaderManager prewarm self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
