//====================================================================================
//                          ImpactSynthSelfTest.h
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          手続き生成 (ImpactSynth / .impact.json) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// ImpactSynth の決定論・正規化・破片の有無、.impact.json の解釈、手続きクリップの登録経路を
// 検証する。**XAudio2 デバイスを一切開かない** (RegisterClip はデバイス無しでも通る)。
// 副作用として試聴用の WAV を %TEMP%\mye_impact_synth\ へ書く (消さない。耳で確認する入口)
bool RunImpactSynthSelfTest();

} // namespace mye
