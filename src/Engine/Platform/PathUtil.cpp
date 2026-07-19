#include "Engine/Platform/PathUtil.h"

#include <filesystem>

#include <Windows.h>

namespace mye {

std::wstring GetExecutableDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

std::wstring FindAssetsRoot()
{
    std::filesystem::path dir = GetExecutableDir();
    for (int i = 0; i < 6; ++i) {
        const std::filesystem::path candidate = dir / L"assets";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) {
            return candidate.wstring();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return (std::filesystem::current_path() / L"assets").wstring();
}

std::string WideToUtf8(std::wstring_view w)
{
    if (w.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

} // namespace mye
