#pragma once
#include <cstdint>

namespace mye {

// PCG32 (https://www.pcg-random.org/) — エンジン標準の決定論的 RNG。
// spec 11.2 規則 8: rand() / std::random_device の直接使用は禁止。乱数は必ず
// シード管理されたこのクラスのストリームから取得する。
// 内部状態 (state/inc) はリプレイのワールドハッシュ対象 (spec 11.3)。
class Pcg32 {
public:
    void Seed(uint64_t initstate, uint64_t initseq = 0xda3e39cb94b95bdbull)
    {
        state_ = 0;
        inc_ = (initseq << 1) | 1u;
        NextU32();
        state_ += initstate;
        NextU32();
    }

    uint32_t NextU32()
    {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
        const uint32_t rot = static_cast<uint32_t>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // [0, 1) — 仮数 24bit を使用 (float で正確に表現できる範囲)
    float NextFloat01() { return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f); }

    float Range(float lo, float hi) { return lo + (hi - lo) * NextFloat01(); }

    // [0, bound) — 実装は変更しないこと (リプレイ互換性が壊れる)
    uint32_t RangeU32(uint32_t bound) { return bound ? NextU32() % bound : 0; }

    int32_t RangeInt(int32_t lo, int32_t hiExclusive)
    {
        if (hiExclusive <= lo) {
            return lo;
        }
        return lo + static_cast<int32_t>(RangeU32(static_cast<uint32_t>(hiExclusive - lo)));
    }

    uint64_t State() const { return state_; }
    uint64_t Inc() const { return inc_; }
    void Restore(uint64_t state, uint64_t inc) { state_ = state; inc_ = inc; }

private:
    uint64_t state_ = 0x853c49e6748fea9bull;
    uint64_t inc_ = 0xda3e39cb94b95bdbull;
};

} // namespace mye
