#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mye {

// クラッシュバンドル (M52f)。
//
// 落ちた瞬間に「再現可能なバグ報告」を 1 フォルダへ残す:
//   crash\<yyyyMMdd_HHmmss>\
//     minidump.dmp  … MiniDumpWriteDump (スタックと積んでいるモジュール)
//     crash.txt     … 例外コード / 障害モジュール + RVA / exe TimeDateStamp / 構成 /
//                     git ハッシュ / 現 tick / 起動コマンドライン / 直近ログ
//     crash.rep     … 埋め込みスナップショット付きの .rep (CrashRing が書く)
//     scene.json    … 元シーンのコピー (ファイル由来のときだけ)
//
// ★**ハンドラの中では新規確保もロック取得もしない**。落ちる直前のヒープは壊れている
//   かもしれないし、落ちたスレッドがロックを握ったまま来ることもある。そこで malloc や
//   std::mutex を踏むと、報告が 1 バイトも残らないまま二重フォルトして終わる。
//   → 出力に使うバッファは Install で全部確保しておき、書式化は crashfmt の
//     固定バッファ Sink だけを使う (CRT の printf 系はロケール経由でヒープを触りうる)。
//   → dbghelp.dll も Install 時に解決しておく (ハンドラ内の LoadLibrary はローダロック)。
//
// ★捕まえるのは 4 経路: SEH (SetUnhandledExceptionFilter) / std::terminate /
//   純粋仮想呼び出し (_purecall) / CRT 不正パラメータ。どれも「既定だと WER ダイアログか
//   無言終了」で、配布ビルドでは手掛かりが何も残らない。

// 固定バッファ書式化 (クラッシュハンドラ専用)。
// 溢れた分は捨てて truncated を立てる — 書けたところまでは必ず終端付きの有効な文字列。
namespace crashfmt {

template <typename C>
class TextSink {
public:
    TextSink(C* buf, size_t cap) : buf_(buf), cap_(cap)
    {
        if (cap_ > 0) {
            buf_[0] = static_cast<C>(0);
        }
    }

    void Ch(C c)
    {
        if (len_ + 1 >= cap_) {
            truncated_ = true;
            return;
        }
        buf_[len_++] = c;
        buf_[len_] = static_cast<C>(0);
    }

    // 同型の文字列 (wchar_t Sink なら wchar_t 列)
    void Str(const C* s)
    {
        if (s == nullptr) {
            return;
        }
        while (*s != static_cast<C>(0)) {
            Ch(*s++);
        }
    }

    // ASCII リテラルをそのまま流す (wchar_t Sink へも使える)。非 ASCII は渡さないこと
    void Ascii(const char* s)
    {
        if (s == nullptr) {
            return;
        }
        while (*s != '\0') {
            Ch(static_cast<C>(static_cast<unsigned char>(*s)));
            ++s;
        }
    }

    void U64(uint64_t v, int minDigits = 0)
    {
        C tmp[24];
        int n = 0;
        do {
            tmp[n++] = static_cast<C>('0' + static_cast<int>(v % 10));
            v /= 10;
        } while (v != 0 && n < 20);
        while (n < minDigits && n < 20) {
            tmp[n++] = static_cast<C>('0');
        }
        while (n > 0) {
            Ch(tmp[--n]);
        }
    }

    void Hex(uint64_t v, int digits)
    {
        if (digits < 1) {
            digits = 1;
        }
        if (digits > 16) {
            digits = 16;
        }
        for (int i = digits - 1; i >= 0; --i) {
            const int nib = static_cast<int>((v >> (i * 4)) & 0xF);
            Ch(static_cast<C>(nib < 10 ? ('0' + nib) : ('A' + nib - 10)));
        }
    }

    size_t Length() const { return len_; }
    bool Truncated() const { return truncated_; }
    const C* Text() const { return buf_; }
    void Clear()
    {
        len_ = 0;
        truncated_ = false;
        if (cap_ > 0) {
            buf_[0] = static_cast<C>(0);
        }
    }

private:
    C* buf_ = nullptr;
    size_t cap_ = 0;
    size_t len_ = 0;
    bool truncated_ = false;
};

using Sink = TextSink<char>;
using WSink = TextSink<wchar_t>;

} // namespace crashfmt

// バンドルへ追加のファイルを書く差し込み口 (crash.rep / scene.json)。
// ★**クラッシュハンドラの中から呼ばれる** — 新規確保・ロック・重い CRT は禁止。
//   事前に組み上げておいたバイト列を WriteFile するだけ、が想定用法 (CrashRing が実例)。
using CrashPayloadFn = void (*)(void* user, const wchar_t* bundleDir);

struct CrashHandlerConfig {
    std::wstring crashRoot; // <crashRoot>\crash\<timestamp>\ へ吐く (プロジェクトルート or exe 隣)
    std::wstring appName;   // "Editor" / "Runtime" (crash.txt の見出し)
    // ハンドラが読む現在値。EngineLoop の ctx が生きている限り有効なポインタを渡す
    const uint64_t* tickIndex = nullptr;
    const uint64_t* frameIndex = nullptr;
    // 走っていたシーンのパス (crash.txt に出す)。**固定バッファを指すこと** —
    // ハンドラ内で std::wstring を触ると再確保済みの領域を掴む事故になる
    const wchar_t* sceneLabel = nullptr;
    CrashPayloadFn payload = nullptr;
    void* payloadUser = nullptr;
};

// 4 経路のハンドラを設置する。2 回目以降の Install は設定の差し替えとして扱う
void InstallCrashHandler(const CrashHandlerConfig& config);
void UninstallCrashHandler();

// ---- 意図的に落とすテスト経路 (--crash-test) ----
// ハンドラ自体が「落ちたときに本当に動くか」は、実際に落として確かめるしかない。
enum class CrashTestKind : uint8_t {
    None = 0,
    AccessViolation, // SEH (0xC0000005)
    PureCall,        // _purecall
    Terminate,       // std::terminate
    InvalidParam,    // CRT 不正パラメータ
    // ★スタックオーバーフロー。ハンドラに残るスタックが 1 ページ程度しか無い最悪ケースで、
    //   「作業領域をスタックへ置かない」という本実装の中心的な前提を実地で検証する唯一の経路
    StackOverflow,
};
CrashTestKind ParseCrashTestKind(const wchar_t* s);
const char* CrashTestKindName(CrashTestKind kind);
void TriggerTestCrash(CrashTestKind kind);

// 生バイト列をファイルへ書く (CreateFileW + WriteFile のみ)。
// CrashPayloadFn の実装が使うためにここに置いてある — ハンドラ内から安全に呼べる唯一の書き出し
bool CrashWriteFileRaw(const wchar_t* path, const void* data, size_t size);

} // namespace mye
