//====================================================================================
//                          StopRequest.h
//  MyEngin/ 秋田蓮音                                                     11/27/2026
//                                          サーバの停止要求の受け口 (コンソールの終了操作 + 名前付きイベント)
//====================================================================================
#pragma once
#include <string>

#include <Windows.h>

namespace mye {

// 外から来る「止めてほしい」を 1 か所で受ける。ホスティングの種類によらず ServerLoop が Poll と同じ
// タイミング (tick 境界) で Requested() を見て、ホスティングの Terminate と同じ経路へ入れる。
// 受け口は 2 つ:
//   - コンソールの Ctrl+C / Ctrl+Break / ウィンドウを閉じる (InstallConsoleHandler)
//   - 名前付きイベント (OpenEvent。エディタなどコンソールを持たない親が立てる)
// コンソールのハンドラはプロセスに 1 つなので、同時に生きる StopRequest でハンドラを入れるのは 1 つだけ
class StopRequest {
public:
    StopRequest() = default;
    ~StopRequest();
    StopRequest(const StopRequest&) = delete;
    StopRequest& operator=(const StopRequest&) = delete;

    // コンソールハンドラを入れる (戻り値 false = 入らなかった。その場合 Ctrl+C は即時終了のまま)
    bool InstallConsoleHandler();
    // 既存の名前付きイベントを開く。無ければ false (作らない: 開けない = 呼び出し側の誤り)
    bool OpenEvent(const std::wstring& name);
    // どちらかが立っているか。ブロックしない。一度立ったら立ちっぱなし
    bool Requested() const;
    // 検証用: 停止要求を立てる
    void Request();

private:
    HANDLE event_ = nullptr;
    bool handlerInstalled_ = false;
    mutable bool latched_ = false; // メインスレッドだけが触る (イベントは一度見えたら保持する)
};

} // namespace mye
