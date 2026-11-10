//====================================================================================
//                          Sha256.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          BCrypt による SHA-256 (配布物の照合用)
//====================================================================================
#pragma once
#include <atomic>
#include <string>
#include <string_view>

namespace mye {

// SHA-256 を小文字の 16 進 64 文字で返す。BCrypt が使えなければ空
std::string Sha256Hex(std::string_view bytes);

// ファイル全体の SHA-256。読めない・取り消されたときは false
bool Sha256FileHex(const std::wstring& path, std::string& hexOut, const std::atomic<bool>* cancel = nullptr);

} // namespace mye
