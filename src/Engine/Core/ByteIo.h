#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mye {

// 生バイト列の追記/読み出し (M52d: sim スナップショット用の最小ヘルパ)。
//
// - x64 固定エンジンなのでエンディアン変換はしない (blob は同一機・同一ビルドで使う)。
//   .rep に埋め込む blob も版で守るので、可搬性はここでは扱わない。
// - 読み側は 1 度でも範囲外を踏んだら ok_ を落とし、以降の読みは既定値を返す。
//   「途中まで復元された世界」を作らないための番人 — 呼び出し側は最後に Ok() を見て、
//   丸ごと採用するか丸ごと捨てるかを決めること。
// - 長さ付きの読みは必ず「残りバイト数」で検証してから確保する。壊れた blob の
//   巨大な長さをそのまま resize すると bad_alloc で落ちる (壊れ方として最悪)
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) : out_(out) {}

    void Raw(const void* p, size_t n)
    {
        if (n == 0) {
            return;
        }
        const size_t at = out_.size();
        out_.resize(at + n);
        std::memcpy(out_.data() + at, p, n);
    }
    template <typename T>
    void Pod(const T& v)
    {
        Raw(&v, sizeof(T));
    }
    void U8(uint8_t v) { Pod(v); }
    void U32(uint32_t v) { Pod(v); }
    void I32(int32_t v) { Pod(v); }
    void U64(uint64_t v) { Pod(v); }
    void F32(float v) { Pod(v); }
    // 要素数/バイト数の前置き (読み側の Count と対)
    void Count(size_t v) { U64(static_cast<uint64_t>(v)); }

    // 長さ前置きの生バイト列
    void Blob(const void* p, size_t n)
    {
        Count(n);
        Raw(p, n);
    }
    void Str(const std::string& s) { Blob(s.data(), s.size()); }
    void WStr(const std::wstring& s) { Blob(s.data(), s.size() * sizeof(wchar_t)); }
    template <typename T>
    void PodVector(const std::vector<T>& v)
    {
        Count(v.size());
        Raw(v.data(), v.size() * sizeof(T));
    }

    size_t Bytes() const { return out_.size(); }

private:
    std::vector<std::byte>& out_;
};

class ByteReader {
public:
    ByteReader(const std::byte* data, size_t size) : p_(data), size_(size) {}

    bool Ok() const { return ok_; }
    void Fail() { ok_ = false; }
    size_t Remaining() const { return ok_ ? size_ - at_ : 0; }

    bool Raw(void* dst, size_t n)
    {
        if (!ok_ || n > size_ - at_) {
            ok_ = false;
            return false;
        }
        if (n != 0) {
            std::memcpy(dst, p_ + at_, n);
            at_ += n;
        }
        return true;
    }
    template <typename T>
    T Pod()
    {
        T v{};
        Raw(&v, sizeof(T));
        return v;
    }
    uint8_t U8() { return Pod<uint8_t>(); }
    uint32_t U32() { return Pod<uint32_t>(); }
    int32_t I32() { return Pod<int32_t>(); }
    uint64_t U64() { return Pod<uint64_t>(); }
    float F32() { return Pod<float>(); }

    // 長さ前置きの要素数を読んで「残りバイト数に収まるか」を検証する。
    // 収まらなければ ok_ を落として 0 を返す (呼び出し側は resize を諦める)
    size_t Count(size_t elemSize)
    {
        const uint64_t n = U64();
        if (!ok_ || elemSize == 0 || n > static_cast<uint64_t>(size_ - at_) / elemSize) {
            ok_ = false;
            return 0;
        }
        return static_cast<size_t>(n);
    }

    std::string Str()
    {
        const size_t n = Count(sizeof(char));
        std::string s(n, '\0');
        Raw(s.data(), n);
        return s;
    }
    std::wstring WStr()
    {
        // 書き手はバイト数を前置きする (Blob と同じ) ので、要素数へ割り戻してから読む。
        // 端数が出る = 別の形式のデータを掴んでいる → 失敗扱い
        const size_t bytes = Count(sizeof(char));
        if (!ok_ || (bytes % sizeof(wchar_t)) != 0) {
            ok_ = false;
            return {};
        }
        std::wstring s(bytes / sizeof(wchar_t), L'\0');
        Raw(s.data(), bytes);
        return s;
    }
    template <typename T>
    std::vector<T> PodVector()
    {
        const size_t n = Count(sizeof(T));
        std::vector<T> v(n);
        Raw(v.data(), n * sizeof(T));
        return v;
    }

private:
    const std::byte* p_ = nullptr;
    size_t size_ = 0;
    size_t at_ = 0;
    bool ok_ = true;
};

} // namespace mye
