#include "Editor/Windows/BuildSettingsWindow.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {

void BuildSettingsWindow::DoPackage(EngineContext& ctx)
{
    std::error_code ec;
    const fs::path exeDir = GetExecutableDir(); // Runtime.exe / GameLogic.dll と同じ場所
    const fs::path out = fs::path(Utf8ToWide(outputDir_));
    const fs::path assetsSrc = fs::path(ctx.assetsRoot);

    fs::create_directories(out, ec);
    if (ec) {
        status_ = "FAILED: cannot create output dir";
        MYE_LOG_ERROR("[build] cannot create %s", outputDir_);
        return;
    }

    auto copyFile = [&](const fs::path& from, const fs::path& to) -> bool {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            MYE_LOG_ERROR("[build] copy failed: %s", WideToUtf8(from.wstring()).c_str());
            return false;
        }
        return true;
    };

    // 1) 実行ファイル + スクリプト DLL
    bool ok = copyFile(exeDir / L"Runtime.exe", out / L"Runtime.exe");
    ok = copyFile(exeDir / L"GameLogic.dll", out / L"GameLogic.dll") && ok;
    if (!ok) {
        status_ = "FAILED: Runtime.exe / GameLogic.dll not found (build Release first)";
        return;
    }

    // 2) assets\ を丸ごとコピー (.prefab.json / .anim.json / .meta.json も漏らさない)
    fs::copy(assetsSrc, out / L"assets",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        status_ = "FAILED: assets copy error";
        MYE_LOG_ERROR("[build] assets copy failed: %s", ec.message().c_str());
        return;
    }

    // 3) ブートシーンを main.scene.json として配置 (Runtime は既定でこれを読む)
    const fs::path chosen = assetsSrc / L"scenes" / Utf8ToWide(bootScene_);
    const fs::path bootDst = out / L"assets" / L"scenes" / L"main.scene.json";
    if (fs::exists(chosen, ec) && chosen.filename() != L"main.scene.json") {
        copyFile(chosen, bootDst);
    }

    int fileCount = 0;
    for (auto it = fs::recursive_directory_iterator(out, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) {
            ++fileCount;
        }
    }
    status_ = "OK: packaged " + std::to_string(fileCount) + " files";
    MYE_LOG_INFO("[build] package ready: %s (%d files, boot=%s)", outputDir_, fileCount,
                 bootScene_.c_str());
}

void BuildSettingsWindow::OnImGui(EngineContext& ctx)
{
    if (!open) {
        return;
    }
    if (!init_) {
        // 既定の出力先 = リポジトリ直下 (assets の親) \dist
        const std::wstring def = (fs::path(ctx.assetsRoot).parent_path() / L"dist").wstring();
        std::snprintf(outputDir_, sizeof(outputDir_), "%s", WideToUtf8(def).c_str());
        init_ = true;
    }
    if (!ImGui::Begin("Build Settings", &open)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Package Runtime.exe + GameLogic.dll + assets\\ into a standalone folder.");
    ImGui::TextDisabled("(Compile the Release config in Visual Studio / MSBuild first.)");
    ImGui::Separator();

    // ---- ブートシーン選択 (assets\scenes\*.scene.json) ----
    std::vector<std::string> scenes;
    {
        std::error_code ec;
        const fs::path dir = fs::path(ctx.assetsRoot) / L"scenes";
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = WideToUtf8(e.path().filename().wstring());
            if (n.size() >= 11 && n.compare(n.size() - 11, 11, ".scene.json") == 0) {
                scenes.push_back(n);
            }
        }
    }
    if (ImGui::BeginCombo("Boot scene", bootScene_.c_str())) {
        for (const std::string& s : scenes) {
            if (ImGui::Selectable(s.c_str(), s == bootScene_)) {
                bootScene_ = s;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::InputText("Output folder", outputDir_, sizeof(outputDir_));

    if (ImGui::Button("Package Build", ImVec2(160, 0))) {
        DoPackage(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) {
        ShellExecuteW(nullptr, L"open", Utf8ToWide(outputDir_).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }

    if (!status_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", status_.c_str());
    }

    ImGui::End();
}

} // namespace mye
