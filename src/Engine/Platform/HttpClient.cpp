//====================================================================================
//                          HttpClient.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          WinHTTP による HTTPS の取得とダウンロードの実装
//====================================================================================
#include "Engine/Platform/HttpClient.h"

#include <cstdio>
#include <filesystem>

#include <Windows.h>
#include <winhttp.h>

#include "Engine/Platform/PathUtil.h"

#pragma comment(lib, "winhttp.lib")

namespace mye {

namespace {

constexpr size_t kReadChunkBytes = 64 * 1024;

// WinHTTP のハンドルを閉じ忘れないための薄い包み
struct HInternet
{
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET handle) : h(handle) {}
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    ~HInternet()
    {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
};

std::string LastErrorText(const char* what)
{
    const DWORD err = GetLastError();
    char buf[160] = {};
    snprintf(buf, sizeof(buf), "%s failed (Win32 error %lu)", what, static_cast<unsigned long>(err));
    return buf;
}

bool IsCancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

// sink が false を返したら中断する (書き込み失敗・上限超過)
using SinkFn = std::function<bool(const char* data, size_t size, std::string& error)>;

HttpResult Transfer(const std::string& url, const HttpOptions& options, const SinkFn& sink,
                    const HttpProgressFn& progress, const std::atomic<bool>* cancel)
{
    HttpResult result;
    const std::wstring wurl = Utf8ToWide(url);

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.dwExtraInfoLength = 1; // クエリ文字列を lpszUrlPath に含めて受け取るための指定
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        result.error = "invalid URL: " + url;
        return result;
    }
    // 平文の http は受け付けない (更新物の取り違え・差し替えを防ぐ)
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        result.error = "only https:// is allowed: " + url;
        return result;
    }

    const std::wstring agent = Utf8ToWide(options.userAgent);
    // 自動プロキシ (Windows 8.1 以降)。使えない環境では既定のプロキシ設定へ落とす
    HInternet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        result.error = LastErrorText("WinHttpOpen");
        return result;
    }
    const int connectMs = static_cast<int>(options.connectTimeoutMs);
    const int receiveMs = static_cast<int>(options.receiveTimeoutMs);
    WinHttpSetTimeouts(session.h, connectMs, connectMs, receiveMs, receiveMs);

    HInternet connection(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connection.h) {
        result.error = LastErrorText("WinHttpConnect");
        return result;
    }
    HInternet request(WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.h) {
        result.error = LastErrorText("WinHttpOpenRequest");
        return result;
    }
    std::wstring headers;
    for (const std::string& h : options.headers) {
        headers += Utf8ToWide(h);
        headers += L"\r\n";
    }
    const BOOL sent = WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                         headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0,
                                         0, 0);
    if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
        result.error = LastErrorText("HTTP request");
        return result;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    if (status < 200 || status >= 300) {
        result.error = "HTTP status " + std::to_string(status);
        return result;
    }

    // Content-Length は 4GB を超えうるので文字列で受ける
    uint64_t total = 0;
    wchar_t lengthText[32] = {};
    DWORD lengthSize = sizeof(lengthText);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                            &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
        total = _wcstoui64(lengthText, nullptr, 10);
    }

    std::string chunk(kReadChunkBytes, '\0');
    uint64_t done = 0;
    for (;;) {
        if (IsCancelled(cancel)) {
            result.cancelled = true;
            result.error = "cancelled";
            return result;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
            result.error = LastErrorText("WinHttpReadData");
            return result;
        }
        if (read == 0) {
            break;
        }
        if (!sink(chunk.data(), read, result.error)) {
            return result;
        }
        done += read;
        if (progress) {
            progress(done, total);
        }
    }
    if (total != 0 && done != total) {
        result.error = "connection closed early (" + std::to_string(done) + " / " + std::to_string(total) + " bytes)";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace

HttpResult HttpGetText(const std::string& url, const HttpOptions& options, std::string& body, size_t maxBytes,
                       const std::atomic<bool>* cancel)
{
    body.clear();
    const SinkFn sink = [&](const char* data, size_t size, std::string& error) {
        if (body.size() + size > maxBytes) {
            error = "response too large (limit " + std::to_string(maxBytes) + " bytes)";
            return false;
        }
        body.append(data, size);
        return true;
    };
    HttpResult r = Transfer(url, options, sink, HttpProgressFn{}, cancel);
    if (!r.ok) {
        body.clear();
    }
    return r;
}

HttpResult HttpDownloadFile(const std::string& url, const std::wstring& path, const HttpOptions& options,
                            const HttpProgressFn& progress, const std::atomic<bool>* cancel)
{
    HttpResult r;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        r.error = "cannot open for writing: " + WideToUtf8(path);
        return r;
    }
    const SinkFn sink = [&](const char* data, size_t size, std::string& error) {
        if (fwrite(data, 1, size, f) != size) {
            error = "write failed: " + WideToUtf8(path);
            return false;
        }
        return true;
    };
    r = Transfer(url, options, sink, progress, cancel);
    // 書き出しの失敗は close で初めて出ることがある
    if (fclose(f) != 0 && r.ok) {
        r.ok = false;
        r.error = "write failed: " + WideToUtf8(path);
    }
    if (!r.ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    return r;
}

} // namespace mye
