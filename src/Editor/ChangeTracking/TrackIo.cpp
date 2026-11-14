//====================================================================================
//                          TrackIo.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Win32 の名前付き pipe (overlapped) と service.json の読み取り
//====================================================================================
#include "Editor/ChangeTracking/TrackIo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include <Windows.h>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/TrackLaunch.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

namespace {

constexpr DWORD kReadChunkBytes = 64 * 1024;
// 送信待ちの上限。要求は小さい (数百バイト〜数 KB) ので、これを超えるのは相手が読んでいない異常
constexpr size_t kMaxOutboundBytes = 4 * 1024 * 1024;

class Win32TrackIo final : public TrackIo {
public:
    explicit Win32TrackIo(std::wstring exeDir) : exeDir_(std::move(exeDir)) {}
    ~Win32TrackIo() override { ClosePipe(); }

    bool ReadServiceToken(const std::wstring& projectRoot, std::string& tokenOut) override
    {
        const std::filesystem::path path =
            std::filesystem::path(projectRoot) / L".mye" / L"ai_changes" / L"service.json";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        const nlohmann::json root = nlohmann::json::parse(f, nullptr, false);
        if (!root.is_object() || !root.contains("token") || !root["token"].is_string()) {
            return false;
        }
        tokenOut = root["token"].get<std::string>();
        return !tokenOut.empty();
    }

    OpenResult OpenPipe(const std::string& pipeName) override
    {
        ClosePipe();
        const int wideLen = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(wideLen > 0 ? wideLen - 1 : 0), L'\0');
        if (wideLen > 1) {
            MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, wide.data(), wideLen);
        }
        // lpSecurityAttributes = nullptr: ハンドルは継承されない (起動するサービスへ渡らない)
        HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) {
                return OpenResult::NotFound;
            }
            return err == ERROR_PIPE_BUSY ? OpenResult::Busy : OpenResult::Failed;
        }
        pipe_ = h;
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (readEvent_ == nullptr || writeEvent_ == nullptr) {
            ClosePipe();
            return OpenResult::Failed;
        }
        readBuffer_.resize(kReadChunkBytes);
        return OpenResult::Opened;
    }

    void ClosePipe() override
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            // 保留中の I/O を取り消し、完了するまで待ってからバッファを手放す
            CancelIoEx(pipe_, nullptr);
            DWORD ignored = 0;
            if (readPending_) {
                GetOverlappedResult(pipe_, &readOverlapped_, &ignored, TRUE);
            }
            if (writePending_) {
                GetOverlappedResult(pipe_, &writeOverlapped_, &ignored, TRUE);
            }
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        for (HANDLE* event : { &readEvent_, &writeEvent_ }) {
            if (*event != nullptr) {
                CloseHandle(*event);
                *event = nullptr;
            }
        }
        readPending_ = false;
        writePending_ = false;
        outbound_.clear();
        writing_.clear();
    }

    bool Write(const std::string& bytes) override
    {
        if (pipe_ == INVALID_HANDLE_VALUE || outbound_.size() + writing_.size() + bytes.size() > kMaxOutboundBytes) {
            return false;
        }
        outbound_ += bytes;
        return true;
    }

    PumpResult Pump(std::string& incoming) override
    {
        if (pipe_ == INVALID_HANDLE_VALUE) {
            return PumpResult::Closed;
        }
        if (!AdvanceWrite()) {
            return PumpResult::Closed;
        }
        return AdvanceRead(incoming) ? PumpResult::Ok : PumpResult::Closed;
    }

    LaunchResult LaunchService(const std::wstring& projectRoot) override
    {
        const std::wstring exe = FindTrackExe(exeDir_);
        if (exe.empty()) {
            return LaunchResult::NoExe;
        }
        uint32_t error = 0;
        if (!LaunchTrackService(exe, projectRoot, &error)) {
            MYE_LOG_INFO("[track] mye_track.exe を起動できない (%lu)", static_cast<unsigned long>(error));
            return LaunchResult::Failed;
        }
        return LaunchResult::Launched;
    }

    int64_t NowMs() override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    // 書き込み 1 本を進める。壊れていれば false
    bool AdvanceWrite()
    {
        if (writePending_) {
            DWORD written = 0;
            if (!GetOverlappedResult(pipe_, &writeOverlapped_, &written, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    return true;
                }
                return false;
            }
            writePending_ = false;
            writing_.erase(0, written);
            if (!writing_.empty()) {
                outbound_.insert(0, writing_); // 書き切れなかった分は先頭へ戻す
                writing_.clear();
            }
        }
        if (outbound_.empty()) {
            return true;
        }
        writing_.swap(outbound_);
        outbound_.clear();
        writeOverlapped_ = {};
        writeOverlapped_.hEvent = writeEvent_;
        DWORD written = 0;
        if (WriteFile(pipe_, writing_.data(), static_cast<DWORD>(writing_.size()), &written, &writeOverlapped_)) {
            writing_.erase(0, written);
            outbound_.insert(0, writing_); // 書き切れなかった分は次の回に回す
            writing_.clear();
            return true;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            writePending_ = true;
            return true;
        }
        return false;
    }

    // 読み取りを 1 回進める。壊れていれば false
    bool AdvanceRead(std::string& incoming)
    {
        if (!readPending_) {
            readOverlapped_ = {};
            readOverlapped_.hEvent = readEvent_;
            DWORD got = 0;
            if (ReadFile(pipe_, readBuffer_.data(), kReadChunkBytes, &got, &readOverlapped_)) {
                incoming.append(readBuffer_.data(), got);
                return true;
            }
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                readPending_ = true;
                return true;
            }
            // ERROR_MORE_DATA は byte モードでは出ない。それ以外 (ERROR_BROKEN_PIPE など) は切断
            return false;
        }
        DWORD got = 0;
        if (GetOverlappedResult(pipe_, &readOverlapped_, &got, FALSE)) {
            readPending_ = false;
            incoming.append(readBuffer_.data(), got);
            return true;
        }
        return GetLastError() == ERROR_IO_INCOMPLETE;
    }

    std::wstring exeDir_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE readEvent_ = nullptr;
    HANDLE writeEvent_ = nullptr;
    OVERLAPPED readOverlapped_ = {};
    OVERLAPPED writeOverlapped_ = {};
    bool readPending_ = false;
    bool writePending_ = false;
    std::string readBuffer_;
    std::string outbound_; // まだ WriteFile へ渡していない
    std::string writing_;  // WriteFile へ渡した (完了待ちを含む)
};

} // namespace

std::unique_ptr<TrackIo> CreateWin32TrackIo(const std::wstring& exeDir)
{
    return std::make_unique<Win32TrackIo>(exeDir);
}

} // namespace mye
