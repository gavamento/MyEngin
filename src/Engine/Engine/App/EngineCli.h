#pragma once
#include <string>

namespace mye {

struct EngineConfig;

// 両 Main (Editor / Runtime) が**同じ意味で**受け取る CLI フラグのうち、EngineConfig の外に置く値。
// Main が起動の前に使う (クラッシュ試験の種類の検査 / 差分ツールとして動いて終了する)
struct EngineCliExtras {
    std::wstring crashTestArg; // --crash-test <kind> (綴りの検査は Main が ParseCrashTestKind で行う)
    std::wstring repDiffA;     // --rep-diff A B (M52h)
    std::wstring repDiffB;
    std::wstring hashDiffA;    // --hash-diff A B (M52a)
    std::wstring hashDiffB;
};

enum class CliParse {
    NotMine,  // 共通フラグではない。値が足りない場合もこれ (= Main 側の分岐へ回す)
    Consumed, // 読んだ。i は最後に読んだ値の位置まで進んでいる
    Error,    // 値の綴り違い。メッセージは stderr へ出してある (Main は exit 1)
};

// argv[i] が共通フラグなら、値まで読んで config / extras へ書く (表は EngineCli.cpp の kEngineCliFlags)
CliParse ParseEngineCliFlag(int argc, wchar_t** argv, int& i, EngineConfig& config, EngineCliExtras& extras);

} // namespace mye
