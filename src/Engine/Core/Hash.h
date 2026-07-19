#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace mye {

// FNV-1a 64bit。
// ワールド状態ハッシュ (spec 11.3) にも使うため、実装は今後一切変更しないこと
// (変更すると過去のリプレイ .rep が検証不能になる)。
inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime  = 1099511628211ull;

constexpr uint64_t HashStr(std::string_view s, uint64_t seed = kFnvOffset)
{
    uint64_t h = seed;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t HashBytes(const void* data, size_t size, uint64_t seed = kFnvOffset)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t HashCombine(uint64_t seed, uint64_t value)
{
    return HashBytes(&value, sizeof(value), seed);
}

} // namespace mye
