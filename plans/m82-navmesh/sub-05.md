# sub-05: NavMeshObstacle (TileCache の切り抜き)

- 依存: sub-03 (sub-04 の後に直列で回す。どちらも `--nav-demo` と golden `nav` を触るため)
- 状態: 未着手
- 往復: 0

## やること
spec 4.1 (Obstacle、Tick の順序 (1))。

1. `NavMeshObstacleComponent` を末尾 append (hash 対象)。Box / Cylinder、`carve`。
2. NavSystem の tick 頭で、Obstacle の集合 (エンティティキー順) と前 tick の差分を TileCache へ反映 (add / remove、移動は remove + add、閾値以下の移動は無視するなら閾値を定数で)。`dtTileCache::update` を upToDate まで同期で回し、その tick の Crowd 更新から新しいタイルを使う。`carve=false` は Crowd の回避対象 (動く障害物) としてだけ扱うか、何もしないかを coder が決めて理由を書く (Unity の carve=false は回避のみ)。
3. SimSnapshot の Nav 節に障害物の状態を含める (sub-01 の方式どおり)。タイルの再構築で salt が変わる経路 (restore 後の一致) を SelfTest で確認。
3b. (sub-03 申し送り / 不安 3) Obstacle コンポーネントと store の障害物の対応 (キー) が restore 後も保たれ、restore 直後の Update が障害物を二重に足さない・消し忘れないこと、restore 後の Commit を含めて連続実行と一致することを SelfTest で確かめる。Obstacle 同期は `NavSystem::Update` の SyncSurfaces の後に足す。
4. Create → 3D Object → NavMesh Obstacle、Add Component、ギズモ (形)、デバッグ描画、インスペクタ、Localization。
5. `--nav-demo` に Obstacle の出し入れ (tick で決まる) を足し、`nav` ジョブを録り直す。
6. TileCache 更新の所要時間を SelfTest のログに出す。**`NavTileStore::Commit` は変更のたびに全タイルを入れ直す (O(全タイル)、sub-01 の決定)** ので、数百タイル + 障害物を毎 tick 動かす条件で計測する。重ければ最適化してよいが、「キー順・スロット順に入れ直して dtNavMesh の履歴依存を消す」性質を保ち、復元一致の SelfTest で確かめる。

## やらないこと (このサブでは)
- Modifier / Link / ABI

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`NavSystem`、`SimSnapshot`、`CreateMenu.cpp`、Inspector / ギズモ (SceneView の既存ギズモ描画箇所)、`DemoContent.cpp`、Localization

## 受け入れ条件 (このサブ)
1. (spec 8) 経路上に carve の Obstacle を置いた tick で Agent が迂回し、消すと元の経路に戻る。— SelfTest
2. restore 後一致 (Obstacle 追加の後の tick で撮って restore → 連続実行と一致)。— SelfTest
3. (spec 4) Create → NavMesh Obstacle が Undo / Redo できる。
4. replay_verify 全 PASS (`nav` 録り直し)、golden は `nav` 以外不変 (`nav` は差分が意図どおりか画像で確認して更新)、0 警告、check_rules 0。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
