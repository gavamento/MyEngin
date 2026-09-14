#pragma once

namespace mye {

// Source Control (M66) のヘッドレス回帰テスト。
//
// 検査項目の一覧は SourceControlSelfTest.cpp の (a)〜(k) の見出し。ほとんどは
// DLL 不要の配線・純関数の検査で、DLL が要るのは次の 3 つだけ:
//   (b) MyeCollab.dll との実往復 — LoadLibrary -> create -> hello -> poll -> destroy。
//       DLL が無ければ SKIP して true (rustup 未導入の環境で selftest 全体を
//       赤くしない)。ただし環境変数 MYE_COLLAB_REQUIRED=1 のときは失敗にする
//       — CI では「Rust をビルドし忘れて静かに素通り」が起きてはいけない
//   (b2) RetryAfterBuild の冪等性 — DLL が無ければ SKIP
//   実 DLL 経由の結線 (末尾) — 環境変数 MYE_COLLAB_PROBE=<repo> のときだけ走る
bool RunSourceControlSelfTest();

} // namespace mye
