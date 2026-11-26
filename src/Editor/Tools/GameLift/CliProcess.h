//====================================================================================
//                          CliProcess.h
//  MyEngin/ 秋田蓮音                                                       11/27/2026
//                                          外部 CLI を 1 本走らせて出力をメモリへ取る子プロセス実行器
//====================================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mye {

// 環境変数 1 つ (名前 = 値)。値が空でも「空で定義する」意味になる
struct CliEnvVar
{
    std::wstring name;
    std::wstring value;
};

// 1 回の実行の入力。exePath は絶対パスでも "cmd.exe" のような裸の名前でもよい (裸の名前は CreateProcess の探索規則)
struct CliRunSpec
{
    std::wstring exePath;
    std::vector<std::wstring> args;
    std::vector<CliEnvVar> extraEnv; // 親の環境 + 固定 3 つ の上に重ねる。同名は後勝ち
    std::wstring workDir;            // 空 = エディタと同じ
    uint32_t timeoutMs = 0;          // 0 = なし
    bool secret = false;             // 出力に秘密を含む。ログへ出さず、使った後 ClearOutput で消す
};

enum class CliRunState
{
    Idle,
    Running,
    Finished,    // 子が自分で終わった (終了コードは ExitCode())
    TimedOut,    // タイムアウトで終了させた
    Cancelled,   // Cancel() で終了させた
    StartFailed, // 起動できなかった (理由は Start の error)
};

// 配列 → Windows のコマンドライン文字列。CommandLineToArgvW で元の配列へ戻る引用規則
std::wstring BuildCommandLine(const std::wstring& exePath, const std::vector<std::wstring>& args);

// 子へ必ず足す環境 (PYTHONIOENCODING / AWS_CLI_FILE_ENCODING / AWS_PAGER=空)
std::vector<CliEnvVar> FixedCliEnvironment();

// エディタ自身の環境変数を読む (書き換えはしない)
std::vector<CliEnvVar> ReadParentEnvironment();

// 親の環境 + 上書き列 → CreateProcess 用の環境ブロック (名前の大文字小文字を無視して後勝ち、名前順、末尾は二重の NUL)
std::wstring BuildEnvironmentBlock(const std::vector<CliEnvVar>& parent, const std::vector<CliEnvVar>& overrides);

// 子プロセスを 1 本走らせ、stdout / stderr を別々にメモリへ取る。
// Start は UI スレッドから呼んでよい (CreateProcess だけで待たない)。Poll を毎フレーム呼んで終了を見る。
// 読み取りは stdout / stderr それぞれの専用スレッド (子が大量に書いても詰まらない)。
// 子は Job Object に入れる: タイムアウト・取消・破棄で孫まで止め、孫がパイプを握り続けて読み取りが終わらない事態を避ける。
// 同時に走らせるのは 1 本。終わってから Start し直すと再利用できる
class CliProcess
{
public:
    CliProcess() = default;
    ~CliProcess();
    CliProcess(const CliProcess&) = delete;
    CliProcess& operator=(const CliProcess&) = delete;

    // 起動する。実行中は false。失敗なら error に理由を入れて StartFailed
    bool Start(const CliRunSpec& spec, std::string& error);

    // 待たずに状態を進める。Running 以外が返ったら出力は確定している
    CliRunState Poll();

    // 実行中なら子 (と孫) を終了させる。結果の確定は Poll で見る。AWS 側で受け付け済みの操作は取り消されない
    void Cancel();

    CliRunState State() const { return state_; }
    bool Running() const { return state_ == CliRunState::Running; }
    uint32_t ExitCode() const { return exitCode_; }
    uint64_t ElapsedMs() const;
    bool Secret() const { return secret_; }

    // 実行中でも呼べる (進捗行の読み取り用)。コピーを返す
    std::string StdoutText() const;
    std::string StderrText() const;

    // 出力を消す (秘密のメモリはゼロで埋めてから)。秘密フラグの呼び出しは結果を使ったら呼ぶ
    void ClearOutput();

    // 出力が上限 (1 本あたり 64 MiB) を超えて切り捨てられたか
    bool Truncated() const { return truncated_.load(); }

private:
    void Terminate();
    void Finalize();
    void ReadLoop(void* pipe, std::string* sink);
    void ReleaseHandles();

    CliRunState state_ = CliRunState::Idle;
    bool secret_ = false;
    bool exited_ = false;
    bool cancelRequested_ = false;
    bool timedOut_ = false;
    uint32_t exitCode_ = 0;
    uint32_t timeoutMs_ = 0;
    uint64_t startTick_ = 0;
    uint64_t exitTick_ = 0;
    uint64_t endTick_ = 0;

    void* process_ = nullptr;
    void* job_ = nullptr;
    void* outRead_ = nullptr;
    void* errRead_ = nullptr;

    mutable std::mutex mutex_; // out_ / err_ を守る
    std::string out_;
    std::string err_;
    std::thread outThread_;
    std::thread errThread_;
    std::atomic<int> readersDone_{0};
    std::atomic<bool> truncated_{false};
};

} // namespace mye
