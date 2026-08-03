#include "Editor/Windows/BuildSettingsWindow.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "Engine/Core/Localization.h"
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

    // 1) 実行ファイル + スクリプト DLL。
    // GameLogic.dll の出所は EngineLoop の監視先と揃える: プロジェクト起動なら
    // <project>\cache\ (Rebuild Scripts がここに吐く)、レガシー起動なら exe の隣。
    // exe 隣を無条件に使うとエンジンのデモスクリプトを配布してしまう
    const fs::path logicSrc = ctx.projectRoot.empty()
        ? exeDir / L"GameLogic.dll"
        : fs::path(ctx.projectRoot) / L"cache" / L"GameLogic.dll";
    bool ok = copyFile(exeDir / L"Runtime.exe", out / L"Runtime.exe");
    ok = copyFile(logicSrc, out / L"GameLogic.dll") && ok;
    if (!ok) {
        status_ = "FAILED: Runtime.exe / GameLogic.dll not found "
                  "(build Release, then Rebuild Scripts)";
        return;
    }

    // 1b) C# スクリプトホスト (CoreCLR) — nethost.dll + マネージド一式 (Roslyn 含む)
    // これらが無いと配布先で C# スクリプトが動かない (native ManagedHost の Init が失敗)。
    copyFile(exeDir / L"nethost.dll", out / L"nethost.dll");
    auto copyGlob = [&](const wchar_t* pattern) {
        for (const auto& e : fs::directory_iterator(exeDir, ec)) {
            if (!e.is_regular_file()) {
                continue;
            }
            const std::wstring fn = e.path().filename().wstring();
            if (fn.rfind(pattern, 0) == 0) { // 前方一致
                copyFile(e.path(), out / e.path().filename());
            }
        }
    };
    copyGlob(L"MyeScripting");         // .dll / .runtimeconfig.json / .deps.json / .pdb
    copyGlob(L"Microsoft.CodeAnalysis"); // Roslyn
    copyGlob(L"System.Collections.Immutable");
    copyGlob(L"System.Reflection.Metadata");

    // 1c) .NET ランタイム同梱 (自己完結配布)。プロセス内 coreclr.dll から SDK レイアウトを特定。
    // 配布先の Runtime.exe は起動時に exe 隣の dotnet\ を DOTNET_ROOT として使う (ManagedHost)。
    if (bundleDotnet_) {
        HMODULE coreclr = GetModuleHandleW(L"coreclr.dll");
        wchar_t clrPath[MAX_PATH] = {};
        if (coreclr && GetModuleFileNameW(coreclr, clrPath, MAX_PATH)) {
            const fs::path clr(clrPath); // ...\dotnet\shared\Microsoft.NETCore.App\<ver>\coreclr.dll
            const fs::path fwVerDir = clr.parent_path();
            const std::wstring ver = fwVerDir.filename().wstring();
            const fs::path dotnetRoot = fwVerDir.parent_path().parent_path().parent_path();
            const fs::path outDn = out / L"dotnet";
            // host\fxr\* (hostfxr.dll) と shared framework の当該バージョンをコピー
            fs::copy(dotnetRoot / L"host", outDn / L"host",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            fs::copy(fwVerDir, outDn / L"shared" / L"Microsoft.NETCore.App" / ver,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                MYE_LOG_WARN("[build] .NET runtime bundle incomplete: %s", ec.message().c_str());
                ec.clear();
            } else {
                MYE_LOG_INFO("[build] bundled .NET runtime %s (self-contained)",
                             WideToUtf8(ver).c_str());
            }
        } else {
            MYE_LOG_WARN("[build] coreclr.dll not located — skipping runtime bundle "
                         "(target needs .NET 8 installed)");
        }
    }

    // 2) assets\ を丸ごとコピー (.prefab.json / .anim.json / .meta.json も漏らさない)
    fs::copy(assetsSrc, out / L"assets",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        status_ = "FAILED: assets copy error";
        MYE_LOG_ERROR("[build] assets copy failed: %s", ec.message().c_str());
        return;
    }

    // 2b) エンジン組込みシェーダを同梱する。シェーダは 2 ルート解決なのでプロジェクトの
    // assets\shaders には上書き分しか無い — ここで補わないと配布先で描画できない。
    // skip_existing でプロジェクト側の上書きを勝たせる (解決順と同じ優先度)。
    // これを入れることで dist\ は実行時にエンジンリポジトリへ依存しない
    {
        const std::wstring engineShaders = FindEngineShaderDir();
        if (engineShaders.empty()) {
            MYE_LOG_WARN("[build] engine shader dir not found - dist may be missing shaders");
        } else {
            fs::copy(engineShaders, out / L"assets" / L"shaders",
                     fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            if (ec) {
                status_ = "FAILED: engine shader copy error";
                MYE_LOG_ERROR("[build] engine shader copy failed: %s", ec.message().c_str());
                return;
            }
            MYE_LOG_INFO("[build] bundled engine shaders from %s",
                         WideToUtf8(engineShaders).c_str());
        }
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
    if (!ImGui::Begin(Tr(StrId::Win_BuildSettings), &open)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Package Runtime.exe + GameLogic.dll + C# host + assets\\ into a folder.");
    ImGui::TextDisabled("(Compile the Release config in Visual Studio / MSBuild first.)");
    ImGui::Separator();

    ImGui::Checkbox("Bundle .NET runtime (self-contained)", &bundleDotnet_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON: .NET 8 ランタイムを dotnet\\ に同梱し、配布先に .NET 不要にする\n"
                          "OFF: 配布先に .NET 8 ランタイムのインストールが必要");
    }

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
