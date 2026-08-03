#pragma once
#include <cstdarg>
#include <string>

namespace mye {

// printf 書式を std::string に組み立てる (M47b)。
// 訳文は言語で長さが変わるため、固定長 char[N] に snprintf すると
// 溢れたりマルチバイト列がバイト境界で切れたりする。長さを実測して確保する。
std::string Format(const char* fmt, ...);
std::string FormatV(const char* fmt, va_list args);

} // namespace mye
