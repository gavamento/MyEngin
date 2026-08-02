#pragma once

namespace mye {

// オーディオの純関数レイヤ (デコード / 合成 / ボイススティール / dB 変換) を検証する。
// **XAudio2 デバイスを一切開かない**ので、ウィンドウも D3D も無いヘッドレス実行で回る。
bool RunAudioSelfTest();

} // namespace mye
