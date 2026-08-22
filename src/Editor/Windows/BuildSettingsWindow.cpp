#include "Editor/Windows/BuildSettingsWindow.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <Windows.h>
#include <shellapi.h>

#include "Editor/AssetOps.h"
#include "Engine/Core/Localization.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Engine/Asset/TerrainAsset.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/FbxLoader.h"
#include "Engine/Engine/ModelLoader.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/SchemaCodegen.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/TextureCook.h"

#include "imgui.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

// 子プロセス (zip 等) を出力リダイレクト + stdin NUL で起動する。
// AssetOps::StartGameLogicBuild と同じ流儀 (対話プロンプトで詰まらせない)
void* StartChildProcess(std::wstring cmdline, const std::wstring& workDir,
                        const std::wstring& logPath)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nulIn;
    PROCESS_INFORMATION pi = {};
    cmdline.push_back(L'\0'); // CreateProcessW は書込可能バッファを要求する
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (nulIn != INVALID_HANDLE_VALUE) {
        CloseHandle(nulIn);
    }
    if (!ok) {
        MYE_LOG_ERROR("[build] CreateProcess failed (%lu)", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// 終了済みなら true を返し、exitCode を書く (未終了なら false)
bool PollProcess(void* handle, uint32_t& exitCode)
{
    if (WaitForSingleObject(handle, 0) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(handle, &code);
    exitCode = code;
    return true;
}

std::wstring LowerExt(const fs::path& p)
{
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

} // namespace

// ---- 段 2: アセットクック温め ----
// assets\ のモデルを RegisterAssets へ通す。ウォームなら .mmdl 再生で即時、コールド
// (セッション中に追加されたばかり等) ならここでパース + クックされる。
// 音声 (.mpcm) は起動時走査が済ませている — cooked ディレクトリごと同梱するので追加作業なし。
// 地形 (.mterr、M58a) は「シーンに置かれていなければ誰もロードしない」ので、ここで
// 明示的に焼く必要がある。焼き忘れは配布物にだけ現れて --package を叩かない限り再現しない
bool BuildSettingsWindow::StageCookWarm(EngineContext& ctx, std::string& detail)
{
    if (!CookedCache::Enabled()) {
        detail = "cook cache disabled (--no-cook-cache)";
        return true; // 温めるものが無いだけで失敗ではない (同梱段が空になる)
    }
    int models = 0, terrains = 0, failed = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(ctx.assetsRoot, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        if (TerrainAsset::IsSourcePath(e.path().wstring())) {
            TerrainAsset::TerrainData terrain;
            TerrainAsset::Load(e.path().wstring(), terrain) ? ++terrains : ++failed;
            continue;
        }
        const std::wstring ext = LowerExt(e.path());
        if (ext != L".glb" && ext != L".gltf" && ext != L".fbx") {
            continue;
        }
        const bool ok = (ext == L".fbx")
            ? FbxLoader::RegisterAssets(*ctx.resources, *ctx.shaders, e.path().wstring(), true)
            : ModelLoader::RegisterAssets(*ctx.resources, *ctx.shaders, e.path().wstring(), true);
        ok ? ++models : ++failed;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d model(s) + %d terrain(s) warm%s", models, terrains,
                  failed ? " (some failed)" : "");
    detail = buf;
    return failed == 0;
}

// ---- 段 3: パッケージコピー (M15 の DoPackage + M51j の cooked 同梱) ----
bool BuildSettingsWindow::StageCopy(EngineContext& ctx, std::string& detail)
{
    std::error_code ec;
    const fs::path exeDir = GetExecutableDir(); // Runtime.exe / GameLogic.dll と同じ場所
    const fs::path out = fs::path(Utf8ToWide(outputDir_));
    const fs::path assetsSrc = fs::path(ctx.assetsRoot);

    fs::create_directories(out, ec);
    if (ec) {
        detail = "cannot create output dir";
        MYE_LOG_ERROR("[build] cannot create %s", outputDir_);
        return false;
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
        detail = "Runtime.exe / GameLogic.dll not found (build Release, then Rebuild Scripts)";
        return false;
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
        detail = "assets copy error";
        MYE_LOG_ERROR("[build] assets copy failed: %s", ec.message().c_str());
        return false;
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
                detail = "engine shader copy error";
                MYE_LOG_ERROR("[build] engine shader copy failed: %s", ec.message().c_str());
                return false;
            }
            MYE_LOG_INFO("[build] bundled engine shaders from %s",
                         WideToUtf8(engineShaders).c_str());
        }
    }

    // 2c) M51j: クック済みキャッシュを同梱し、封印マーカーを書く。
    // モデルのサブアセット AssetID は正規化した**絶対パス**由来なので、配布シーンは
    // パッケージ元パスから導出された ID を参照している。移設先で再クック (フレッシュ
    // パース) すると別 ID が登録されシーンの参照が空振りする — 封印キャッシュが
    // 「クック時の登録列」をそのまま再生することが、配布物の移設耐性そのもの (spec §10)
    if (CookedCache::Enabled()) {
        const fs::path cookedSrc = fs::path(CookedCache::Dir());
        const fs::path cookedDst = out / L"cache" / L"cooked";
        fs::create_directories(cookedDst, ec);
        int cooked = 0;
        for (const auto& e : fs::directory_iterator(cookedSrc, ec)) {
            if (!e.is_regular_file()) {
                continue;
            }
            const std::wstring ext = LowerExt(e.path());
            if (ext == L".mmdl" || ext == L".mpcm" || ext == TerrainAsset::kTerrainExt) {
                if (copyFile(e.path(), cookedDst / e.path().filename())) {
                    ++cooked;
                }
            }
        }
        std::ofstream seal(cookedDst / CookedCache::kSealedMarker, std::ios::binary);
        seal << "sealed by MyEngine build\n";
        MYE_LOG_INFO("[build] bundled %d cooked file(s) + sealed marker", cooked);
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
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d file(s), boot=%s", fileCount, bootScene_.c_str());
    detail = buf;
    MYE_LOG_INFO("[build] package ready: %s (%d files, boot=%s)", outputDir_, fileCount,
                 bootScene_.c_str());
    return true;
}

// ---- 段 4: DDS 一括クック (opt-in) ----
// パッケージ内の .png/.jpg/.jpeg/.tga を各自の .meta インポート設定で .dds 化し、元画像を
// 除去する (.meta は残す — srgb/GUID 解決に使う)。ランタイムは「元画像なし + 同名 .dds」を
// 同じ AssetID で読む (TextureLibrary::LoadFile のフォールバック)。開発環境側は一切不変
bool BuildSettingsWindow::StageDds(EngineContext& ctx, std::string& detail)
{
    (void)ctx;
    std::error_code ec;
    const fs::path outAssets = fs::path(Utf8ToWide(outputDir_)) / L"assets";
    int cooked = 0, skipped = 0, failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(outAssets, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::wstring ext = LowerExt(e.path());
        if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".tga") {
            continue;
        }
        const fs::path dds = fs::path(e.path()).replace_extension(L".dds");
        if (fs::exists(dds, ec)) {
            ++skipped; // 手動クック済みの .dds を上書きしない (元画像も残す)
            continue;
        }
        AssetMeta m;
        AssetDatabase::ReadMeta(e.path().wstring() + L".meta", m); // 不在なら既定値
        TextureCook::CookOptions co;
        co.generateMips = m.tex.generateMips != 0;
        co.compress = m.tex.compress == 0;
        if (TextureCook::CookImageToDds(e.path().wstring(), dds.wstring(), co)) {
            fs::remove(e.path(), ec); // 配布物から元画像を落とす (.meta は残す)
            ++cooked;
        } else {
            ++failed;
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d cooked, %d skipped, %d failed", cooked, skipped, failed);
    detail = buf;
    return failed == 0;
}

void BuildSettingsWindow::FinishStage(StrId name, bool ok, bool skipped, std::string detail)
{
    results_.push_back({ name, ok, skipped, std::move(detail) });
    MYE_LOG_INFO("[build] stage %s: %s%s%s", Tr(results_.back().name),
                 skipped ? "skipped" : (ok ? "OK" : "FAILED"),
                 results_.back().detail.empty() ? "" : " - ",
                 results_.back().detail.c_str());
}

void BuildSettingsWindow::StartCliPackage(const std::wstring& outDir, bool dds, bool zip,
                                          const std::string& bootScene)
{
    std::snprintf(outputDir_, sizeof(outputDir_), "%s", WideToUtf8(outDir).c_str());
    init_ = true; // 既定出力先の上書きを防ぐ
    ddsCook_ = dds;
    zipOutput_ = zip;
    if (!bootScene.empty()) {
        bootScene_ = bootScene;
    }
    results_.clear();
    status_.clear();
    stage_ = Stage::Scripts;
    MYE_LOG_INFO("[build] CLI package started: %s", outputDir_);
}

bool BuildSettingsWindow::PipelineSucceeded() const
{
    if (stage_ != Stage::Done) {
        return false;
    }
    for (const StageResult& r : results_) {
        if (!r.ok && !r.skipped) {
            return false;
        }
    }
    return true;
}

void BuildSettingsWindow::AdvancePipeline(EngineContext& ctx)
{
    switch (stage_) {
    case Stage::Idle:
    case Stage::Done:
        return;

    case Stage::Scripts: {
        if (!rebuildScripts_) {
            FinishStage(StrId::Build_StScripts, true, true, {});
            stage_ = Stage::CompileCs;
            return;
        }
        if (proc_ == nullptr) {
            proc_ = StartGameLogicBuild(ctx, procLog_);
            if (proc_ == nullptr) {
                FinishStage(StrId::Build_StScripts, false, false, "could not start (see log)");
                stage_ = Stage::Done;
                status_ = Tr(StrId::Build_Failed);
            }
            return;
        }
        uint32_t code = 1;
        if (!PollProcess(proc_, code)) {
            return; // 実行中 — 次フレームでまた見る (UI は生きたまま)
        }
        CloseHandle(proc_);
        proc_ = nullptr;
        const bool ok = code == 0;
        FinishStage(StrId::Build_StScripts, ok, false,
                    ok ? std::string() : "exit " + std::to_string(code) + " (see "
                                             + WideToUtf8(procLog_) + ")");
        if (!ok) {
            stage_ = Stage::Done;
            status_ = Tr(StrId::Build_Failed);
            return;
        }
        stage_ = Stage::CompileCs;
        return;
    }

    case Stage::CompileCs: {
        // rebuildScripts_ off のときは C# も温存 (対で 1 つの opt-out)
        if (!rebuildScripts_) {
            FinishStage(StrId::Build_StCs, true, true, {});
            stage_ = Stage::CookWarm;
            return;
        }
        bool ok = true;
        std::string detail;
        if (ctx.managedHost == nullptr || !ctx.managedHost->IsReady()) {
            detail = "no C# host (skipped)"; // ホスト無し環境では C# 資産も動かないので害なし
        } else {
            schema::WriteCSharpBindings(ctx.assetsRoot);
            ok = ctx.managedHost->CompileScripts(ctx.assetsRoot + L"\\scripts");
            if (!ok) {
                detail = "compile errors (see Console)";
            }
        }
        FinishStage(StrId::Build_StCs, ok, false, std::move(detail));
        stage_ = ok ? Stage::CookWarm : Stage::Done;
        if (!ok) {
            status_ = Tr(StrId::Build_Failed);
        }
        return;
    }

    case Stage::CookWarm: {
        std::string detail;
        const bool ok = StageCookWarm(ctx, detail);
        FinishStage(StrId::Build_StCook, ok, false, std::move(detail));
        stage_ = ok ? Stage::Copy : Stage::Done;
        if (!ok) {
            status_ = Tr(StrId::Build_Failed);
        }
        return;
    }

    case Stage::Copy: {
        std::string detail;
        const bool ok = StageCopy(ctx, detail);
        FinishStage(StrId::Build_StCopy, ok, false, std::move(detail));
        stage_ = ok ? Stage::Dds : Stage::Done;
        if (!ok) {
            status_ = Tr(StrId::Build_Failed);
        }
        return;
    }

    case Stage::Dds: {
        if (!ddsCook_) {
            FinishStage(StrId::Build_StDds, true, true, {});
            stage_ = Stage::Zip;
            return;
        }
        std::string detail;
        const bool ok = StageDds(ctx, detail);
        FinishStage(StrId::Build_StDds, ok, false, std::move(detail));
        stage_ = ok ? Stage::Zip : Stage::Done;
        if (!ok) {
            status_ = Tr(StrId::Build_Failed);
        }
        return;
    }

    case Stage::Zip: {
        if (!zipOutput_) {
            FinishStage(StrId::Build_StZip, true, true, {});
            stage_ = Stage::Done;
            status_ = Tr(StrId::Build_DoneOk);
            return;
        }
        const fs::path out = fs::path(Utf8ToWide(outputDir_));
        const fs::path zip = fs::path(out.wstring() + L".zip");
        if (proc_ == nullptr) {
            std::error_code ec;
            fs::remove(zip, ec); // 古い zip を先に落とす (tar は truncate するが明示的に)
            procLog_ = out.wstring() + L".zip.log";
            // Windows 標準の bsdtar (-a = 拡張子から zip 形式を推定)。出力フォルダの中身を
            // アーカイブ直下に入れる (-C out .) — 展開してそのまま実行できる形
            const std::wstring cmd = L"tar.exe -a -c -f \"" + zip.wstring() + L"\" -C \""
                + out.wstring() + L"\" .";
            proc_ = StartChildProcess(cmd, out.parent_path().wstring(), procLog_);
            if (proc_ == nullptr) {
                FinishStage(StrId::Build_StZip, false, false, "tar.exe not available?");
                stage_ = Stage::Done;
                status_ = Tr(StrId::Build_Failed);
            }
            return;
        }
        uint32_t code = 1;
        if (!PollProcess(proc_, code)) {
            return;
        }
        CloseHandle(proc_);
        proc_ = nullptr;
        const bool ok = code == 0;
        FinishStage(StrId::Build_StZip, ok, false,
                    ok ? WideToUtf8(zip.filename().wstring())
                       : "exit " + std::to_string(code));
        stage_ = Stage::Done;
        status_ = ok ? Tr(StrId::Build_DoneOk) : Tr(StrId::Build_Failed);
        return;
    }
    }
}

void BuildSettingsWindow::OnImGui(EngineContext& ctx)
{
    AdvancePipeline(ctx); // 実行中はウィンドウが閉じられても進める
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

    ImGui::TextUnformatted(Tr(StrId::Build_Desc));
    ImGui::TextDisabled("%s", Tr(StrId::Build_ReleaseNote));
    ImGui::Separator();

    const bool running = stage_ != Stage::Idle && stage_ != Stage::Done;
    ImGui::BeginDisabled(running);
    ImGui::Checkbox(Tr(StrId::Build_BundleDotnet), &bundleDotnet_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Build_TipBundle));
    }
    ImGui::Checkbox(Tr(StrId::Build_OptScripts), &rebuildScripts_);
    ImGui::Checkbox(Tr(StrId::Build_OptDds), &ddsCook_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr(StrId::Build_TipDds));
    }
    ImGui::Checkbox(Tr(StrId::Build_OptZip), &zipOutput_);

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
    if (ImGui::BeginCombo(Tr(StrId::Build_BootScene), bootScene_.c_str())) {
        for (const std::string& s : scenes) {
            if (ImGui::Selectable(s.c_str(), s == bootScene_)) {
                bootScene_ = s;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::InputText(Tr(StrId::Build_OutputFolder), outputDir_, sizeof(outputDir_));

    if (ImGui::Button(Tr(StrId::Build_Package), ImVec2(160, 0))) {
        results_.clear();
        status_.clear();
        stage_ = Stage::Scripts;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(StrId::Build_OpenFolder))) {
        ShellExecuteW(nullptr, L"open", Utf8ToWide(outputDir_).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }

    // ---- 段の結果一覧 + 実行中表示 ----
    if (!results_.empty() || running) {
        ImGui::Separator();
        for (const StageResult& r : results_) {
            if (r.skipped) {
                ImGui::TextDisabled("- %s %s", Tr(r.name), Tr(StrId::Build_Skipped));
            } else if (r.ok) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f), "OK  %s", Tr(r.name));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.35f, 1.0f), "NG  %s", Tr(r.name));
            }
            if (!r.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", r.detail.c_str());
            }
        }
        if (running) {
            // 今どの段に居るか (spinner 代わりに経過ドット)
            const StrId cur = stage_ == Stage::Scripts ? StrId::Build_StScripts
                : stage_ == Stage::CompileCs           ? StrId::Build_StCs
                : stage_ == Stage::CookWarm            ? StrId::Build_StCook
                : stage_ == Stage::Copy                ? StrId::Build_StCopy
                : stage_ == Stage::Dds                 ? StrId::Build_StDds
                                                       : StrId::Build_StZip;
            const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
            ImGui::Text("%s %s%.*s", Tr(StrId::Build_Working), Tr(cur), dots, "...");
        }
    }

    if (!status_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", status_.c_str());
    }

    ImGui::End();
}

} // namespace mye
