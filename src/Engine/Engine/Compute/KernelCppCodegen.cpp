//====================================================================================
//                          KernelCppCodegen.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          検査済みカーネルの C++ カーネル表 (Kernels.gen.{h,cpp}) の生成
//====================================================================================
#include "Engine/Engine/Compute/KernelCppCodegen.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Compute/KernelAst.h"
#include "Engine/Engine/Schema/SchemaCodegen.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::kernel {

namespace {

namespace fs = std::filesystem;

// tools\gen_project_files.ps1 の banner と本文が一致すること (一致は KernelLintSelfTest が見る)。banner だけは別文面
constexpr const char* kBanner =
    "// 自動生成 (MyEngine エディタ / Rebuild Scripts)。手編集しても次の生成で上書きされる。\n"
    "// <project>\\assets\\shaders\\kernels\\*.kernel.hlsl を GameLogic.dll の CPU 版として取り込み、カーネル表を作る。\n"
    "// .kernel.hlsl を編集したら Rebuild Scripts で GameLogic を再ビルドすること\n"
    "// (表のハッシュとディスク上のファイルが食い違うと、エンジンは CPU 版を古いものとして扱う)。\n";

constexpr std::string_view kKernelSuffix = ".kernel.hlsl";

bool EndsWithKernelSuffix(const std::string& name)
{
    return name.size() > kKernelSuffix.size()
        && name.compare(name.size() - kKernelSuffix.size(), kKernelSuffix.size(), kKernelSuffix) == 0;
}

struct TableEntry {
    std::string name;
    std::string includePath;
    uint64_t hash = 0;
    std::vector<std::string> argNames;
};

std::string Hex16(uint64_t v)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
    return s;
}

} // namespace

KernelCppBatch BuildKernelsCpp(std::vector<KernelCppSourceFile> files, const std::string& headerInclude,
                               KernelLintCache* cache)
{
    KernelCppBatch batch;
    std::vector<TableEntry> table;
    for (const KernelCppSourceFile& f : files) {
        std::shared_ptr<const KernelLintResult> result;
        if (cache) {
            result = cache->Get(f.fileName, f.text);
        } else {
            result = std::make_shared<const KernelLintResult>(LintKernelSource(f.fileName, f.text));
        }
        if (!result->Ok()) {
            batch.diagnostics.insert(batch.diagnostics.end(), result->diagnostics.begin(), result->diagnostics.end());
            continue;
        }
        TableEntry e;
        e.name = result->kernelName;
        e.includePath = f.includePath;
        e.hash = result->sourceHash;
        for (const lint::BufferDecl& b : result->program->kernel.buffers) {
            e.argNames.push_back(b.name);
        }
        table.push_back(std::move(e));
    }
    // 名前の序数順 (文化圏に依らず同じ並びにする)
    std::sort(table.begin(), table.end(), [](const TableEntry& a, const TableEntry& b) { return a.name < b.name; });

    std::string h = kBanner;
    h += "#pragma once\n#include \"Shared/KernelCompat.h\"\n";
    std::string c = kBanner;
    c += "#include \"" + headerInclude + "\"\n#include \"Shared/KernelRangeFn.h\"\n\nnamespace {\n";
    for (const TableEntry& k : table) {
        // 無名名前空間に入れる: カーネルの自由関数は inline ではないので、複数の TU がこのヘッダを include しても
        // リンクが衝突しない。型は TU ごとに別になるが、境界はバイト列 (void*) で越えるので問題ない
        h += "\n#pragma warning(push)\n#pragma warning(disable : 4100 4505)\n";
        h += "namespace mye::kernel::cpu::" + k.name + " {\nnamespace {\n#include \"" + k.includePath
            + "\"\n} // namespace\n}\n";
        h += "#pragma warning(pop)\n";
        h += "namespace MyeKernels::" + k.name + " { using namespace ::mye::kernel::cpu::" + k.name + "; }\n";

        const std::string fn = "&::mye::kernel::cpu::" + k.name + "::" + k.name + "_Cpu";
        std::string names;
        for (size_t i = 0; i < k.argNames.size(); ++i) {
            names += (i ? ", \"" : "\"") + k.argNames[i] + "\"";
        }
        c += "\n// " + k.name + "\n";
        c += "constexpr const char* const k" + k.name + "_argNames[] = { " + names + " };\n";
        c += "constexpr auto k" + k.name + "_args = ::mye::kernel::MakeKernelArgs<" + fn + ">(k" + k.name
            + "_argNames);\n";
        c += "const ::mye_script_detail::KernelRegistrar k" + k.name + "_registrar(\n";
        c += "    ::mye::kernel::MakeKernelDesc<" + fn + ">(\"" + k.name + "\", 0x" + Hex16(k.hash) + "ull, k" + k.name
            + "_args.data()));\n";
        batch.emitted.push_back(k.name);
    }
    c += "\n} // namespace\n";
    batch.header = std::move(h);
    batch.source = std::move(c);
    return batch;
}

int WriteCppKernels(const std::wstring& kernelDir, const std::wstring& genDir, const std::string& headerInclude,
                    bool absoluteIncludes, KernelLintCache* cache)
{
    std::vector<fs::path> paths;
    std::error_code ec;
    if (fs::is_directory(fs::path(kernelDir), ec)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(kernelDir), ec)) {
            if (entry.is_regular_file(ec) && EndsWithKernelSuffix(entry.path().filename().string())) {
                paths.push_back(entry.path());
            }
        }
    }
    std::sort(paths.begin(), paths.end());

    fs::create_directories(fs::path(genDir), ec);
    std::vector<KernelCppSourceFile> files;
    for (const fs::path& p : paths) {
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            MYE_LOG_ERROR("[kernel codegen] cannot read %s", WideToUtf8(p.wstring()).c_str());
            continue;
        }
        KernelCppSourceFile f;
        f.fileName = p.filename().string();
        f.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        const fs::path inc = absoluteIncludes ? fs::absolute(p, ec).lexically_normal()
                                              : fs::relative(p, fs::path(genDir), ec).lexically_normal();
        f.includePath = WideToUtf8(inc.generic_wstring());
        files.push_back(std::move(f));
    }

    const KernelCppBatch batch = BuildKernelsCpp(std::move(files), headerInclude, cache);
    for (const KernelDiagnostic& d : batch.diagnostics) {
        MYE_LOG_ERROR("[kernel lint] %s", FormatKernelDiagnostic(d).c_str());
    }
    const bool wroteHeader = schema::WriteIfChanged(genDir + L"\\Kernels.gen.h", batch.header);
    const bool wroteSource = schema::WriteIfChanged(genDir + L"\\Kernels.gen.cpp", batch.source);
    if (wroteHeader || wroteSource) {
        MYE_LOG_INFO("[kernel codegen] wrote %s (%zu kernel(s))", WideToUtf8(genDir).c_str(), batch.emitted.size());
    }
    std::vector<std::string> bad;
    for (const KernelDiagnostic& d : batch.diagnostics) {
        if (std::find(bad.begin(), bad.end(), d.file) == bad.end()) {
            bad.push_back(d.file);
        }
    }
    return static_cast<int>(bad.size());
}

} // namespace mye::kernel
