//====================================================================================
//                          ComputeSettingsSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          compute 節・Compute 欄の行・カーネルのホットリロードの回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/ComputeSettingsSelfTest.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"
#include "Engine/Engine/Compute/ComputeInspectorRows.h"
#include "Engine/Engine/Compute/ComputeService.h"
#include "Engine/Engine/Compute/ComputeSettings.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelRange.h"
#include "Engine/Engine/Session/Provenance.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/Shader/ShaderManager.h"
#include "Shared/KernelCompat.h"

// ComputeServiceSelfTest.cpp と同じカーネルを別の名前空間に取り込む (重複定義を避ける)
namespace mye::kernel::cpu::SettingsSaxpy {
#include "../../../../assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl"
}

namespace mye {
namespace {

using namespace compute;
namespace fs = std::filesystem;
namespace sx = kernel::cpu::SettingsSaxpy;

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

constexpr const char* kScript = "RowScript";
constexpr const char* kSaxpy = "SelfTestSaxpy";

void WriteText(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string EngineKernelText(const char* leaf)
{
    return ReadText(fs::path(FindEngineShaderDir()) / L"kernels" / leaf);
}

// Saxpy のソースを別名のカーネルとして書き直す (KERNEL の名前はファイル名と一致しなければ検査に落ちる)
std::string RenamedSaxpy(const std::string& text, const std::string& name)
{
    const std::string from = "KERNEL(SelfTestSaxpy,";
    std::string out = text;
    const size_t at = out.find(from);
    if (at != std::string::npos) {
        out.replace(at, from.size(), "KERNEL(" + name + ",");
    }
    return out;
}

const ComputeInspectorRow* FindRow(const std::vector<ComputeInspectorRow>& rows, const char* kernel)
{
    for (const ComputeInspectorRow& r : rows) {
        if (r.kernel == kernel) {
            return &r;
        }
    }
    return nullptr;
}

void TestParse()
{
    ComputeProjectSettings s;
    Check(!ParseComputeSettings("{}", s) && s.config.gpuMinElements == 4096 && s.config.cpuMaxChunks == 0
              && s.config.fxUploadBytesPerTick == 8388608 && s.config.fxDispatchPerTick == 64
              && s.config.stateTotalBytes == 16777216 && s.kernelOverridesCpu.empty(),
          "compute 節が無ければ既定値");
    Check(!ParseComputeSettings("not json", s) && s.config.stateTotalBytes == 16777216, "壊れた JSON は既定値");
    Check(ParseComputeSettings(
              R"({"compute":{"gpuMinElements":1,"cpuMaxChunks":8,"fxUploadBytesPerTick":1024,"fxDispatchPerTick":2,
                  "stateTotalBytes":2048,"kernelOverrides":{"A":"cpu","B":"auto","C":"gpu","D":3}}})",
              s)
              && s.config.gpuMinElements == 1 && s.config.cpuMaxChunks == 8 && s.config.fxUploadBytesPerTick == 1024
              && s.config.fxDispatchPerTick == 2 && s.config.stateTotalBytes == 2048,
          "全部のキーを読める");
    Check(s.kernelOverridesCpu == std::set<std::string>{ "A" }, "kernelOverrides は \"cpu\" だけが上書き (auto・不正な値は無視)");

    // 不正な値はそのキーだけ既定値に倒れ、他のキーは生きる
    Check(ParseComputeSettings(
              R"({"compute":{"gpuMinElements":-5,"cpuMaxChunks":"many","fxUploadBytesPerTick":1.5,
                  "fxDispatchPerTick":99999999,"stateTotalBytes":4096}})",
              s)
              && s.config.gpuMinElements == 4096 && s.config.cpuMaxChunks == 0 && s.config.fxUploadBytesPerTick == 8388608
              && s.config.fxDispatchPerTick == 64 && s.config.stateTotalBytes == 4096,
          "負数・文字列・小数・範囲外は既定値に倒れ、正しいキーは読める");
    Check(!ParseComputeSettings(R"({"compute":[1,2]})", s) && s.config.stateTotalBytes == 16777216,
          "compute がオブジェクトでなければ既定値");
    Check(ParseComputeSettings(R"({"compute":{"kernelOverrides":["A"]}})", s) && s.kernelOverridesCpu.empty(),
          "kernelOverrides がオブジェクトでなければ無視");
}

void TestLoadApplySave(const fs::path& root)
{
    const fs::path assets = root / L"assets";
    const fs::path file = assets / L"project_settings.json";
    WriteText(file, R"({"physicsLayers":["Default"],"particleBackend":"gpu","ui":{"referenceW":1280,"referenceH":720},)"
                    R"("compute":{"gpuMinElements":16,"stateTotalBytes":65536,"kernelOverrides":{"SelfTestSaxpy":"cpu"}}})");
    const ComputeProjectSettings loaded = LoadComputeSettings(assets.wstring());
    Check(loaded.config.gpuMinElements == 16 && loaded.config.stateTotalBytes == 65536
              && loaded.kernelOverridesCpu.count(kSaxpy) == 1,
          "ファイルから compute 節を読める");
    Check(LoadComputeSettings((root / L"none").wstring()).config.gpuMinElements == 4096 && LoadComputeSettings(L"").config.gpuMinElements == 4096,
          "ファイルが無い・assetsRoot が空なら既定値");

    ComputeService svc;
    ComputeConfig preset;
    preset.forceCpu = true;
    preset.alwaysWaitGpu = true;
    svc.SetConfig(preset);
    svc.SetKernelOverrideCpu("StaleFromSession", true);
    ApplyComputeSettings(svc, loaded);
    Check(svc.Config().gpuMinElements == 16 && svc.Config().stateTotalBytes == 65536 && svc.Config().forceCpu
              && svc.Config().alwaysWaitGpu,
          "適用は forceCpu / alwaysWaitGpu を保ったまま負荷枠を入れ替える");
    Check(svc.KernelOverridesCpu() == std::set<std::string>{ kSaxpy }, "適用は上書きをファイルの内容に置き換える");

    // 書き戻し: 他のキー (他機能・compute の他のキー) を消さない
    Check(SaveComputeKernelOverrides(assets.wstring(), { "B", "A" }), "上書きを書き戻せる");
    nlohmann::json j;
    Check(ReadProjectSettingsFile(file.wstring(), j) == ProjectSettingsRead::Ok && j["particleBackend"] == "gpu"
              && j["ui"]["referenceW"] == 1280 && j["physicsLayers"].size() == 1 && j["compute"]["gpuMinElements"] == 16
              && j["compute"]["stateTotalBytes"] == 65536 && j["compute"]["kernelOverrides"].size() == 2
              && j["compute"]["kernelOverrides"]["A"] == "cpu" && j["compute"]["kernelOverrides"]["B"] == "cpu",
          "書き戻しは他のキーと compute の他の値を保ち、上書きだけ入れ替える");
    Check(LoadComputeSettings(assets.wstring()).kernelOverridesCpu == std::set<std::string>({ "A", "B" }), "書き戻した上書きを読み直せる");
    const std::string text = ReadText(file);
    Check(text.find('\r') == std::string::npos, "書き戻しは LF のまま (既存ファイルの改行に合わせる)");

    Check(SaveComputeKernelOverrides(assets.wstring(), {}), "上書きを空にして書き戻せる");
    Check(ReadProjectSettingsFile(file.wstring(), j) == ProjectSettingsRead::Ok && !j["compute"].contains("kernelOverrides")
              && j["compute"]["gpuMinElements"] == 16,
          "空にすると kernelOverrides だけ消え、compute の他のキーは残る");
    WriteText(file, R"({"compute":{"kernelOverrides":{"A":"cpu"}}})");
    SaveComputeKernelOverrides(assets.wstring(), {});
    Check(ReadProjectSettingsFile(file.wstring(), j) == ProjectSettingsRead::Ok && !j.contains("compute"),
          "compute が空になったら節ごと消す");
    WriteText(file, "{ broken");
    Check(!SaveComputeKernelOverrides(assets.wstring(), { "A" }) && ReadText(file) == "{ broken",
          "壊れた project_settings.json は触らずに失敗する");
}

void TestContentHash(const fs::path& root)
{
    const fs::path assets = root / L"hashassets";
    WriteText(assets / L"project_settings.json", R"({"compute":{"stateTotalBytes":1048576}})");
    WriteText(assets / L"other.txt", "x");
    std::vector<ContentEntry> a;
    Check(CollectContentEntries(assets.wstring(), a), "contentHash の対象を列挙できる");
    bool listed = false;
    for (const ContentEntry& e : a) {
        listed = listed || e.path == "project_settings.json";
    }
    const uint64_t before = FoldContentEntries(a);
    WriteText(assets / L"project_settings.json", R"({"compute":{"stateTotalBytes":2097152}})");
    std::vector<ContentEntry> b;
    CollectContentEntries(assets.wstring(), b);
    Check(listed && before != FoldContentEntries(b),
          "project_settings.json は contentHash の対象で、compute 節を変えるとハッシュが変わる (Runtime / Server も同じファイルを読む)");
}

// Saxpy を登録した小さなサービス。lane の言語向けの CPU 版を持つ
void RegisterSaxpy(ComputeService& svc, const std::string& text, bool cpp, bool csharp)
{
    ComputeKernelDesc d;
    d.name = kSaxpy;
    d.paramsSize = sizeof(sx::SelfTestSaxpyParams);
    d.args = { { "y", true, sizeof(float) }, { "x", false, sizeof(float) } };
    const uint64_t hash = kernel::HashKernelSource(text);
    if (cpp) {
        d.cpp = { kernel::MakeRangeFn<&sx::SelfTestSaxpy_Cpu>(), hash };
    }
    if (csharp) {
        d.csharp = { kernel::MakeRangeFn<&sx::SelfTestSaxpy_Cpu>(), hash };
    }
    svc.RegisterKernel(d);
}

uint64_t RunSaxpy(ComputeService& svc, CallerKind lane, const char* script, const char* kernelName)
{
    constexpr uint32_t kN = 64;
    std::vector<float> data(kN, 1.0f);
    const uint64_t y = svc.CreateBuffer(lane, kN, sizeof(float), BufferKind::State);
    const uint64_t x = svc.CreateBuffer(lane, kN, sizeof(float), BufferKind::State);
    svc.WriteBuffer(lane, y, data.data(), kN);
    svc.WriteBuffer(lane, x, data.data(), kN);
    const sx::SelfTestSaxpyParams params = { 2.0f };
    const uint64_t buffers[] = { y, x };
    ForEachArgs args;
    args.caller = { lane, script };
    args.kernel = kernelName;
    args.execution = Execution::Auto;
    args.params = &params;
    args.paramsSize = sizeof(params);
    args.buffers = buffers;
    args.bufferCount = 2;
    return svc.ForEach(args);
}

void TestRows()
{
    jobs::JobSystem js;
    js.Init();
    {
        ComputeService svc;
        svc.Bind(nullptr, nullptr, &js);
        const std::string saxpyText = EngineKernelText("SelfTestSaxpy.kernel.hlsl");
        const std::string unusedText = EngineKernelText("SelfTestUnused.kernel.hlsl");
        Check(!saxpyText.empty() && !unusedText.empty(), "エンジンのカーネルを読める (リポジトリのルートから実行すること)");
        RegisterSaxpy(svc, saxpyText, /*cpp=*/true, /*csharp=*/false);
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText);
        svc.UpdateKernelSource("SelfTestUnused.kernel.hlsl", unusedText);
        svc.UpdateKernelSource("Broken.kernel.hlsl", "int x;\n"); // 検査に落ちる (KERNEL が無い)
        svc.ReplaceDeclarations(CallerKind::Cpp, { { kScript, { kSaxpy, "SelfTestUnused", "Ghost", "Broken" } } });
        svc.ReplaceDeclarations(CallerKind::CSharp, { { kScript, { kSaxpy } } });

        Check(RunSaxpy(svc, CallerKind::Cpp, kScript, kSaxpy) == kJobDoneCpu, "宣言したカーネルを実行できる");
        Check(RunSaxpy(svc, CallerKind::Cpp, kScript, "SelfTestMix") == kJobFailed, "宣言していないカーネルは失敗する");
        svc.SetKernelOverrideCpu("SelfTestUnused", true);

        const std::vector<ComputeInspectorRow> rows = BuildComputeInspectorRows(svc, CallerKind::Cpp, kScript);
        bool sorted = std::is_sorted(rows.begin(), rows.end(),
                                     [](const ComputeInspectorRow& a, const ComputeInspectorRow& b) { return a.kernel < b.kernel; });
        Check(rows.size() == 5 && sorted, "行 = 宣言 ∪ 観測 (5 本) が名前順に並ぶ");

        const ComputeInspectorRow* ran = FindRow(rows, kSaxpy);
        Check(ran != nullptr && ran->declared && ran->state == RowState::Ran && ran->target == RowTarget::Cpu
                  && ran->elements == 64 && ran->calls == 1 && ran->version == RowVersion::Match && !ran->overrideCpu,
              "宣言あり・実行済み: CPU で実行、要素数 64、版は一致");
        const ComputeInspectorRow* notRun = FindRow(rows, "SelfTestUnused");
        Check(notRun != nullptr && notRun->declared && notRun->state == RowState::NotRun && notRun->target == RowTarget::None
                  && notRun->calls == 0 && notRun->version == RowVersion::NoCpuVersion && notRun->overrideCpu,
              "宣言のみ (未実行): 実行先なし、CPU 版なし、カーネル単位の上書きが見える");
        const ComputeInspectorRow* notDeclared = FindRow(rows, "SelfTestMix");
        Check(notDeclared != nullptr && !notDeclared->declared && notDeclared->state == RowState::FailedNotDeclared
                  && notDeclared->target == RowTarget::Failed && notDeclared->lastFail == FailReason::NotDeclared
                  && notDeclared->failures == 1,
              "宣言なしで失敗: 観測だけの行として出て、理由は NotDeclared");
        const ComputeInspectorRow* ghost = FindRow(rows, "Ghost");
        Check(ghost != nullptr && ghost->declared && ghost->state == RowState::DeclaredNoKernel && ghost->version == RowVersion::Unknown,
              "宣言あり・カーネル無し (D28-f)");
        const ComputeInspectorRow* broken = FindRow(rows, "Broken");
        Check(broken != nullptr && broken->state == RowState::LintViolation && broken->violation.find("KL") != std::string::npos
                  && broken->violation.find("Broken.kernel.hlsl") != std::string::npos,
              "検査違反: 規則 (KLxxx) とファイル・行が出る");

        // 版ずれ: ディスク上のソースが変わった
        svc.UpdateKernelSource("SelfTestSaxpy.kernel.hlsl", saxpyText + "\n// edit\n");
        const std::vector<ComputeInspectorRow> staleRows = BuildComputeInspectorRows(svc, CallerKind::Cpp, kScript);
        const ComputeInspectorRow* stale = FindRow(staleRows, kSaxpy);
        Check(stale != nullptr && stale->version == RowVersion::Mismatch, "ソースを編集すると C++ の行は版ずれ (再ビルド待ち)");

        // C# の行: 宣言表も CPU 版もレーンごと
        const std::vector<ComputeInspectorRow> csRows = BuildComputeInspectorRows(svc, CallerKind::CSharp, kScript);
        const ComputeInspectorRow* csSaxpy = FindRow(csRows, kSaxpy);
        Check(csRows.size() == 2 && csSaxpy != nullptr && csSaxpy->declared && csSaxpy->version == RowVersion::NoCpuVersion,
              "C# の行は C# 側の宣言表と C# の CPU 版で決まる (C++ の CPU 版があっても NoCpuVersion)");
        RegisterSaxpy(svc, saxpyText + "\n// edit\n", /*cpp=*/false, /*csharp=*/true);
        const std::vector<ComputeInspectorRow> csOkRows = BuildComputeInspectorRows(svc, CallerKind::CSharp, kScript);
        const ComputeInspectorRow* csOk = FindRow(csOkRows, kSaxpy);
        Check(csOk != nullptr && csOk->version == RowVersion::Match, "C# の CPU 版が再生成されると一致に戻る");
        Check(BuildComputeInspectorRows(svc, CallerKind::Cpp, "NoSuchScript").empty(), "宣言も観測も無いスクリプトは行が空");
    }
    js.Shutdown();
}

void TestReloadAndAgreement(const fs::path& root)
{
    const fs::path assets = root / L"reloadassets";
    const fs::path kernels = assets / L"shaders" / L"kernels";
    const std::string saxpy = RenamedSaxpy(EngineKernelText("SelfTestSaxpy.kernel.hlsl"), "MixedCase");
    WriteText(kernels / L"MixedCase.kernel.hlsl", saxpy);
    ComputeService svc;
    ShaderManager shaders; // Init しない = デバイス無し。索引と解決だけ使う
    shaders.SetAssetsRoot(assets.wstring());
    shaders.RebuildProjectShaderIndex();
    Check(svc.ScanKernelSources(kernels.wstring()) == 1 && svc.HasKernelSource("MixedCase"), "kernelDir を走査してカーネルを覚える");
    svc.Bind(nullptr, &shaders);
    const KernelSourceInfo* info = svc.FindKernelSource("MixedCase");
    Check(info != nullptr && info->lint == LintState::Ok && info->conflict.empty() && !info->path.empty(),
          "CPU が読むファイルと GPU (ShaderManager の索引) が読むファイルが同じなら競合なし");

    // FileWatcher のパスは小文字化済み。綴りはディスクのファイル名から取る
    const std::wstring norm = NormalizePathKey((kernels / L"MixedCase.kernel.hlsl").wstring());
    Check(!svc.ReloadKernelSource(norm), "変更が無ければ false (C# を再コンパイルしない)");
    WriteText(kernels / L"MixedCase.kernel.hlsl", saxpy + "\n// edit\n");
    const uint64_t oldHash = svc.FindKernelSource("MixedCase")->hash;
    Check(svc.ReloadKernelSource(norm) && svc.FindKernelSource("MixedCase")->hash != oldHash && !svc.HasKernelSource("mixedcase"),
          "内容が変わると true で、ハッシュが新しくなる。カーネル名の綴りはディスクのまま");
    WriteText(kernels / L"Added.kernel.hlsl", RenamedSaxpy(EngineKernelText("SelfTestSaxpy.kernel.hlsl"), "Added"));
    Check(svc.ReloadKernelSource(NormalizePathKey((kernels / L"Added.kernel.hlsl").wstring())) && svc.HasKernelSource("Added"),
          "新しいファイルは表に加わる");
    std::error_code ec;
    fs::remove(kernels / L"Added.kernel.hlsl", ec);
    Check(svc.ReloadKernelSource(NormalizePathKey((kernels / L"Added.kernel.hlsl").wstring())) && !svc.HasKernelSource("Added"),
          "消えたファイルは表から外れる");
    Check(!svc.ReloadKernelSource(NormalizePathKey((root / L"elsewhere" / L"X.kernel.hlsl").wstring())),
          "kernelDir の外のファイルは無視する");

    // 同じ名前が assets の別の場所にもある: 索引は両方を無効にし、GPU は読めない。CPU だけ使わせない
    WriteText(assets / L"other" / L"MixedCase.kernel.hlsl", saxpy);
    shaders.RebuildProjectShaderIndex();
    Check(svc.VerifyKernelSourceAgreement() == 1, "同名のカーネルが別の場所にもあると、CPU と GPU が別のファイルを読む → 1 本の食い違い");
    info = svc.FindKernelSource("MixedCase");
    Check(info != nullptr && info->lint == LintState::Violation && !info->conflict.empty(),
          "食い違うカーネルは検査違反扱いになり、理由が残る (ForEach は失敗する)");
    const std::vector<ComputeInspectorRow> rows = BuildComputeInspectorRows(svc, CallerKind::Cpp, "None");
    Check(rows.empty(), "(対照) 無関係なスクリプトの行は空");
    fs::remove(assets / L"other" / L"MixedCase.kernel.hlsl", ec);
    shaders.RebuildProjectShaderIndex();
    svc.ScanKernelSources(kernels.wstring());
    Check(svc.VerifyKernelSourceAgreement() == 0 && svc.FindKernelSource("MixedCase")->conflict.empty(),
          "重複を消して読み直すと競合が消える");
}

} // namespace

bool RunComputeSettingsSelfTest()
{
    MYE_LOG_INFO("==== Compute settings / inspector rows / kernel reload (M91h) self test ====");
    g_fails = 0;
    const fs::path root = fs::temp_directory_path() / L"mye_compute_settings_selftest";
    std::error_code ec;
    fs::remove_all(root, ec);
    TestParse();
    TestLoadApplySave(root);
    TestContentHash(root);
    TestRows();
    TestReloadAndAgreement(root);
    fs::remove_all(root, ec);
    MYE_LOG_INFO("==== Compute settings self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
