//====================================================================================
//                          ModalSynthSelfTest.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          ModalSynth (BuildModes / 合成 / Mel) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// BuildModes の材質スケール則・mask 閾値・過減衰/ナイキストの棄却、ModalSynthRender の
// 決定論・周波数・減衰・長さ規則、MelBandCenters のドリフト検知を検証する。
// **XAudio2 デバイスも World も一切使わない** (純関数だけを叩く)
bool RunModalSynthSelfTest();

} // namespace mye
