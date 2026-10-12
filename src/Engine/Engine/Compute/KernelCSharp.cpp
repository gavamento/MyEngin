//====================================================================================
//                          KernelCSharp.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          検査済みカーネルの C# への変換器と Kernels.gen.cs の生成
//====================================================================================
#include "Engine/Engine/Compute/KernelCSharp.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Compute/KernelAst.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Schema/SchemaCodegen.h"
#include "Engine/Platform/PathUtil.h"

namespace mye::kernel {

namespace {

using namespace lint;

// C# の予約語に当たるユーザー識別子は @ で逃がす
std::string Esc(const std::string& name)
{
    static const std::unordered_set<std::string> kKeywords = {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const",
        "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
        "false", "finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface",
        "internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override",
        "params", "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
        "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof",
        "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
    };
    return kKeywords.count(name) != 0 ? "@" + name : name;
}

std::string HexU64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llXUL", static_cast<unsigned long long>(v));
    return buf;
}

class Emitter {
public:
    Emitter(const KernelProgram& prog, uint64_t hash) : prog_(prog), hash_(hash) {}

    std::string Run()
    {
        const KernelDef& k = prog_.kernel;
        Line(0, "namespace MyeKernels." + k.name);
        Line(0, "{");
        for (const TopItem& item : prog_.items) {
            if (item.kind == TopItem::Kind::Struct) {
                EmitStruct(prog_.structs[item.index]);
                out_ += "\n";
            }
        }
        Line(1, "public static unsafe class CpuKernel");
        Line(1, "{");
        Line(2, "public const string KernelName = \"" + k.name + "\";");
        Line(2, "public const ulong SourceHash = " + HexU64(hash_) + ";");
        for (const TopItem& item : prog_.items) {
            if (item.kind == TopItem::Kind::Func) {
                out_ += "\n";
                EmitFunction(prog_.funcs[item.index]);
            }
        }
        out_ += "\n";
        EmitBody();
        out_ += "\n";
        EmitRange();
        Line(1, "}");
        Line(0, "}");
        return out_;
    }

private:
    void Line(int indent, const std::string& text)
    {
        out_.append(static_cast<size_t>(indent) * 4, ' ');
        out_ += text;
        out_ += "\n";
    }

    void EmitStruct(const StructDef& s)
    {
        Line(1, "[StructLayout(LayoutKind.Sequential, Pack = 4)]");
        Line(1, "public struct " + s.name);
        Line(1, "{");
        for (const StructMember& m : s.members) {
            Line(2, "public " + m.typeName + " " + Esc(m.name) + ";");
        }
        Line(1, "}");
    }

    void EmitFunction(const FuncDef& fn)
    {
        std::string sig = "private static " + fn.retTypeName + " " + Esc(fn.name) + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            sig += (i ? ", " : "") + fn.params[i].typeName + " " + Esc(fn.params[i].name);
        }
        Line(2, sig + ")");
        EmitUncheckedBody(*fn.body, 2);
    }

    void EmitBody()
    {
        const KernelDef& k = prog_.kernel;
        std::string sig = "public static void Body(KernelId id, in " + k.paramsName + " @params";
        for (const BufferDecl& b : k.buffers) {
            sig += std::string(", ") + (b.rw ? "RWView<" : "ROView<") + b.typeName + "> " + Esc(b.name);
        }
        Line(2, sig + ")");
        EmitUncheckedBody(*k.body, 2);
    }

    // 関数の本体。unchecked で包むのは、定数式の桁あふれを C# のコンパイルエラーにしないため
    void EmitUncheckedBody(const Stmt& block, int indent)
    {
        Line(indent, "{");
        Line(indent + 1, "unchecked");
        EmitBlock(block, indent + 1);
        Line(indent, "}");
    }

    void EmitRange()
    {
        const KernelDef& k = prog_.kernel;
        Line(2, "[UnmanagedCallersOnly]");
        Line(2, "public static void Range(void* paramsPtr, KernelBuffer* buffers, uint bufferCount, uint begin, uint end)");
        Line(2, "{");
        Line(3, "ref readonly " + k.paramsName + " prm = ref *(" + k.paramsName + "*)paramsPtr;");
        std::string call = "Body(new KernelId(i), in prm";
        for (size_t i = 0; i < k.buffers.size(); ++i) {
            const BufferDecl& b = k.buffers[i];
            const std::string view = std::string(b.rw ? "RWView<" : "ROView<") + b.typeName + ">";
            const std::string var = "buf" + std::to_string(i);
            Line(3, view + " " + var + " = new " + view + "((" + b.typeName + "*)buffers[" + std::to_string(i)
                        + "].Data, buffers[" + std::to_string(i) + "].Count);");
            call += ", " + var;
        }
        Line(3, "for (uint i = begin; i < end; ++i)");
        Line(3, "{");
        Line(4, call + ");");
        Line(3, "}");
        Line(2, "}");
    }

    // ---- 文 ----

    void EmitBlock(const Stmt& block, int indent)
    {
        Line(indent, "{");
        for (const StmtPtr& s : block.stmts) {
            EmitStmt(*s, indent + 1);
        }
        Line(indent, "}");
    }

    // if / for / while の本体。ブロックでなければ { } で包む (宣言は無いので意味は変わらない)
    void EmitEmbedded(const Stmt& s, int indent)
    {
        if (s.kind == StmtKind::Block) {
            EmitBlock(s, indent);
            return;
        }
        Line(indent, "{");
        EmitStmt(s, indent + 1);
        Line(indent, "}");
    }

    std::string InlineStmt(const Stmt& s) const
    {
        switch (s.kind) {
        case StmtKind::Decl:
            return s.typeName + " " + Esc(s.name) + " = " + Expr(*s.expr, true);
        case StmtKind::Assign: return Expr(*s.lhs, true) + " " + s.op + " " + AssignRhs(s);
        case StmtKind::IncDec:
            return s.prefix ? s.op + Expr(*s.expr, true) : Expr(*s.expr, true) + s.op;
        default: return std::string();
        }
    }

    // 複合シフトの右辺は int 定数で書く (C# のシフト量は int)
    std::string AssignRhs(const Stmt& s) const
    {
        if ((s.op == "<<=" || s.op == ">>=") && s.expr->kind == ExprKind::IntLit) {
            return std::to_string(s.expr->intValue);
        }
        return Expr(*s.expr, true);
    }

    void EmitStmt(const Stmt& s, int indent)
    {
        switch (s.kind) {
        case StmtKind::Block: EmitBlock(s, indent); break;
        case StmtKind::Decl:
        case StmtKind::Assign:
        case StmtKind::IncDec: Line(indent, InlineStmt(s) + ";"); break;
        case StmtKind::If: EmitIf(s, indent, false); break;
        case StmtKind::While:
            Line(indent, "while (" + Expr(*s.expr, true) + ")");
            EmitEmbedded(*s.thenStmt, indent);
            break;
        case StmtKind::For:
            Line(indent, "for (" + (s.init ? InlineStmt(*s.init) : std::string()) + "; "
                             + (s.expr ? Expr(*s.expr, true) : std::string()) + "; "
                             + (s.step ? InlineStmt(*s.step) : std::string()) + ")");
            EmitEmbedded(*s.thenStmt, indent);
            break;
        case StmtKind::Break: Line(indent, "break;"); break;
        case StmtKind::Continue: Line(indent, "continue;"); break;
        case StmtKind::Return: Line(indent, s.expr ? "return " + Expr(*s.expr, true) + ";" : std::string("return;")); break;
        case StmtKind::ExprStmt: Line(indent, Expr(*s.expr, true) + ";"); break;
        }
    }

    void EmitIf(const Stmt& s, int indent, bool afterElse)
    {
        Line(indent, std::string(afterElse ? "else " : "") + "if (" + Expr(*s.expr, true) + ")");
        EmitEmbedded(*s.thenStmt, indent);
        if (!s.elseStmt) {
            return;
        }
        if (s.elseStmt->kind == StmtKind::If) {
            EmitIf(*s.elseStmt, indent, true);
        } else {
            Line(indent, "else");
            EmitEmbedded(*s.elseStmt, indent);
        }
    }

    // ---- 式 (二項・三項はすべて括弧で包む。優先順位を C# の規則に頼らないため) ----

    bool IsRwBuffer(const std::string& name) const
    {
        for (const BufferDecl& b : prog_.kernel.buffers) {
            if (b.name == name) {
                return b.rw;
            }
        }
        return false;
    }

    std::string Expr(const lint::Expr& e, bool top = false) const
    {
        switch (e.kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit: return e.text;
        case ExprKind::BoolLit: return e.boolValue ? "true" : "false";
        case ExprKind::Ident:
            if (e.ref == lint::Expr::Ref::KernelId) {
                return "id.Value";
            }
            if (e.ref == lint::Expr::Ref::KernelParams) {
                return "@params";
            }
            return Esc(e.text);
        case ExprKind::Unary: return "(" + e.text + Expr(*e.args[0]) + ")";
        case ExprKind::Binary: {
            std::string rhs = Expr(*e.args[1]);
            if ((e.text == "<<" || e.text == ">>") && e.args[1]->kind == ExprKind::IntLit) {
                rhs = std::to_string(e.args[1]->intValue);
            }
            const std::string s = Expr(*e.args[0]) + " " + e.text + " " + rhs;
            return top ? s : "(" + s + ")";
        }
        case ExprKind::Ternary: {
            const std::string s = Expr(*e.args[0]) + " ? " + Expr(*e.args[1]) + " : " + Expr(*e.args[2]);
            return top ? s : "(" + s + ")";
        }
        case ExprKind::Call: {
            std::string s;
            for (size_t i = 0; i < e.args.size(); ++i) {
                s += (i ? ", " : "") + Expr(*e.args[i], true);
            }
            if (e.callKind == lint::Expr::CallKind::Ctor) {
                return "new " + e.text + "(" + s + ")";
            }
            return Esc(e.text) + "(" + s + ")";
        }
        case ExprKind::Cast: return "((" + e.text + ")" + Expr(*e.args[0]) + ")";
        case ExprKind::Member: return Expr(*e.args[0]) + "." + Esc(e.text);
        case ExprKind::Index: {
            const lint::Expr& base = *e.args[0];
            const lint::Expr& index = *e.args[1];
            // RW は KernelId で添字する。RO は uint (id は .Value で渡す)
            const std::string ix = IsRwBuffer(base.text) ? std::string("id") : Expr(index, true);
            return Esc(base.text) + "[" + ix + "]";
        }
        }
        return std::string();
    }

    const KernelProgram& prog_;
    uint64_t hash_;
    std::string out_;
};

constexpr const char* kFileHeader =
    "// 自動生成 (MyEngine カーネル変換器、M91b)。手編集禁止 — エンジンが assets\\shaders\\kernels\\*.kernel.hlsl から上書きする。\n"
    "// Compute.ForEach の C# の CPU 版。正本は .kernel.hlsl (検査器を通ったカーネルだけを出力する)。\n"
    "// 演算順は Shared/KernelCompat.h (C++ 版) と同一。MyeScripting.KernelCompat の型と関数を使う。\n"
    "using System.Runtime.InteropServices;\n"
    "using MyeScripting.KernelCompat;\n"
    "using static MyeScripting.KernelCompat.KernelMath;\n";

} // namespace

std::string EmitKernelCSharp(const KernelLintResult& result)
{
    if (!result.Ok()) {
        return std::string();
    }
    return Emitter(*result.program, result.sourceHash).Run();
}

KernelCSharpBatch BuildKernelsCSharp(std::vector<KernelSourceFile> files, KernelLintCache* cache)
{
    std::sort(files.begin(), files.end(),
              [](const KernelSourceFile& a, const KernelSourceFile& b) { return a.fileName < b.fileName; });
    KernelCSharpBatch batch;
    batch.source = kFileHeader;
    for (const KernelSourceFile& f : files) {
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
        batch.source += "\n";
        batch.source += EmitKernelCSharp(*result);
        batch.emitted.push_back(result->kernelName);
    }
    return batch;
}

int WriteCSharpKernels(const std::wstring& assetsRoot, KernelLintCache* cache)
{
    namespace fs = std::filesystem;
    const fs::path kernelDir = fs::path(assetsRoot) / L"shaders" / L"kernels";
    const std::wstring outDir = assetsRoot + L"\\scripts\\Generated";
    const std::wstring outPath = outDir + L"\\Kernels.gen.cs";

    std::vector<KernelSourceFile> files;
    std::error_code ec;
    if (fs::is_directory(kernelDir, ec)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(kernelDir, ec)) {
            const std::string name = entry.path().filename().string();
            constexpr std::string_view kSuffix = ".kernel.hlsl";
            if (!entry.is_regular_file(ec) || name.size() <= kSuffix.size()
                || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) {
                MYE_LOG_ERROR("[kernel codegen] cannot read %s", WideToUtf8(entry.path().wstring()).c_str());
                continue;
            }
            KernelSourceFile f;
            f.fileName = name;
            f.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            files.push_back(std::move(f));
        }
    }
    if (files.empty() && !fs::exists(outPath, ec)) {
        return 0; // カーネル無し + 既存ファイル無し = 何も作らない
    }

    const KernelCSharpBatch batch = BuildKernelsCSharp(std::move(files), cache);
    for (const KernelDiagnostic& d : batch.diagnostics) {
        MYE_LOG_ERROR("[kernel lint] %s", FormatKernelDiagnostic(d).c_str());
    }
    fs::create_directories(outDir, ec);
    if (schema::WriteIfChanged(outPath, batch.source)) {
        MYE_LOG_INFO("[kernel codegen] wrote %s (%zu kernel(s))", WideToUtf8(outPath).c_str(), batch.emitted.size());
    }
    // 違反したカーネルの数 = 診断を出したファイルの数
    std::vector<std::string> bad;
    for (const KernelDiagnostic& d : batch.diagnostics) {
        if (std::find(bad.begin(), bad.end(), d.file) == bad.end()) {
            bad.push_back(d.file);
        }
    }
    return static_cast<int>(bad.size());
}

} // namespace mye::kernel
