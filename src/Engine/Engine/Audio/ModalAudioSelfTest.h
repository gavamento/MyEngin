//====================================================================================
//                          ModalAudioSelfTest.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          衝突 → モーダル一発再生の橋渡しの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// CollectModalImpacts (接触 → PendingModalImpact) / RestingImpulse の抽出 / PushModalImpact の
// キュー上限 / MakeModalShotPlay (cell 選択 → BuildModes → 合成) / cooldown 判定 /
// ResolveWaveShotSound の口封じ / suspend 中のキュー掃除 / CLI 2 本 (EngineCliSelfTest 側) を検証する。
// XAudio2 デバイスは一切開かない (T18 と同じ「Init を呼ばない」流儀)
bool RunModalAudioSelfTest();

} // namespace mye
