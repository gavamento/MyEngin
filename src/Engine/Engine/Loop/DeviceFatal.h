//====================================================================================
//                          DeviceFatal.h
//  MyEngine/ 秋田蓮音                                                      10/07/2026
//                                          デバイス消失で続行できないときの理由表示
//====================================================================================
#pragma once
#include <string>

#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

// 致命停止の通知文 (Tr の日英。HRESULT は 16 進) を組み立てる。
// detail: 呼び出し側が末尾へ足す文 (退避先など)。空なら足さない
std::wstring BuildDeviceFatalMessage(const DeviceFatalInfo& info, const std::wstring& detail);

// 通知文をエラーログへ出し、info.interactive ならメッセージボックスも出す (閉じるまで戻らない)。
// hwnd: 所有窓 (Win32 の HWND。null 可)
void ReportDeviceFatal(void* hwnd, const DeviceFatalInfo& info, const std::wstring& detail);

} // namespace mye
