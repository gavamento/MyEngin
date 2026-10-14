//====================================================================================
//                          HostServerProcess.h
//  MyEngin/ 秋田蓮音                                                     10/14/2026
//                                          オンラインのホストが自分の PC で動かす Server.exe (子プロセス)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

namespace mye {

// ホストの Server.exe に渡す起動条件。決定論に効くオプションはホスト自身の EngineConfig と一致させる
// (サーバが Hello の configBits を照合して、食い違えば拒否する)
struct HostServerLaunch {
    std::wstring serverExe;     // 既定は Runtime / Editor と同じフォルダの Server.exe
    std::wstring projectRoot;   // 空 = レガシー起動 (エンジンの assets)
    std::wstring sceneArgs;     // 起動シーンの引数 ("--scene \"x\"" / "--local-demo")。空 = 既定の解決 (Runtime と同じ)
    std::wstring logPath;       // サーバの stdout / stderr の行き先
    uint16_t port = 0;          // 127.0.0.1 の UDP ポート (FindFreeLoopbackPort で選ぶ)
    uint32_t maxPlayers = 4;
    int inputDelay = 3;
    bool synthInput = false;
    bool useJobs = true;
    bool useSimCache = true;
    bool useCookCache = true;
    bool computeForceCpu = false;
};

// ★ホストの Runtime が落ちても Server.exe が残らないよう、子はジョブオブジェクト (閉じたら道連れ) に入れる。
// ★サーバは --loopback で 127.0.0.1 にだけ bind する。外の参加者のパケットはホストの Runtime が
//   オンラインの P2P から受けて中継するので、サーバのポートを外へ開ける必要がない
class HostServerProcess {
public:
    HostServerProcess() = default;
    ~HostServerProcess();
    HostServerProcess(const HostServerProcess&) = delete;
    HostServerProcess& operator=(const HostServerProcess&) = delete;

    bool Start(const HostServerLaunch& launch);
    // 終了していたら true (exitCode を書く)。動いていない (Start 前 / Stop 後) も true で exitCode = 0
    bool Exited(uint32_t& exitCode);
    // 強制終了してハンドルを閉じる。全員が抜けたあとは --exit-when-empty で自分から終わっているので、
    // ここは残っていた場合の後始末
    void Stop();
    bool Running();
    uint16_t Port() const { return port_; }

private:
    void* process_ = nullptr;
    void* job_ = nullptr;
    uint16_t port_ = 0;
};

// 127.0.0.1 で空いている UDP ポートを 1 つ選ぶ (一度開いて閉じる)。失敗で 0。
// 閉じてからサーバが開くまでの間に他が取る可能性はゼロではないが、そのときはサーバの起動失敗として見える
uint16_t FindFreeLoopbackPort();

// 実行ファイルと同じフォルダの Server.exe
std::wstring DefaultServerExePath();

} // namespace mye
