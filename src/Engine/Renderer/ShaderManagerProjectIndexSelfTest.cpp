/*----
 ShaderManagerProjectIndexSelfTest.cpp  プロジェクトシェーダ索引の回帰テスト
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ShaderManagerProjectIndexSelfTest.h"

#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ShaderManager.h"

namespace fs = std::filesystem;

namespace mye {

bool RunShaderManagerProjectIndexSelfTest()
{
    MYE_LOG_INFO("==== ShaderManager project index self test ====");
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
    const fs::path root = fs::temp_directory_path(ec) / L"mye_shader_index_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"a", ec);
    fs::create_directories(root / L"b", ec);
    fs::create_directories(root / L"fx", ec);
    fs::create_directories(root / L"surf", ec);

    auto writeHlsl = [&](const fs::path& p) {
        std::ofstream f(p, std::ios::binary);
        f << "// test\n";
    };

    const fs::path uniquePath = root / L"fx" / L"Only.post.hlsl";
    writeHlsl(uniquePath);
    writeHlsl(root / L"a" / L"Dup.post.hlsl");
    writeHlsl(root / L"b" / L"Dup.post.hlsl");

    // M79: *.surface.hlsl も同じ索引に乗る (短名 "Foo.surface")
    const fs::path uniqueSurfacePath = root / L"surf" / L"Only.surface.hlsl";
    writeHlsl(uniqueSurfacePath);
    writeHlsl(root / L"a" / L"Dup.surface.hlsl");
    writeHlsl(root / L"b" / L"Dup.surface.hlsl");

    ShaderManager sm;
    sm.SetAssetsRoot(root.wstring());
    sm.RebuildProjectShaderIndex();

    check(NormalizePathKey(sm.ResolveShaderPath("Only.post"))
              == NormalizePathKey(uniquePath.wstring()),
          "unique project post resolves to its path under assets");

    const std::wstring dupResolved = sm.ResolveShaderPath("Dup.post");
    check(NormalizePathKey(dupResolved) != NormalizePathKey((root / L"a" / L"Dup.post.hlsl").wstring())
              && NormalizePathKey(dupResolved)
                     != NormalizePathKey((root / L"b" / L"Dup.post.hlsl").wstring()),
          "duplicate short names are not indexed (no silent pick)");

    check(NormalizePathKey(sm.ResolveShaderPath("Only.surface"))
              == NormalizePathKey(uniqueSurfacePath.wstring()),
          "unique project surface shader resolves to its path under assets");

    const std::wstring dupSurfaceResolved = sm.ResolveShaderPath("Dup.surface");
    check(NormalizePathKey(dupSurfaceResolved)
                  != NormalizePathKey((root / L"a" / L"Dup.surface.hlsl").wstring())
              && NormalizePathKey(dupSurfaceResolved)
                     != NormalizePathKey((root / L"b" / L"Dup.surface.hlsl").wstring()),
          "duplicate surface shader short names are not indexed (no silent pick)");

    if (failCount == 0) {
        MYE_LOG_INFO("==== ShaderManager project index self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== ShaderManager project index self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
