//====================================================================================
//                          KernelParser.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル (HLSL サブセット) の字句解析と構文解析
//====================================================================================
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

#include "Engine/Engine/Compute/KernelLintInternal.h"

namespace mye::kernel::lint {

namespace {

// ---- 名前の表 ----

const std::unordered_set<std::string_view>& ControlKeywords()
{
    static const std::unordered_set<std::string_view> s = {
        "switch", "case", "default", "do", "goto", "discard", "typedef", "static", "groupshared", "namespace",
        "class", "template", "using", "enum", "union", "extern", "inline", "cbuffer", "tbuffer", "interface",
        "shared", "uniform", "volatile", "register", "precise", "nointerpolation", "row_major", "column_major",
        "export", "try", "catch", "throw", "new", "delete", "sizeof", "asm", "typename",
    };
    return s;
}

const std::unordered_set<std::string_view>& CppKeywords()
{
    static const std::unordered_set<std::string_view> s = {
        "alignas", "alignof", "and", "and_eq", "auto", "bitand", "bitor", "bool", "break", "char", "char8_t",
        "char16_t", "char32_t", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "double", "dynamic_cast", "else", "explicit",
        "false", "float", "for", "friend", "if", "int", "long", "mutable", "noexcept", "not", "not_eq", "nullptr",
        "operator", "or", "or_eq", "private", "protected", "public", "reinterpret_cast", "requires", "return",
        "short", "signed", "static_assert", "static_cast", "struct", "this", "thread_local", "true", "typeid",
        "union", "unsigned", "virtual", "void", "wchar_t", "while", "xor", "xor_eq", "vector", "matrix", "string",
        "texture", "sampler", "technique", "pass", "half", "uint", "dword",
    };
    return s;
}

const std::unordered_set<std::string_view>& KernelWords()
{
    static const std::unordered_set<std::string_view> s = {
        "params", "id", "KernelId", "HashU32", "HashToUnitFloat", "FloatToInt", "FloatToUint", "KERNEL", "KERNEL_PARAMS", "RW", "RO",
        // 生成する C# のクラス内の名前とぶつかる語
        "Body", "Range", "SourceHash", "CpuKernel", "KernelName",
    };
    return s;
}

const std::unordered_set<std::string_view>& WhitelistedFunctions()
{
    static const std::unordered_set<std::string_view> s = {
        "min", "max", "clamp", "abs", "saturate", "lerp", "step", "smoothstep", "floor", "ceil", "frac", "sqrt",
        "rsqrt", "rcp", "dot", "cross", "length", "normalize", "distance", "sign", "sin", "cos",
    };
    return s;
}

const std::unordered_set<std::string_view>& HlslIntrinsics()
{
    static const std::unordered_set<std::string_view> s = {
        "acos", "all", "any", "asdouble", "asfloat", "asin", "asint", "asuint", "atan", "atan2", "clip", "cosh",
        "countbits", "ddx", "ddy", "degrees", "determinant", "dst", "exp", "exp2", "f16tof32", "f32tof16",
        "faceforward", "firstbithigh", "firstbitlow", "fma", "fmod", "frexp", "fwidth", "isfinite", "isinf", "isnan",
        "ldexp", "lit", "log", "log10", "log2", "mad", "modf", "msad4", "mul", "noise", "pow", "radians", "reflect",
        "refract", "reversebits", "round", "sincos", "sinh", "tan", "tanh", "transpose", "trunc", "printf", "errorf",
        "abort", "abs", "fmax", "fmin", "fabs", "sqrtf", "sinf", "cosf", "powf", "floorf", "ceilf",
    };
    return s;
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix;
}

} // namespace

bool ParseBuiltinTypeName(std::string_view name, KType* out)
{
    ScalarKind kind;
    std::string_view rest;
    if (StartsWith(name, "float")) {
        kind = ScalarKind::Float;
        rest = name.substr(5);
    } else if (StartsWith(name, "uint")) {
        kind = ScalarKind::Uint;
        rest = name.substr(4);
    } else if (StartsWith(name, "int")) {
        kind = ScalarKind::Int;
        rest = name.substr(3);
    } else {
        return false;
    }
    if (rest.empty()) {
        if (out) {
            *out = KType::Scalar(kind);
        }
        return true;
    }
    if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') {
        if (out) {
            *out = KType::Vec(kind, rest[0] - '0');
        }
        return true;
    }
    return false;
}

const char* ScalarName(ScalarKind s)
{
    switch (s) {
    case ScalarKind::Float: return "float";
    case ScalarKind::Int: return "int";
    case ScalarKind::Uint: return "uint";
    }
    return "?";
}

std::string TypeToString(const KType& t, const KernelProgram* program)
{
    switch (t.kind) {
    case KType::Kind::Error: return "<error>";
    case KType::Kind::Void: return "void";
    case KType::Kind::Bool: return "bool";
    case KType::Kind::Scalar: return ScalarName(t.scalar);
    case KType::Kind::Vector: return std::string(ScalarName(t.scalar)) + std::to_string(t.lanes);
    case KType::Kind::Struct:
        if (program && t.structIndex >= 0 && t.structIndex < static_cast<int>(program->structs.size())) {
            return program->structs[t.structIndex].name;
        }
        return "struct";
    }
    return "?";
}

bool IsUnsupportedTypeName(std::string_view name)
{
    static const std::unordered_set<std::string_view> exact = {
        "bool", "double", "half", "string", "vector", "matrix", "dword", "long", "short", "char", "signed",
        "unsigned", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "Buffer", "RWBuffer", "SamplerState", "SamplerComparisonState", "ByteAddressBuffer",
        "RWByteAddressBuffer", "StructuredBuffer", "RWStructuredBuffer", "AppendStructuredBuffer",
        "ConsumeStructuredBuffer", "cbuffer",
    };
    if (exact.count(name) != 0) {
        return true;
    }
    if (StartsWith(name, "Texture") || StartsWith(name, "RWTexture") || StartsWith(name, "min16")
        || StartsWith(name, "min10") || StartsWith(name, "min12") || StartsWith(name, "Sampler")) {
        return true;
    }
    // bool2 / double3 / half4 と、float3x3 / int2x4 などの行列
    for (std::string_view base : { "bool", "double", "half", "float", "int", "uint" }) {
        if (!StartsWith(name, base) || name.size() == base.size()) {
            continue;
        }
        const std::string_view rest = name.substr(base.size());
        const bool isVec = rest.size() == 1 && rest[0] >= '1' && rest[0] <= '4';
        const bool isMat = rest.size() == 3 && rest[0] >= '1' && rest[0] <= '4' && rest[1] == 'x'
            && rest[2] >= '1' && rest[2] <= '4';
        if (isMat) {
            return true;
        }
        if (isVec) {
            // float1 / int1 / uint1 も未対応。float2..4 / int2..4 / uint2..4 は対応
            const bool supportedBase = base == "float" || base == "int" || base == "uint";
            if (!supportedBase || rest[0] == '1') {
                return true;
            }
        }
    }
    return false;
}

bool IsControlKeyword(std::string_view name)
{
    return ControlKeywords().count(name) != 0;
}

bool IsBarrierOrAtomic(std::string_view name)
{
    return StartsWith(name, "Interlocked") || name.find("MemoryBarrier") != std::string_view::npos;
}

bool IsKnownHlslIntrinsic(std::string_view name)
{
    return HlslIntrinsics().count(name) != 0 && WhitelistedFunctions().count(name) == 0;
}

bool IsReservedIdentifier(std::string_view name)
{
    KType unused;
    if (ParseBuiltinTypeName(name, &unused) || IsUnsupportedTypeName(name) || IsControlKeyword(name)
        || CppKeywords().count(name) != 0 || KernelWords().count(name) != 0
        || WhitelistedFunctions().count(name) != 0 || HlslIntrinsics().count(name) != 0
        || IsBarrierOrAtomic(name)) {
        return true;
    }
    if (StartsWith(name, "__")) {
        return true;
    }
    for (std::string_view suffix : { "_Body", "_Cpu", "_KernelName" }) {
        if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
            return true;
        }
    }
    return false;
}

namespace {

// ---- 字句解析 ----

struct Token {
    enum class Kind : uint8_t { Ident, IntLit, FloatLit, Punct, Directive, String, End };
    Kind kind = Kind::End;
    std::string text; // IntLit / FloatLit は正規化済み
    int line = 1;
    uint64_t value = 0; // IntLit
    bool isUint = false;
};

bool IsIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}
bool IsIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

class Lexer {
public:
    Lexer(std::string_view src, const DiagSink& sink) : src_(src), sink_(sink) {}

    std::vector<Token> Run()
    {
        std::vector<Token> out;
        // UTF-8 BOM は読み飛ばす
        if (src_.substr(0, 3) == "\xEF\xBB\xBF") {
            pos_ = 3;
        }
        bool lineStart = true;
        while (true) {
            // 空白とコメント
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if (c == '\n') {
                    ++line_;
                    ++pos_;
                    lineStart = true;
                } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                    ++pos_;
                } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                    while (pos_ < src_.size() && src_[pos_] != '\n') {
                        ++pos_;
                    }
                } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                    const int startLine = line_;
                    pos_ += 2;
                    bool closed = false;
                    while (pos_ < src_.size()) {
                        if (src_[pos_] == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                            pos_ += 2;
                            closed = true;
                            break;
                        }
                        if (src_[pos_] == '\n') {
                            ++line_;
                        }
                        ++pos_;
                    }
                    if (!closed) {
                        Fail(KernelRule::Syntax, startLine, "閉じていないブロックコメント");
                    }
                } else {
                    break;
                }
            }
            if (pos_ >= src_.size()) {
                break;
            }
            const char c = src_[pos_];
            Token t;
            t.line = line_;
            if (c == '#') {
                if (!lineStart) {
                    Fail(KernelRule::Preprocessor, line_, "# は行頭の前処理でだけ使える");
                }
                size_t end = pos_;
                while (end < src_.size() && src_[end] != '\n') {
                    ++end;
                }
                std::string text(src_.substr(pos_, end - pos_));
                const size_t comment = text.find("//");
                if (comment != std::string::npos) {
                    text.resize(comment);
                }
                while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
                    text.pop_back();
                }
                t.kind = Token::Kind::Directive;
                t.text = std::move(text);
                pos_ = end;
            } else if (IsIdentStart(c)) {
                size_t end = pos_;
                while (end < src_.size() && IsIdentChar(src_[end])) {
                    ++end;
                }
                t.kind = Token::Kind::Ident;
                t.text = std::string(src_.substr(pos_, end - pos_));
                pos_ = end;
            } else if (std::isdigit(static_cast<unsigned char>(c)) != 0
                       || (c == '.' && pos_ + 1 < src_.size()
                           && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) != 0)) {
                LexNumber(t);
            } else if (c == '"') {
                size_t end = pos_ + 1;
                while (end < src_.size() && src_[end] != '"' && src_[end] != '\n') {
                    ++end;
                }
                if (end >= src_.size() || src_[end] != '"') {
                    Fail(KernelRule::Syntax, line_, "閉じていない文字列");
                }
                t.kind = Token::Kind::String;
                t.text = std::string(src_.substr(pos_ + 1, end - pos_ - 1));
                pos_ = end + 1;
            } else {
                t.kind = Token::Kind::Punct;
                t.text = LexPunct();
            }
            lineStart = false;
            out.push_back(std::move(t));
        }
        Token end;
        end.kind = Token::Kind::End;
        end.line = line_;
        out.push_back(std::move(end));
        return out;
    }

private:
    [[noreturn]] void Fail(KernelRule rule, int line, std::string msg) const
    {
        sink_.Add(rule, line, std::move(msg));
        throw ParseAbort{};
    }

    std::string LexPunct()
    {
        static const char* const three[] = { "<<=", ">>=" };
        static const char* const two[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                                           "==", "!=", "<=", ">=", "&&", "||", "<<", ">>" };
        const std::string_view rest = src_.substr(pos_);
        for (const char* p : three) {
            if (StartsWith(rest, p)) {
                pos_ += 3;
                return p;
            }
        }
        for (const char* p : two) {
            if (StartsWith(rest, p)) {
                pos_ += 2;
                return p;
            }
        }
        const char c = src_[pos_];
        static const std::string_view singles = "+-*/%&|^~!<>=?:;,.()[]{}";
        if (singles.find(c) == std::string_view::npos) {
            const unsigned char u = static_cast<unsigned char>(c);
            std::string what = u >= 0x80 ? "非 ASCII 文字" : std::string("'") + c + "'";
            Fail(KernelRule::Syntax, line_, "サブセットで使えない文字: " + what);
        }
        ++pos_;
        return std::string(1, c);
    }

    void LexNumber(Token& t)
    {
        size_t j = pos_;
        bool isFloat = false;
        bool hex = false;
        if (src_[j] == '0' && j + 1 < src_.size() && (src_[j + 1] == 'x' || src_[j + 1] == 'X')) {
            hex = true;
            j += 2;
            while (j < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[j])) != 0) {
                ++j;
            }
        } else {
            while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j])) != 0) {
                ++j;
            }
            if (j < src_.size() && src_[j] == '.') {
                isFloat = true;
                ++j;
                while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j])) != 0) {
                    ++j;
                }
            }
            if (j < src_.size() && (src_[j] == 'e' || src_[j] == 'E')) {
                size_t k = j + 1;
                if (k < src_.size() && (src_[k] == '+' || src_[k] == '-')) {
                    ++k;
                }
                if (k < src_.size() && std::isdigit(static_cast<unsigned char>(src_[k])) != 0) {
                    isFloat = true;
                    j = k;
                    while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j])) != 0) {
                        ++j;
                    }
                }
            }
        }
        const size_t numEnd = j;
        while (j < src_.size() && IsIdentChar(src_[j])) {
            ++j;
        }
        const std::string mantissa(src_.substr(pos_, numEnd - pos_));
        const std::string suffix(src_.substr(numEnd, j - numEnd));
        const std::string whole(src_.substr(pos_, j - pos_));
        pos_ = j;

        if (isFloat) {
            if (suffix != "f" && suffix != "F") {
                Fail(KernelRule::FloatLiteral, t.line,
                     "小数リテラル '" + whole + "' に f が無い (HLSL では float だが C++ / C# では double になる)。"
                         + mantissa + "f と書く");
            }
            // C# にそのまま書ける形へ: 先頭の . の前に 0、"1." の後ろに 0、サフィックスは小文字 f
            std::string m = mantissa;
            if (m[0] == '.') {
                m = "0" + m;
            }
            const size_t dot = m.find('.');
            if (dot != std::string::npos && (dot + 1 >= m.size() || !std::isdigit(static_cast<unsigned char>(m[dot + 1])))) {
                m.insert(dot + 1, "0");
            }
            t.kind = Token::Kind::FloatLit;
            t.text = m + "f";
            return;
        }
        if (suffix == "f" || suffix == "F") {
            Fail(KernelRule::FloatLiteral, t.line, "整数 '" + whole + "' に f は付けられない。" + mantissa + ".0f と書く");
        }
        if (!suffix.empty() && suffix != "u" && suffix != "U") {
            Fail(KernelRule::IntLiteral, t.line, "整数リテラルの接尾辞 '" + suffix + "' は使えない (u だけ)");
        }
        const bool isUint = !suffix.empty();
        uint64_t value = 0;
        if (hex) {
            if (mantissa.size() <= 2) {
                Fail(KernelRule::IntLiteral, t.line, "16 進リテラルに桁が無い");
            }
            for (size_t k = 2; k < mantissa.size(); ++k) {
                const char c = mantissa[k];
                const uint64_t d = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0' : (std::tolower(c) - 'a' + 10);
                value = value * 16 + d;
                if (value > 0xFFFFFFFFull) {
                    Fail(KernelRule::IntLiteral, t.line, "整数リテラル '" + whole + "' が 32 bit を超える");
                }
            }
        } else {
            if (mantissa.size() > 1 && mantissa[0] == '0') {
                Fail(KernelRule::IntLiteral, t.line, "先頭が 0 の整数 '" + whole + "' は C++ では 8 進、C# では 10 進になる");
            }
            for (char c : mantissa) {
                value = value * 10 + static_cast<uint64_t>(c - '0');
                if (value > 0xFFFFFFFFull) {
                    Fail(KernelRule::IntLiteral, t.line, "整数リテラル '" + whole + "' が 32 bit を超える");
                }
            }
        }
        if (!isUint && value > 0x7FFFFFFFull) {
            Fail(KernelRule::IntLiteral, t.line,
                 "整数リテラル '" + whole + "' が int の範囲を超える (uint なら u を付ける)");
        }
        t.kind = Token::Kind::IntLit;
        t.value = value;
        t.isUint = isUint;
        t.text = mantissa + (isUint ? "u" : "");
    }

    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    const DiagSink& sink_;
};

// ---- 構文解析 ----

bool IsAssignOp(const std::string& s)
{
    return s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=" || s == "%=" || s == "&=" || s == "|="
        || s == "^=" || s == "<<=" || s == ">>=";
}

int BinaryPrecedence(const std::string& s)
{
    if (s == "||") return 1;
    if (s == "&&") return 2;
    if (s == "|") return 3;
    if (s == "^") return 4;
    if (s == "&") return 5;
    if (s == "==" || s == "!=") return 6;
    if (s == "<" || s == ">" || s == "<=" || s == ">=") return 7;
    if (s == "<<" || s == ">>") return 8;
    if (s == "+" || s == "-") return 9;
    if (s == "*" || s == "/" || s == "%") return 10;
    return 0;
}

// 入れ子と式の長さの上限。再帰降下 (パーサ・検査器・変換器) がスタックを使い切らないため
constexpr int kMaxNesting = 200;
constexpr int kMaxBinaryChain = 500;

class Parser {
public:
    Parser(std::vector<Token> toks, const DiagSink& sink, KernelProgram& prog)
        : toks_(std::move(toks)), sink_(sink), prog_(prog)
    {
    }

    void ParseProgram()
    {
        int includeCount = 0;
        bool sawItem = false;
        while (Cur().kind != Token::Kind::End) {
            if (prog_.hasKernel) {
                Fail(KernelRule::KernelDecl, Cur().line, "KERNEL の後ろには何も置けない (KERNEL は末尾の 1 本だけ)");
            }
            const Token& t = Cur();
            if (t.kind == Token::Kind::Directive) {
                ParseDirective(includeCount, sawItem);
                continue;
            }
            if (t.kind != Token::Kind::Ident) {
                Fail(KernelRule::Syntax, t.line, "トップレベルに '" + t.text + "' は置けない");
            }
            sawItem = true;
            if (t.text == "struct") {
                ParseStruct(false);
            } else if (t.text == "KERNEL_PARAMS") {
                ParseStruct(true);
            } else if (t.text == "KERNEL") {
                ParseKernel();
            } else {
                ParseTopLevelOther();
            }
        }
        if (includeCount == 0) {
            Fail(KernelRule::Preprocessor, 1, "#include \"kernel_common.hlsli\" が無い");
        }
        if (!prog_.hasKernel) {
            Fail(KernelRule::KernelDecl, Cur().line, "KERNEL(...) が無い");
        }
    }

private:
    // 再帰の深さを数える。上限を超えたら打ち切る
    class NestGuard {
    public:
        explicit NestGuard(Parser& p) : parser_(p)
        {
            if (++parser_.nest_ > kMaxNesting) {
                parser_.Fail(KernelRule::Syntax, parser_.Cur().line, "入れ子が深すぎる (上限 200)");
            }
        }
        ~NestGuard() { --parser_.nest_; }
        NestGuard(const NestGuard&) = delete;
        NestGuard& operator=(const NestGuard&) = delete;

    private:
        Parser& parser_;
    };

    // ---- トークンの操作 ----
    const Token& Cur() const { return toks_[pos_]; }
    const Token& Peek(size_t n = 1) const { return toks_[std::min(pos_ + n, toks_.size() - 1)]; }
    void Advance() { if (pos_ + 1 < toks_.size()) ++pos_; }
    bool IsPunct(const char* p) const { return Cur().kind == Token::Kind::Punct && Cur().text == p; }
    bool IsIdent(const char* p) const { return Cur().kind == Token::Kind::Ident && Cur().text == p; }
    bool PeekIsPunct(size_t n, const char* p) const { return Peek(n).kind == Token::Kind::Punct && Peek(n).text == p; }

    [[noreturn]] void Fail(KernelRule rule, int line, std::string msg) const
    {
        sink_.Add(rule, line, std::move(msg));
        throw ParseAbort{};
    }

    // 期待した記号が無いとき。代入・増減が来ていれば専用の規則で報告する
    [[noreturn]] void FailExpected(const char* want) const
    {
        const Token& t = Cur();
        if (t.kind == Token::Kind::Punct && (IsAssignOp(t.text) || t.text == "++" || t.text == "--")) {
            Fail(KernelRule::AssignInExpr, t.line,
                 "式の中で代入や ++ -- は使えない ('" + t.text + "')。文として分けて書く");
        }
        Fail(KernelRule::Syntax, t.line, std::string("'") + want + "' が必要 (見つかったのは '" + DescribeCur() + "')");
    }

    std::string DescribeCur() const
    {
        return Cur().kind == Token::Kind::End ? std::string("ファイル末尾") : Cur().text;
    }

    void ExpectPunct(const char* p)
    {
        if (!IsPunct(p)) {
            FailExpected(p);
        }
        Advance();
    }

    std::string ExpectIdent(const char* what)
    {
        if (Cur().kind != Token::Kind::Ident) {
            Fail(KernelRule::Syntax, Cur().line, std::string(what) + "が必要 (見つかったのは '" + DescribeCur() + "')");
        }
        std::string s = Cur().text;
        Advance();
        return s;
    }

    bool IsTypeName(const std::string& name) const
    {
        return ParseBuiltinTypeName(name, nullptr) || structNames_.count(name) != 0;
    }

    // ---- トップレベル ----

    void ParseDirective(int& includeCount, bool sawItem)
    {
        const Token& t = Cur();
        // "#include \"kernel_common.hlsli\"" (空白は許す)
        std::string compact;
        for (char c : t.text) {
            if (c != ' ' && c != '\t') {
                compact += c;
            }
        }
        if (compact != "#include\"kernel_common.hlsli\"") {
            Fail(KernelRule::Preprocessor, t.line,
                 "前処理は #include \"kernel_common.hlsli\" だけ使える ('" + t.text + "')");
        }
        if (sawItem) {
            Fail(KernelRule::Preprocessor, t.line, "#include は他の宣言より前に書く");
        }
        if (++includeCount > 1) {
            Fail(KernelRule::Preprocessor, t.line, "#include \"kernel_common.hlsli\" が 2 回ある");
        }
        Advance();
    }

    void ParseStruct(bool isParams)
    {
        StructDef def;
        def.line = Cur().line;
        def.isParams = isParams;
        Advance(); // struct / KERNEL_PARAMS
        if (isParams) {
            ExpectPunct("(");
            def.name = ExpectIdent("KERNEL_PARAMS の名前");
            ExpectPunct(")");
        } else {
            def.name = ExpectIdent("struct の名前");
        }
        ExpectPunct("{");
        while (!IsPunct("}")) {
            StructMember m;
            m.line = Cur().line;
            m.typeName = ParseTypeName("メンバの型");
            m.name = ExpectIdent("メンバ名");
            if (IsPunct("[")) {
                Fail(KernelRule::Array, Cur().line, "配列メンバは使えない ('" + m.name + "')");
            }
            ExpectPunct(";");
            def.members.push_back(std::move(m));
        }
        Advance(); // }
        ExpectPunct(";");
        structNames_.insert(def.name);
        prog_.items.push_back(TopItem{ TopItem::Kind::Struct, static_cast<int>(prog_.structs.size()) });
        prog_.structs.push_back(std::move(def));
    }

    // 型名 1 語を読む (未対応の型はここで弾く)
    std::string ParseTypeName(const char* what)
    {
        if (Cur().kind != Token::Kind::Ident) {
            Fail(KernelRule::Syntax, Cur().line, std::string(what) + "が必要 (見つかったのは '" + DescribeCur() + "')");
        }
        const std::string name = Cur().text;
        if (IsUnsupportedTypeName(name)) {
            Fail(KernelRule::UnsupportedType, Cur().line, "型 '" + name + "' はサブセットに無い (float / int / uint とそのベクトル、それらだけの struct)");
        }
        if (IsControlKeyword(name)) {
            Fail(KernelRule::Keyword, Cur().line, "'" + name + "' はサブセットに無い");
        }
        Advance();
        return name;
    }

    void ParseTopLevelOther()
    {
        const Token& t = Cur();
        if (IsControlKeyword(t.text)) {
            Fail(KernelRule::Keyword, t.line, "'" + t.text + "' はサブセットに無い");
        }
        if (t.text == "const") {
            Fail(KernelRule::Global, t.line, "グローバル変数 / 定数は使えない (値は KERNEL_PARAMS で渡す)");
        }
        // 関数: 戻り値の型 名前 (
        const int line = t.line;
        if (t.text == "void") {
            Fail(KernelRule::Syntax, line, "void 関数は使えない (副作用が無いので意味が無い。値を返す関数だけ)");
        }
        const std::string retType = ParseTypeName("宣言");
        if (Cur().kind != Token::Kind::Ident) {
            Fail(KernelRule::Syntax, Cur().line, "関数名が必要 (見つかったのは '" + DescribeCur() + "')");
        }
        const std::string name = Cur().text;
        Advance();
        if (!IsPunct("(")) {
            if (IsPunct("=") || IsPunct(";") || IsPunct("[") || IsPunct(",")) {
                Fail(KernelRule::Global, line, "グローバル変数 '" + name + "' は使えない (値は KERNEL_PARAMS で渡す)");
            }
            FailExpected("(");
        }
        Advance();
        FuncDef fn;
        fn.line = line;
        fn.name = name;
        fn.retTypeName = retType;
        if (!IsPunct(")")) {
            while (true) {
                FuncParam p;
                p.line = Cur().line;
                if (Cur().kind == Token::Kind::Ident
                    && (Cur().text == "out" || Cur().text == "inout" || Cur().text == "in")) {
                    Fail(KernelRule::OutParam, Cur().line,
                         "引数の修飾子 '" + Cur().text + "' は使えない (値渡しだけ。結果は戻り値で返す)");
                }
                if (Cur().kind == Token::Kind::Ident && Cur().text == "const") {
                    Fail(KernelRule::Syntax, Cur().line, "引数に const は付けられない (C# に対応する書き方が無い)");
                }
                p.typeName = ParseTypeName("引数の型");
                p.name = ExpectIdent("引数名");
                if (IsPunct("[")) {
                    Fail(KernelRule::Array, Cur().line, "配列の引数は使えない ('" + p.name + "')");
                }
                fn.params.push_back(std::move(p));
                if (IsPunct(",")) {
                    Advance();
                    continue;
                }
                break;
            }
        }
        ExpectPunct(")");
        if (IsPunct(":")) {
            Fail(KernelRule::Syntax, Cur().line, "セマンティクスは使えない");
        }
        if (!IsPunct("{")) {
            FailExpected("{");
        }
        fn.body = ParseBlock();
        prog_.items.push_back(TopItem{ TopItem::Kind::Func, static_cast<int>(prog_.funcs.size()) });
        prog_.funcs.push_back(std::move(fn));
    }

    void ParseKernel()
    {
        KernelDef& k = prog_.kernel;
        k.line = Cur().line;
        Advance(); // KERNEL
        ExpectPunct("(");
        k.name = ExpectIdent("カーネル名");
        if (!IsPunct(",")) {
            FailExpected(",");
        }
        Advance();
        k.paramsName = ExpectIdent("KERNEL_PARAMS の名前");
        if (!IsPunct(",")) {
            FailExpected(",");
        }
        Advance();
        // RW(T, n) RO(U, m) をコンマ無しで並べる
        while (Cur().kind == Token::Kind::Ident && (Cur().text == "RW" || Cur().text == "RO")) {
            BufferDecl b;
            b.line = Cur().line;
            b.rw = Cur().text == "RW";
            Advance();
            ExpectPunct("(");
            b.typeName = ParseTypeName("バッファの要素型");
            if (!IsPunct(",")) {
                FailExpected(",");
            }
            Advance();
            b.name = ExpectIdent("バッファ名");
            ExpectPunct(")");
            k.buffers.push_back(std::move(b));
            if (IsPunct(",")) {
                Fail(KernelRule::KernelDecl, Cur().line,
                     "RW(...) / RO(...) の間にコンマは書かない (FXC のマクロは可変長引数を持てない)。空白で並べる");
            }
        }
        if (!IsPunct(")")) {
            Fail(KernelRule::KernelDecl, Cur().line,
                 "KERNEL のバッファ引数は RW(型, 名前) / RO(型, 名前) の並び (見つかったのは '" + DescribeCur() + "')");
        }
        Advance();
        if (!IsPunct("{")) {
            FailExpected("{");
        }
        k.body = ParseBlock();
        prog_.hasKernel = true;
    }

    // ---- 文 ----

    StmtPtr NewStmt(StmtKind kind, int line) const
    {
        auto s = std::make_unique<Stmt>();
        s->kind = kind;
        s->line = line;
        return s;
    }

    StmtPtr ParseBlock()
    {
        auto block = NewStmt(StmtKind::Block, Cur().line);
        ExpectPunct("{");
        while (!IsPunct("}")) {
            if (Cur().kind == Token::Kind::End) {
                Fail(KernelRule::Syntax, Cur().line, "閉じていない '{'");
            }
            block->stmts.push_back(ParseStatement(false));
        }
        Advance();
        return block;
    }

    bool AtDeclaration() const
    {
        if (Cur().kind != Token::Kind::Ident) {
            return false;
        }
        if (Cur().text == "const") {
            return true;
        }
        // 型名の次が識別子なら宣言 (未知の型名でも宣言として読み、検査器が未定義の型として報告する)
        return Peek().kind == Token::Kind::Ident && Cur().text != "return" && Cur().text != "else";
    }

    StmtPtr ParseDeclaration()
    {
        auto s = NewStmt(StmtKind::Decl, Cur().line);
        if (IsIdent("const")) {
            s->isConst = true;
            Advance();
        }
        s->typeName = ParseTypeName("型");
        s->name = ExpectIdent("変数名");
        if (IsPunct("[")) {
            Fail(KernelRule::Array, Cur().line, "配列は使えない ('" + s->name + "')");
        }
        if (IsPunct(",")) {
            Fail(KernelRule::Syntax, Cur().line, "1 つの宣言で複数の変数は宣言できない (1 宣言 1 変数)");
        }
        if (IsPunct(":")) {
            Fail(KernelRule::Syntax, Cur().line, "セマンティクスは使えない");
        }
        if (IsPunct("=")) {
            Advance();
            s->expr = ParseExpr();
        }
        return s;
    }

    // 代入・増減・式文。末尾の ';' は読まない
    StmtPtr ParseSimpleStatement()
    {
        const int line = Cur().line;
        if (IsPunct("++") || IsPunct("--")) {
            auto s = NewStmt(StmtKind::IncDec, line);
            s->op = Cur().text;
            s->prefix = true;
            Advance();
            s->expr = ParseUnary();
            return s;
        }
        // 文の左辺の末尾に限り、postfix の ++ -- を式の外で受け取る (式の途中は ParsePostfix が弾く)
        stmtIncDecDepth_ = exprDepth_ + 1;
        ExprPtr lhs = ParseExpr();
        stmtIncDecDepth_ = -1;
        if (Cur().kind == Token::Kind::Punct && IsAssignOp(Cur().text)) {
            auto s = NewStmt(StmtKind::Assign, line);
            s->op = Cur().text;
            Advance();
            s->lhs = std::move(lhs);
            s->expr = ParseExpr();
            if (Cur().kind == Token::Kind::Punct && IsAssignOp(Cur().text)) {
                Fail(KernelRule::AssignInExpr, Cur().line, "連続代入 (a = b = c) は式の中の代入になるので使えない");
            }
            return s;
        }
        if (IsPunct("++") || IsPunct("--")) {
            if (lhs->kind != ExprKind::Ident && lhs->kind != ExprKind::Member && lhs->kind != ExprKind::Index) {
                Fail(KernelRule::AssignInExpr, Cur().line, "式の中で ++ -- は使えない。文として分けて書く");
            }
            auto s = NewStmt(StmtKind::IncDec, line);
            s->op = Cur().text;
            Advance();
            s->expr = std::move(lhs);
            return s;
        }
        auto s = NewStmt(StmtKind::ExprStmt, line);
        s->expr = std::move(lhs);
        return s;
    }

    // embedded: if / for / while の本体として読むとき (宣言は C# で書けないので禁止)
    StmtPtr ParseStatement(bool embedded)
    {
        NestGuard guard(*this);
        const Token& t = Cur();
        if (t.kind == Token::Kind::Directive) {
            Fail(KernelRule::Preprocessor, t.line, "関数の中に前処理は書けない ('" + t.text + "')");
        }
        if (t.kind == Token::Kind::Punct) {
            if (t.text == "{") {
                return ParseBlock();
            }
            if (t.text == "[") {
                Fail(KernelRule::Attribute, t.line, "属性 ([unroll] など) は使えない (C++ では通らない)");
            }
            if (t.text == ";") {
                Fail(KernelRule::Syntax, t.line, "空文は使えない");
            }
        }
        if (t.kind == Token::Kind::Ident) {
            const std::string& w = t.text;
            if (w == "if") {
                return ParseIf();
            }
            if (w == "for") {
                return ParseFor();
            }
            if (w == "while") {
                return ParseWhile();
            }
            if (w == "break" || w == "continue") {
                auto s = NewStmt(w == "break" ? StmtKind::Break : StmtKind::Continue, t.line);
                Advance();
                ExpectPunct(";");
                return s;
            }
            if (w == "return") {
                auto s = NewStmt(StmtKind::Return, t.line);
                Advance();
                if (!IsPunct(";")) {
                    s->expr = ParseExpr();
                }
                ExpectPunct(";");
                return s;
            }
            if (w == "else") {
                Fail(KernelRule::Syntax, t.line, "対応する if の無い else");
            }
            if (IsControlKeyword(w) || IsBarrierOrAtomic(w)) {
                Fail(KernelRule::Keyword, t.line, "'" + w + "' はサブセットに無い");
            }
            if (IsUnsupportedTypeName(w) && Peek().kind == Token::Kind::Ident) {
                Fail(KernelRule::UnsupportedType, t.line, "型 '" + w + "' はサブセットに無い");
            }
            if (AtDeclaration()) {
                if (embedded) {
                    Fail(KernelRule::Syntax, t.line, "if / for / while の直下に宣言は置けない ({ } で囲む)");
                }
                StmtPtr d = ParseDeclaration();
                ExpectPunct(";");
                return d;
            }
        }
        StmtPtr s = ParseSimpleStatement();
        ExpectPunct(";");
        return s;
    }

    StmtPtr ParseIf()
    {
        auto s = NewStmt(StmtKind::If, Cur().line);
        Advance();
        ExpectPunct("(");
        s->expr = ParseExpr();
        ExpectPunct(")");
        s->thenStmt = ParseStatement(true);
        if (IsIdent("else")) {
            Advance();
            s->elseStmt = ParseStatement(true);
        }
        return s;
    }

    StmtPtr ParseWhile()
    {
        auto s = NewStmt(StmtKind::While, Cur().line);
        Advance();
        ExpectPunct("(");
        s->expr = ParseExpr();
        ExpectPunct(")");
        s->thenStmt = ParseStatement(true);
        return s;
    }

    StmtPtr ParseFor()
    {
        auto s = NewStmt(StmtKind::For, Cur().line);
        Advance();
        ExpectPunct("(");
        if (!IsPunct(";")) {
            s->init = AtDeclaration() ? ParseDeclaration() : ParseSimpleStatement();
        }
        ExpectPunct(";");
        if (!IsPunct(";")) {
            s->expr = ParseExpr();
        }
        ExpectPunct(";");
        if (!IsPunct(")")) {
            s->step = ParseSimpleStatement();
        }
        ExpectPunct(")");
        s->thenStmt = ParseStatement(true);
        return s;
    }

    // ---- 式 ----

    ExprPtr NewExpr(ExprKind kind, int line, std::string text = {}) const
    {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->line = line;
        e->text = std::move(text);
        return e;
    }

    ExprPtr ParseExpr()
    {
        ++exprDepth_;
        ExprPtr e = ParseTernary();
        --exprDepth_;
        return e;
    }

    ExprPtr ParseTernary()
    {
        NestGuard guard(*this);
        ExprPtr cond = ParseBinary(1);
        if (IsPunct("?")) {
            auto e = NewExpr(ExprKind::Ternary, Cur().line);
            Advance();
            ExprPtr a = ParseExpr();
            ExpectPunct(":");
            ExprPtr b = ParseExpr();
            e->args.push_back(std::move(cond));
            e->args.push_back(std::move(a));
            e->args.push_back(std::move(b));
            return e;
        }
        return cond;
    }

    ExprPtr ParseBinary(int minPrec)
    {
        ExprPtr lhs = ParseUnary();
        int chain = 0;
        while (Cur().kind == Token::Kind::Punct) {
            const int prec = BinaryPrecedence(Cur().text);
            if (prec == 0 || prec < minPrec) {
                break;
            }
            if (++chain > kMaxBinaryChain) {
                Fail(KernelRule::Syntax, Cur().line, "1 つの式に演算子が多すぎる (上限 500)");
            }
            auto e = NewExpr(ExprKind::Binary, Cur().line, Cur().text);
            Advance();
            ExprPtr rhs = ParseBinary(prec + 1);
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr ParseUnary()
    {
        NestGuard guard(*this);
        const Token& t = Cur();
        if (t.kind == Token::Kind::Punct) {
            if (t.text == "-" || t.text == "!" || t.text == "~") {
                auto e = NewExpr(ExprKind::Unary, t.line, t.text);
                Advance();
                e->args.push_back(ParseUnary());
                return e;
            }
            if (t.text == "+") {
                Fail(KernelRule::Syntax, t.line, "単項 + は使えない");
            }
            if (t.text == "++" || t.text == "--") {
                Fail(KernelRule::AssignInExpr, t.line, "式の中で ++ -- は使えない。文として分けて書く");
            }
            if (t.text == "(" && Peek().kind == Token::Kind::Ident && PeekIsPunct(2, ")")) {
                const std::string& tn = Peek().text;
                KType parsed;
                if (ParseBuiltinTypeName(tn, &parsed) || IsUnsupportedTypeName(tn)) {
                    if (parsed.kind == KType::Kind::Scalar) {
                        auto e = NewExpr(ExprKind::Cast, t.line, tn);
                        Advance();
                        Advance();
                        Advance();
                        e->args.push_back(ParseUnary());
                        return e;
                    }
                    Fail(KernelRule::TypeMismatch, t.line,
                         "キャストできるのは float / int / uint のスカラーだけ ('(" + tn + ")')");
                }
            }
        }
        return ParsePostfix();
    }

    ExprPtr ParsePostfix()
    {
        ExprPtr e = ParsePrimary();
        while (true) {
            if (IsPunct(".")) {
                const int line = Cur().line;
                Advance();
                auto m = NewExpr(ExprKind::Member, line, ExpectIdent("メンバ名"));
                m->args.push_back(std::move(e));
                e = std::move(m);
            } else if (IsPunct("[")) {
                const int line = Cur().line;
                Advance();
                auto ix = NewExpr(ExprKind::Index, line);
                ix->args.push_back(std::move(e));
                ix->args.push_back(ParseExpr());
                ExpectPunct("]");
                e = std::move(ix);
            } else if (IsPunct("++") || IsPunct("--")) {
                if (exprDepth_ == stmtIncDecDepth_) {
                    break; // 文の末尾の i++; は ParseSimpleStatement が受け取る
                }
                Fail(KernelRule::AssignInExpr, Cur().line, "式の中で ++ -- は使えない。文として分けて書く");
            } else {
                break;
            }
        }
        return e;
    }

    ExprPtr ParsePrimary()
    {
        const Token& t = Cur();
        switch (t.kind) {
        case Token::Kind::IntLit: {
            auto e = NewExpr(ExprKind::IntLit, t.line, t.text);
            e->intValue = t.value;
            e->boolValue = t.isUint;
            Advance();
            return e;
        }
        case Token::Kind::FloatLit: {
            auto e = NewExpr(ExprKind::FloatLit, t.line, t.text);
            Advance();
            return e;
        }
        case Token::Kind::Ident: {
            const int line = t.line;
            const std::string name = t.text;
            if (name == "true" || name == "false") {
                auto e = NewExpr(ExprKind::BoolLit, line, name);
                e->boolValue = name == "true";
                Advance();
                return e;
            }
            if (IsControlKeyword(name) || IsBarrierOrAtomic(name)) {
                Fail(KernelRule::Keyword, line, "'" + name + "' はサブセットに無い");
            }
            Advance();
            if (IsPunct("(")) {
                Advance();
                auto e = NewExpr(ExprKind::Call, line, name);
                if (!IsPunct(")")) {
                    while (true) {
                        e->args.push_back(ParseExpr());
                        if (IsPunct(",")) {
                            Advance();
                            continue;
                        }
                        break;
                    }
                }
                ExpectPunct(")");
                return e;
            }
            return NewExpr(ExprKind::Ident, line, name);
        }
        case Token::Kind::Punct:
            if (t.text == "(") {
                Advance();
                ExprPtr e = ParseExpr();
                ExpectPunct(")");
                return e;
            }
            break;
        case Token::Kind::Directive:
            Fail(KernelRule::Preprocessor, t.line, "式の途中に前処理は書けない");
        default:
            break;
        }
        FailExpected("式");
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    int exprDepth_ = 0;
    int nest_ = 0;
    int stmtIncDecDepth_ = -1;
    const DiagSink& sink_;
    KernelProgram& prog_;
    std::set<std::string> structNames_;
};

} // namespace

bool ParseKernelSource(std::string_view text, const DiagSink& sink, KernelProgram& out)
{
    try {
        Lexer lexer(text, sink);
        Parser parser(lexer.Run(), sink, out);
        parser.ParseProgram();
        return true;
    } catch (const ParseAbort&) {
        return false;
    }
}

} // namespace mye::kernel::lint
