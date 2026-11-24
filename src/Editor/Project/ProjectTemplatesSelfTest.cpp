//====================================================================================
//                          ProjectTemplatesSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     11/24/2026
//                                          プロジェクトのテンプレートと開始内容の自己テスト
//====================================================================================
#include "Editor/Project/ProjectTemplatesSelfTest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "Editor/Project/ProjectTemplates.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/App/Project.h"
#include "Engine/Engine/Demo/StartScene.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Scene/Scene.h"

namespace mye {

namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// キー無しの古いマニフェストを置いたプロジェクトを作る (assets は空)
void MakeLegacyProject(const fs::path& root)
{
    std::error_code ec;
    fs::create_directories(root / L"assets", ec);
    WriteText(root / kProjectManifestFile, "{\"formatVersion\":1,\"name\":\"legacy\"}");
}

} // namespace

bool RunProjectTemplatesSelfTest()
{
    MYE_LOG_INFO("==== ProjectTemplates self test ====");
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
    const fs::path tempRoot = fs::temp_directory_path(ec) / L"mye_project_templates_selftest";
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);

    // ---- (1) マニフェストの startContent が往復する ----
    {
        const fs::path dir = tempRoot / L"manifest";
        fs::create_directories(dir, ec);
        ProjectManifest written;
        written.name = "m";

        written.startContent = ProjectStartContent::Empty;
        ProjectManifest loaded;
        check(SaveProjectManifest(dir.wstring(), written) && LoadProjectManifest(dir.wstring(), loaded)
                  && loaded.startContent == ProjectStartContent::Empty,
              "manifest: startContent Empty survives a save/load round trip");

        written.startContent = ProjectStartContent::Demo;
        loaded = {};
        check(SaveProjectManifest(dir.wstring(), written) && LoadProjectManifest(dir.wstring(), loaded)
                  && loaded.startContent == ProjectStartContent::Demo,
              "manifest: startContent Demo survives a save/load round trip");

        written.startContent = ProjectStartContent::Unspecified;
        loaded = {};
        loaded.startContent = ProjectStartContent::Demo; // 読み込みが上書きすることを見る
        const bool saved = SaveProjectManifest(dir.wstring(), written);
        const std::string text = ReadText(dir / kProjectManifestFile);
        check(saved && text.find("startContent") == std::string::npos,
              "manifest: Unspecified does not write the key");
        check(LoadProjectManifest(dir.wstring(), loaded)
                  && loaded.startContent == ProjectStartContent::Unspecified,
              "manifest: a missing key loads as Unspecified");

        WriteText(dir / kProjectManifestFile, "{\"name\":\"m\",\"startContent\":\"something-new\"}");
        loaded = {};
        check(LoadProjectManifest(dir.wstring(), loaded)
                  && loaded.startContent == ProjectStartContent::Unspecified,
              "manifest: an unknown startContent value loads as Unspecified");
    }

    // ---- (2) CreateProject がテンプレートに応じた値を書く ----
    const fs::path engineAssets = tempRoot / L"engine_assets";
    WriteText(engineAssets / L"textures" / L"test.png", "not a real png");
    const fs::path emptyProject = tempRoot / L"empty_project";
    const fs::path demoProject = tempRoot / L"demo_project";
    {
        std::string err;
        ProjectManifest m;
        check(CreateProject(emptyProject.wstring(), "e", ProjectTemplate::Empty, engineAssets.wstring(), &err)
                  && LoadProjectManifest(emptyProject.wstring(), m)
                  && m.startContent == ProjectStartContent::Empty,
              "create: the Empty template writes startContent Empty");
        check(!fs::exists(emptyProject / L"assets" / L"textures" / L"test.png", ec),
              "create: the Empty template does not copy demo assets");
        m = {};
        check(CreateProject(demoProject.wstring(), "d", ProjectTemplate::Demo3D, engineAssets.wstring(), &err)
                  && LoadProjectManifest(demoProject.wstring(), m)
                  && m.startContent == ProjectStartContent::Demo,
              "create: the 3D Demo template writes startContent Demo");
    }

    // ---- (3) 判定の表 (ファイルに触れない本体) ----
    {
        DemoContentQuery q;
        check(UsesDemoContent(q), "decide: no project always uses demo content");

        q.hasProject = true;
        q.startContent = ProjectStartContent::Empty;
        q.sceneExists = true;
        q.demoTextureExists = true;
        check(!UsesDemoContent(q), "decide: explicit Empty never uses demo content");
        q.showcaseRequested = true;
        check(UsesDemoContent(q), "decide: a requested showcase overrides Empty");

        q = {};
        q.hasProject = true;
        q.startContent = ProjectStartContent::Demo;
        check(UsesDemoContent(q), "decide: explicit Demo uses demo content even with no assets");

        q = {};
        q.hasProject = true;
        check(!UsesDemoContent(q), "decide: Unspecified with no scene and no demo texture is empty");
        q.sceneExists = true;
        check(UsesDemoContent(q), "decide: Unspecified with a saved scene keeps demo content");
        q.sceneExists = false;
        q.demoTextureExists = true;
        check(UsesDemoContent(q), "decide: Unspecified with the demo texture keeps demo content");
    }

    // ---- (4) 実ファイルから入力を集める版 ----
    {
        EngineContext ctx;
        const std::wstring noScene = (tempRoot / L"no_such.scene.json").wstring();
        check(ProjectUsesDemoContent(ctx, noScene, false),
              "collect: an empty projectRoot uses demo content");

        ctx.projectRoot = emptyProject.wstring();
        ctx.assetsRoot = (emptyProject / L"assets").wstring();
        check(!ProjectUsesDemoContent(ctx, noScene, false),
              "collect: a project created with Empty does not use demo content");
        check(ProjectUsesDemoContent(ctx, noScene, true),
              "collect: a requested showcase uses demo content in an Empty project");

        ctx.projectRoot = demoProject.wstring();
        ctx.assetsRoot = (demoProject / L"assets").wstring();
        check(ProjectUsesDemoContent(ctx, noScene, false),
              "collect: a project created with 3D Demo uses demo content");

        const fs::path legacy = tempRoot / L"legacy_project";
        MakeLegacyProject(legacy);
        ctx.projectRoot = legacy.wstring();
        ctx.assetsRoot = (legacy / L"assets").wstring();
        check(!ProjectUsesDemoContent(ctx, noScene, false),
              "collect: a legacy manifest with no scene and no demo texture is treated as empty");

        const fs::path scene = legacy / L"assets" / L"scenes" / L"main.scene.json";
        WriteText(scene, "{}");
        check(ProjectUsesDemoContent(ctx, scene.wstring(), false),
              "collect: a legacy manifest with a saved scene keeps demo content");
        fs::remove(scene, ec);

        // 配布物は元画像の代わりに同名の .dds を持つ
        WriteText(legacy / L"assets" / L"textures" / L"test.dds", "cooked");
        check(ProjectUsesDemoContent(ctx, noScene, false),
              "collect: a cooked demo texture (.dds) counts as present");
    }

    // ---- (5) 空の開始シーンはカメラとライトだけ ----
    {
        Scene scene;
        EngineContext ctx;
        ctx.scene = &scene;
        BuildEmptyStartScene(ctx);
        auto countWith = [&](ComponentTypeId type) {
            uint32_t count = 0;
            const ComponentTypeId required[] = { type };
            scene.GetWorld().ForEachArchetype(required, [&](Archetype& arch) { count += arch.Count(); });
            return count;
        };
        check(scene.GetWorld().AliveCount() == 2, "empty scene: exactly two entities");
        check(countWith(CameraComponent::sTypeId) == 1 && countWith(LightComponent::sTypeId) == 1,
              "empty scene: one camera and one light");
        check(countWith(MeshRendererComponent::sTypeId) == 0, "empty scene: nothing to draw");
    }

    fs::remove_all(tempRoot, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== ProjectTemplates self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== ProjectTemplates self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
