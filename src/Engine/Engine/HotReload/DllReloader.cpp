#include "Engine/Engine/HotReload/DllReloader.h"

#include <filesystem>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Script/ScriptHost.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr uint64_t kPollIntervalMs = 500;

uint64_t GetWriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | data.ftLastWriteTime.dwLowDateTime;
}

} // namespace

void DllReloader::Init(ScriptHost* host, const std::wstring& buildDllPath,
                       const std::wstring& cacheDir)
{
    host_ = host;
    dllPath_ = buildDllPath;
    pdbPath_ = buildDllPath.substr(0, buildDllPath.find_last_of(L'.')) + L".pdb";
    cacheDir_ = cacheDir;

    // 前セッションのコピーを掃除 (ロック中のものはスキップ)
    std::error_code ec;
    std::filesystem::remove_all(cacheDir_, ec);
    std::filesystem::create_directories(cacheDir_, ec);
}

bool DllReloader::IsWritable(const std::wstring& path) const
{
    // 排他オープンが成功 = リンカ/コピー等の書き込みが完了している
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

bool DllReloader::TryCopyAndLoad()
{
    if (!IsWritable(dllPath_)) {
        return false; // リンカがまだ書いている — 次フレーム再試行
    }
    const bool hasPdb = GetWriteTime(pdbPath_) != 0;

    ++counter_;
    wchar_t sub[64];
    swprintf_s(sub, L"v%u", counter_);
    const std::wstring dir = cacheDir_ + L"\\" + sub;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // 元のファイル名のままコピー (/PDBALTPATH:GameLogic.pdb が隣の PDB を指す)
    const std::wstring dllCopy = dir + L"\\GameLogic.dll";
    if (!CopyFileW(dllPath_.c_str(), dllCopy.c_str(), FALSE)) {
        MYE_LOG_WARN("[dll] copy failed (%lu) - retrying", GetLastError());
        --counter_;
        return false;
    }
    if (hasPdb) {
        // PDB コピー失敗は致命ではない (デバッグ情報が古くなるだけ)
        CopyFileW(pdbPath_.c_str(), (dir + L"\\GameLogic.pdb").c_str(), FALSE);
    }

    const uint64_t writeTime = GetWriteTime(dllPath_);
    if (!host_->LoadModule(dllCopy)) {
        MYE_LOG_ERROR("[dll] reload failed - keeping previous logic");
        return false;
    }
    lastWriteTime_ = writeTime;
    MYE_LOG_INFO("[dll] hot reload complete (v%u)", counter_);
    return true;
}

bool DllReloader::LoadInitial()
{
    if (GetWriteTime(dllPath_) == 0) {
        MYE_LOG_WARN("[dll] GameLogic.dll not found: %s", WideToUtf8(dllPath_).c_str());
        return false;
    }
    return TryCopyAndLoad();
}

bool DllReloader::Update()
{
    // ポーリング間隔を絞る (毎フレームのファイルアクセスを避ける)
    const uint64_t now = GetTickCount64();
    if (now - lastPollMs_ < kPollIntervalMs) {
        return false;
    }
    lastPollMs_ = now;

    const uint64_t writeTime = GetWriteTime(dllPath_);
    if (writeTime == 0 || writeTime == lastWriteTime_) {
        return false;
    }
    // ビルド完了検出 (spec 8.4 手順 1-2)
    return TryCopyAndLoad();
}

} // namespace mye
