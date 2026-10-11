//====================================================================================
//                          KernelHash.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネルのソースハッシュ (BOM 除去・CRLF 正規化の FNV-1a 64)
//====================================================================================
#pragma once
#include <cstdint>
#include <string_view>

namespace mye::kernel {

// カーネルのソースハッシュ (版ずれ検出用)。
//
// 手順 (PowerShell の codegen、C# の変換器も同じ手順で同じ値を出すこと):
//   1. 先頭の UTF-8 BOM (EF BB BF) を 1 個だけ取り除く
//   2. 「CR の直後に LF」が続く CR を取り除く (CRLF -> LF。単独の CR はそのまま)
//   3. 残ったバイト列の FNV-1a 64bit (offset 14695981039346656037、prime 1099511628211)
// 空文字列は offset basis そのもの。
uint64_t HashKernelSource(std::string_view text);

} // namespace mye::kernel
