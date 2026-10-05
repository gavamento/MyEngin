# sub-04: AI ノード 4 種 (FindRandomPoint / FindNearestTarget / SearchArea / FindTarget)

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること
- spec 4.1.4 の 4 種。
- FindRandomPoint: `NavSystem::QueryRandomPoint` (`NavSystem.h:243`) に `World::Rng()`。見つからなければ Failure (Nav は失敗時に引かない、`NavSystem.h:240-244`)。
- FindNearestTarget: 自分の `AIPerception.percepts` (`Components.h:2060-2069`、相手キー順に並ぶ) から sense マスク・currentlySensedOnly で絞り、`lastSensedPos` までの距離が最小 (同距離は entity キー小)。名乗らない音 (target null) は Vector 出力だけ。BT はフェーズ 3.4a の後なので**同じ tick の知覚結果**を読む。
- SearchArea: 起点 (Vector キー、または usePrediction で `predictedPos`) を `QuerySamplePosition` で吸着 → MoveTo → radius 内の到達可能点を pointCount 個順に (FindRandomPoint と同じ方法)。終了キーの相手が視覚で見えたら Success (Abort で抜けるのではなくノード自身が見る)、回り切ったら Failure。巡回中の点の index・残り数は BT 節。**点の列は持たない**: 次の点はその点へ向かい始める時に 1 つずつ生成する (FindRandomPoint と同じ方法、RNG はその時に引く)。状態は「起点 (float3)・今向かっている点 (float3)・残り個数・段階」の固定長 (sub-03 で入れた種類ごとの追加状態に置く)。pointCount の上限は 32 (`kBtMaxSearchPoints`)。
- FindTarget: `AIStimulusSource` を持つ有効なエンティティから、`PerceptionAttitudeOf` (`PerceptionSystem.h:28`) の態度ビットとタグの AND マスク (`Tags::OwnMask`) で絞り、最近 (同距離は entity キー小)。

## やらないこと (このサブでは)
- デモ、デバッグ線

## 触る場所 (planner の見立て)
- `BehaviorTreeSystem.cpp` / ノード表 / `BehaviorTreeSelfTest.cpp`。参照: `PerceptionSystem.h:28-48`、`Engine\Engine\Scene\Tags.h`、`NavSystem.h:232-243`

## 受け入れ条件 (このサブ)
1. (spec 7) 各ノードの成功・失敗、同距離の決着、名乗らない音、予測位置の吸着、陣営とタグの絞り込み — `BehaviorTreeSelfTest` (知覚と NavMesh を実際に回す)。
2. (spec 3) SearchArea の途中で保存 → 復元 → 連続実行一致 (RNG 状態を含む)。
3. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
