//====================================================================================
//                          KernelCSharp.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          検査済みカーネルの C# への変換器と Kernels.gen.cs の生成
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Compute/KernelLint.h"

namespace mye::kernel {

// 検査に通ったカーネル 1 本を C# の名前空間ブロック (MyeKernels.<Name>) にする。
// 出力は struct (Pack = 4)、自由関数、Body、[UnmanagedCallersOnly] の Range を持つ CpuKernel クラス。
// 生成コードは例外を投げない (RO の範囲外は 0、整数の / % は 0 でない定数が右辺 — 検査器が保証)。
// 前提: result.Ok()
std::string EmitKernelCSharp(const KernelLintResult& result);

struct KernelSourceFile {
    std::string fileName; // 葉の名前 (Foo.kernel.hlsl)
    std::string text;
};

struct KernelCSharpBatch {
    std::string source;                          // Kernels.gen.cs の中身 (変換できたカーネルが 0 本でも出す)
    std::vector<KernelDiagnostic> diagnostics;   // 検査に落ちたカーネルの違反 (そのカーネルは source に入らない)
    std::vector<std::string> emitted;            // 変換できたカーネル名 (ファイル名順)
};

// 複数のカーネルをまとめて 1 本の C# にする。ファイル名の昇順で並べるので出力は決定的
KernelCSharpBatch BuildKernelsCSharp(std::vector<KernelSourceFile> files, KernelLintCache* cache = nullptr);

// assetsRoot\shaders\kernels\*.kernel.hlsl を読み、assetsRoot\scripts\Generated\Kernels.gen.cs へ書く
// (内容が同じなら書かない)。違反はログに出す。カーネルが 0 本で既存ファイルも無ければ何も作らない。
// 戻り値: 検査に落ちたカーネルの数
int WriteCSharpKernels(const std::wstring& assetsRoot, KernelLintCache* cache = nullptr);

} // namespace mye::kernel
