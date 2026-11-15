//====================================================================================
//                          TrackIo.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡クライアントの I/O の口 (試験で偽物に差し替える)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace mye {

// TrackClient が外の世界に触る口を全部ここに集める。試験は偽物に差し替えて、
// 呼び出し回数・応答の分割・時計を決める (実サービスが無くても配線を検査できる)。
// どの関数も待たない (pipe の読み書きは overlapped、時計以外は 1 回の API 呼び出しで返る)
class TrackIo {
public:
    virtual ~TrackIo() = default;

    enum class OpenResult { Opened, NotFound, Busy, Failed };
    enum class PumpResult { Ok, Closed };
    enum class LaunchResult { Launched, NoExe, Failed };

    // <project>\.mye\ai_changes\service.json の token。読めなければ false
    virtual bool ReadServiceToken(const std::wstring& projectRoot, std::string& tokenOut) = 0;

    // pipe を 1 本開く (開いている間は呼ばない)。NotFound = サービスが居ない、Busy = 空きインスタンスが無い
    virtual OpenResult OpenPipe(const std::string& pipeName) = 0;
    virtual void ClosePipe() = 0;

    // 送信待ちへ積む。積めなければ (上限超え・閉じている) false。書き込みの完了は Pump が進める
    virtual bool Write(const std::string& bytes) = 0;
    // 書き込みを進め、読めたバイトを incoming へ追記する (1 回の呼び出しで読むのは高々 1 チャンク)。
    // 相手が閉じた・壊れたら Closed
    virtual PumpResult Pump(std::string& incoming) = 0;

    // サービスを探して切り離し起動する。NoExe = 見つからない (探し直しは呼び出し側が決める)
    virtual LaunchResult LaunchService(const std::wstring& projectRoot) = 0;

    // 単調な時計 (ミリ秒)。時間切れと再接続の間隔を測る。フレームの時刻は使わない
    virtual int64_t NowMs() = 0;
    // 単調な時計 (マイクロ秒)。Poll() の時間の上限を測る (NowMs は 2 ms の上限には粗い)
    virtual int64_t NowMicros() = 0;
};

// 実物 (Win32 の名前付き pipe と CreateProcess)。exeDir はサービスの exe を探す場所 (Editor の exe の隣)
std::unique_ptr<TrackIo> CreateWin32TrackIo(const std::wstring& exeDir);

} // namespace mye
