#include "Engine/Engine/HotReload/DllReloaderSelfTest.h"

#include <filesystem>
#include <string>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/HotReload/DllReloader.h"

namespace mye {
namespace {

// 指定のアクセス/share でファイルを握る RAII。テストの「相手プロセスのハンドル」役 —
// 共有検査はプロセス内のハンドル同士でも同じ規則で働くので、実プロセスを 2 個
// 立てなくても衝突の再現になる
class HandleHolder
{
public:
    HandleHolder(const std::wstring& path, DWORD access, DWORD share)
    {
        h_ = CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~HandleHolder()
    {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
    }
    bool Valid() const { return h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

} // namespace

bool RunDllReloaderSelfTest()
{
    MYE_LOG_INFO("==== DllReloader (write-completion probe) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
        return cond;
    };

    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    const std::wstring filePath = (tempDir / L"mye_dllreloader_selftest.bin").wstring();
    {
        // 被験ファイルを作る (中身は何でもよい)
        const HANDLE h = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            MYE_LOG_ERROR("  FAIL: could not create the probe target (%lu)", GetLastError());
            return false;
        }
        DWORD written = 0;
        const char payload[] = "probe";
        WriteFile(h, payload, sizeof(payload), &written, nullptr);
        CloseHandle(h);
    }

    // ---- 1. 基準線: 誰も掴んでいない ----
    check(DllReloader::ProbeWritable(filePath) == 0, "誰も掴んでいなければ 0");

    // ---- 2. 書き手 (share 0) — リンカ型 ----
    {
        HandleHolder w(filePath, GENERIC_WRITE, 0);
        check(w.Valid(), "書き手 (share 0) を握れる");
        check(DllReloader::ProbeWritable(filePath) == ERROR_SHARING_VIOLATION,
              "書き手 (share 0) が居る間は共有違反");
    }

    // ---- 3. 書き手 (share 全許可) ----
    // ★「プローブの share を全部許す」雑な直しへの回帰防止。共有検査は双方向なので、
    //   相手が譲っていても「こちらが FILE_SHARE_WRITE を許さない」ことで書き手は弾ける —
    //   その方向が生きていることを固定する
    {
        HandleHolder w(filePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE);
        check(w.Valid(), "書き手 (share 全許可) を握れる");
        check(DllReloader::ProbeWritable(filePath) == ERROR_SHARING_VIOLATION,
              "相手が share を譲っていても書き手は弾く");
    }

    // ---- 4. 読み手 (share_read) — 本バグの回帰テスト ----
    // net_verify case A/D のフレーク: もう片方のエンジンプロセスのプローブ /
    // CopyFile のソース読みと共存できること (排他オープンだった頃はここで衝突した)
    {
        HandleHolder r(filePath, GENERIC_READ, FILE_SHARE_READ);
        check(r.Valid(), "読み手 (share_read) を握れる");
        check(DllReloader::ProbeWritable(filePath) == 0,
              "読み手とは共存する (2 プロセス同時起動のプローブ)");
    }

    // ---- 5. 不在パスでは待たない ----
    {
        const std::wstring missing =
            (tempDir / L"mye_dllreloader_selftest_missing.bin").wstring();
        const uint64_t t0 = GetTickCount64();
        const unsigned long err = DllReloader::WaitUntilWritable(missing, 2000);
        const uint64_t elapsed = GetTickCount64() - t0;
        check(err == ERROR_FILE_NOT_FOUND, "不在は ERROR_FILE_NOT_FOUND");
        // 健全な実装は 1ms 未満で返る。上限だけ主張する (下限のタイミング assert は
        // フレークの温床なのでしない)
        check(elapsed < 500, "不在ではタイムアウトまで粘らない");
    }

    // ---- 6. 有界性: 書き手が居座ったら諦めて戻る ----
    {
        HandleHolder w(filePath, GENERIC_WRITE, 0);
        const unsigned long err = DllReloader::WaitUntilWritable(filePath, 30);
        check(err == ERROR_SHARING_VIOLATION, "書き手が居座ったら共有違反のまま有界で戻る");
    }

    std::filesystem::remove(filePath, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("==== DllReloader self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== DllReloader self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
