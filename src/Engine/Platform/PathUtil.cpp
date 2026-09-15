#include "Engine/Platform/PathUtil.h"

#include <filesystem>
#include <fstream>

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

std::wstring FindEngineRepoRoot()
{
    // FindAssetsRoot と同じ探索。目印は C++ スクリプトのビルドに必須なヘッダそのもの
    std::filesystem::path dir = GetExecutableDir();
    for (int i = 0; i < 6; ++i) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / L"src" / L"Shared" / L"ScriptAPI.h", ec)) {
            return dir.wstring();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

std::wstring FindEngineShaderDir()
{
    const std::wstring repo = FindEngineRepoRoot();
    if (repo.empty()) {
        return {};
    }
    const std::filesystem::path p = std::filesystem::path(repo) / L"assets" / L"shaders";
    std::error_code ec;
    return std::filesystem::is_directory(p, ec) ? p.wstring() : std::wstring{};
}

std::wstring NormalizePathKey(const std::wstring& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    std::wstring s = ec ? path : p.lexically_normal().wstring();
    for (wchar_t& c : s) {
        if (c == L'/') {
            c = L'\\';
        } else {
            c = static_cast<wchar_t>(towlower(c));
        }
    }
    return s;
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

// ★ofstream で直接開くと、その時点で既存の中身が切り詰められる — 容量不足などで書き込みが途中で
//   失敗すると、前の内容も新しい内容も残らない。
// ★テンポラリ名には PID を混ぜる (CookedCache と同じ理由: 並列の検証プロセスが同じファイルへ書き得る)。
//   rename はアトミックなので、読み手には「前の完全な内容」か「新しい完全な内容」しか見えない
bool WriteFileReplacing(const std::wstring& path, std::string_view bytes)
{
    const std::wstring tmpPath = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::error_code ec;
    {
        std::ofstream f(std::filesystem::path(tmpPath), std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.close(); // 書き出しの失敗は close (flush) で初めて出ることがあるので、閉じてから見る
        if (f.fail()) {
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

} // namespace mye
