#include "Engine/Core/Utf8.h"

#include <cstring>

namespace mye::utf8 {
namespace {

// 先頭バイトから見たシーケンス長。継続バイト (0x80-0xBF) が単独で来た場合は
// 不正だが 1 バイトとして扱い、そのまま通す (ここは検証器ではなく切り詰め器)。
size_t SeqLen(unsigned char c)
{
    if (c >= 0xF0) {
        return 4;
    }
    if (c >= 0xE0) {
        return 3;
    }
    if (c >= 0xC0) {
        return 2;
    }
    return 1;
}

} // namespace

size_t CopyTruncated(char* dst, size_t dstCap, const char* src)
{
    if (dst == nullptr || dstCap == 0) {
        return 0;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return 0;
    }

    const size_t maxBytes = dstCap - 1; // ヌル終端の分を残す
    size_t n = 0;
    while (src[n] != '\0') {
        const size_t seq = SeqLen(static_cast<unsigned char>(src[n]));
        if (n + seq > maxBytes) {
            break; // この文字は丸ごと落とす
        }
        // 継続バイトが揃う前に終端している (壊れた入力) 場合もその文字ごと落とす
        bool complete = true;
        for (size_t k = 1; k < seq; ++k) {
            if (src[n + k] == '\0') {
                complete = false;
                break;
            }
        }
        if (!complete) {
            break;
        }
        n += seq;
    }

    std::memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

} // namespace mye::utf8
