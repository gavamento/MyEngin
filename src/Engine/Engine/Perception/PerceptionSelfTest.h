//====================================================================================
//                          PerceptionSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          AI の知覚 (AIPerception / AIStimulusSource) の回帰テスト (M83)
//====================================================================================
#pragma once

namespace mye {

// TransformSystem と PerceptionSystem だけを回す最小の tick で、次を確かめる:
//   - 視覚: 距離・視野角・必ず気付く距離・見失う距離、壁 (トリガーとレイヤーマスクは遮らない)、陣営
//   - 聴覚: Distance モードの ReportNoise (距離の減衰・聞こえる距離・自分の音・味方の音)、Acoustic モードの耳
//   - ダメージ (見えない攻撃者)、接触 (CharacterController どうし)
//   - 記憶 (forgetTicks で忘れる・消えた相手は即座に忘れる)、予測 (最後の速度で進める)、スロットの上限 8
//   - AIPerception が無いシーンのハッシュを動かさない、SimSnapshot の往復で結果が連続実行と一致する
bool RunPerceptionSelfTest();

} // namespace mye
