//====================================================================================
//                          BrowserEmbedSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/18/2026
//                                          ブラウザ埋め込みの回帰テスト (宣言)
//====================================================================================
#pragma once

namespace mye {

// Fast (suite "browserembed"): 純関数だけ。候補の解析 (重複・引用符・引数・%1・IE 除外・存在しない exe・日本語パス・既定の印・並び) /
// 起動引数の系統 / 窓の特定 (版ディレクトリ配下・除外条件・Z 順先頭・別ブラウザ) / 状態機械の全遷移 / 見せる条件 1〜6 / 矩形の回数上限
bool RunBrowserEmbedSelfTest();

// Full (suite "browserembedproc"): Editor.exe 自身をダミー窓プロセス (--browser-dummy-window) として起動し、実プロセス間で
// 差分による特定 / Attach (WS_CHILD・親・矩形) / Detach (元のスタイル・親なし・可視) / 再 Attach / Closed と Lost の 1 秒以内の検出 /
// 応答なしの印と 1 フレーム 50 ms 以内を確かめる。ブラウザは起動しない
bool RunBrowserEmbedProcSelfTest();

} // namespace mye
