//====================================================================================
//                          ZipExtract.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          zip の安全な展開 (配布物のインストール用)
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace mye {

struct ZipExtractResult
{
    bool ok = false;
    bool cancelled = false;
    std::string error;
    uint32_t files = 0;
    uint64_t bytes = 0;
};

// done / total は展開後のバイト数
using ZipProgressFn = std::function<void(uint64_t done, uint64_t total)>;

// zip のエントリ名を destDir 配下の相対パス ('/' 区切り、末尾 '/' なし) に直す。
// 絶対パス・ドライブ指定・".." (zip slip)・':' (代替データストリーム)・制御文字・
// Windows の予約名 (CON / NUL など)・末尾がドットや空白の要素は false
bool SanitizeZipEntryPath(std::string_view name, std::string& out);

// zipPath を destDir へ展開する。destDir は呼び出し側が作っておく。
// stripSingleTopDir = true なら、全エントリが同じ最上位フォルダの下にあるときその 1 段を外す
// (配布 zip は "MyEngine/" の下に入っている)。
// 1 件でも不正な名前があれば何も書かずに失敗する。途中で失敗したときの書きかけは消さない
// (呼び出し側が staging フォルダごと消す前提)
ZipExtractResult ExtractZip(const std::wstring& zipPath, const std::wstring& destDir, bool stripSingleTopDir,
                            const ZipProgressFn& progress, const std::atomic<bool>* cancel);

} // namespace mye
