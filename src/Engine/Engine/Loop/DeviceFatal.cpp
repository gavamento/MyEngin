//====================================================================================
//                          DeviceFatal.cpp
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失で続行できないときの理由表示
//====================================================================================
#include "Engine/Engine/Loop/DeviceFatal.h"

#include <cstdio>

#include <Windows.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::wstring HexResult(int32_t value)
{
    wchar_t text[16] = {};
    swprintf_s(text, L"0x%08X", static_cast<unsigned int>(value));
    return text;
}

} // namespace

std::wstring BuildDeviceFatalMessage(const DeviceFatalInfo& info, const std::wstring& detail)
{
    // Tr() の結果は書式文字列にせず、文字列として連結する
    std::wstring message = Utf8ToWide(Tr(StrId::DevLost_Reason));
    if (info.simulated) {
        message += L"\n" + Utf8ToWide(Tr(StrId::DevLost_Simulated));
    }
    message += L"\n\n" + Utf8ToWide(Tr(StrId::DevLost_PresentLabel)) + L": " + HexResult(info.presentHr);
    message += L"\n" + Utf8ToWide(Tr(StrId::DevLost_RemovedLabel)) + L": " + HexResult(info.removedReason);
    if (!detail.empty()) {
        message += L"\n\n" + detail;
    }
    return message;
}

void ReportDeviceFatal(void* hwnd, const DeviceFatalInfo& info, const std::wstring& detail)
{
    const std::wstring message = BuildDeviceFatalMessage(info, detail);
    MYE_LOG_ERROR("[device] fatal: %s", WideToUtf8(message).c_str());
    if (!info.interactive) {
        return; // CI / 撮影 / replay の実行でダイアログを出すと止まる
    }
    const std::wstring title = Utf8ToWide(Tr(StrId::DevLost_Title));
    MessageBoxW(static_cast<HWND>(hwnd), message.c_str(), title.c_str(),
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace mye
