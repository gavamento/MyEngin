//====================================================================================
//                          KernelCppCodegen.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          検査済みカーネルの C++ カーネル表 (Kernels.gen.{h,cpp}) の生成
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Compute/KernelLint.h"

namespace mye::kernel {

struct KernelCppSourceFile {
    std::string fileName;    // 葉の名前 (Foo.kernel.hlsl)
    std::string text;
    std::string includePath; // Kernels.gen.h から見た #include の文字列 ('/' 区切り)
};

struct KernelCppBatch {
    std::string header;                          // Kernels.gen.h (カーネルが 0 本でも出す)
    std::string source;                          // Kernels.gen.cpp (同上)
    std::vector<KernelDiagnostic> diagnostics;   // 検査に落ちたカーネルの違反 (そのカーネルは表に入らない)
    std::vector<std::string> emitted;            // 表に入ったカーネル名 (序数順)
};

// カーネルごとの CPU 版の取り込みと登録 (MyeKernelDesc) を書く。引数の HLSL 変数名は構文解析器から取り、
// ハッシュは HashKernelSource。並びは名前の序数順で決定的。
// headerInclude: Kernels.gen.cpp が自分のヘッダを include する文字列 (例 "Generated/Kernels.gen.h")
KernelCppBatch BuildKernelsCpp(std::vector<KernelCppSourceFile> files, const std::string& headerInclude,
                               KernelLintCache* cache = nullptr);

// kernelDir\*.kernel.hlsl を読み、genDir\Kernels.gen.{h,cpp} へ書く (内容が同じなら書かない)。
// 違反はログに出す。カーネルが 0 本でも空の表を書く (GameLogic のビルドが通るように)。
// absoluteIncludes: true なら .kernel.hlsl の #include を絶対パスで、false なら genDir からの相対パスで書く。
// 戻り値: 検査に落ちたカーネルの数
int WriteCppKernels(const std::wstring& kernelDir, const std::wstring& genDir, const std::string& headerInclude,
                    bool absoluteIncludes, KernelLintCache* cache = nullptr);

} // namespace mye::kernel
