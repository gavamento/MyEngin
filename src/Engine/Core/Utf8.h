#pragma once
#include <cstddef>

// UTF-8 バイト列の最小限のユーティリティ。
// エンジンの内部文字列はすべて UTF-8 (ソースは /utf-8 でコンパイルされ、ImGui も UTF-8 前提)。
// 固定長バッファへ落とす箇所で「バイト境界で切って不正 UTF-8 を作る」事故を防ぐために使う。
namespace mye::utf8 {

// src を dst へコピーする。dstCap はヌル終端を含む容量。
// 入り切らない場合、**マルチバイト列を分断せず**その文字の手前で打ち切る。
// 戻り値 = 書き込んだバイト数 (ヌル終端を除く)。dst は常にヌル終端される。
size_t CopyTruncated(char* dst, size_t dstCap, const char* src);

} // namespace mye::utf8
