# sub-06: NavMeshModifier とエリアコスト

- 依存: sub-03 (sub-05 の後に直列で回す。TileCache の更新経路を共有するため)
- 状態: OK (commit d05f179)
- 往復: 1

## やること
spec 2. #10、4.1 (Modifier、エリア)。

1. `NavMeshModifierComponent` を末尾 append (hash 対象)。ローカル AABB + `area` (0..15)。
1b. (sub-03 VERDICT) `NavMeshAgentComponent` に `areaMask` u32 (既定 全エリア可) を**末尾**に追加 (hash 対象)。`NavMeshProcess::process` のポリゴンフラグをエリア別にすると NavDeterminismSelfTest の期待ハッシュが動く — 動いたら理由を SELF_EVAL に書いて更新。
2. ベイク時: Recast の `rcMarkBoxArea` / convex volume で範囲内のエリアを書き換える。実行時: Modifier が動いた / 増減したら、TileCache の `dtTileCacheMeshProcess` か層の area 書き換えで反映 (Obstacle と同じ tick 境界の同期更新)。方式は coder が決め、restore 後一致を SelfTest で確かめる。
3. Surface の `areaCosts[16]` を dtQueryFilter に反映、Agent の `areaMask` で include flags。エリアの名前は `project_settings.json` (表示のみ、既存の PartTagNames / PhysicsLayerNames の読み書きの流儀)。0 = Walkable / 1 = NotWalkable / 2 = Jump は固定名。
4. Create → 3D Object → NavMesh Modifier、Add Component、ギズモ、デバッグ描画のエリア色、インスペクタ、Localization。

## やらないこと (このサブでは)
- Agent ごとのコスト上書き (spec 3 で除外)、Link

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`NavBake`、`NavSystem`、`src\Editor\Project\` (エリア名、新規 NavAreaNames)、Inspector、`CreateMenu.cpp`、Localization

## 受け入れ条件 (このサブ)
1. (spec 9) 高コストの Modifier 域を迂回する経路になり、コストを下げると突っ切る。`areaMask` で除外したエリアは通らない。— SelfTest
2. 実行時に Modifier を動かすと同じ tick で経路に反映され、restore 後も連続実行と一致。— SelfTest
3. (spec 4) Create → NavMesh Modifier が Undo / Redo できる。
4. replay_verify 全 PASS、golden は `nav` 以外不変 (`nav` は差分が意図どおりか画像で確認して更新)、0 警告、check_rules 0。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)
- 方式: Modifier は**ベイクに焼き込まず**、Obstacle と同じく層 (ベイク結果) の上のオーバーレイにした。DetourTileCache に patch 3 (`PATCHES.md`)
  を当て、障害物に「切り抜きではなくエリアの塗り替え (paint)」を持たせた。`NavSystem::SyncObstacles` が毎 tick、Obstacle と Modifier を
  store の一覧 (キー順、Modifier は最上位ビット付きキー) と突き合わせ、撤去 -> 追加 -> Commit を 1 回で確定する。
- エリア: poly flag は 1 << area (エリア 1 だけ 0)、層では エリア 0 <-> 63 / 1 <-> 0 (通行不可) / k <-> k。Agent は areaMask ごとに dtCrowd の
  filter を割り当て (昇順、16 種まで)、コストは Surface.areaCosts を毎 tick 全 filter へ写す。
- 画像: `plans\m82-navmesh\screenshots\sub-06_*.png` (Inspector の Modifier 節 / Surface のエリアコスト表 / Agent の歩けるエリア / Runtime の nav)。
- 再焼きしたもの: NavAgentSelfTest の庭ハッシュ 5D708FD4EEC92492 (Agent に areaMask が増えて hash 対象が変わった)、NavDeterminism の
  capture.A.store 7ACDCC5314884736 / capture.B.store A0A4001609A5BCE7 (store の状態書式 v3 = 障害物に area 1 byte)。mesh / query / crowd の
  ハッシュは不変 = patch 3 は切り抜きの結果を変えていない。いずれも Debug = Release 一致。golden は nav.png だけ (hand capture)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。ベイク時の焼き込みをやめたこと (spec 4.1)、パッチ 3、OPTIMIZE_VIS を外すこと、エリア 1 の範囲 = NavMesh の外、`affects` の削除を承認。should: Inspector のエリアコストのドラッグ 1 回 = 1 Undo と、ProjectSettings のエリア名の画面は手で操作していない → reviewer が観察する。
