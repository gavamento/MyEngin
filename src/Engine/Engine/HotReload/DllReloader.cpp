#include "Engine/Engine/HotReload/DllReloader.h"

#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

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

// PID が今も生きているか。棚の掃除で「他プロセスが使用中のものを消さない」ための判定。
// ★ファイルのロック有無で代用してはいけない — コピーしてから LoadLibrary するまでの
//   一瞬はロックが無く、そこを掃除に踏まれると自分の棚が消えて LoadLibrary が
//   ERROR_PATH_NOT_FOUND (3) で落ちる (実測)
bool IsProcessAlive(unsigned long pid)
{
    const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        return false; // 存在しない (アクセス拒否なら「生きている」と読めるが、
                      // その場合も消せないので結果は同じ)
    }
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

} // namespace

void DllReloader::Init(ScriptHost* host, const std::wstring& buildDllPath,
                       const std::wstring& cacheDir)
{
    host_ = host;
    dllPath_ = buildDllPath;
    pdbPath_ = buildDllPath.substr(0, buildDllPath.find_last_of(L'.')) + L".pdb";
    // ★シャドウコピーの棚は**プロセスごとに分ける** (M52h)。
    //   cacheDir はリポジトリの cache\hot 1 個で、Debug/Release も Editor/Runtime も
    //   同じ場所を指す。ここを共有したまま 2 プロセスを同時に走らせると:
    //     ・後から起きた側の remove_all が相手のロック中コピーに当たって失敗する
    //     ・両方が v1 へコピーしようとして CopyFile が共有違反 (32) で弾かれる
    //   → 片方だけ **C++ スクリプトが 1 本も登録されない世界**になる。
    //   ネット対戦は 2 プロセス同時起動が常態なのでこれが日常的に踏まれる
    //   (実際に net_verify で踏み、開始ワールドハッシュ照合が検出した)。
    //   PID を 1 段挟むだけで衝突は原理的に無くなる。
    const std::wstring root = cacheDir;
    const unsigned long selfPid = GetCurrentProcessId();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    // 掃除は「もう居ないプロセスの棚」だけ。**走っている相手の棚には触らない** —
    // ここを一律 remove_all にすると、同時起動した相手がコピー直後・LoadLibrary 直前の
    // 一瞬に自分の棚を消され、相手が「スクリプト 0 本の世界」で走り出す。
    // 走査中に消すと反復子が壊れるので、いったん集めてから消す
    std::vector<std::filesystem::path> stale;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.size() < 2 || name[0] != L'p') {
            stale.push_back(e.path()); // 旧レイアウト (PID 無しの v<n>) は無条件で片付ける
            continue;
        }
        const unsigned long pid = std::wcstoul(name.c_str() + 1, nullptr, 10);
        if (pid == selfPid || !IsProcessAlive(pid)) {
            stale.push_back(e.path());
        }
    }
    for (const auto& dead : stale) {
        std::error_code rmEc;
        std::filesystem::remove_all(dead, rmEc);
    }
    cacheDir_ = root + L"\\p" + std::to_wstring(selfPid);
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
