# sub-04: AI ノード 4 種 (FindRandomPoint / FindNearestTarget / SearchArea / FindTarget)

- 依存: sub-03
- 状態: OK (M85d としてコミット、ハッシュは台帳)
- 往復: 2

## やること
- spec 4.1.4 の 4 種。
- FindRandomPoint: `NavSystem::QueryRandomPoint` (`NavSystem.h:243`) に `World::Rng()`。見つからなければ Failure (Nav は失敗時に引かない、`NavSystem.h:240-244`)。
- FindNearestTarget: 自分の `AIPerception.percepts` (`Components.h:2060-2069`、相手キー順に並ぶ) から sense マスク・currentlySensedOnly で絞り、`lastSensedPos` までの距離が最小 (同距離は entity キー小)。名乗らない音 (target null) は Vector 出力だけ。BT はフェーズ 3.4a の後なので**同じ tick の知覚結果**を読む。
- SearchArea: 起点 (Vector キー、または usePrediction で `predictedPos`) を `QuerySamplePosition` で吸着 → MoveTo → radius 内の到達可能点を pointCount 個順に (FindRandomPoint と同じ方法)。終了キーの相手が視覚で見えたら Success (Abort で抜けるのではなくノード自身が見る)、回り切ったら Failure。巡回中の点の index・残り数は BT 節。**点の列は持たない**: 次の点はその点へ向かい始める時に 1 つずつ生成する (FindRandomPoint と同じ方法、RNG はその時に引く)。状態は「起点 (float3)・今向かっている点 (float3)・残り個数・段階」の固定長 (sub-03 で入れた種類ごとの追加状態に置く)。pointCount の上限は 32 (`kBtMaxSearchPoints`)。
- FindTarget: `AIStimulusSource` を持つ有効なエンティティから、`PerceptionAttitudeOf` (`PerceptionSystem.h:28`) の態度ビットとタグの AND マスク (`Tags::OwnMask`) で絞り、最近 (同距離は entity キー小)。

- (round 1 VERDICT で追加) SearchArea に `failOnStuck` (bool、既定 false)。true なら巡回中の点でも起点へ向かう途中でも Stuck になった tick に Failure (目的地を倒して止める、MoveTo と同じ)。false は現状どおり Running (spec 2. #18)。

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

### round 1 (SELF_EVAL 写し)
- 実装: BehaviorTreeLibrary.h/.cpp に AI 4 種 (種類表・params/keys の並び・BtSearchAreaState 32 バイト・kBtMaxSearchPoints 32・BtNodeCategory::Ai・BtParamType::Mask)。BehaviorTreeSystem.cpp に VisitFindRandomPoint / VisitFindNearestTarget / VisitFindTarget / VisitSearchArea と ReleaseSearchArea。Update に `const NavSystem* nav` を追加 (TickRunner が ts.navSystem を渡す)。kSimSnapshotVersion 35 → 36。
- 仕様との差分 ([追加]):
  - BtParamType::Mask を追加 (FindTarget の tagMask = 64 ビット。Guid と同じ u / 16 進で保存。Int は 32 ビットでタグ 64 個に足りないため)。
  - Update のシグネチャに nav を追加 (BT は NavSystem を引く必要がある。null なら FindRandomPoint / SearchArea は Failure)。
  - FindRandomPoint / FindNearestTarget(Entity 以外) / SearchArea は NavMeshAgent (agentTypeId・areaMask・navFilter を借りる) が無いと Failure。FindNearestTarget / FindTarget は自分の AIPerception が無ければ Failure。
  - FindRandomPoint の中心 key は空文字 = 自分。書き先 key が Vector でない・未指定は RNG を引く前に Failure。
  - FindNearestTarget の params: sight / hearing / damage / touch (既定 true) と currentlySensedOnly (既定 false)。currentOnly = false は lastSenses、true は currentSenses で sense マスクと照合。名乗らない音を選んだら Entity キーは「空にする」(書けないので古い値を残さない)。Vector 出力キーは任意。
  - FindTarget の params: radius 15 / enemies true / neutrals false / friendlies false / tagMask 0 (= 条件なし。非 0 は PassesFilter = どれか 1 ビット一致)。距離は 3D、自分は除く、無効 (IsEntityActive) は除く。
  - SearchArea: usePrediction が真で終了キーの相手の percept があれば predictedPos、無ければ origin キーを起点にする。起点は QuerySamplePosition (水平 2 m・垂直 4 m) で吸着。着いた判定は水平 0.5 m (定数)。起点が NoPath / Inactive は Failure、巡回中の点が NoPath / Inactive なら飛ばす、Stuck は MoveTo の既定と同じく Running のまま。点の生成に失敗したらその点を 1 個消費して次の tick にやり直す (最後の点なら Failure)。終了キーの判定は percepts の currentSenses の視覚ビット (PerceptionCanSee のレイは撃たない = 同 tick の知覚の結果を読む)。
- 検証:
  - Debug / Release の MSBuild (MyEngine.sln) → 0 警告 0 エラー
  - Debug `Editor.exe --selftest` → 全 PASS (BehaviorTree 新規 FAIL 0、既知 flake は今回 1 回目から 0 件)
  - Release `Editor.exe --selftest` → 全 PASS (`[search] destinations 4, ended at tick 587` は Debug と一致)
  - Debug `Server.exe --selftest` → ALL PASS
  - `pwsh tools\check_rules.ps1` → 0 error / 0 warning
  - 新規テスト: アセット往復と丸め / FindRandomPoint (半径・決定論・失敗 6 種) / FindNearestTarget (最近・同距離・聴覚・視覚・名乗らない音・currentOnly 両方・失敗) / FindTarget (敵・中立・味方・タグ・範囲・無効・自分の除外) / SearchArea (全周回って Failure・視覚で Success・未設定でSuccessしない・予測位置の吸着と fallback・起点失敗 2 種・Timeout の Abort・保存 → 復元 3 種)。NavSim に知覚を足した。roundTrip を外側へ出して tree の GUID を引数化。
- 自己採点: 仕様適合 4 (差分は上の [追加] 群) / 正しさ 4 (全テストが実走、Debug / Release で一致) / コード品質 4 / テスト 4 (同距離のタイブレークは percept も archetype も key 順に並ぶため、KeyLess を外しても通る = 実効の検出力は弱い)
- 触ったファイル: src\Engine\Engine\AI\BehaviorTreeLibrary.h / .cpp / BehaviorTreeSystem.h / .cpp / BehaviorTreeSelfTest.cpp、src\Engine\Engine\Loop\TickRunner.cpp、src\Engine\Engine\Replay\SimSnapshot.h、src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp (版の数値追随)

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 1 件 = SearchArea に failOnStuck (ユーザーの MoveTo の判断との整合、spec 2. #18)。それ以外の [追加] はすべて承認 (spec 8. 変更履歴)
- round 2: VERDICT OK (planner)。must 1 (SearchArea の failOnStuck) 解消、false / true の両方を実走で確認。spec 2. #18 はユーザー回答で確定。テスト内の Wait(5) 前置きは承認 (Surface の読み込み待ち)。nit#2 (タイブレークの検出力) は sub-14 の ADR へ
