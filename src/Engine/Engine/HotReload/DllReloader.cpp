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

// 起動時 1 発勝負 (LoadInitial) のためだけの保険。Update() の 500ms ポーリングとは別物 —
// あちらはリロード検出の間引きで、こちらは「起動直後にまだ書き手が居る」窓を跨ぐ短い待機
constexpr uint32_t kInitialDllWaitMs = 250;

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

unsigned long DllReloader::ProbeWritable(const std::wstring& path)
{
    // 書き手 (リンカ / コピー等) の不在確認。★share を 0 (完全排他) にしないこと —
    //   Win32 の共有検査は双方向なので、GENERIC_READ + FILE_SHARE_READ のオープンは
    //     ・書き手が居れば必ず失敗する (こちらが FILE_SHARE_WRITE を許さないので、
    //       相手の share 設定に依存せず弾ける = リンカ検出はこれで足りる)
    //     ・読み手 (もう片方のエンジンプロセスの同じプローブ / CopyFile のソース読み)
    //       とは共存する
    //   排他オープンだった頃は、ネットの 2 プロセス同時起動で互いのプローブが衝突し、
    //   負けた側が「C++ スクリプト 0 本の世界」で開始 → 開始ワールドハッシュ照合で
    //   接続拒否になっていた (net_verify case A/D のフレーク。コピー先は M52h の
    //   p<pid> 分離で解決済みだったが、コピー元の排他読みがここに残っていた)。
    //   FILE_SHARE_DELETE は足さない — DELETE を持つ相手 (差し替え直前) は弾くべき
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    CloseHandle(h);
    return 0;
}

unsigned long DllReloader::WaitUntilWritable(const std::wstring& path, uint32_t timeoutMs)
{
    // 共有違反 (32) = 「書き手が今書いている。待てば通る」のときだけ待つ。
    // 不在 (2/3) 等は待っても結果が変わらないので即返す
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    unsigned long err = ProbeWritable(path);
    while (err == ERROR_SHARING_VIOLATION && GetTickCount64() < deadline) {
        Sleep(1);
        err = ProbeWritable(path);
    }
    return err;
}

bool DllReloader::TryCopyAndLoad()
{
    if (ProbeWritable(dllPath_) != 0) {
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
    // ここは 1 発勝負 (ネット起動では Update() の 500ms 再試行が開始ワールドハッシュ
    // 照合に間に合わない) なので、書き手が居る間だけ短く待ってから 1 回だけ試す。
    // TryCopyAndLoad 全体はリトライしない — LoadModule 失敗 (待っても直らない) を
    // 巻き込み、counter_ も無駄に進むため
    const unsigned long err = WaitUntilWritable(dllPath_, kInitialDllWaitMs);
    if (err != 0) {
        MYE_LOG_WARN("[dll] initial load: GameLogic.dll is still busy (err=%lu): %s",
                     err, WideToUtf8(dllPath_).c_str());
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
