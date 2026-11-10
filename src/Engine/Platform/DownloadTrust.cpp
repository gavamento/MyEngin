//====================================================================================
//                          DownloadTrust.cpp
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          ダウンロード物の印付けとスキャンの実装 (IAttachmentExecute)
//====================================================================================
#include "Engine/Platform/DownloadTrust.h"

#include <cstdio>
#include <filesystem>

#include <Windows.h>
#include <shobjidl.h>

#include "Engine/Platform/PathUtil.h"

#pragma comment(lib, "ole32.lib")

namespace mye {

namespace {

// Attachment Manager がクライアントごとの設定 (信頼の記憶など) を分けるための識別子。MyEngine の Launcher 固有
// {5B0E6F2A-98D1-4C1B-9E37-6A4C2D8F0B86}
constexpr GUID kClientGuid = { 0x5b0e6f2a, 0x98d1, 0x4c1b, { 0x9e, 0x37, 0x6a, 0x4c, 0x2d, 0x8f, 0x0b, 0x86 } };

constexpr const wchar_t* kZoneStream = L":Zone.Identifier";

std::string HrText(const char* what, HRESULT hr)
{
    char buf[96] = {};
    snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

DownloadTrustResult MarkAndScanDownload(const std::wstring& path, const std::string& sourceUrl)
{
    DownloadTrustResult result;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IAttachmentExecute* ae = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ae));
    if (FAILED(hr)) {
        result.error = HrText("CoCreateInstance(AttachmentServices)", hr);
    } else {
        const std::wstring source = Utf8ToWide(sourceUrl);
        const std::wstring fileName = std::filesystem::path(path).filename().wstring();
        ae->SetClientGuid(kClientGuid);
        ae->SetClientTitle(L"MyEngine Launcher");
        ae->SetLocalPath(path.c_str());
        ae->SetFileName(fileName.c_str());
        ae->SetSource(source.c_str());
        // Save: 印 (ゾーン) を書き、登録されたウイルス対策にスキャンさせる。拒否されると失敗が返り、
        // ファイルは消されていることがある
        hr = ae->Save();
        if (SUCCEEDED(hr)) {
            result.ok = true;
        } else {
            result.blocked = true;
            result.error = HrText("the download was rejected by the security check (IAttachmentExecute::Save)", hr);
        }
        ae->Release();
    }
    if (coInit == S_OK || coInit == S_FALSE) {
        CoUninitialize();
    }
    return result;
}

bool ReadZoneIdentifier(const std::wstring& path, std::string& content)
{
    content.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (path + kZoneStream).c_str(), L"rb") != 0 || !f) {
        return false;
    }
    char buf[1024];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    fclose(f);
    return !content.empty();
}

bool WriteZoneIdentifier(const std::wstring& path, const std::string& content)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, (path + kZoneStream).c_str(), L"wb") != 0 || !f) {
        return false;
    }
    const bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    return fclose(f) == 0 && ok;
}

} // namespace mye
