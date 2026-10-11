//====================================================================================
//                          KernelLintInternal.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          検査器のパーサと型検査が共有する内部宣言
//====================================================================================
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Engine/Compute/KernelAst.h"
#include "Engine/Engine/Compute/KernelLint.h"

namespace mye::kernel::lint {

// 診断の出力先
struct DiagSink {
    std::string file;
    std::vector<KernelDiagnostic>* diags = nullptr;

    void Add(KernelRule rule, int line, std::string message) const
    {
        diags->push_back(KernelDiagnostic{ file, line, rule, std::move(message) });
    }
};

// 構文を読み切れない違反。診断を出した後に投げ、ParseKernelSource が受けて打ち切る
struct ParseAbort {};

// 字句 + 構文解析。構文違反は sink に出して false を返す (木は不完全)
bool ParseKernelSource(std::string_view text, const DiagSink& sink, KernelProgram& out);

// 型検査 (名前解決・型・規則)。違反は sink に出す。木に型と参照先を書き込む
void CheckKernelProgram(KernelProgram& program, const std::string& kernelName, const DiagSink& sink);

// ---- 名前の表 (パーサと型検査で共有) ----

// float / int / uint とそのベクトルなら true。out があれば型を返す
bool ParseBuiltinTypeName(std::string_view name, KType* out);
const char* ScalarName(ScalarKind s);
std::string TypeToString(const KType& t, const KernelProgram* program);

bool IsUnsupportedTypeName(std::string_view name);  // bool / double / half / 行列 / テクスチャなど
bool IsControlKeyword(std::string_view name);       // switch / groupshared / cbuffer など (KL021)
bool IsReservedIdentifier(std::string_view name);   // 変数・関数・struct の名前に使えない語
bool IsKnownHlslIntrinsic(std::string_view name);   // HLSL の組込みだがホワイトリスト外
bool IsBarrierOrAtomic(std::string_view name);      // Interlocked* / *MemoryBarrier*

} // namespace mye::kernel::lint
