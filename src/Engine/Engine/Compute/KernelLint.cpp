//====================================================================================
//                          KernelLint.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル (HLSL サブセット) の型検査と検査器の入口
//====================================================================================
#include "Engine/Engine/Compute/KernelLint.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Engine/Engine/Compute/KernelAst.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelLintInternal.h"

namespace mye::kernel {

namespace {

struct RuleInfo {
    const char* id;
    const char* name;
};

constexpr RuleInfo kRules[] = {
    { "KL001", "syntax" },       { "KL002", "preprocessor" }, { "KL003", "unsupported-type" },
    { "KL004", "array" },        { "KL005", "swizzle" },      { "KL006", "float-literal" },
    { "KL007", "int-literal" },  { "KL008", "type-mismatch" }, { "KL009", "int-division" },
    { "KL010", "vector-compare" }, { "KL011", "builtin" },    { "KL012", "attribute" },
    { "KL013", "out-param" },    { "KL014", "assign-in-expr" }, { "KL015", "reserved" },
    { "KL016", "rw-index" },     { "KL017", "scalar-mix" },   { "KL018", "shadow" },
    { "KL019", "unknown-name" }, { "KL020", "kernel-decl" },  { "KL021", "keyword" },
    { "KL022", "shift" },        { "KL023", "missing-return" }, { "KL024", "read-only" },
    { "KL025", "condition" },    { "KL026", "global" },       { "KL027", "struct-shape" },
    { "KL028", "uninitialized" },
};
static_assert(sizeof(kRules) / sizeof(kRules[0]) == static_cast<size_t>(KernelRule::Count),
              "KernelRule と kRules の数が合わない");

} // namespace

const char* KernelRuleId(KernelRule rule)
{
    return kRules[static_cast<size_t>(rule)].id;
}

const char* KernelRuleName(KernelRule rule)
{
    return kRules[static_cast<size_t>(rule)].name;
}

std::string FormatKernelDiagnostic(const KernelDiagnostic& d)
{
    return d.file + "(" + std::to_string(d.line) + "): error " + KernelRuleId(d.rule) + " ["
        + KernelRuleName(d.rule) + "]: " + d.message;
}

namespace lint {
namespace {

using Ref = Expr::Ref;

bool IsInteger(const KType& t)
{
    return t.IsNumeric() && t.scalar != ScalarKind::Float;
}

bool IsFloatFamily(const KType& t)
{
    return t.IsNumeric() && t.scalar == ScalarKind::Float;
}

// 代入・引数・戻り値で受け入れる型。完全一致か int -> float のスカラーだけ
bool Assignable(const KType& dst, const KType& src)
{
    if (dst == src) {
        return true;
    }
    return dst.IsScalar() && dst.scalar == ScalarKind::Float && src.IsScalar() && src.scalar == ScalarKind::Int;
}

bool IsKernelBuiltin(const std::string& n)
{
    static const std::unordered_set<std::string> s = {
        "min", "max", "clamp", "abs", "saturate", "lerp", "step", "smoothstep", "floor", "ceil", "frac", "sqrt",
        "rsqrt", "rcp", "dot", "cross", "length", "normalize", "distance", "sign", "sin", "cos", "HashU32",
        "HashToUnitFloat", "FloatToInt", "FloatToUint",
    };
    return s.count(n) != 0;
}

class Checker {
public:
    Checker(KernelProgram& program, const std::string& kernelName, const DiagSink& sink)
        : prog_(program), kernelName_(kernelName), sink_(sink)
    {
    }

    void Run()
    {
        for (const TopItem& item : prog_.items) {
            if (item.kind == TopItem::Kind::Struct) {
                CheckStruct(item.index);
            } else {
                CheckFunction(item.index);
            }
        }
        CheckKernel();
    }

private:
    struct VarInfo {
        KType type;
        bool isConst = false;
        Ref ref = Ref::Local;
        bool bufferRw = false;
    };
    struct Scope {
        std::unordered_map<std::string, VarInfo> vars;
        std::unordered_set<std::string> reserved; // このスコープの直下で後から宣言される名前
    };

    void Report(KernelRule rule, int line, std::string msg) const { sink_.Add(rule, line, std::move(msg)); }
    std::string Str(const KType& t) const { return TypeToString(t, &prog_); }

    // ---- 型の解決 ----

    KType ResolveType(const std::string& name, int line)
    {
        KType t;
        if (ParseBuiltinTypeName(name, &t)) {
            return t;
        }
        auto it = structIndex_.find(name);
        if (it != structIndex_.end()) {
            return KType::Struct(it->second);
        }
        if (IsUnsupportedTypeName(name)) {
            Report(KernelRule::UnsupportedType, line, "型 '" + name + "' はサブセットに無い");
        } else {
            Report(KernelRule::UnknownName, line, "未定義の型 '" + name + "' (struct は使う前に宣言する)");
        }
        return KType::Err();
    }

    // 名前の検査 (予約語・遮蔽)。ok なら true
    bool CheckGlobalName(const std::string& name, int line, const char* what)
    {
        if (IsReservedIdentifier(name)) {
            Report(KernelRule::Reserved, line, std::string(what) + "名 '" + name + "' は予約語 / 組込み名と衝突する");
            return false;
        }
        if (name == kernelName_) {
            Report(KernelRule::Reserved, line, std::string(what) + "名 '" + name + "' はカーネル名と同じ (C# の名前空間と衝突する)");
            return false;
        }
        if (globalNames_.count(name) != 0) {
            Report(KernelRule::Shadow, line, std::string(what) + "名 '" + name + "' は既に宣言されている");
            return false;
        }
        return true;
    }

    // ---- struct ----

    void CheckStruct(int index)
    {
        StructDef& def = prog_.structs[index];
        const bool nameOk = CheckGlobalName(def.name, def.line, "struct ");
        if (def.members.empty()) {
            Report(KernelRule::StructShape, def.line, "空の struct は使えない ('" + def.name + "')");
        }
        std::unordered_set<std::string> seen;
        for (StructMember& m : def.members) {
            m.type = ResolveType(m.typeName, m.line);
            if (m.type.kind == KType::Kind::Struct) {
                Report(KernelRule::StructShape, m.line, "struct の中に struct は置けない ('" + m.name + "')");
                m.type = KType::Err();
            }
            if (IsReservedIdentifier(m.name) || m.name == def.name) {
                Report(KernelRule::Reserved, m.line, "メンバ名 '" + m.name + "' は予約語 / 組込み名 / struct 名と衝突する");
            } else if (!seen.insert(m.name).second) {
                Report(KernelRule::Shadow, m.line, "メンバ名 '" + m.name + "' が重複している");
            }
        }
        if (nameOk) {
            globalNames_.insert(def.name);
            structIndex_[def.name] = index;
        }
    }

    // ---- 関数 ----

    void CheckFunction(int index)
    {
        FuncDef& fn = prog_.funcs[index];
        const bool nameOk = CheckGlobalName(fn.name, fn.line, "関数");
        fn.ret = ResolveType(fn.retTypeName, fn.line);
        scopes_.clear();
        scopes_.emplace_back();
        std::unordered_set<std::string> seen;
        for (FuncParam& p : fn.params) {
            p.type = ResolveType(p.typeName, p.line);
            if (IsReservedIdentifier(p.name) || globalNames_.count(p.name) != 0 || p.name == kernelName_) {
                Report(KernelRule::Reserved, p.line, "引数名 '" + p.name + "' は予約語 / 組込み名 / 既存の型・関数名と衝突する");
            } else if (!seen.insert(p.name).second) {
                Report(KernelRule::Shadow, p.line, "引数名 '" + p.name + "' が重複している");
            } else {
                VarInfo v;
                v.type = p.type;
                scopes_.back().vars[p.name] = v;
            }
        }
        inFunction_ = true;
        currentFunction_ = fn.name;
        returnType_ = fn.ret;
        loopDepth_ = 0;
        CheckBlock(*fn.body);
        if (!fn.ret.IsError() && !AlwaysReturns(*fn.body)) {
            Report(KernelRule::MissingReturn, fn.line, "関数 '" + fn.name + "' に値を返さない経路がある (C# ではコンパイルエラー)");
        }
        inFunction_ = false;
        currentFunction_.clear();
        if (nameOk) {
            globalNames_.insert(fn.name);
            funcIndex_[fn.name] = index;
        }
    }

    static bool AlwaysReturns(const Stmt& s)
    {
        switch (s.kind) {
        case StmtKind::Return: return true;
        case StmtKind::Block:
            return std::any_of(s.stmts.begin(), s.stmts.end(), [](const StmtPtr& c) { return AlwaysReturns(*c); });
        case StmtKind::If: return s.elseStmt && AlwaysReturns(*s.thenStmt) && AlwaysReturns(*s.elseStmt);
        default: return false;
        }
    }

    // ---- KERNEL ----

    void CheckKernel()
    {
        KernelDef& k = prog_.kernel;
        if (k.name != kernelName_) {
            Report(KernelRule::KernelDecl, k.line,
                   "KERNEL の名前 '" + k.name + "' がファイル名のカーネル名 '" + kernelName_ + "' と違う");
        }
        int paramsCount = 0;
        for (size_t i = 0; i < prog_.structs.size(); ++i) {
            if (prog_.structs[i].isParams) {
                ++paramsCount;
                if (prog_.structs[i].name == k.paramsName) {
                    k.paramsStruct = static_cast<int>(i);
                }
            }
        }
        if (paramsCount != 1) {
            Report(KernelRule::KernelDecl, k.line, "KERNEL_PARAMS は 1 つだけ宣言する (今は " + std::to_string(paramsCount) + " 個)");
        } else if (k.paramsStruct < 0) {
            Report(KernelRule::KernelDecl, k.line, "KERNEL の params 型 '" + k.paramsName + "' が KERNEL_PARAMS の名前と違う");
        }
        if (k.buffers.empty()) {
            Report(KernelRule::KernelDecl, k.line, "KERNEL には RW のバッファが 1 つ以上要る (id の範囲は第 1 引数の RW の要素数)");
        } else if (!k.buffers[0].rw) {
            Report(KernelRule::KernelDecl, k.buffers[0].line, "第 1 引数は RW でなければならない (id の範囲になるため)");
        }
        scopes_.clear();
        scopes_.emplace_back();
        Scope& root = scopes_.back();
        VarInfo idv;
        idv.type = KType::Scalar(ScalarKind::Uint);
        idv.ref = Ref::KernelId;
        root.vars["id"] = idv;
        VarInfo pv;
        pv.type = k.paramsStruct >= 0 ? KType::Struct(k.paramsStruct) : KType::Err();
        pv.ref = Ref::KernelParams;
        root.vars["params"] = pv;
        std::unordered_set<std::string> seen;
        for (BufferDecl& b : k.buffers) {
            b.elem = ResolveType(b.typeName, b.line);
            if (IsReservedIdentifier(b.name) || globalNames_.count(b.name) != 0 || b.name == kernelName_) {
                Report(KernelRule::Reserved, b.line, "バッファ名 '" + b.name + "' は予約語 / 組込み名 / 既存の型・関数名と衝突する");
            } else if (!seen.insert(b.name).second) {
                Report(KernelRule::Shadow, b.line, "バッファ名 '" + b.name + "' が重複している");
            } else {
                VarInfo v;
                v.type = b.elem;
                v.ref = Ref::Buffer;
                v.bufferRw = b.rw;
                root.vars[b.name] = v;
            }
        }
        inFunction_ = false;
        loopDepth_ = 0;
        CheckBlock(*k.body);
    }

    // ---- スコープと宣言 ----

    const VarInfo* Lookup(const std::string& name) const
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->vars.find(name);
            if (f != it->vars.end()) {
                return &f->second;
            }
        }
        return nullptr;
    }

    void Declare(const std::string& name, int line, const KType& type, bool isConst)
    {
        if (IsReservedIdentifier(name) || name == kernelName_) {
            Report(KernelRule::Reserved, line, "変数名 '" + name + "' は予約語 / 組込み名 / カーネル名と衝突する");
            return;
        }
        if (globalNames_.count(name) != 0) {
            Report(KernelRule::Shadow, line, "変数名 '" + name + "' は型 / 関数の名前を隠す");
            return;
        }
        for (size_t i = 0; i + 1 < scopes_.size(); ++i) {
            if (scopes_[i].vars.count(name) != 0 || scopes_[i].reserved.count(name) != 0) {
                Report(KernelRule::Shadow, line,
                       "変数名 '" + name + "' が外側のスコープの宣言と重なる (C# では CS0136。別の名前にする)");
                return;
            }
        }
        if (scopes_.back().vars.count(name) != 0) {
            Report(KernelRule::Shadow, line, "変数名 '" + name + "' は同じスコープで再宣言されている");
            return;
        }
        VarInfo v;
        v.type = type;
        v.isConst = isConst;
        scopes_.back().vars[name] = v;
    }

    void CheckBlock(Stmt& block)
    {
        scopes_.emplace_back();
        for (const StmtPtr& s : block.stmts) {
            if (s->kind == StmtKind::Decl) {
                scopes_.back().reserved.insert(s->name);
            }
        }
        for (StmtPtr& s : block.stmts) {
            CheckStmt(*s);
        }
        scopes_.pop_back();
    }

    // ---- 文 ----

    void CheckStmt(Stmt& s)
    {
        switch (s.kind) {
        case StmtKind::Block: CheckBlock(s); break;
        case StmtKind::Decl: CheckDecl(s); break;
        case StmtKind::Assign: CheckAssign(s); break;
        case StmtKind::IncDec: CheckIncDec(s); break;
        case StmtKind::If:
            CheckCondition(*s.expr);
            CheckEmbedded(*s.thenStmt);
            if (s.elseStmt) {
                CheckEmbedded(*s.elseStmt);
            }
            break;
        case StmtKind::While:
            CheckCondition(*s.expr);
            ++loopDepth_;
            CheckEmbedded(*s.thenStmt);
            --loopDepth_;
            break;
        case StmtKind::For: CheckFor(s); break;
        case StmtKind::Break:
        case StmtKind::Continue:
            if (loopDepth_ == 0) {
                Report(KernelRule::Syntax, s.line, s.kind == StmtKind::Break ? "ループの外の break" : "ループの外の continue");
            }
            break;
        case StmtKind::Return: CheckReturn(s); break;
        case StmtKind::ExprStmt:
            CheckExpr(*s.expr);
            if (s.expr->kind != ExprKind::Call) {
                Report(KernelRule::Syntax, s.line, "効果の無い式文 (文にできるのは代入・増減・宣言・制御構文)");
            }
            break;
        }
    }

    // if / while / for の本体 (宣言は無い。ブロックでなければ独自のスコープを持たない)
    void CheckEmbedded(Stmt& s) { CheckStmt(s); }

    void CheckFor(Stmt& s)
    {
        scopes_.emplace_back();
        if (s.init && s.init->kind == StmtKind::Decl) {
            scopes_.back().reserved.insert(s.init->name);
        }
        if (s.init) {
            CheckStmt(*s.init);
        }
        if (s.expr) {
            CheckCondition(*s.expr);
        }
        if (s.step) {
            CheckStmt(*s.step);
        }
        ++loopDepth_;
        CheckEmbedded(*s.thenStmt);
        --loopDepth_;
        scopes_.pop_back();
    }

    void CheckCondition(Expr& e)
    {
        const KType t = CheckExpr(e);
        if (!t.IsError() && t.kind != KType::Kind::Bool) {
            Report(KernelRule::Condition, e.line, "条件式は bool でなければならない (今は " + Str(t) + ")。比較で書く");
        }
    }

    void CheckDecl(Stmt& s)
    {
        const KType t = ResolveType(s.typeName, s.line);
        if (!t.IsError() && t.kind != KType::Kind::Scalar && t.kind != KType::Kind::Vector && t.kind != KType::Kind::Struct) {
            Report(KernelRule::UnsupportedType, s.line, "この型の変数は宣言できない");
        }
        if (!s.expr) {
            Report(KernelRule::Uninitialized, s.line,
                   "'" + s.name + "' に初期化子が無い (C++ / GPU では未定義の値になる。宣言時に初期化する)");
        }
        if (s.expr) {
            const KType it = CheckExpr(*s.expr);
            if (!t.IsError() && !it.IsError() && !Assignable(t, it)) {
                Report(KernelRule::TypeMismatch, s.line,
                       "'" + s.name + "' (" + Str(t) + ") を " + Str(it) + " で初期化できない" + NarrowingHint(t, it));
            }
        }
        Declare(s.name, s.line, t, s.isConst);
    }

    static const char* NarrowingHint(const KType& dst, const KType& src)
    {
        if (dst.IsScalar() && src.IsScalar()) {
            return " (暗黙の縮小変換 / int と uint の混在は禁止。明示的にキャストする)";
        }
        return "";
    }

    void CheckReturn(Stmt& s)
    {
        if (!inFunction_) {
            if (s.expr) {
                CheckExpr(*s.expr);
                Report(KernelRule::Syntax, s.line, "KERNEL の本体は値を返さない (return; だけ)");
            }
            return;
        }
        if (!s.expr) {
            Report(KernelRule::Syntax, s.line, "関数は値を返す (return 式;)");
            return;
        }
        const KType t = CheckExpr(*s.expr);
        if (!t.IsError() && !returnType_.IsError() && !Assignable(returnType_, t)) {
            Report(KernelRule::TypeMismatch, s.line,
                   "戻り値の型 " + Str(returnType_) + " に " + Str(t) + " は返せない" + NarrowingHint(returnType_, t));
        }
    }

    void CheckAssign(Stmt& s)
    {
        const KType lt = CheckExpr(*s.lhs);
        const KType rt = CheckExpr(*s.expr);
        if (!s.lhs->assignable && !lt.IsError()) {
            Report(KernelRule::ReadOnly, s.line,
                   "代入先に書き込めない (RO バッファ・params・const・id・式の結果には書けない)");
        }
        if (lt.IsError() || rt.IsError()) {
            return;
        }
        if (s.op == "=") {
            if (!Assignable(lt, rt)) {
                Report(KernelRule::TypeMismatch, s.line, Str(rt) + " を " + Str(lt) + " へ代入できない" + NarrowingHint(lt, rt));
            }
            return;
        }
        const std::string base = s.op.substr(0, s.op.size() - 1);
        const KType result = BinaryType(base, *s.lhs, *s.expr, s.line);
        if (!result.IsError() && result != lt) {
            Report(KernelRule::TypeMismatch, s.line,
                   "'" + s.op + "' の結果 " + Str(result) + " を " + Str(lt) + " へ戻せない" + NarrowingHint(lt, result));
        }
    }

    void CheckIncDec(Stmt& s)
    {
        const KType t = CheckExpr(*s.expr);
        if (t.IsError()) {
            return;
        }
        if (!s.expr->assignable) {
            Report(KernelRule::ReadOnly, s.line, "増減の対象に書き込めない");
        }
        if (!t.IsScalar() || t.scalar == ScalarKind::Float) {
            Report(KernelRule::TypeMismatch, s.line, "++ -- は int / uint のスカラー変数だけ (今は " + Str(t) + ")");
        }
    }

    // ---- 式 ----

    KType CheckExpr(Expr& e)
    {
        e.type = CheckExprImpl(e);
        return e.type;
    }

    KType CheckExprImpl(Expr& e)
    {
        switch (e.kind) {
        case ExprKind::IntLit: return KType::Scalar(e.boolValue ? ScalarKind::Uint : ScalarKind::Int);
        case ExprKind::FloatLit: return KType::Scalar(ScalarKind::Float);
        case ExprKind::BoolLit: return KType::Bool();
        case ExprKind::Ident: return CheckIdent(e);
        case ExprKind::Unary: return CheckUnary(e);
        case ExprKind::Binary: {
            CheckExpr(*e.args[0]);
            CheckExpr(*e.args[1]);
            return BinaryType(e.text, *e.args[0], *e.args[1], e.line);
        }
        case ExprKind::Ternary: return CheckTernary(e);
        case ExprKind::Call: return CheckCall(e);
        case ExprKind::Cast: return CheckCast(e);
        case ExprKind::Member: return CheckMember(e);
        case ExprKind::Index: return CheckIndex(e);
        }
        return KType::Err();
    }

    KType CheckIdent(Expr& e)
    {
        const VarInfo* v = Lookup(e.text);
        if (!v) {
            if (globalNames_.count(e.text) != 0) {
                Report(KernelRule::UnknownName, e.line, "'" + e.text + "' は型 / 関数の名前で、値としては使えない");
            } else if (e.text == "id" || e.text == "params") {
                Report(KernelRule::UnknownName, e.line, "関数の中では id / params / バッファは見えない。引数で渡す");
            } else {
                Report(KernelRule::UnknownName, e.line, "未宣言の名前 '" + e.text + "'");
            }
            return KType::Err();
        }
        e.ref = v->ref;
        if (v->ref == Ref::Buffer) {
            Report(KernelRule::Syntax, e.line, "バッファ '" + e.text + "' は 名前[添字] の形でだけ使える");
            return KType::Err();
        }
        e.assignable = v->ref == Ref::Local && !v->isConst;
        return v->type;
    }

    KType CheckUnary(Expr& e)
    {
        const KType t = CheckExpr(*e.args[0]);
        if (t.IsError()) {
            return t;
        }
        if (e.text == "-") {
            if (t.IsNumeric() && t.scalar != ScalarKind::Uint) {
                return t;
            }
            Report(KernelRule::TypeMismatch, e.line, "単項 - は float / int (とそのベクトル) だけ。uint の符号反転は C# で long になる (今は " + Str(t) + ")");
        } else if (e.text == "!") {
            if (t.kind == KType::Kind::Bool) {
                return t;
            }
            Report(KernelRule::TypeMismatch, e.line, "! は bool だけ (今は " + Str(t) + ")");
        } else if (e.text == "~") {
            if (t.IsScalar() && t.scalar != ScalarKind::Float) {
                return t;
            }
            Report(KernelRule::TypeMismatch, e.line, "~ は int / uint のスカラーだけ (今は " + Str(t) + ")");
        }
        return KType::Err();
    }

    KType CheckTernary(Expr& e)
    {
        CheckCondition(*e.args[0]);
        const KType a = CheckExpr(*e.args[1]);
        const KType b = CheckExpr(*e.args[2]);
        if (a.IsError() || b.IsError()) {
            return KType::Err();
        }
        if (!a.IsScalar() || !b.IsScalar()) {
            Report(KernelRule::VectorCompare, e.line, "三項演算子の結果はスカラーだけ (ベクトル / struct は if で書く)");
            return KType::Err();
        }
        if (a != b) {
            Report(KernelRule::TypeMismatch, e.line, "三項演算子の両辺の型が違う (" + Str(a) + " と " + Str(b) + ")");
            return KType::Err();
        }
        return a;
    }

    KType CheckCast(Expr& e)
    {
        const KType t = CheckExpr(*e.args[0]);
        KType target;
        ParseBuiltinTypeName(e.text, &target);
        if (t.IsError()) {
            return target;
        }
        if (!t.IsScalar()) {
            Report(KernelRule::TypeMismatch, e.line, "キャストできるのはスカラーだけ (今は " + Str(t) + ")");
        } else if (t.scalar == ScalarKind::Float && target.scalar != ScalarKind::Float) {
            // C++ では範囲外と NaN が未定義で、C# / HLSL と結果が割れる
            Report(KernelRule::TypeMismatch, e.line,
                   std::string("float から ") + ScalarName(target.scalar) + " への C 形式のキャストは使えない。"
                       + (target.scalar == ScalarKind::Int ? "FloatToInt(x)" : "FloatToUint(x)") + " を使う");
        }
        return target;
    }

    KType CheckMember(Expr& e)
    {
        const KType b = CheckExpr(*e.args[0]);
        if (b.IsError()) {
            return b;
        }
        const std::string& m = e.text;
        e.assignable = e.args[0]->assignable;
        if (b.kind == KType::Kind::Struct) {
            const StructDef& def = prog_.structs[b.structIndex];
            for (const StructMember& sm : def.members) {
                if (sm.name == m) {
                    return sm.type;
                }
            }
            Report(KernelRule::UnknownName, e.line, "struct '" + def.name + "' にメンバ '" + m + "' が無い");
            return KType::Err();
        }
        if (b.IsNumeric()) {
            const bool swizzleLike = !m.empty() && m.find_first_not_of("xyzwrgba") == std::string::npos;
            const int lane = m == "x" ? 0 : (m == "y" ? 1 : (m == "z" ? 2 : (m == "w" ? 3 : -1)));
            if (b.IsVector() && lane >= 0) {
                if (lane < b.lanes) {
                    return KType::Scalar(b.scalar);
                }
                Report(KernelRule::Swizzle, e.line, Str(b) + " に成分 ." + m + " は無い");
                return KType::Err();
            }
            if (swizzleLike) {
                Report(KernelRule::Swizzle, e.line, "スウィズル ." + m + " は使えない (成分は .x .y .z .w を 1 つずつ)");
            } else {
                Report(KernelRule::UnknownName, e.line, Str(b) + " にメンバ '" + m + "' は無い");
            }
            return KType::Err();
        }
        Report(KernelRule::UnknownName, e.line, "メンバ '" + m + "' を持たない型 (" + Str(b) + ")");
        return KType::Err();
    }

    KType CheckIndex(Expr& e)
    {
        Expr& base = *e.args[0];
        Expr& index = *e.args[1];
        const KType it = CheckExpr(index);
        const VarInfo* v = base.kind == ExprKind::Ident ? Lookup(base.text) : nullptr;
        if (!v || v->ref != Ref::Buffer) {
            if (base.kind == ExprKind::Ident && !v) {
                Report(KernelRule::UnknownName, base.line, "未宣言の名前 '" + base.text + "'");
            } else {
                Report(KernelRule::Array, e.line, "添字を付けられるのは KERNEL のバッファだけ (配列は使えない)");
            }
            return KType::Err();
        }
        base.ref = Ref::Buffer;
        base.type = v->type;
        if (v->bufferRw) {
            const bool isId = index.kind == ExprKind::Ident && index.ref == Ref::KernelId;
            if (!isId && !it.IsError()) {
                Report(KernelRule::RwIndex, e.line,
                       "RW バッファ '" + base.text + "' の添字は id だけ (他の要素への書き込みは禁止。ADR-028)");
            }
        } else if (!it.IsError() && !(it.IsScalar() && it.scalar == ScalarKind::Uint)) {
            Report(KernelRule::TypeMismatch, e.line, "RO バッファの添字は uint (今は " + Str(it) + ")" + NarrowingHint(KType::Scalar(ScalarKind::Uint), it));
        }
        e.assignable = v->bufferRw;
        return v->type;
    }

    // ---- 二項演算 ----

    KType BinaryType(const std::string& op, const Expr& le, const Expr& re, int line)
    {
        const KType& l = le.type;
        const KType& r = re.type;
        if (l.IsError() || r.IsError()) {
            return KType::Err();
        }
        if (op == "&&" || op == "||") {
            if (l.kind == KType::Kind::Bool && r.kind == KType::Kind::Bool) {
                return KType::Bool();
            }
            Report(KernelRule::TypeMismatch, line, op + " の両辺は bool でなければならない (今は " + Str(l) + " と " + Str(r) + ")");
            return KType::Err();
        }
        if (op == "<<" || op == ">>") {
            if (!l.IsScalar() || l.scalar == ScalarKind::Float) {
                Report(l.IsVector() ? KernelRule::Shift : KernelRule::TypeMismatch, line,
                       "シフトできるのは int / uint のスカラーだけ (今は " + Str(l) + ")");
                return KType::Err();
            }
            if (re.kind != ExprKind::IntLit || re.intValue > 31) {
                Report(KernelRule::Shift, line, "シフト量は 0..31 の整数リテラルだけ (範囲外は C++ で未定義、C# / HLSL ではマスクされる)");
                return KType::Err();
            }
            return l;
        }
        const bool isCompare = op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
        if (isCompare) {
            if (l.IsVector() || r.IsVector()) {
                Report(KernelRule::VectorCompare, line, "ベクトルの比較は使えない (成分ごとに書く)");
                return KType::Err();
            }
            const bool intFloat = l.IsScalar() && r.IsScalar()
                && ((l.scalar == ScalarKind::Float && r.scalar == ScalarKind::Int)
                    || (l.scalar == ScalarKind::Int && r.scalar == ScalarKind::Float));
            if (l.IsScalar() && r.IsScalar() && (l == r || intFloat)) {
                return KType::Bool();
            }
            Report(KernelRule::TypeMismatch, line, "比較の両辺の型が合わない (" + Str(l) + " と " + Str(r) + ")" + NarrowingHint(l, r));
            return KType::Err();
        }
        // 算術・ビット
        if (!l.IsNumeric() || !r.IsNumeric()) {
            Report(KernelRule::TypeMismatch, line, "演算子 " + op + " は数値型どうしだけ (今は " + Str(l) + " と " + Str(r) + ")");
            return KType::Err();
        }
        const bool bitOp = op == "&" || op == "|" || op == "^";
        const bool intOnly = bitOp || op == "%";
        if (intOnly && (l.scalar == ScalarKind::Float || r.scalar == ScalarKind::Float)) {
            Report(KernelRule::TypeMismatch, line, "演算子 " + op + " は整数だけ (今は " + Str(l) + " と " + Str(r) + ")");
            return KType::Err();
        }
        KType result;
        if (l.IsScalar() && r.IsScalar()) {
            if (l == r) {
                result = l;
            } else if (!intOnly && l.scalar != ScalarKind::Uint && r.scalar != ScalarKind::Uint) {
                result = KType::Scalar(ScalarKind::Float); // int と float の混在は float
            }
        } else if (l.IsVector() && r.IsVector()) {
            if (l == r) {
                result = l;
            }
        } else {
            const KType& vec = l.IsVector() ? l : r;
            const KType& sc = l.IsVector() ? r : l;
            if (sc.scalar == vec.scalar || (vec.scalar == ScalarKind::Float && sc.scalar == ScalarKind::Int)) {
                result = vec;
            }
        }
        if (result.IsError()) {
            Report(KernelRule::TypeMismatch, line,
                   "演算子 " + op + " の両辺の型が合わない (" + Str(l) + " と " + Str(r) + ")。"
                   "暗黙の縮小変換と int / uint の混在は禁止 (キャストするか u を付ける)");
            return result;
        }
        if ((op == "/" || op == "%") && IsInteger(result)) {
            if (re.kind != ExprKind::IntLit || re.intValue == 0) {
                Report(KernelRule::IntDivision, line,
                       "整数の " + op + " は右辺が 0 でない整数リテラルのときだけ使える (0 除算は C# で例外、C++ で未定義)");
            }
        }
        return result;
    }

    // ---- 呼び出し ----

    KType CheckCall(Expr& e)
    {
        std::vector<KType> argTypes;
        bool argError = false;
        for (ExprPtr& a : e.args) {
            argTypes.push_back(CheckExpr(*a));
            argError = argError || argTypes.back().IsError();
        }
        const std::string& name = e.text;

        KType ctorType;
        if (ParseBuiltinTypeName(name, &ctorType)) {
            if (ctorType.IsVector()) {
                e.callKind = Expr::CallKind::Ctor;
                return argError ? ctorType : CheckCtor(e, ctorType, argTypes);
            }
            Report(KernelRule::Syntax, e.line, "関数形式のキャスト " + name + "(x) は使えない。(" + name + ")x と書く");
            return KType::Err();
        }
        auto fi = funcIndex_.find(name);
        if (fi != funcIndex_.end()) {
            e.callKind = Expr::CallKind::User;
            const FuncDef& fn = prog_.funcs[fi->second];
            if (!argError) {
                if (argTypes.size() != fn.params.size()) {
                    Report(KernelRule::TypeMismatch, e.line,
                           "関数 '" + name + "' の引数は " + std::to_string(fn.params.size()) + " 個 (今は " + std::to_string(argTypes.size()) + " 個)");
                } else {
                    for (size_t i = 0; i < argTypes.size(); ++i) {
                        if (!fn.params[i].type.IsError() && !Assignable(fn.params[i].type, argTypes[i])) {
                            Report(KernelRule::TypeMismatch, e.line,
                                   "関数 '" + name + "' の引数 " + std::to_string(i + 1) + " は " + Str(fn.params[i].type) + " (今は " + Str(argTypes[i]) + ")"
                                       + NarrowingHint(fn.params[i].type, argTypes[i]));
                        }
                    }
                }
            }
            return fn.ret;
        }
        if (IsKernelBuiltin(name)) {
            e.callKind = Expr::CallKind::Builtin;
            return argError ? KType::Err() : CheckBuiltin(e, argTypes);
        }
        if (IsBarrierOrAtomic(name)) {
            Report(KernelRule::Keyword, e.line, "'" + name + "' はサブセットに無い (barrier / atomic は使えない)");
        } else if (IsKnownHlslIntrinsic(name)) {
            Report(KernelRule::Builtin, e.line, "組込み関数 '" + name + "' はホワイトリストに無い (C++ / C# で結果をそろえられないため)");
        } else if (name == currentFunction_) {
            Report(KernelRule::UnknownName, e.line, "関数 '" + name + "' が自分自身を呼んでいる (再帰は使えない)");
        } else if (structIndex_.count(name) != 0) {
            Report(KernelRule::Syntax, e.line, "struct '" + name + "' のコンストラクタ呼び出しは使えない (メンバを 1 つずつ代入する)");
        } else {
            Report(KernelRule::UnknownName, e.line, "未宣言の関数 '" + name + "' (関数は使う前に宣言する)");
        }
        return KType::Err();
    }

    KType CheckCtor(Expr& e, const KType& ct, const std::vector<KType>& a)
    {
        const std::string tn = Str(ct);
        auto fail = [&]() {
            Report(KernelRule::TypeMismatch, e.line, "コンストラクタ " + tn + "(...) の引数が合わない (スカラー / 小さいベクトルを成分数ちょうどに並べる)");
            return ct;
        };
        std::vector<int> shape;
        for (const KType& t : a) {
            if (t.IsScalar()) {
                const bool ok = t.scalar == ct.scalar || (ct.scalar == ScalarKind::Float && t.scalar == ScalarKind::Int);
                if (!ok) {
                    return fail();
                }
                shape.push_back(1);
            } else if (t.IsVector() && t.scalar == ct.scalar) {
                shape.push_back(t.lanes);
            } else {
                return fail();
            }
        }
        const int n = ct.lanes;
        // 1 引数の splat (float3(1.0f)) は FXC が受けない (X3014)。(float3)1.0f も使えないので、成分を並べて書く
        if (shape.size() == 1) {
            return fail();
        }
        static const std::vector<std::vector<int>> kShapes2 = { { 1, 1 } };
        static const std::vector<std::vector<int>> kShapes3 = { { 1, 1, 1 }, { 2, 1 }, { 1, 2 } };
        static const std::vector<std::vector<int>> kShapes4 = { { 1, 1, 1, 1 }, { 3, 1 }, { 1, 3 }, { 2, 2 } };
        const auto& allowed = n == 2 ? kShapes2 : (n == 3 ? kShapes3 : kShapes4);
        for (const auto& s : allowed) {
            if (s == shape) {
                return ct;
            }
        }
        return fail();
    }

    KType CheckBuiltin(Expr& e, const std::vector<KType>& a)
    {
        const std::string& n = e.text;
        const size_t argc = a.size();
        auto fail = [&](const std::string& why) {
            std::string sig = n + "(";
            for (size_t i = 0; i < a.size(); ++i) {
                sig += (i ? ", " : "") + Str(a[i]);
            }
            Report(KernelRule::Builtin, e.line, "組込み関数 " + sig + ") は使えない: " + why);
            return KType::Err();
        };
        auto mixFail = [&](const std::string& why) {
            Report(KernelRule::ScalarMix, e.line,
                   "組込み関数 " + n + " でスカラーとベクトルを混ぜられない: " + why
                       + " (混ぜてよいのは min max clamp lerp step smoothstep の 6 関数だけ)");
            return KType::Err();
        };
        auto isFloatScalar = [](const KType& t) { return t.IsScalar() && t.scalar == ScalarKind::Float; };
        auto isFloatVec = [](const KType& t) { return t.IsVector() && t.scalar == ScalarKind::Float; };
        auto needArgc = [&](size_t want) { return argc == want; };

        static const std::unordered_set<std::string> kFloatUnary = { "floor", "ceil", "frac", "sqrt", "rsqrt", "rcp", "saturate", "sin", "cos" };
        if (kFloatUnary.count(n) != 0) {
            if (needArgc(1) && IsFloatFamily(a[0])) {
                return a[0];
            }
            return fail("float / floatN だけ");
        }
        if (n == "abs") {
            if (needArgc(1) && a[0].IsNumeric() && a[0].scalar != ScalarKind::Uint) {
                return a[0];
            }
            return fail("float / int とそのベクトルだけ");
        }
        if (n == "sign") {
            if (needArgc(1) && a[0].IsNumeric() && a[0].scalar != ScalarKind::Uint) {
                return a[0].IsScalar() ? KType::Scalar(ScalarKind::Int) : KType::Vec(ScalarKind::Int, a[0].lanes);
            }
            return fail("float / int とそのベクトルだけ (結果は int)");
        }
        if (n == "min" || n == "max") {
            if (needArgc(2) && a[0].IsNumeric() && a[1].IsNumeric()) {
                if (a[0] == a[1]) {
                    return a[0];
                }
                if (a[0].IsVector() && a[1].IsScalar() && a[1].scalar == a[0].scalar) {
                    return a[0];
                }
                if (a[0].IsScalar() && a[1].IsVector()) {
                    return mixFail("(スカラー, ベクトル) の順は使えない。(ベクトル, スカラー) に入れ替える");
                }
            }
            return fail("同じ型の 2 引数、または (ベクトル, 同じ要素型のスカラー)");
        }
        if (n == "clamp") {
            if (needArgc(3) && a[0].IsNumeric()) {
                if (a[0] == a[1] && a[1] == a[2]) {
                    return a[0];
                }
                if (a[0].IsVector() && a[1].IsScalar() && a[2].IsScalar() && a[1].scalar == a[0].scalar && a[2].scalar == a[0].scalar) {
                    return a[0];
                }
            }
            return fail("同じ型の 3 引数、または (ベクトル, スカラー, スカラー)");
        }
        if (n == "lerp") {
            if (needArgc(3) && IsFloatFamily(a[0])) {
                if (a[0] == a[1] && a[1] == a[2]) {
                    return a[0];
                }
                if (isFloatVec(a[0]) && a[1] == a[0] && isFloatScalar(a[2])) {
                    return a[0];
                }
            }
            return fail("float の同じ型 3 引数、または (floatN, floatN, float)");
        }
        if (n == "step") {
            if (needArgc(2) && IsFloatFamily(a[0]) && IsFloatFamily(a[1])) {
                if (a[0] == a[1]) {
                    return a[0];
                }
                if (isFloatScalar(a[0]) && isFloatVec(a[1])) {
                    return a[1];
                }
            }
            return fail("float の同じ型 2 引数、または (float, floatN)");
        }
        if (n == "smoothstep") {
            if (needArgc(3) && IsFloatFamily(a[0]) && IsFloatFamily(a[1]) && IsFloatFamily(a[2])) {
                if (a[0] == a[1] && a[1] == a[2]) {
                    return a[0];
                }
                if (isFloatScalar(a[0]) && isFloatScalar(a[1]) && isFloatVec(a[2])) {
                    return a[2];
                }
            }
            return fail("float の同じ型 3 引数、または (float, float, floatN)");
        }
        if (n == "dot" || n == "distance") {
            if (needArgc(2) && ((a[0].IsScalar() && a[1].IsVector()) || (a[0].IsVector() && a[1].IsScalar()))) {
                return mixFail("両引数を同じ floatN にする");
            }
            if (needArgc(2) && isFloatVec(a[0]) && a[0] == a[1]) {
                return KType::Scalar(ScalarKind::Float);
            }
            return fail("同じ floatN の 2 引数だけ");
        }
        if (n == "cross") {
            const KType f3 = KType::Vec(ScalarKind::Float, 3);
            if (needArgc(2) && a[0] == f3 && a[1] == f3) {
                return f3;
            }
            return fail("float3 の 2 引数だけ");
        }
        if (n == "length") {
            if (needArgc(1) && isFloatVec(a[0])) {
                return KType::Scalar(ScalarKind::Float);
            }
            return fail("floatN だけ");
        }
        if (n == "normalize") {
            if (needArgc(1) && isFloatVec(a[0])) {
                return a[0];
            }
            return fail("floatN だけ");
        }
        if (n == "FloatToInt" || n == "FloatToUint") {
            if (needArgc(1) && isFloatScalar(a[0])) {
                return KType::Scalar(n == "FloatToInt" ? ScalarKind::Int : ScalarKind::Uint);
            }
            return fail("float の 1 引数だけ");
        }
        if (n == "HashU32" || n == "HashToUnitFloat") {
            if (needArgc(1) && a[0] == KType::Scalar(ScalarKind::Uint)) {
                return n == "HashU32" ? KType::Scalar(ScalarKind::Uint) : KType::Scalar(ScalarKind::Float);
            }
            return fail("uint の 1 引数だけ");
        }
        return fail("未対応");
    }

    KernelProgram& prog_;
    const std::string& kernelName_;
    const DiagSink& sink_;
    std::vector<Scope> scopes_;
    std::unordered_map<std::string, int> structIndex_;
    std::unordered_map<std::string, int> funcIndex_;
    std::unordered_set<std::string> globalNames_;
    bool inFunction_ = false;
    std::string currentFunction_;
    KType returnType_;
    int loopDepth_ = 0;
};

} // namespace

void CheckKernelProgram(KernelProgram& program, const std::string& kernelName, const DiagSink& sink)
{
    Checker(program, kernelName, sink).Run();
}

} // namespace lint

namespace {

constexpr std::string_view kKernelSuffix = ".kernel.hlsl";

} // namespace

KernelLintResult LintKernelSource(std::string_view fileName, std::string_view text)
{
    KernelLintResult result;
    result.sourceHash = HashKernelSource(text);
    std::string leaf(fileName);
    const size_t slash = leaf.find_last_of("/\\");
    if (slash != std::string::npos) {
        leaf = leaf.substr(slash + 1);
    }
    lint::DiagSink sink;
    sink.file = leaf;
    sink.diags = &result.diagnostics;
    if (leaf.size() <= kKernelSuffix.size() || leaf.substr(leaf.size() - kKernelSuffix.size()) != kKernelSuffix) {
        sink.Add(KernelRule::KernelDecl, 1, "ファイル名は <カーネル名>.kernel.hlsl でなければならない");
        return result;
    }
    result.kernelName = leaf.substr(0, leaf.size() - kKernelSuffix.size());

    auto program = std::make_shared<lint::KernelProgram>();
    program->fileName = leaf;
    if (lint::ParseKernelSource(text, sink, *program)) {
        lint::CheckKernelProgram(*program, result.kernelName, sink);
    }
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const KernelDiagnostic& a, const KernelDiagnostic& b) { return a.line < b.line; });
    if (result.diagnostics.empty()) {
        result.program = std::move(program);
    }
    return result;
}

std::shared_ptr<const KernelLintResult> KernelLintCache::Get(const std::string& fileName, std::string_view text)
{
    const uint64_t hash = HashKernelSource(text);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(fileName);
    if (it != entries_.end() && it->second->sourceHash == hash) {
        return it->second;
    }
    auto fresh = std::make_shared<const KernelLintResult>(LintKernelSource(fileName, text));
    entries_[fileName] = fresh;
    return fresh;
}

void KernelLintCache::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

} // namespace mye::kernel
