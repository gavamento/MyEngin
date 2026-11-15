//====================================================================================
//                          RestoreFileIo.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の判定が使うファイルの読み取り口 (Win32 の読み取りだけ)
//====================================================================================
#include "Editor/ChangeTracking/RestoreFileIo.h"

#include <algorithm>

#include <Windows.h>

namespace mye {

namespace {

class SystemRestoreFileIo final : public RestoreFileIo {
public:
    uint32_t Attributes(const std::wstring& absPath) override
    {
        return static_cast<uint32_t>(GetFileAttributesW(absPath.c_str()));
    }

    ReadStatus Read(const std::wstring& absPath, uint64_t offset, size_t maxBytes, std::string& out, uint64_t& totalSize) override
    {
        // 他のプロセス (エディタ・AI) が書いている最中でも読めるように共有を許す。読むだけで、リンクはたどらない
        HANDLE file = CreateFileW(absPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReadStatus::Absent : ReadStatus::Error;
        }
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size)) {
            CloseHandle(file);
            return ReadStatus::Error;
        }
        totalSize = static_cast<uint64_t>(size.QuadPart);
        const uint64_t remaining = offset >= totalSize ? 0 : totalSize - offset;
        const size_t want = static_cast<size_t>((std::min)(static_cast<uint64_t>(maxBytes), remaining));
        bool ok = true;
        if (want > 0) {
            LARGE_INTEGER at = {};
            at.QuadPart = static_cast<LONGLONG>(offset);
            ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE;
            const size_t base = out.size();
            out.resize(base + want);
            size_t got = 0;
            while (ok && got < want) {
                DWORD read = 0;
                const DWORD slice = static_cast<DWORD>((std::min<size_t>)(want - got, 1u << 20));
                ok = ReadFile(file, out.data() + base + got, slice, &read, nullptr) != FALSE;
                if (ok && read == 0) {
                    break; // 読んでいる間に縮んだ
                }
                got += read;
            }
            out.resize(base + got);
        }
        CloseHandle(file);
        return ok ? ReadStatus::Ok : ReadStatus::Error;
    }
};

} // namespace

std::unique_ptr<RestoreFileIo> MakeSystemRestoreFileIo()
{
    return std::make_unique<SystemRestoreFileIo>();
}

} // namespace mye
