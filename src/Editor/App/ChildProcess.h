#pragma once
#include <cstdint>
#include <string>

namespace mye {

// 子プロセス (ビルド bat / tar など) を窓なし (CREATE_NO_WINDOW) で起動する。
// stdout と stderr は logPath へ、stdin は NUL へ繋ぐ。
// ★stdin を NUL にするのは、bat の失敗系 `pause` が EOF を読んで即抜ける = エディタが詰まらないため。
// 戻り値はプロセスハンドル (失敗で nullptr。失敗は "<logTag> CreateProcess failed<forWhat> (err)" でログ)。
// ハンドルは呼び出し側が PollChildProcess で終了を見てから CloseChildProcess で閉じる
void* StartChildProcess(const std::wstring& cmdline, const std::wstring& workDir, const std::wstring& logPath,
                        const char* logTag, const char* forWhat);

// 終了済みなら true を返して exitCode を書く (実行中なら false)。ハンドルは閉じない
bool PollChildProcess(void* process, uint32_t& exitCode);

// ハンドルを閉じる (nullptr は何もしない)
void CloseChildProcess(void* process);

} // namespace mye
