//====================================================================================
//                          TerminalVsEnv.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          VS 開発者環境の取得 (set 出力の解析・差分・非同期の読み込み) とエンジン環境変数の組み立て
//====================================================================================
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Editor/Tools/Terminal/TerminalEnvironment.h"

namespace mye {

// ---- cmd.exe の `set` 出力の解析・差分 (Win32 非依存の純関数) ----

// VsDevCmd.bat の後に標準出力へ出す目印の行。これより前 (バナー・警告) は環境変数として読まない。
// `echo` は行末に空白が付きうるので、比較は行末の空白と CR を落としてから行う
inline constexpr std::string_view kTerminalVsEnvMarker = "MYE_TERMINAL_VSENV_BEGIN";

// `set` の出力 (UTF-8) を NAME=VALUE の並びにする。marker が空でなければ、その行より後だけを読む (見つからなければ false で out は空)。
// 行は LF / CRLF 区切り。値は 1 文字目以外の最初の '=' より後ろ全部 (値に '=' を含んでよい。空値も可)。
// 名前が '=' で始まる隠し変数 ("=C:" "=ExitCode") は 2 文字目以降の最初の '=' で区切って残す。'=' を含まない行・名前が空の行は捨てる。
// 同名 (大文字小文字無視) が重なれば先に出た方を残す。バイト列は変換しない (UTF-8 の値はそのまま)
bool ParseTerminalSetOutput(std::string_view output, std::string_view marker, std::vector<TerminalEnvVar>& out);

// vsEnv (VsDevCmd 後の環境) のうち、parent (親環境) に無い変数と値が違う変数を、vsEnv の綴りと値で vsEnv の順に返す。
// 名前の比較は大文字小文字を無視。値は完全一致。'=' で始まる名前は cmd のプロセスごとの状態なので含めない。
// vsEnv にあって parent に無い「消えた」変数は扱わない (VsDevCmd は変数を消さない)
std::vector<TerminalEnvVar> ComputeTerminalEnvironmentDiff(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& vsEnv);

// parent に diff を重ねる。同名 (大文字小文字無視) は親の名前の綴りのまま値を差し替え、親に無いものは末尾へ足す
std::vector<TerminalEnvVar> ApplyTerminalEnvironmentDiff(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& diff);

// ---- エンジンの環境変数 (spec 4.2) ----

inline constexpr std::string_view kTerminalEnvProjectRoot = "MYE_PROJECT_ROOT";
inline constexpr std::string_view kTerminalEnvTerminalId = "MYE_TERMINAL_ID";
inline constexpr std::string_view kTerminalEnvTermProgram = "TERM_PROGRAM";
inline constexpr std::string_view kTerminalEnvColorTerm = "COLORTERM";
inline constexpr std::string_view kTerminalTermProgramValue = "MyEngine";
inline constexpr std::string_view kTerminalColorTermValue = "truecolor";
// PATH の先頭へ足す、プロジェクトルート直下のフォルダ名
inline constexpr std::string_view kTerminalToolsDirName = "tools";

// MYE_TERMINAL_ID の値: "<エディタの PID>-<エディタの起動時刻 (UNIX 秒)>-<シェル起動の連番>"
std::string FormatTerminalId(uint32_t editorPid, uint64_t editorStartUnixSeconds, int serial);

// id が、この PID・起動時刻のエディタが FormatTerminalId で作ったものか ("<PID>-<起動時刻>-" で始まり、連番が 10 進の数字だけ)。
// AI Changes 窓が「このエディタの統合ターミナルで始まったセッション」を見分けるのに使う。連番は見ない (シェルを再起動すると増えるが、
// 同じエディタの同じ窓の別シェルとして扱いたいため)
bool TerminalIdBelongsToEditor(std::string_view id, uint32_t editorPid, uint64_t editorStartUnixSeconds);

// 子プロセスへ渡す環境の全体。parent (親環境) → vsDiff (VS 開発者環境の差分。空なら無し) の順に重ね、
// MYE_TERMINAL_ID / TERM_PROGRAM / MYE_PROJECT_ROOT (projectRoot が空でなければ) は親の値があっても自分の値で置き換え、
// COLORTERM は親に無いときだけ足し、PATH の先頭へ "<projectRoot>\tools;" を前置する (BuildTerminalEnvironment の規則で整列済み)。
// MYE_* と TERM_PROGRAM を置き換えるのは、エディタを別のターミナル (別の MyEngine のターミナル・VS Code) から起動したとき、
// 親の値が子に残って「どのターミナルの子か」を誤らせないため。projectRoot が空 or ';' を含む (PATH に安全に書けない) ときは前置しない
std::vector<TerminalEnvVar> BuildTerminalLaunchEnvironment(const std::vector<TerminalEnvVar>& parent, const std::vector<TerminalEnvVar>& vsDiff,
                                                           std::string_view projectRoot, std::string_view terminalId);

// ---- VS 開発者環境の非同期読み込み (Win32 非依存の状態機械。実処理は fetch として差し込む) ----

// 読み込み全体の待ち時間の上限 (spec 2: 30 秒で諦めて素の環境)
inline constexpr std::chrono::milliseconds kTerminalVsEnvTimeout{ 30000 };

enum class TerminalVsEnvStatus
{
    Ok,              // 差分を取れた (差分が空でもよい: 既に VS の環境で起動されていた)
    NotInstalled,    // vswhere が VS を返さない / VsDevCmd.bat が無い
    Failed,          // 起動・実行・解析の失敗
    TimedOut,        // 時間切れ
};

struct TerminalVsEnvFetchResult
{
    TerminalVsEnvStatus status = TerminalVsEnvStatus::Failed;
    std::vector<TerminalEnvVar> diff;   // Ok のときだけ有効
    std::string detail;                 // 人が読む理由 (英語。ログ・ツールチップ用)
};

// ワーカースレッドで呼ばれる。cancel が立ったら速やかに戻ること (呼び出し側が時間切れにした / エディタ終了)
using TerminalVsEnvFetch = std::function<TerminalVsEnvFetchResult(const std::atomic<bool>& cancel)>;

enum class TerminalVsEnvState
{
    Idle,           // まだ始めていない
    Loading,
    Ready,          // Diff() が使える
    NotInstalled,
    Failed,
    TimedOut,
};

// VS 開発者環境の差分を、エディタ起動中 1 回だけワーカースレッドで取る。所有者は Terminal 窓 (UI スレッド)。
// Start / Poll / State / Diff は UI スレッドだけが呼ぶ。決まった結果 (Ready / NotInstalled / Failed / TimedOut) はやり直さない。
// 時間切れ後に fetch が遅れて成功しても採用しない (状態が後から変わると、起動済みのシェルと表示が食い違うため)。
// 破棄時は cancel を立て、shutdownWait だけ終了を待つ。それでも戻らなければ (vswhere が固まったなど) スレッドを切り離して先へ進む
// (スレッドが触るのは shared_ptr で共有した状態だけなので、遅れて終わっても安全)
class TerminalVsEnvLoader
{
public:
    using Clock = std::chrono::steady_clock;

    explicit TerminalVsEnvLoader(TerminalVsEnvFetch fetch, std::chrono::milliseconds timeout = kTerminalVsEnvTimeout,
                                 std::chrono::milliseconds shutdownWait = std::chrono::milliseconds(2000));
    ~TerminalVsEnvLoader();
    TerminalVsEnvLoader(const TerminalVsEnvLoader&) = delete;
    TerminalVsEnvLoader& operator=(const TerminalVsEnvLoader&) = delete;

    // Idle のときだけスレッドを起こして Loading にする (それ以外は何もしない)。now から timeout で時間切れ
    void Start(Clock::time_point now);
    // 毎フレーム呼ぶ。完了していれば結果を取り込む。終わっていなくて now が期限以降なら cancel を立てて TimedOut にする。
    // 完了と期限が同時なら完了を優先する
    TerminalVsEnvState Poll(Clock::time_point now);

    TerminalVsEnvState State() const { return state_; }
    // Ready のときだけ意味がある
    const std::vector<TerminalEnvVar>& Diff() const { return diff_; }
    const std::string& Detail() const { return detail_; }

private:
    struct Shared
    {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        TerminalVsEnvFetchResult result;
        std::atomic<bool> cancel{ false };
    };

    TerminalVsEnvFetch fetch_;
    std::chrono::milliseconds timeout_;
    std::chrono::milliseconds shutdownWait_;
    TerminalVsEnvState state_ = TerminalVsEnvState::Idle;
    Clock::time_point deadline_;
    std::shared_ptr<Shared> shared_;
    std::thread worker_;
    std::vector<TerminalEnvVar> diff_;
    std::string detail_;
};

// ---- Win32 で実装するもの (TerminalVsEnvWin32.cpp。試験は偽物に差し替える) ----

// 親 (エディタ自身) の環境変数。GetEnvironmentStringsW の順。名前が '=' で始まる隠し変数も含む
std::vector<TerminalEnvVar> GetTerminalParentEnvironment();

// エディタの PID と起動時刻 (UNIX 秒)。MYE_TERMINAL_ID の材料
void GetTerminalEditorIdentity(uint32_t& pid, uint64_t& startUnixSeconds);

// vswhere で最新の VS を探し、`cmd /s /c ""<VsDevCmd.bat>" -arch=x64 -no_logo && echo <目印>&& set"` を非表示で実行して、
// 親環境との差分を返す。ワーカースレッドから呼ぶ (UI スレッドで呼ばない)。cancel が立てば子を殺して戻る
TerminalVsEnvFetchResult FetchTerminalVsDevEnvironment(const std::atomic<bool>& cancel);

} // namespace mye
