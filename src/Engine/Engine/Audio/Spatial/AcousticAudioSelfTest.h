//====================================================================================
//                          AcousticAudioSelfTest.h
//  MyEngine/ 秋田蓮音                                                      09/06/2026
//                                          音響 × オーディオのヘッドレス回帰テスト
//====================================================================================
#pragma once

namespace mye {

// リスナー場 (Dial の 3 本目) と遮蔽・回折の整形を検証する。
// **D3D もウィンドウも XAudio2 も作らない** — AcousticField は DebugSetGrid で手組みし、
// 整形は純関数なので、--selftest のヘッドレス実行でそのまま回る。
bool RunAcousticAudioSelfTest();

} // namespace mye
