//====================================================================================
//                          TerminalHost.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルの子プロセスを動かす口 (ConPTY の実装と試験用の偽物で差し替える)
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Editor/Tools/Terminal/TerminalEnvironment.h"

namespace mye {

// 起動する子の指定。文字列はすべて UTF-8。引数の引用は実装側 (QuoteCommandLineArg) が行う
struct TerminalLaunchSpec
{
    std::string exePath;                    // 絶対パス (ResolveTerminalShell の結果)
    std::vector<std::string> args;          // 引用前の生の値
    std::string workDir;                    // 空なら親の作業ディレクトリ
    std::vector<TerminalEnvVar> environment; // BuildTerminalEnvironment の結果。空なら親の環境をそのまま継がせる
};

enum class TerminalHostStartResult
{
    Started,
    Unsupported,   // ConPTY の API が無い (Windows 10 1809 より前など)。再試行しても変わらない
    Failed,        // パイプ・疑似コンソール・プロセスの作成に失敗
};

// TerminalSession が子プロセスに触る口を全部ここに集める。試験は偽物に差し替える。
// Start / Write / Resize / PollExit / Close / ReadOutput は UI スレッドから呼ぶ。
// 子の出力は実装内部の読み取りスレッドが溜め、ReadOutput が取り出す
class TerminalHost
{
public:
    virtual ~TerminalHost() = default;

    // 子を cols x rows の疑似端末で起動する。起動中 (Close 前) に呼ばない。
    // 失敗時は errorOut に人が読む理由 (英語、ログ・表示用)
    virtual TerminalHostStartResult Start(const TerminalLaunchSpec& spec, int cols, int rows, std::string& errorOut) = 0;
    // 溜まった出力を最大 maxBytes まで out の末尾へ移し、移したバイト数を返す。Close 後も残りを読める
    virtual size_t ReadOutput(std::string& out, size_t maxBytes) = 0;
    // 子の入力へ書く (同期)。起動していなければ false
    virtual bool Write(std::string_view bytes) = 0;
    // 疑似端末の大きさを変える。起動していなければ false
    virtual bool Resize(int cols, int rows) = 0;
    // 子が終わっていれば true と終了コード。起動していない・終わっていなければ false
    virtual bool PollExit(uint32_t& exitCode) = 0;
    // 疑似端末を閉じ、読み取りスレッドを止め、ハンドルを放す。子孫が残っていれば終わらせる。何度呼んでもよい。
    // keepPendingOutput: true なら閉じる間に届いた最後の出力も ReadOutput で読めるように残す (子が自分で終わったとき)。
    //                    false なら捨てる (破棄・再起動)
    virtual void Close(bool keepPendingOutput) = 0;
};

// ConPTY の解決方法。Missing は「kernel32 に関数が無い」状態を作る試験用 (関数ポインタを null にする)
enum class TerminalConPtyApiSource
{
    System,
    Missing,
};

// ConPTY で子を動かすホストを作る (実装は Win32 の TerminalConPtyHost.cpp に閉じる)
std::unique_ptr<TerminalHost> CreateConPtyTerminalHost(TerminalConPtyApiSource apiSource = TerminalConPtyApiSource::System);

// TerminalExecutableFinder の Win32 実装: 名前またはパスを SearchPathW (lpPath = null の既定の探索順。System32 と PATH を含む) で探し、
// 見つかれば絶対パス (UTF-8)、無ければ空を返す
std::string FindTerminalExecutable(const std::string& nameOrPath);

} // namespace mye
