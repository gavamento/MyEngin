# sub-06: NavMeshModifier とエリアコスト

- 依存: sub-03 (sub-05 の後に直列で回す。TileCache の更新経路を共有するため)
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
