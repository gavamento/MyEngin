//====================================================================================
//                          KernelLintSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル検査器と C# への変換器の回帰テスト
//====================================================================================
#include "Engine/Engine/Compute/KernelLintSelfTest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Compute/KernelCSharp.h"
#include "Engine/Engine/Compute/KernelHash.h"
#include "Engine/Engine/Compute/KernelLint.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

namespace fs = std::filesystem;
using kernel::KernelRule;

int g_fails = 0;

void Check(bool cond, const std::string& msg)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", msg.c_str());
    } else {
        MYE_LOG_ERROR("  FAIL: %s", msg.c_str());
        ++g_fails;
    }
}

std::string Join(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& l : lines) {
        s += l;
        s += "\n";
    }
    return s;
}

// ---- 負例 ----
//
// 全体の形 (行番号は 1 始まり):
//   1: #include            2: KERNEL_PARAMS         3..: pre      次: KERNEL(...)   次: {   本体...   }
// pre は KERNEL の前に置く struct / 関数。body の何行目で弾かれるべきかを n で指定する

struct NegCase {
    std::string name;
    KernelRule rule;
    int line;
    std::string source;
};

constexpr const char* kInclude = "#include \"kernel_common.hlsli\"";
constexpr const char* kParams = "KERNEL_PARAMS(NegParams) { float a; };";
constexpr const char* kHeader = "KERNEL(Neg, NegParams, RW(float, y) RO(float, x))";

NegCase Neg(const char* name, KernelRule rule, std::vector<std::string> body, int n, std::vector<std::string> pre = {})
{
    std::vector<std::string> lines = { kInclude, kParams };
    lines.insert(lines.end(), pre.begin(), pre.end());
    lines.push_back(kHeader);
    lines.push_back("{");
    const int bodyStart = static_cast<int>(lines.size()) + 1;
    lines.insert(lines.end(), body.begin(), body.end());
    lines.push_back("}");
    return NegCase{ name, rule, bodyStart + n - 1, Join(lines) };
}

// pre の n 行目で弾かれるべきケース
NegCase NegPre(const char* name, KernelRule rule, std::vector<std::string> pre, int n)
{
    NegCase c = Neg(name, rule, { "y[id] = x[id];" }, 1, std::move(pre));
    c.line = 2 + n;
    return c;
}

std::vector<NegCase> BuildNegativeCases()
{
    std::vector<NegCase> c;
    // KL001 syntax
    c.push_back(Neg("セミコロン無し", KernelRule::Syntax, { "float v = 1.0f", "y[id] = v;" }, 2));
    c.push_back(Neg("単項 +", KernelRule::Syntax, { "float v = +1.0f;", "y[id] = v;" }, 1));
    c.push_back(Neg("関数形式のキャスト", KernelRule::Syntax, { "float v = float(1);", "y[id] = v;" }, 1));
    c.push_back(Neg("ループの外の break", KernelRule::Syntax, { "break;" }, 1));
    c.push_back(Neg("効果の無い式文", KernelRule::Syntax, { "x[0u];" }, 1));
    c.push_back(Neg("if の直下の宣言", KernelRule::Syntax, { "if (true) float v = 1.0f;" }, 1));
    c.push_back(Neg("対応する if の無い else", KernelRule::Syntax, { "else { }" }, 1));
    c.push_back(Neg("サブセット外の文字", KernelRule::Syntax, { "float v = 1.0f @ 2.0f;" }, 1));
    c.push_back(NegPre("void 関数", KernelRule::Syntax, { "void F(float a) { }" }, 1));
    c.push_back(Neg("1 宣言 1 変数", KernelRule::Syntax, { "float a = 1.0f, b = 2.0f;" }, 1));
    c.push_back(Neg("KERNEL の本体の値付き return", KernelRule::Syntax, { "return 1.0f;" }, 1));
    c.push_back(Neg("深すぎる括弧", KernelRule::Syntax, { "float v = " + std::string(150, '(') + "1.0f" + std::string(150, ')') + ";" }, 1));
    c.push_back(Neg("長すぎる式", KernelRule::Syntax, { [] {
                        std::string e = "float v = 1.0f";
                        for (int i = 0; i < 600; ++i) {
                            e += " + 1.0f";
                        }
                        return e + ";";
                    }() }, 1));
    // KL002 preprocessor
    c.push_back(NegPre("#define", KernelRule::Preprocessor, { "#define K 1.0f" }, 1));
    c.push_back(NegPre("別の #include", KernelRule::Preprocessor, { "#include \"other.hlsli\"" }, 1));
    c.push_back(Neg("関数の中の前処理", KernelRule::Preprocessor, { "#pragma once" }, 1));
    // KL003 unsupported-type
    c.push_back(Neg("bool 変数", KernelRule::UnsupportedType, { "bool b = true;" }, 1));
    c.push_back(Neg("double", KernelRule::UnsupportedType, { "double d = 1;" }, 1));
    c.push_back(Neg("行列", KernelRule::UnsupportedType, { "float3x3 m;" }, 1));
    c.push_back(Neg("half", KernelRule::UnsupportedType, { "half h;" }, 1));
    c.push_back(NegPre("bool メンバ", KernelRule::UnsupportedType, { "struct S { bool b; };" }, 1));
    // KL004 array
    c.push_back(Neg("配列変数", KernelRule::Array, { "float arr[4];" }, 1));
    c.push_back(NegPre("配列メンバ", KernelRule::Array, { "struct S { float a[2]; };" }, 1));
    c.push_back(Neg("配列でない物への添字", KernelRule::Array, { "float v = 1.0f;", "float w = v[0];" }, 2));
    // KL005 swizzle
    c.push_back(Neg("スウィズル", KernelRule::Swizzle, { "float3 v = float3(1.0f, 2.0f, 3.0f);", "float2 w = v.xy;" }, 2));
    c.push_back(Neg("成分の無い次元", KernelRule::Swizzle, { "float2 q = float2(1.0f, 2.0f);", "float z = q.z;" }, 2));
    // KL006 float-literal
    c.push_back(Neg("f の無い小数", KernelRule::FloatLiteral, { "y[id] = x[id] * 2.0;" }, 1));
    c.push_back(Neg("1f", KernelRule::FloatLiteral, { "y[id] = 1f;" }, 1));
    c.push_back(Neg("1.0h", KernelRule::FloatLiteral, { "y[id] = 1.0h;" }, 1));
    // KL007 int-literal
    c.push_back(Neg("8 進", KernelRule::IntLiteral, { "int v = 010;" }, 1));
    c.push_back(Neg("int を超える", KernelRule::IntLiteral, { "int v = 3000000000;" }, 1));
    c.push_back(Neg("uint を超える", KernelRule::IntLiteral, { "uint v = 5000000000u;" }, 1));
    c.push_back(Neg("接尾辞 l", KernelRule::IntLiteral, { "int v = 1l;" }, 1));
    // KL008 type-mismatch
    c.push_back(Neg("float -> int", KernelRule::TypeMismatch, { "int i = 1.5f;" }, 1));
    c.push_back(Neg("int -> uint", KernelRule::TypeMismatch, { "uint u = 1;" }, 1));
    c.push_back(Neg("int と uint の算術", KernelRule::TypeMismatch, { "int i = 2;", "uint u = 3u;", "u = u + i;" }, 3));
    c.push_back(Neg("uint -> float", KernelRule::TypeMismatch, { "float f = id;" }, 1));
    c.push_back(Neg("uint -> int", KernelRule::TypeMismatch, { "int k = id;" }, 1));
    c.push_back(Neg("float と uint の算術", KernelRule::TypeMismatch, { "y[id] = x[id] + 1u;" }, 1));
    c.push_back(Neg("スカラー -> ベクトル", KernelRule::TypeMismatch, { "float3 v = 1.0f;" }, 1));
    c.push_back(Neg("RO の添字が int", KernelRule::TypeMismatch, { "int i = 0;", "y[id] = x[i];" }, 2));
    c.push_back(Neg("uint の単項マイナス", KernelRule::TypeMismatch, { "uint u = 1u;", "uint w = -u;" }, 2));
    c.push_back(Neg("1 引数のコンストラクタ (FXC が受けない)", KernelRule::TypeMismatch, { "float3 v = float3(1.0f);" }, 1));
    c.push_back(Neg("float -> int の C 形式のキャスト", KernelRule::TypeMismatch, { "int i = (int)x[id];" }, 1));
    c.push_back(Neg("float -> uint の C 形式のキャスト", KernelRule::TypeMismatch, { "uint u = (uint)1.5f;" }, 1));
    c.push_back(Neg("FloatToInt の引数の型", KernelRule::Builtin, { "int i = FloatToInt(1);" }, 1));
    c.push_back(Neg("初期化子の無い宣言", KernelRule::Uninitialized, { "float v;", "y[id] = x[id];" }, 1));
    c.push_back(Neg("初期化子の無い struct の宣言", KernelRule::Uninitialized, { "S e;", "y[id] = x[id];" }, 1, { "struct S { float a; };" }));
    c.push_back(Neg("初期化子の無い const", KernelRule::Uninitialized, { "const float v;" }, 1));
    c.push_back(Neg("ベクトルのキャスト", KernelRule::TypeMismatch, { "float3 v = (float3)1.0f;" }, 1));
    c.push_back(Neg("関数の引数の縮小", KernelRule::TypeMismatch, { "float v = F(1.5f);" }, 1,
                    { "float F(int a)", "{", "    return (float)a;", "}" }));
    c.push_back(Neg("複合代入の縮小", KernelRule::TypeMismatch, { "int i = 1;", "i += 1.5f;" }, 2));
    c.push_back(Neg("三項の型不一致", KernelRule::TypeMismatch, { "float v = true ? 1.0f : 2;" }, 1));
    // KL009 int-division
    c.push_back(Neg("整数の / に変数", KernelRule::IntDivision, { "int i = 4;", "int j = 8 / i;" }, 2));
    c.push_back(Neg("整数の / 0", KernelRule::IntDivision, { "int j = 8 / 0;" }, 1));
    c.push_back(Neg("整数の % 0u", KernelRule::IntDivision, { "uint q = 9u % 0u;" }, 1));
    c.push_back(Neg("整数の /= に変数", KernelRule::IntDivision, { "int i = 4;", "int j = 8;", "j /= i;" }, 3));
    // KL010 vector-compare
    c.push_back(Neg("ベクトルの ==", KernelRule::VectorCompare, { "float3 a = float3(1.0f, 2.0f, 3.0f);", "if (a == a) { }" }, 2));
    c.push_back(Neg("ベクトルの三項演算子", KernelRule::VectorCompare, { "float3 a = float3(1.0f, 2.0f, 3.0f);", "float3 b = true ? a : a;" }, 2));
    // KL011 builtin
    c.push_back(Neg("pow", KernelRule::Builtin, { "y[id] = pow(x[id], 2.0f);" }, 1));
    c.push_back(Neg("saturate(int)", KernelRule::Builtin, { "float v = saturate(1);" }, 1));
    c.push_back(Neg("abs(uint)", KernelRule::Builtin, { "uint v = abs(1u);" }, 1));
    c.push_back(Neg("min(float, int)", KernelRule::Builtin, { "float v = min(x[id], 1);" }, 1));
    c.push_back(Neg("引数の個数", KernelRule::Builtin, { "float v = lerp(x[id], 1.0f);" }, 1));
    // KL012 attribute
    c.push_back(Neg("[unroll]", KernelRule::Attribute, { "[unroll]", "for (int i = 0; i < 4; ++i) { }" }, 1));
    // KL013 out-param
    c.push_back(NegPre("out 引数", KernelRule::OutParam, { "float F(float a, out float b) { b = a; return a; }" }, 1));
    c.push_back(NegPre("inout 引数", KernelRule::OutParam, { "float F(inout float a) { return a; }" }, 1));
    // KL014 assign-in-expr
    c.push_back(Neg("式の中の代入", KernelRule::AssignInExpr, { "int i = 0;", "int j = (i = 3);" }, 2));
    c.push_back(Neg("式の中の i++", KernelRule::AssignInExpr, { "int i = 0;", "int j = i++;" }, 2));
    c.push_back(Neg("式の中の ++i", KernelRule::AssignInExpr, { "int i = 0;", "int j = ++i;" }, 2));
    c.push_back(Neg("連続代入", KernelRule::AssignInExpr, { "float a = 0.0f;", "float b = 0.0f;", "a = b = 1.0f;" }, 3));
    c.push_back(Neg("&& の項の代入", KernelRule::AssignInExpr, { "int i = 0;", "if (i > 0 && (i = 1) > 0) { }" }, 2));
    // KL015 reserved
    c.push_back(Neg("min を変数名に", KernelRule::Reserved, { "int min = 1;" }, 1));
    c.push_back(Neg("params を変数名に", KernelRule::Reserved, { "int params = 1;" }, 1));
    c.push_back(NegPre("sin を関数名に", KernelRule::Reserved, { "float sin(float a) { return a; }" }, 1));
    c.push_back(Neg("__ で始まる名前", KernelRule::Reserved, { "int __k = 1;" }, 1));
    c.push_back(Neg("C++ の予約語 (auto)", KernelRule::Reserved, { "int auto = 1;" }, 1));
    c.push_back(Neg("C++ の予約語 (this)", KernelRule::Reserved, { "int this = 1;" }, 1));
    c.push_back(Neg("HLSL の予約語 (default)", KernelRule::Reserved, { "int default = 1;" }, 1));
    // KL016 rw-index
    c.push_back(Neg("id + 1u への書き込み", KernelRule::RwIndex, { "y[id + 1u] = 1.0f;" }, 1));
    c.push_back(Neg("定数への書き込み", KernelRule::RwIndex, { "y[0u] = x[id];" }, 1));
    c.push_back(Neg("RW の読み取りも id だけ", KernelRule::RwIndex, { "float v = y[id + 1u];" }, 1));
    // KL017 scalar-mix
    c.push_back(Neg("dot のスカラー混在", KernelRule::ScalarMix, { "float d = dot(float3(1.0f, 2.0f, 3.0f), 2.0f);" }, 1));
    c.push_back(Neg("min(スカラー, ベクトル)", KernelRule::ScalarMix, { "float3 d = min(1.0f, float3(1.0f, 2.0f, 3.0f));" }, 1));
    // KL018 shadow
    c.push_back(Neg("入れ子で同名", KernelRule::Shadow, { "float t = 1.0f;", "{", "float t = 2.0f;", "}" }, 3));
    c.push_back(Neg("同じスコープで再宣言", KernelRule::Shadow, { "float u = 1.0f;", "float u = 2.0f;" }, 2));
    c.push_back(Neg("後から宣言される外側の名前", KernelRule::Shadow, { "{", "float t = 1.0f;", "}", "float t = 2.0f;" }, 2));
    c.push_back(Neg("関数名と同じ変数", KernelRule::Shadow, { "float F = 1.0f;" }, 1, { "float F(float a)", "{", "    return a;", "}" }));
    c.push_back(NegPre("引数名の重複", KernelRule::Shadow, { "float F(float a, float a) { return a; }" }, 1));
    // KL019 unknown-name
    c.push_back(Neg("未宣言の変数", KernelRule::UnknownName, { "y[id] = foo;" }, 1));
    c.push_back(Neg("未宣言の関数", KernelRule::UnknownName, { "y[id] = Foo(1.0f);" }, 1));
    c.push_back(NegPre("再帰", KernelRule::UnknownName, { "float F(float a) { return F(a); }" }, 1));
    c.push_back(NegPre("関数の中の id", KernelRule::UnknownName, { "float F(float a) { return a + (float)id; }" }, 1));
    c.push_back(NegPre("関数の中の params", KernelRule::UnknownName, { "float F(float a) { return a + params.a; }" }, 1));
    c.push_back(Neg("未定義の型", KernelRule::UnknownName, { "Foo f;" }, 1));
    c.push_back(Neg("宣言より前の使用", KernelRule::UnknownName, { "float v = w;", "float w = 1.0f;" }, 1));
    // KL021 keyword
    c.push_back(Neg("Interlocked", KernelRule::Keyword, { "InterlockedAdd(1, 2);" }, 1));
    c.push_back(NegPre("groupshared", KernelRule::Keyword, { "groupshared float s;" }, 1));
    c.push_back(Neg("switch", KernelRule::Keyword, { "switch (1) { }" }, 1));
    c.push_back(Neg("discard", KernelRule::Keyword, { "discard;" }, 1));
    c.push_back(Neg("barrier", KernelRule::Keyword, { "GroupMemoryBarrierWithGroupSync();" }, 1));
    // KL022 shift
    c.push_back(Neg("シフト量が変数", KernelRule::Shift, { "int k = 2;", "uint s = 1u << k;" }, 2));
    c.push_back(Neg("シフト量が範囲外", KernelRule::Shift, { "uint s = 1u << 40;" }, 1));
    c.push_back(Neg("ベクトルのシフト", KernelRule::Shift, { "int2 v = int2(1, 2);", "int2 w = v << 1;" }, 2));
    // KL023 missing-return
    c.push_back(NegPre("return の無い経路", KernelRule::MissingReturn, { "float F(float a)", "{", "    if (a > 0.0f)", "    {", "        return a;", "    }", "}" }, 1));
    // KL024 read-only
    c.push_back(Neg("RO への書き込み", KernelRule::ReadOnly, { "x[id] = 1.0f;" }, 1));
    c.push_back(Neg("params への書き込み", KernelRule::ReadOnly, { "params.a = 1.0f;" }, 1));
    c.push_back(Neg("const への代入", KernelRule::ReadOnly, { "const float c = 1.0f;", "c = 2.0f;" }, 2));
    c.push_back(Neg("id への代入", KernelRule::ReadOnly, { "id = 3u;" }, 1));
    c.push_back(Neg("RO の要素のメンバ", KernelRule::ReadOnly, { "x[id] += 1.0f;" }, 1));
    // KL025 condition
    c.push_back(Neg("uint を条件に", KernelRule::Condition, { "if (id) { }" }, 1));
    c.push_back(Neg("while (1)", KernelRule::Condition, { "while (1) { break; }" }, 1));
    // KL026 global
    c.push_back(NegPre("グローバル変数", KernelRule::Global, { "float gk = 1.0f;" }, 1));
    c.push_back(NegPre("グローバル const", KernelRule::Global, { "const float gk = 1.0f;" }, 1));
    // KL027 struct-shape
    c.push_back(NegPre("入れ子の struct", KernelRule::StructShape, { "struct Inner { float a; };", "struct Outer { Inner i; };" }, 2));
    c.push_back(NegPre("空の struct", KernelRule::StructShape, { "struct Empty { };" }, 1));
    return c;
}

// KERNEL の形 (KL020)。ファイル名は Neg.kernel.hlsl で検査する
std::vector<NegCase> BuildKernelDeclCases()
{
    std::vector<NegCase> c;
    c.push_back({ "RW/RO の間のコンマ", KernelRule::KernelDecl, 3,
                  Join({ kInclude, kParams, "KERNEL(Neg, NegParams, RW(float, y), RO(float, x))", "{", "}" }) });
    c.push_back({ "KERNEL 名がファイル名と違う", KernelRule::KernelDecl, 3,
                  Join({ kInclude, kParams, "KERNEL(Other, Neg, RW(float, y))", "{", "}" }) });
    c.push_back({ "第 1 引数が RO", KernelRule::KernelDecl, 3,
                  Join({ kInclude, kParams, "KERNEL(Neg, NegParams, RO(float, x) RW(float, y))", "{", "}" }) });
    c.push_back({ "バッファが 0 本", KernelRule::KernelDecl, 3,
                  Join({ kInclude, kParams, "KERNEL(Neg, NegParams, )", "{", "}" }) });
    c.push_back({ "params 型が KERNEL_PARAMS と違う", KernelRule::KernelDecl, 3,
                  Join({ kInclude, kParams, "KERNEL(Neg, Other, RW(float, y))", "{", "}" }) });
    c.push_back({ "KERNEL の後ろに宣言", KernelRule::KernelDecl, 6,
                  Join({ kInclude, kParams, "KERNEL(Neg, NegParams, RW(float, y))", "{", "}", "float F(float a) { return a; }" }) });
    c.push_back({ "include 無し", KernelRule::Preprocessor, 1,
                  Join({ kParams, "KERNEL(Neg, NegParams, RW(float, y))", "{", "}" }) });
    c.push_back({ "KERNEL_PARAMS が無い", KernelRule::KernelDecl, 2,
                  Join({ kInclude, "KERNEL(Neg, NegParams, RW(float, y))", "{", "}" }) });
    return c;
}

// ---- 正例: サブセットの構文と関数を一通り使う (GPU / C++ でも通る書き方) ----

constexpr const char* kLintAllSource = R"KSRC(// 検査器の正例: ホワイトリストの関数とサブセットの構文を一通り使う
#include "kernel_common.hlsli"

struct LintAllElem
{
    float3 pos;
    float val;
    int2 cell;
    uint tag;
};

KERNEL_PARAMS(LintAllParams)
{
    float time;
    float3 axis;
    uint seed;
    int count;
};

float LintAllSum(float a, int n)
{
    float acc = 0.0f;
    for (int k = 0; k < n; ++k)
    {
        acc += a * (float)k;
    }
    return acc;
}

float3 LintAllNorm(float3 v)
{
    if (dot(v, v) > 0.0f)
    {
        return normalize(v);
    }
    return float3(0.0f, 1.0f, 0.0f);
}

int LintAllPick(int i)
{
    return i < 0 ? -i : (i > 9 ? 9 : i);
}

KERNEL(LintAll, LintAllParams, RW(LintAllElem, outv) RO(float4, src))
{
    const float4 s = src[id];
    const float2 a2 = float2(s.x, s.y);
    const float3 a3 = float3(a2, s.z);
    const float4 a4 = float4(a3, s.w);
    float f = 0.0f;
    f += min(s.x, s.y) + max(s.y, s.z) + clamp(s.z, -1.0f, 1.0f);
    f += abs(s.x) + saturate(s.y) + lerp(s.x, s.y, 0.25f) + step(0.5f, s.z) + smoothstep(0.0f, 1.0f, s.w);
    f += floor(s.x) + ceil(s.y) + frac(s.z) + sqrt(abs(s.w)) + rsqrt(abs(s.x) + 1.0f) + rcp(abs(s.y) + 1.0f);
    f += dot(a3, a3) + length(a4) + distance(a2, float2(1.0f, 2.0f)) + sin(s.x) + cos(s.y);
    f += (float)sign(s.z) + (float)abs(-3) + (float)sign(-2);
    f += LintAllSum(f, 3) + (float)LintAllPick(params.count);

    const float3 c = cross(a3, params.axis);
    float3 v = LintAllNorm(c + 0.001f);
    v = min(v, 2.0f) + max(v, c) - clamp(v, 0.0f, 1.0f);
    v = lerp(v, c, 0.5f) + lerp(v, c, c) + step(0.5f, v) * smoothstep(0.0f, 1.0f, v);
    v = abs(v) + saturate(v) + floor(v) + ceil(v) + frac(v) + sqrt(abs(v)) + rsqrt(abs(v) + 1.0f) + rcp(abs(v) + 1.0f);
    v = v + sin(v) + cos(v);
    v = -v * 2.0f / 3.0f;
    v.y = v.x;
    f += (float)sign(v).x;
    f += (float)FloatToInt(f * 0.001f);

    int2 q = int2(1, 2) * 3;
    q += int2(1, 1);
    q = q / 2;
    q = q % 3;
    const int2 m = min(q, 5) + max(q, int2(0, 0)) + clamp(q, 0, 9) + abs(q);
    uint3 u3 = uint3(1u, 2u, 3u);
    u3 = (u3 & 255u) | uint3(1u, 1u, 1u);
    u3 = u3 ^ 15u;
    uint h = HashU32(params.seed ^ id);
    h = ((h >> 3) & 0xFFu) | (1u << 2);
    h = ~h;

    int w = 0;
    while (w < 8)
    {
        w += 1;
        if (w == 2)
        {
            continue;
        }
        if (w > 5 || (w < 0 && !(f > 1.0f)))
        {
            break;
        }
    }
    w++;
    --w;
    int sgn = 0;
    if (f > 0.5f)
    {
        sgn = 1;
    }
    else if (f < -0.5f)
    {
        sgn = -1;
    }
    else
    {
        sgn = 0;
    }
    const float tf = f > 0.0f ? f : -f;
    const uint b = FloatToUint(abs(f)) + (uint)w;

    outv[id].pos = v;
    outv[id].val = tf + HashToUnitFloat(h) + (float)m.x + (float)u3.y + (float)sgn + (float)b + a3.x;
    outv[id].cell = q;
    outv[id].tag = h ^ u3.z;
    outv[id].tag = outv[id].tag + 1u;
}
)KSRC";

// ---- golden: 自己テスト用カーネルの変換結果 (SourceHash は @HASH@ に置き換えて比べる) ----

constexpr const char* kGoldenSaxpy = R"KGOLD(namespace MyeKernels.SelfTestSaxpy
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct SelfTestSaxpyParams
    {
        public float a;
    }

    public static unsafe class CpuKernel
    {
        public const string KernelName = "SelfTestSaxpy";
        public const ulong SourceHash = @HASH@;

        public static void Body(KernelId id, in SelfTestSaxpyParams @params, RWView<float> y, ROView<float> x)
        {
            unchecked
            {
                y[id] = (@params.a * x[id.Value]) + y[id];
            }
        }

        [UnmanagedCallersOnly]
        public static void Range(uint begin, uint end, void* paramsPtr, void** buffers, uint* counts)
        {
            ref readonly SelfTestSaxpyParams prm = ref *(SelfTestSaxpyParams*)paramsPtr;
            RWView<float> buf0 = new RWView<float>((float*)buffers[0], counts[0]);
            ROView<float> buf1 = new ROView<float>((float*)buffers[1], counts[1]);
            for (uint i = begin; i < end; ++i)
            {
                Body(new KernelId(i), in prm, buf0, buf1);
            }
        }
    }
}
)KGOLD";
constexpr const char* kGoldenMix = R"KGOLD(namespace MyeKernels.SelfTestMix
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct SelfTestMixElem
    {
        public float3 pos;
        public float val;
        public uint tag;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct SelfTestMixProbe
    {
        public float3 dir;
        public float weight;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct SelfTestMixParams
    {
        public float time;
        public float3 axis;
        public uint seed;
    }

    public static unsafe class CpuKernel
    {
        public const string KernelName = "SelfTestMix";
        public const ulong SourceHash = @HASH@;

        private static float SelfTestWave(float t)
        {
            unchecked
            {
                return (sin(t) * 0.5f) + 0.5f;
            }
        }

        public static void Body(KernelId id, in SelfTestMixParams @params, RWView<SelfTestMixElem> outv, ROView<SelfTestMixProbe> probes)
        {
            unchecked
            {
                float jitter = HashToUnitFloat(HashU32(@params.seed ^ id.Value));
                float t = @params.time + (jitter * 3.0f);
                float3 d = new float3(sin(t * 3.0f), cos(t * 0.5f), t + 1.0f);
                float acc = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    SelfTestMixProbe p = probes[id.Value + ((uint)i)];
                    acc = acc + (dot(p.dir, d) * p.weight);
                }
                int steps = 0;
                float walk = 0.0f;
                while (steps < 16)
                {
                    steps = steps + 1;
                    if ((steps % 2) == 0)
                    {
                        continue;
                    }
                    walk = walk + SelfTestWave(t + ((float)steps));
                    if (walk > 3.0f)
                    {
                        break;
                    }
                }
                float3 n = normalize(d);
                float3 c = cross(n, @params.axis);
                outv[id].pos = lerp(n, c, smoothstep(0.0f, 1.0f, saturate(jitter))) * clamp(length(d), 1.0f, 4.0f);
                outv[id].val = ((((acc > 1.0f) ? acc : (-acc)) + abs(walk)) + ((float)sign(c.x))) + min(distance(n, c), 2.0f);
                outv[id].tag = HashU32(id.Value) >> 8;
            }
        }

        [UnmanagedCallersOnly]
        public static void Range(uint begin, uint end, void* paramsPtr, void** buffers, uint* counts)
        {
            ref readonly SelfTestMixParams prm = ref *(SelfTestMixParams*)paramsPtr;
            RWView<SelfTestMixElem> buf0 = new RWView<SelfTestMixElem>((SelfTestMixElem*)buffers[0], counts[0]);
            ROView<SelfTestMixProbe> buf1 = new ROView<SelfTestMixProbe>((SelfTestMixProbe*)buffers[1], counts[1]);
            for (uint i = begin; i < end; ++i)
            {
                Body(new KernelId(i), in prm, buf0, buf1);
            }
        }
    }
}
)KGOLD";

// ---- 補助 ----

std::string StripCr(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c != '\r') {
            r += c;
        }
    }
    return r;
}

std::string HexU64(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llXUL", static_cast<unsigned long long>(v));
    return buf;
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Describe(const std::vector<kernel::KernelDiagnostic>& diags)
{
    std::string s;
    for (const auto& d : diags) {
        s += "\n      " + kernel::FormatKernelDiagnostic(d);
    }
    return s;
}

// 環境変数 MYE_KERNEL_GOLDEN_DUMP のフォルダへ変換結果を書き出す (golden の更新・C# の実走プローブの入力用)。
// SourceHash は @HASH@ に戻す。selftest の一時フォルダは終了時に消えるので、保存先は環境変数で指す
void DumpForTools(const std::string& name, const std::string& csharp, const std::string& hashText)
{
    const char* dumpDir = std::getenv("MYE_KERNEL_GOLDEN_DUMP");
    if (!dumpDir) {
        return;
    }
    std::string text = csharp;
    if (!hashText.empty()) {
        for (size_t at = text.find(hashText); at != std::string::npos; at = text.find(hashText, at)) {
            text.replace(at, hashText.size(), "@HASH@");
        }
    }
    std::ofstream(fs::path(dumpDir) / ("mye_kernel_golden_" + name + ".txt"), std::ios::binary) << text;
}

// 開発用: 環境変数 MYE_KERNEL_LINT_FILES (; 区切りのパス) のカーネルを検査し、違反をログに出す。
// 通ったものは MYE_KERNEL_GOLDEN_DUMP のフォルダへ C# を書き出す。合否には影響しない
void LintExternalFiles()
{
    const char* list = std::getenv("MYE_KERNEL_LINT_FILES");
    if (!list) {
        return;
    }
    std::string rest = list;
    while (!rest.empty()) {
        const size_t semi = rest.find(';');
        const fs::path path = rest.substr(0, semi);
        rest = semi == std::string::npos ? std::string() : rest.substr(semi + 1);
        const std::string text = ReadAll(path);
        const kernel::KernelLintResult r = kernel::LintKernelSource(path.filename().string(), text);
        for (const auto& d : r.diagnostics) {
            MYE_LOG_ERROR("  [lint] %s", kernel::FormatKernelDiagnostic(d).c_str());
        }
        if (r.Ok()) {
            MYE_LOG_INFO("  [lint] %s: OK", path.filename().string().c_str());
            DumpForTools(r.kernelName, kernel::EmitKernelCSharp(r), HexU64(r.sourceHash));
        }
    }
}

// ---- テスト本体 ----

void TestNegativeCases()
{
    MYE_LOG_INFO("-- 規則ごとの負例 --");
    std::vector<NegCase> cases = BuildNegativeCases();
    const std::vector<NegCase> declCases = BuildKernelDeclCases();
    cases.insert(cases.end(), declCases.begin(), declCases.end());

    bool seen[static_cast<size_t>(KernelRule::Count)] = {};
    for (const NegCase& nc : cases) {
        const kernel::KernelLintResult r = kernel::LintKernelSource("Neg.kernel.hlsl", nc.source);
        bool ruleHit = false;
        bool exact = false;
        for (const auto& d : r.diagnostics) {
            if (d.rule == nc.rule) {
                ruleHit = true;
                exact = exact || d.line == nc.line;
            }
        }
        const bool ok = !r.Ok() && ruleHit && exact && r.diagnostics.front().file == "Neg.kernel.hlsl";
        if (!ok) {
            MYE_LOG_ERROR("    期待: %s %s 行 %d%s", kernel::KernelRuleId(nc.rule), kernel::KernelRuleName(nc.rule), nc.line,
                          Describe(r.diagnostics).c_str());
        }
        Check(ok, std::string(kernel::KernelRuleId(nc.rule)) + " " + nc.name);
        if (ok) {
            seen[static_cast<size_t>(nc.rule)] = true;
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(KernelRule::Count); ++i) {
        const auto rule = static_cast<KernelRule>(i);
        Check(seen[i], std::string("規則 ") + kernel::KernelRuleId(rule) + " [" + kernel::KernelRuleName(rule) + "] に負例がある");
    }
}

void TestPositiveCases(const std::wstring& kernelDir)
{
    MYE_LOG_INFO("-- 正例 --");
    {
        const kernel::KernelLintResult r = kernel::LintKernelSource("LintAll.kernel.hlsl", kLintAllSource);
        if (!r.Ok()) {
            MYE_LOG_ERROR("    %s", Describe(r.diagnostics).c_str());
        }
        Check(r.Ok(), "全機能入りのカーネルが通る");
        const std::string cs = kernel::EmitKernelCSharp(r);
        DumpForTools("LintAll", cs, std::string());
        Check(cs.find("namespace MyeKernels.LintAll") != std::string::npos
                  && cs.find("[UnmanagedCallersOnly]") != std::string::npos,
              "全機能入りのカーネルが C# に変換できる");
    }
    if (kernelDir.empty()) {
        Check(false, "カーネルのフォルダが見つかる");
        return;
    }
    for (const char* name : { "SelfTestSaxpy", "SelfTestMix" }) {
        const fs::path p = fs::path(kernelDir) / (std::string(name) + ".kernel.hlsl");
        const std::string text = ReadAll(p);
        const kernel::KernelLintResult r = kernel::LintKernelSource(p.filename().string(), text);
        if (!r.Ok()) {
            MYE_LOG_ERROR("    %s", Describe(r.diagnostics).c_str());
        }
        Check(!text.empty() && r.Ok(), std::string("実カーネル ") + name + " が通る");
    }
}

void TestGolden(const std::wstring& kernelDir)
{
    MYE_LOG_INFO("-- 変換結果の golden --");
    const struct {
        const char* name;
        const char* golden;
    } kCases[] = { { "SelfTestSaxpy", kGoldenSaxpy }, { "SelfTestMix", kGoldenMix } };
    for (const auto& c : kCases) {
        const fs::path p = fs::path(kernelDir) / (std::string(c.name) + ".kernel.hlsl");
        const std::string text = ReadAll(p);
        const kernel::KernelLintResult r = kernel::LintKernelSource(p.filename().string(), text);
        std::string actual = StripCr(kernel::EmitKernelCSharp(r));
        std::string expected = StripCr(c.golden);
        const std::string placeholder = "@HASH@";
        for (size_t at = expected.find(placeholder); at != std::string::npos; at = expected.find(placeholder, at)) {
            expected.replace(at, placeholder.size(), HexU64(kernel::HashKernelSource(text)));
        }
        if (actual != expected) {
            MYE_LOG_ERROR("    変換結果が golden と違う (MYE_KERNEL_GOLDEN_DUMP を設定すると実際の出力を書き出す)");
        }
        DumpForTools(c.name, actual, HexU64(kernel::HashKernelSource(text)));
        Check(!actual.empty() && actual == expected, std::string(c.name) + " の変換結果が golden と一致する");
    }
}

void TestGeneration(const std::wstring& kernelDir)
{
    MYE_LOG_INFO("-- Kernels.gen.cs の生成とキャッシュ --");
    const std::string good = ReadAll(fs::path(kernelDir) / "SelfTestSaxpy.kernel.hlsl");
    const std::string bad = "#include \"kernel_common.hlsli\"\nKERNEL_PARAMS(BadParams) { float a; };\nKERNEL(Bad, BadParams, RW(float, y))\n{\n    y[id] = 1.0;\n}\n";

    // 並べ替えても同じ出力 / 違反したカーネルは入らず診断が出る
    std::vector<kernel::KernelSourceFile> files = { { "Bad.kernel.hlsl", bad }, { "SelfTestSaxpy.kernel.hlsl", good } };
    const kernel::KernelCSharpBatch a = kernel::BuildKernelsCSharp(files);
    std::reverse(files.begin(), files.end());
    const kernel::KernelCSharpBatch b = kernel::BuildKernelsCSharp(files);
    Check(a.source == b.source, "入力の順序によらず出力が同じ");
    Check(a.emitted.size() == 1 && a.emitted[0] == "SelfTestSaxpy" && a.source.find("MyeKernels.Bad") == std::string::npos,
          "違反したカーネルは出力に入らない");
    Check(a.diagnostics.size() == 1 && a.diagnostics[0].rule == KernelRule::FloatLiteral && a.diagnostics[0].line == 5
              && a.diagnostics[0].file == "Bad.kernel.hlsl",
          "違反は (ファイル名, 行, 規則) つきで返る");

    // キャッシュ: 同じ内容なら同じ結果、内容が変われば作り直す
    kernel::KernelLintCache cache;
    const auto first = cache.Get("SelfTestSaxpy.kernel.hlsl", good);
    const auto second = cache.Get("SelfTestSaxpy.kernel.hlsl", good);
    const auto changed = cache.Get("SelfTestSaxpy.kernel.hlsl", good + "\n// edit\n");
    Check(first == second, "同じソースはキャッシュが返る");
    Check(changed != first && changed->sourceHash != first->sourceHash, "ソースが変わればキャッシュを作り直す");
    const auto crlf = [&] {
        std::string s;
        for (char ch : good) {
            if (ch == '\n') {
                s += '\r';
            }
            s += ch;
        }
        return cache.Get("SelfTestSaxpy.kernel.hlsl", s);
    }();
    Check(crlf->sourceHash == first->sourceHash, "CRLF 化してもハッシュは同じ (正規化)");

    // ディスクへの書き出し (一時フォルダ)
    const fs::path root = fs::temp_directory_path() / "mye_kernel_lint_selftest";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "shaders" / "kernels", ec);
    {
        std::ofstream(root / "shaders" / "kernels" / "SelfTestSaxpy.kernel.hlsl", std::ios::binary) << good;
        std::ofstream(root / "shaders" / "kernels" / "Bad.kernel.hlsl", std::ios::binary) << bad;
        std::ofstream(root / "shaders" / "kernels" / "kernel_common.hlsli", std::ios::binary) << "// 対象外\n";
    }
    const int badCount = kernel::WriteCSharpKernels(root.wstring());
    const fs::path outPath = root / "scripts" / "Generated" / "Kernels.gen.cs";
    const std::string written = ReadAll(outPath);
    Check(badCount == 1 && !written.empty() && written == a.source, "Kernels.gen.cs が書かれ、違反数が返る");
    fs::remove_all(root, ec);

    // カーネルが無く既存ファイルも無いなら何も作らない
    const fs::path emptyRoot = fs::temp_directory_path() / "mye_kernel_lint_selftest_empty";
    fs::remove_all(emptyRoot, ec);
    fs::create_directories(emptyRoot, ec);
    kernel::WriteCSharpKernels(emptyRoot.wstring());
    Check(!fs::exists(emptyRoot / "scripts" / "Generated" / "Kernels.gen.cs", ec), "カーネルが無ければファイルを作らない");
    fs::remove_all(emptyRoot, ec);
}

void TestFormat()
{
    MYE_LOG_INFO("-- 診断の書式 --");
    kernel::KernelDiagnostic d;
    d.file = "Foo.kernel.hlsl";
    d.line = 12;
    d.rule = KernelRule::Swizzle;
    d.message = "テスト";
    Check(kernel::FormatKernelDiagnostic(d) == "Foo.kernel.hlsl(12): error KL005 [swizzle]: テスト", "MSVC 風の書式");
}

} // namespace

bool RunKernelLintSelfTest()
{
    MYE_LOG_INFO("==== Kernel lint / C# 変換 self test ====");
    g_fails = 0;

    const std::wstring shaderDir = FindEngineShaderDir();
    const std::wstring kernelDir = shaderDir.empty() ? std::wstring() : (fs::path(shaderDir) / L"kernels").wstring();

    LintExternalFiles();
    TestFormat();
    TestNegativeCases();
    TestPositiveCases(kernelDir);
    if (!kernelDir.empty()) {
        TestGolden(kernelDir);
        TestGeneration(kernelDir);
    }

    MYE_LOG_INFO("==== Kernel lint self test: %s ====", g_fails == 0 ? "PASS" : "FAIL");
    return g_fails == 0;
}

} // namespace mye
