//====================================================================================
//                          RestoreWriteIo.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元がファイルシステムを書き換える唯一の口 (Win32)
//====================================================================================
#include "Editor/ChangeTracking/RestoreWriteIo.h"

#include <algorithm>
#include <filesystem>

#include <Windows.h>

#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

class SystemRestoreWriteIo final : public RestoreWriteIo {
public:
    bool Write(const std::wstring& absPath, std::string_view bytes) override
    {
        return WriteFileReplacing(absPath, bytes);
    }

    bool Append(const std::wstring& absPath, uint64_t offset, std::string_view bytes) override
    {
        HANDLE file = CreateFileW(absPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, offset == 0 ? CREATE_ALWAYS : OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool ok = true;
        if (offset != 0) {
            LARGE_INTEGER at = {};
            at.QuadPart = static_cast<LONGLONG>(offset);
            ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) != FALSE;
        }
        size_t done = 0;
        while (ok && done < bytes.size()) {
            DWORD written = 0;
            const DWORD slice = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
            ok = WriteFile(file, bytes.data() + done, slice, &written, nullptr) != FALSE && written == slice;
            done += written;
        }
        ok = CloseHandle(file) != FALSE && ok;
        return ok;
    }

    bool Remove(const std::wstring& absPath) override
    {
        return DeleteFileW(absPath.c_str()) != FALSE;
    }

    bool Rename(const std::wstring& from, const std::wstring& to, bool replace) override
    {
        return MoveFileExW(from.c_str(), to.c_str(), (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    bool Copy(const std::wstring& from, const std::wstring& to) override
    {
        return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
    }

    bool MakeDirs(const std::wstring& absDir) override
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(absDir), ec);
        return !ec && std::filesystem::is_directory(std::filesystem::path(absDir), ec);
    }

    bool RemoveTree(const std::wstring& absDir) override
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(absDir), ec);
        return !ec;
    }
};

} // namespace

std::unique_ptr<RestoreWriteIo> MakeSystemRestoreWriteIo()
{
    return std::make_unique<SystemRestoreWriteIo>();
}

} // namespace mye
