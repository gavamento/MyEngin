//====================================================================================
//                          ServerProcess.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          Server.exe を 1 本起動して監視する子プロセス (ログはファイル、停止は名前付きイベント)
//====================================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "Editor/Tools/GameLift/CliProcess.h"

namespace mye {

// 起動の入力。トークンなどの秘密は extraEnv だけに入れる (args に入れない)
struct ServerLaunchSpec
{
    std::wstring exePath;
    std::vector<std::wstring> args;
    std::vector<CliEnvVar> extraEnv; // 親の環境の上に重ねる。同名は後勝ち
    std::wstring workDir;            // 空 = エディタと同じ
    std::wstring logPath;            // stdout / stderr の向け先 (親フォルダは作る。毎回作り直す)
    std::wstring stopEventName;      // 空 = 作らない (停止は強制終了だけになる)

    // 秘密を含む extraEnv の値をゼロで埋めて空にする (起動の後で呼ぶ)
    void WipeSecrets();
};

enum class ServerPhase
{
    NotStarted,
    Starting, // 起動した。ProcessReady はまだ
    Ready,    // ログに "ProcessReady sent" が出た (セッションの割り当て待ち)
    Stopping, // 停止要求を出した (猶予の間)
    Exited,   // 終わった (終了コードとログ末尾は View)
};

struct ServerView
{
    ServerPhase phase = ServerPhase::NotStarted;
    uint32_t exitCode = 0;
    bool forcedKill = false;               // 猶予内に終わらず TerminateProcess した
    std::vector<std::string> logTail;      // ログの末尾 (最大 kServerLogTailLines 行。UTF-8)
    std::wstring logPath;

    // Starting / Ready / Stopping = まだ動いている
    bool Active() const
    {
        return phase == ServerPhase::Starting || phase == ServerPhase::Ready || phase == ServerPhase::Stopping;
    }
};

constexpr size_t kServerLogTailLines = 8;

// ログに出る目印 (GameLiftHosting の「ProcessReady sent」の行)
constexpr char kServerReadyMarker[] = "ProcessReady sent";

// Server.exe の子プロセス。CliProcess と違い stdout / stderr を**ファイル**へ向ける (長く動き、出力が増え続けるため)。
// 作りは CliProcess::Start と同じ: 環境ブロックは BuildEnvironmentBlock、継承ハンドルは HANDLE_LIST で絞り、
// Job (KILL_ON_JOB_CLOSE) に入れる (エディタが落ちたら道連れ)。
// 毎フレーム Poll でログを読み、終了を見る。UI スレッドから使い、他スレッドは持たない
class ServerProcess
{
public:
    ServerProcess() = default;
    ~ServerProcess();
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // 起動する。動いている間は false。stopEventName があれば、起動の前に名前付きイベントを作る
    bool Start(const ServerLaunchSpec& spec, std::string& error);

    // 待たずにログを読み、終了と停止の猶予切れを見る
    void Poll();

    // 停止要求: イベントを立て、graceMs の間に終わらなければ Poll が TerminateProcess する。動いていなければ何もしない
    void RequestStop(uint32_t graceMs);

    // 終わるまで待つ停止 (エディタの終了用)。graceMs 待って終わらなければ強制終了
    void StopBlocking(uint32_t graceMs);

    ServerView View() const;

private:
    bool ProcessExited();
    void ReadLog();
    void AppendLogText(const char* data, size_t size);
    void Kill();
    void ReleaseHandles();

    bool started_ = false;
    bool exited_ = false;
    bool ready_ = false;
    bool stopRequested_ = false;
    bool forcedKill_ = false;
    uint32_t exitCode_ = 0;
    uint32_t graceMs_ = 0;
    uint64_t stopTick_ = 0;
    std::wstring logPath_;

    void* process_ = nullptr;
    void* job_ = nullptr;
    void* stopEvent_ = nullptr;
    void* logRead_ = nullptr;

    std::string partialLine_;
    std::deque<std::string> tail_;
};

} // namespace mye
