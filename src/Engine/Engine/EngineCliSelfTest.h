#pragma once

namespace mye {

// 両 Main 共通の CLI フラグ表 (ParseEngineCliFlag) の回帰テスト。
// フラグごとに「どのフィールドへ何が入るか」「値が足りないときは共通フラグとして読まない」
// 「省略可能な値は '-' で始まる次の引数を食わない」「綴り違いは Error」を固定する
bool RunEngineCliSelfTest();

} // namespace mye
