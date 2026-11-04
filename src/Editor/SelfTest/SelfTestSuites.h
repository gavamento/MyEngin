//====================================================================================
//                          SelfTestSuites.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          selftest のスイート登録表と選択・実行
//====================================================================================
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace mye {

// Fast は --selftest の既定集合、Full は --selftest-full / MYE_SELFTEST_FULL=1 でだけ加わる
enum class SelfTestTier { Fast, Full };

struct SelfTestSuite
{
    const char* name;   // 関数名から Run と SelfTest を除いて小文字化したもの
    bool (*run)();
    SelfTestTier tier;
    const char* note;   // 何を検査するか (--selftest-list には出さない)
};

// MYE_SELFTEST_ONLY に書ける名前のまとまり。members はカンマ区切りで、並びは単独指定のときだけ守る
struct SelfTestAlias
{
    const char* name;
    const char* members;
};

enum class SelfTestMode { Fast, Full, Only };

struct SelfTestSelection
{
    SelfTestMode mode = SelfTestMode::Fast;
    std::vector<size_t> indices;   // 回すスイートの GetSelfTestSuites() 上の添字 (実行順)
    std::string unknownName;       // 解決できなかった最初の名前 (関数の戻り値が false のとき)
};

// 全走の順の登録表
std::span<const SelfTestSuite> GetSelfTestSuites();
std::span<const SelfTestAlias> GetSelfTestAliases();

// onlyValue (MYE_SELFTEST_ONLY の値。空なら未指定) と full から回す集合を決める。
// 名前が解決できなければ false (out.unknownName に名前)。純関数で、スイートは実行しない。
// only 指定は tier を問わず表の順に重複なく回す。例外: 別名 1 個だけの指定は別名の並びで回す
bool ResolveSelfTestSelection(const std::string& onlyValue, bool full, SelfTestSelection& out);

// --selftest-list の出力 (名前と tier、続けて別名)
void PrintSelfTestList();

// 選ばれたスイートを (先行失敗があっても) 全部回し、所要時間つきのログを出す。全て成功なら true
bool RunSelfTestSelection(const SelfTestSelection& selection);

} // namespace mye
