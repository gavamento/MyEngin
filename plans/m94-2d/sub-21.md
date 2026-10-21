# sub-21: GridAgent2D、BT MoveTo の fallback、Perception forwardAxis

- 依存: sub-11, sub-09
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #17, #30、4.1.13、7. R7

## やること
1. `GridAgent2DComponent` (**hash 対象**、TypeId 末尾、spec 4.1.13 の全フィールド。`status` の値は `navagentstatus::` を流用)。Catalog、Create → 2D → Grid Agent。`kSimSnapshotVersion` +1。
2. `GridNavSystem` (`src\Engine\Engine\Navigation\GridNav2D.{h,cpp}`): TickRunner の 3.4b (Nav) の直後に `if (stepSim)` で `Update`。`TickServices` にポインタ、`EngineLoop` / `HeadlessSim` の配線、`simRefs` には入れない (経路は導出値、コンポーネント以外の状態を持たない。進行中の探索 (open list) も**持たない** — 1 tick の上限 4096 展開で終わらなければ次 tick に**最初から**やり直す (決定的で単純。R7 の「数 tick かかる」はこの意味)。上限はコンポーネントではなく定数)。
   - グラフ: `tilemap` の `collision` レイヤーから歩行可能セル (空 + 直下が固体 / oneWay)。辺は spec 4.1.13 (左右 10、落下 10 + 高さ、ジャンプ 20 + 距離。ジャンプの経路セルが空であること)。
   - A*: 二分ヒープ、キー `(f, g, cellKey)` の全順序、ヒューリスティックはマンハッタン × 10。目的地セルが歩行不可なら最寄りの歩行可能セル (距離 2 以内) に置き換え、無ければ NoPath。
   - 追従: 次セルの中心へ `cc->moveInput.x = ±speed`、ジャンプ辺の始点で `cc->jumpSpeed = jumpSpeed`。セル中心 ± 0.1 で次へ。目的地セルに入って `Arrived`。`replanInterval` tick ごと、または目的地が変わったら再計画。30 tick 進展が無ければ `Stuck`。CC が無い / `plane2D` でない / Tilemap が無効なら `Inactive`。
   - Agent は entity.index 順に処理。
3. BT の MoveTo (`BehaviorTreeSystem.cpp:507-566`、`ReleaseMoveTo` :221): `NavMeshAgentComponent` が無く `GridAgent2DComponent` があればそちらへ書き、`status` を同じ switch で読む。`HorizontalDistance` は GridAgent のとき XY。`--bt-demo` は不変 (NavMeshAgent がある)。
4. `AIPerceptionComponent` に `int32_t forwardAxis = 0` を末尾 append (hash 対象の append なので、焼いた WorldHash 定数 (`grep -rn "kExpected" src --include=*SelfTest.cpp`) を同じサブで再採取する — spec 4.4)。LOS の `CollectLosColliders` にも `StaticShapeSource::Collect` (sub-14 でクエリに足した口) を使い、タイルの壁が視線を遮る (selftest 1 項目)。`SightGeometry` (`PerceptionSystem.cpp:254-274`) の前方を `forwardAxis == 1` なら +X 行 (`_11,_12,_13`) に。ギズモ (`SceneViewWindow.cpp:1049-1063`) も XY に。`kSimSnapshotVersion` は上と同じ 1 回で済ませる。
5. selftest `GridNav2DSelfTest`: 10×10 の面 (段差 2、穴 1) で Arrived、到達不能で NoPath、2 ワールド並走一致、Debug / Release ハッシュ焼き込み、BT MoveTo の fallback (`BehaviorTreeSelfTest` に 1 項目)、Perception `forwardAxis = 1` で +X 側だけ見える (`PerceptionSelfTest` に 1 項目)。

## やらないこと (このサブでは)
- 群衆回避、はしご、経路の可視化以外のデバッグ UI

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Navigation\GridNav2D.{h,cpp}`、`GridNav2DSelfTest.{h,cpp}`
- `Components.h` / `Components.cpp` (GridAgent2D、Perception の append)、`EditorComponentCatalog.cpp`
- `TickRunner.{h,cpp}` (:427 Nav の後)、`EngineLoop.cpp:1024-1026`、`HeadlessSim.cpp:208-209`
- `src\Engine\Engine\AI\BehaviorTreeSystem.cpp:221, 490-566`、`src\Engine\Engine\Perception\PerceptionSystem.cpp:160-164, 254-274`
- `SceneViewWindow.cpp` (経路ギズモ 橙、Perception の XY 扇形)、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj
- 参考: `plans\m82-navmesh\spec.md` の Agent の status と Stuck の規則、`docs\adr\ADR-025-behavior-tree.md:183`

## 受け入れ条件 (このサブ)
1. (spec 35) selftest 全項目 PASS (Debug / Release ハッシュ同値)。
2. `--bt-demo` / `--nav-demo` / `--perception-demo` の hash-diff が Perception の append のみ (replay_verify 全体は sub-23)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `GridNav2DSelfTest` を足して両構成 PASS (BT / Perception の suite もこの分岐に)、snapshot +1 のみ。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --bt-demo / --nav-demo / --perception-demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-23)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-21 (round 1)
実装:
  - Components.h/.cpp: GridAgent2DComponent (TypeId 91、hash 対象、spec の 11 欄 + 追従の隠し欄 6 つ)、perceptionforward、AIPerception 末尾に forwardAxis。登録は末尾 append
  - Navigation/GridNav2D.{h,cpp}: GridNavSystem。歩行可能セル判定 (GridNavView、衝突レイヤー + パレットの collider/oneWay)、辺 (歩き / 落下 / ジャンプ)、二分ヒープ A* (キー (f, g, cellKey))、1 つ先のセルだけ覚える追従、Stuck / Inactive / Idle
  - TickRunner.{h,cpp}: TickServices.gridNav、Nav の直後に stepSim で Update (TilemapSystem は ts.tilemap を流用)。EngineLoop / HeadlessSim が所有・配線 (simRefs には入れない)
  - BehaviorTreeSystem.cpp: MoveTo / ReleaseMoveTo の GridAgent2D fallback (NavMeshAgent が無いときだけ。status は同じ switch、到着距離は XY)
  - PerceptionSystem.cpp: MakePerceiver が forwardAxis で +X 行を前方に、CollectLosColliders が physicsquery::CollectStaticShapes でタイル / SpriteShape の箱も遮蔽に (stable_sort)
  - SimSnapshot.h v63 → v64、AcousticAudioSelfTest の版固定を 64 に
  - Editor: カタログ (GridAgent2D)、Create → 2D → Grid Agent 2D (CC plane2D + GridAgent2D)、LocalizationTable.inl、SceneView の GridAgent2D ギズモ (橙: 次のセルへの線 + 次 / 目的地セルの枠) と Perception 扇形の XY 対応
  - selftest: GridNav2DSelfTest 新設 (EditorMain の m94 分岐と通常一覧に追加)、PerceptionSelfTest に forwardAxis 3 項目 + Tilemap の遮蔽 3 項目、BehaviorTreeSelfTest に MoveTo fallback 4 項目、m94 分岐へ Perception / BT の suite を追加
仕様との差分:
  - [追加] GridAgent2D に隠し欄 hasPlan / planAge / goalCellX / goalCellY / stuckTicks / bestDistance — spec は「コンポーネント以外の状態を持たない」で、replanInterval / Stuck (30 tick 進展なし) / 目的地変更の検出にコンポーネント側の記憶が要る。経路そのものは持たず 1 つ先のセル (nextCellX/Y) だけ。hash 対象なので復元後も続きが一致する (SimSnapshot 往復の selftest で確認)
  - [逸脱] 展開上限 (4096) を超えたときは「次 tick に最初からやり直す」のではなく、届いたノードのうち目的地に最も近い (h, g, cellKey 最小) ノードへの経路で動く (status は Moving)。最初からのやり直しは同じ入力で同じ結果になり永遠に進まないため。NoPath は open list を使い切ったときだけ
  - [追加] 経路の追従は「現在のセルから毎回引き直し、次のセルに着いたら / 目的地セルが変わったら / replanInterval / 辺が成り立たなくなったら」。replanInterval = 0 は「着いたときと目的地セル変更のみ」
  - [追加] 辺の細部: ジャンプは dy ∈ [-maxJumpHeight, maxJumpHeight]・距離 1..maxJumpLength、経路セル = 上昇してから水平 (dy >= 0) / 水平してから降下 (dy < 0)、コスト 20 + 距離 + |dy|。同じ高さで途中が全部歩ける跳び (平らな床での跳び) は辺にしない (コスト 23 < 歩き 30 で床の上を跳ねるため)。落下は隣の列を直下が床になる最初のセルまで (段差任意、コスト 10 + 高さ)
  - [追加] 目的地 / 開始セルが歩行不可なら距離二乗 → cellKey で最寄りの歩行可能セル (チェビシェフ 2 以内) に置き換え。BT の目的地は主人公の中心 (足より上) になるため、開始セルにも同じ規則を適用
  - [追加] jumpSpeed の既定 8 (m/s)、speed 3、maxJumpHeight 2、maxJumpLength 3、replanInterval 30
  - [追加] 空中では引き直さない (着地後)。目的地を取り下げた tick に moveInput.x = 0 を 1 回書く (Idle の間は CC に触らない)
  - [追加] m94 分岐に RunPerceptionSelfTest / RunBehaviorTreeSelfTest を足した (sub 指示「BT / Perception の suite もこの分岐に」)
  - [未実装] 経路全体のギズモ (1 つ先のセルしか持たないため、今向かっている区間と目的地セルだけ描く)
検証:
  - Debug / Release `MyEngine.sln /p:MyeWarnAsError=true` → 両方 0 警告 0 エラー (Release 初回から通過)
  - `MYE_SELFTEST_ONLY=m94` Debug / Release → FAIL: 0、ALL PASS 20 スイート (GridNav2D・Perception・BehaviorTree 含む)
  - 全体 `Editor.exe --selftest` Debug / Release (append の影響確認) → 両方 FAIL: 0、PASS 8681 (基点 cp4 は 8648)。焼き値の動いたものは無し (下の申し送り)
  - GridNav2D シナリオ A (穴 1 + 段差 2、236 tick で Arrived) の WorldHash 0x05C77D287CAAA51B が Debug == Release
  - `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり、増減なし)
  - hash-diff (Release、base_head 26aa0fa vs 現在、各 300 tick、dump は tick 250): `--nav-demo` identical (total DEE82C329D2AAEEF)。`--perception-demo` / `--bt-demo` は `AIPerception.forwardAxis` の行 (値 0、3 体 / 2 体) が増えるだけで、他の違いは `#entity` 行のハッシュ (新欄が入るので当然) と `UIInteraction.prevSurfX/Y` (実マウスのミラー、既知) のみ
  - 未実行: replay_verify / shot_verify / build_managed (チェックポイント sub-23 で)、Create → 2D → Grid Agent 2D と SceneView ギズモの実操作 (目視)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1-3 を満たす。展開上限の扱いと隠し欄が spec からの逸脱 / 追加 (理由つき)
  正しさ: 4 — 10x10 の穴 + 段差 2 を実物理 (CC + タイル衝突) で越えて Arrived、NoPath / 上限 / Inactive / 並走一致 / SimSnapshot 往復一致を確認。親付き Agent、動く床の上、oneWay の床の上での歩行は selftest 無し
  コード品質: 4 — 辺の列挙を ForEachEdge 1 本に集約 (拡張と検証が同じ源)。UpdateAgent がやや長い
  テスト: 4 — GridNav2D 16 項目 + Perception 6 + BT 4。partial 経路の「その後 Arrived まで着くか」と oneWay 経路は未検証
不安・質問:
  - Q: 展開上限の逸脱 (最初からやり直し → 最良ノードへの部分経路) を planner が追認するか。spec R7 の「数 tick かかる」は、この意味でなら成り立つ (広い面は引き直しのたびに近づく)
  - 状態: 動く床 / 剛体の上に立つと直下がタイルでないので歩行不可扱い (最寄りの歩行可能セルへ置き換えるか NoPath)。sub-23 のデモの敵はタイルの上を歩かせること
触ったファイル:
  - src/Engine/Core/Ecs/Components.h, Components.cpp
  - src/Engine/Engine/Navigation/GridNav2D.h, GridNav2D.cpp, GridNav2DSelfTest.h, GridNav2DSelfTest.cpp (新規)
  - src/Engine/Engine/Loop/TickRunner.h, TickRunner.cpp, EngineLoop.cpp, HeadlessSim.cpp
  - src/Engine/Engine/AI/BehaviorTreeSystem.cpp, BehaviorTreeSelfTest.cpp
  - src/Engine/Engine/Perception/PerceptionSystem.cpp, PerceptionSelfTest.cpp
  - src/Engine/Engine/Replay/SimSnapshot.h
  - src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  - src/Engine/Core/Localization/LocalizationTable.inl
  - src/Editor/App/EditorMain.cpp, Widgets/CreateMenu.cpp, Widgets/EditorComponentCatalog.cpp, Windows/Scene/SceneViewWindow.cpp, SceneViewWindow.h
  - (生成) build/Engine.vcxproj, build/Engine.vcxproj.filters
  - plans/m94-2d/sub-21.md (実装メモ)
申し送り:
  - (a) 焼き値: `kExpected*` / 16 桁定数を含む全 selftest が Debug / Release とも通る = 動いたものは無し。AIPerception を持つシーンで焼き値のあるものが無いため。動かない側 (plane2D 639107476EA401A3 / joint2D A82F779A97738D34 / effector2D 9504211B9041ECC2 / Tilemap 326BA3CD14F4BBB8 / SpriteShape AE6DC1DD4BAEC0E6 / NavAgent Yard 8581BEE4A676337C 等) は全て PASS のまま。動く側は perception / bt デモの WorldHash (焼き込みではなく replay_verify が毎回録り直す)。.rep は git 管理外 (cache\*.rep) で、毎回録り直すので append で読めなくなる資産は無い
  - (b) LOS は physicsquery::CollectStaticShapes (mask 全層、判定は LosCollider.layer = 供給元の col->layer = Tilemap.layer / SpriteShape の layer) で、selftest はタイル壁の遮蔽と Tilemap.layer による losLayerMask 除外
  - sub-23: 敵は tilemap に GridAgent2D.tilemap を向ける (Create メニューは空のまま)。CC は plane2D 必須、足元の下がタイルであること。BT の MoveTo は NavMeshAgent が無ければ自動で GridAgent2D を使う。--bt-demo は不変 (NavMeshAgent がある)
  - 限界欄 (sub-24): 経路は 1 つ先のセルだけ記憶 / ジャンプは maxJumpHeight・Length 内で CC の jumpSpeed が足りるかは未検証 (既定 8 m/s で高さ 2 セル・距離 3 セルは越える) / 頭上の空き (カプセルの高さ) は見ない / 動く床・剛体の上は歩行不可 / 群衆回避なし

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。シナリオ A (穴 + 段差 2 を実物理で 236 tick で Arrived、焼き値 05C77D287CAAA51B 両構成同値)、NoPath / 上限 / Inactive / 並走一致 / snapshot 往復、BT fallback 4 項目、Perception forwardAxis 3 + タイル遮蔽 3 を確認。全体 selftest 両構成 FAIL 0 で焼き値の再採取は不要だった (AIPerception を持つ焼き値シーンが無い)。hash-diff は nav identical、perception / bt は forwardAxis 行の追加だけ。展開上限の部分経路と隠し欄は採用 (spec 4.1.13)。動く床の上は歩行不可を v1 の限界として記録。
