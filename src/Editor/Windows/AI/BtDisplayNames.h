//====================================================================================
//                          BtDisplayNames.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                  ビヘイビアツリーの識別子を UI に出すときの表示名
//====================================================================================
#pragma once

#include <string>

namespace mye {

// .bt.json の識別子 (ノードの種類・Decorator・パラメータ・選択肢・キー欄の名前) の表示名。
// 日本語 UI では「日本語名 (識別子)」、英語 UI では識別子のまま。ファイルに保存する識別子は変えない。
// 表に無い識別子 (C++ / C# タスクのフィールド名など) はそのまま返す
const char* BtDisplayName(const char* identifier);

// 箱の 2 行目や Decorator の帯など狭い場所用。日本語 UI では日本語名だけ、英語 UI では識別子
std::string BtShortName(const char* identifier);

// ImGui の欄の見出し。ID は識別子に固定し、言語を切り替えても欄の状態が変わらないようにする
std::string BtFieldLabel(const char* identifier);

} // namespace mye
