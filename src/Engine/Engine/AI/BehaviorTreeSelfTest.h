//====================================================================================
//                          BehaviorTreeSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの核の回帰テスト (M85)
//====================================================================================
#pragma once

namespace mye {

// BehaviorTreeSystem だけを回す最小の tick で、次を確かめる:
//   - .bt.json / .bb.json の往復と防波堤 (未知の type・重複 id・壊れた子参照・循環・子の数・列挙名・範囲外の丸め)
//   - Composite 3 種 (Selector / Sequence / SimpleParallel の Immediate・Delayed・背景のやり直し) と Wait の結果
//   - 根が終わった次の tick の根からのやり直し、1 tick の手数の上限、無効化・読み直し・外れた / 消えたときのライフサイクル
//   - Scene::Clear (シーンの読み直し) で前の表を引き継がない、表がエンティティキー順であること
//   - SimSnapshot の BT 節: 保存 → 新しいシステムへ復元 → 連続実行と毎 tick のハッシュ一致、壊れた節の拒否
//   - BT が無いシーンのハッシュと RNG を動かさない
//   - 100 体 x 30 ノードの 1 tick の所要時間 (ログだけ)
bool RunBehaviorTreeSelfTest();

} // namespace mye
