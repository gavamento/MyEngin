//====================================================================================
//                          TerminalSessionSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/19/2026
//                                          ターミナルセッション (受け渡しキュー・セッション状態遷移・ConPTY 往復) の回帰テスト (宣言)
//====================================================================================
#pragma once

namespace mye {

// Fast (suite "terminalsession"): 子プロセスを使わない。TerminalByteQueue (上限・背圧・モード切替) と、
// 偽ホストを差したTerminalSession (起動成否と状態遷移・1 フレームの投入上限・DA 応答の返送・入力の送信・
// 終了検出と最後の出力・リサイズの伝搬・再起動・破棄時の Close)
bool RunTerminalSessionSelfTest();

// Full (suite "terminalconpty"): 実際に ConPTY で cmd.exe を動かす。`cmd /c echo <マーカー>` のマーカーが画面に出て
// 終了コード 0、起動中の ResizePseudoConsole 成功、再起動、ConPTY が無い状態 (関数ポインタ null) で Unsupported
bool RunTerminalConPtySelfTest();

} // namespace mye
