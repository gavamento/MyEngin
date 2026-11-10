//====================================================================================
//                          Sha256.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          BCrypt による SHA-256 の実装
//====================================================================================
#include "Engine/Platform/Sha256.h"

#include <cstdio>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace mye {

namespace {

constexpr size_t kDigestBytes = 32;
constexpr size_t kFileChunkBytes = 1024 * 1024;

// 1 回分のハッシュ計算。BCRYPT_SHA256_ALG_HANDLE (Windows 10 以降) を使い、アルゴリズムの開閉を省く
class HashState
{
public:
    HashState()
    {
        ok_ = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash_, nullptr, 0, nullptr, 0, 0));
    }
    HashState(const HashState&) = delete;
    HashState& operator=(const HashState&) = delete;
    ~HashState()
    {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
    }

    bool Update(const void* data, size_t size)
    {
        // BCryptHashData の長さは ULONG なので大きな入力は分けて渡す
        const auto* p = static_cast<const unsigned char*>(data);
        while (ok_ && size > 0) {
            const ULONG n = static_cast<ULONG>(size > 0x40000000u ? 0x40000000u : size);
            ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(p), n, 0));
            p += n;
            size -= n;
        }
        return ok_;
    }

    std::string FinishHex()
    {
        unsigned char digest[kDigestBytes] = {};
        if (!ok_ || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, static_cast<ULONG>(kDigestBytes), 0))) {
            return {};
        }
        static const char kHex[] = "0123456789abcdef";
        std::string out(kDigestBytes * 2, '0');
        for (size_t i = 0; i < kDigestBytes; ++i) {
            out[i * 2] = kHex[digest[i] >> 4];
            out[i * 2 + 1] = kHex[digest[i] & 0x0F];
        }
        return out;
    }

private:
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

} // namespace

std::string Sha256Hex(std::string_view bytes)
{
    HashState h;
    h.Update(bytes.data(), bytes.size());
    return h.FinishHex();
}

bool Sha256FileHex(const std::wstring& path, std::string& hexOut, const std::atomic<bool>* cancel)
{
    hexOut.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        return false;
    }
    HashState h;
    std::vector<char> buf(kFileChunkBytes);
    bool ok = true;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            ok = false;
            break;
        }
        const size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n > 0 && !h.Update(buf.data(), n)) {
            ok = false;
            break;
        }
        if (n < buf.size()) {
            ok = ferror(f) == 0;
            break;
        }
    }
    fclose(f);
    if (!ok) {
        return false;
    }
    hexOut = h.FinishHex();
    return !hexOut.empty();
}

} // namespace mye
