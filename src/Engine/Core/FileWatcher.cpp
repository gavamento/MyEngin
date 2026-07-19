#include "Engine/Core/FileWatcher.h"

#include <algorithm>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr uint64_t kDebounceMs = 150;
constexpr DWORD kBufferSize = 64 * 1024;

} // namespace

bool FileWatcher::Start(const std::wstring& directory)
{
    Stop();
    root_ = directory;

    HANDLE dir = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        MYE_LOG_ERROR("FileWatcher: cannot open %s (%lu)", WideToUtf8(directory).c_str(),
                      GetLastError());
        return false;
    }
    dirHandle_ = dir;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { ThreadProc(); });
    MYE_LOG_INFO("FileWatcher: watching %s", WideToUtf8(directory).c_str());
    return true;
}

void FileWatcher::Stop()
{
    if (stopEvent_) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (dirHandle_) {
        CloseHandle(static_cast<HANDLE>(dirHandle_));
        dirHandle_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
}

void FileWatcher::ThreadProc()
{
    alignas(4) uint8_t buffer[kBufferSize];
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const HANDLE dir = static_cast<HANDLE>(dirHandle_);
    const HANDLE stop = static_cast<HANDLE>(stopEvent_);

    for (;;) {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(dir, buffer, kBufferSize, TRUE,
                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE
                                       | FILE_NOTIFY_CHANGE_FILE_NAME,
                                   nullptr, &overlapped, nullptr)) {
            MYE_LOG_ERROR("FileWatcher: ReadDirectoryChangesW failed (%lu)", GetLastError());
            break;
        }

        const HANDLE waits[2] = { overlapped.hEvent, stop };
        const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which != WAIT_OBJECT_0) {
            CancelIoEx(dir, &overlapped);
            break; // stop 要求
        }
        if (!GetOverlappedResult(dir, &overlapped, &bytes, FALSE)) {
            continue;
        }
        if (bytes == 0) {
            // バッファオーバーフロー: イベント欠落。全ファイルの再確認は上位に委ねる
            MYE_LOG_WARN("FileWatcher: event buffer overflow (some changes may be lost)");
            continue;
        }

        const uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lock(mutex_);
        const uint8_t* p = buffer;
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            if (info->Action == FILE_ACTION_MODIFIED || info->Action == FILE_ACTION_ADDED
                || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::wstring rel(info->FileName, info->FileNameLength / sizeof(wchar_t));
                const std::wstring full = NormalizePathKey(root_ + L"\\" + rel);
                bool found = false;
                for (Pending& pend : pending_) {
                    if (pend.path == full) {
                        pend.lastEventMs = now;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    pending_.push_back({ full, now });
                }
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            p += info->NextEntryOffset;
        }
    }
    CloseHandle(overlapped.hEvent);
}

std::vector<std::wstring> FileWatcher::DrainChanges()
{
    std::vector<std::wstring> out;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < pending_.size();) {
        if (now - pending_[i].lastEventMs >= kDebounceMs) {
            out.push_back(std::move(pending_[i].path));
            pending_.erase(pending_.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace mye
