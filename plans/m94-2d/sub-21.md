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

## フィードバック履歴
