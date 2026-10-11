//====================================================================================
//                          KernelAst.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル (HLSL サブセット) の構文木と型
//====================================================================================
#pragma once
// KernelLint (検査器) と KernelCSharp (C# 変換器) が共有する構文木。
// サブセット (spec 4.1.2) の外は木に載せない — パーサが診断を出して打ち切る。
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mye::kernel::lint {

enum class ScalarKind : uint8_t { Float, Int, Uint };

// カーネルの型。struct は KernelProgram::structs の添字で持つ
struct KType {
    enum class Kind : uint8_t { Error, Void, Bool, Scalar, Vector, Struct };
    Kind kind = Kind::Error;
    ScalarKind scalar = ScalarKind::Float;
    uint8_t lanes = 1;
    int structIndex = -1;

    static KType Err() { return {}; }
    static KType Void() { return KType{ Kind::Void }; }
    static KType Bool() { return KType{ Kind::Bool }; }
    static KType Scalar(ScalarKind s) { return KType{ Kind::Scalar, s, 1, -1 }; }
    static KType Vec(ScalarKind s, int lanes) { return KType{ Kind::Vector, s, static_cast<uint8_t>(lanes), -1 }; }
    static KType Struct(int index) { return KType{ Kind::Struct, ScalarKind::Float, 1, index }; }

    bool IsError() const { return kind == Kind::Error; }
    bool IsScalar() const { return kind == Kind::Scalar; }
    bool IsVector() const { return kind == Kind::Vector; }
    bool IsNumeric() const { return kind == Kind::Scalar || kind == Kind::Vector; }
    bool operator==(const KType& o) const
    {
        return kind == o.kind && structIndex == o.structIndex
            && (kind != Kind::Scalar && kind != Kind::Vector ? true : scalar == o.scalar && lanes == o.lanes);
    }
    bool operator!=(const KType& o) const { return !(*this == o); }
};

// ---- 式 ----

enum class ExprKind : uint8_t { IntLit, FloatLit, BoolLit, Ident, Unary, Binary, Ternary, Call, Cast, Member, Index };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    // 識別子が何を指すか (検査器が決める。変換器が id / params / バッファを書き分けるのに使う)
    enum class Ref : uint8_t { None, Local, KernelId, KernelParams, Buffer };
    // 呼び出しの種別 (検査器が決める)
    enum class CallKind : uint8_t { None, User, Builtin, Ctor };

    ExprKind kind = ExprKind::IntLit;
    int line = 0;
    // IntLit/FloatLit: 正規化済みのリテラル (C# にそのまま書ける形) / Ident・Call・Member: 名前 /
    // Unary・Binary: 演算子 / Cast: 変換先の型名
    std::string text;
    uint64_t intValue = 0;
    bool boolValue = false;
    std::vector<ExprPtr> args; // Unary:1 Binary:2 Ternary:3 Call:n Cast:1 Member:1 Index:2 (基底, 添字)

    // 検査結果
    KType type;
    bool assignable = false;
    Ref ref = Ref::None;
    CallKind callKind = CallKind::None;
};

// ---- 文 ----

enum class StmtKind : uint8_t { Block, Decl, Assign, IncDec, If, For, While, Break, Continue, Return, ExprStmt };

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind = StmtKind::Block;
    int line = 0;
    bool isConst = false;
    std::string typeName; // Decl
    std::string name;     // Decl
    std::string op;       // Assign (= += ...) / IncDec (++ --)
    bool prefix = false;  // IncDec
    ExprPtr expr;         // Decl の初期化子 / Assign の右辺 / If・While・For の条件 / Return の値 / ExprStmt / IncDec の対象
    ExprPtr lhs;          // Assign の左辺
    StmtPtr init;         // For
    StmtPtr step;         // For
    StmtPtr thenStmt;     // If の then / For・While の本体
    StmtPtr elseStmt;     // If の else
    std::vector<StmtPtr> stmts; // Block
};

// ---- トップレベル ----

struct StructMember {
    std::string typeName;
    std::string name;
    int line = 0;
    KType type;
};

struct StructDef {
    std::string name;
    int line = 0;
    bool isParams = false; // KERNEL_PARAMS
    std::vector<StructMember> members;
};

struct FuncParam {
    std::string typeName;
    std::string name;
    int line = 0;
    KType type;
};

struct FuncDef {
    std::string name;
    int line = 0;
    std::string retTypeName;
    KType ret;
    std::vector<FuncParam> params;
    StmtPtr body;
};

struct BufferDecl {
    bool rw = false;
    std::string typeName;
    std::string name;
    int line = 0;
    KType elem;
};

struct KernelDef {
    std::string name;
    std::string paramsName;
    int paramsStruct = -1;
    int line = 0;
    std::vector<BufferDecl> buffers;
    StmtPtr body;
};

// 宣言の出現順 (名前は先に宣言されたものだけが見える)
struct TopItem {
    enum class Kind : uint8_t { Struct, Func } kind = Kind::Struct;
    int index = 0;
};

struct KernelProgram {
    std::string fileName;
    std::vector<StructDef> structs;
    std::vector<FuncDef> funcs;
    std::vector<TopItem> items;
    KernelDef kernel;
    bool hasKernel = false;
};

} // namespace mye::kernel::lint
