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

7. (sub-10 VERDICT、spec 2. #20) **詰まり検出**: `Moving` の Agent が、経路の残り距離を一定 tick (名前付き定数。例: 60 tick) の間に一定量 (例: radius の 1/4) 以上縮められなかったら、`Stuck` (新しい状態。status の値を末尾に追加) にして止め、変わった tick に 1 回 WARN を出す。目的地を変えたら解除する。原因は 2 つある: NavMesh と CC の段差判定の量子化のずれ (残りは 1 セル未満)、Obstacle で塞がれた後の押し合い。SelfTest: maxClimb + 1 セル未満の台に向かう Agent が `Stuck` になること。決定論 (tick 数で判定、時間は使わない) と、Nav 節の restore 後一致 (詰まりのカウンタは Agent のコンポーネントか Nav 節に持たせる)。

8. (sub-04 VERDICT) **実行時の NavMesh の変化を表示へ反映する**: いまの `NavDebugView` は .mnav から読んだ静的な形を描く。Play 中 (と Runtime) は、NavSystem の store が持つ dtNavMesh の世代番号 (Commit で増える) を鍵に加え、変わったときだけ NavSystem の dtNavMesh から作り直す。作り直しの回数をログに出す。編集中は従来どおり .mnav から作る。描画レーンは sim 状態を書き換えない。Play 中に同じ .mnav を表示用にもう 1 回読んでいる (sub-04 の SELF_EVAL) のも、NavSystem の dtNavMesh を参照する形に寄せられるならここで解消する。golden `nav` は Obstacle が NavMesh を切り抜いた絵で撮り直す (`tests\golden\nav.png` だけを手撮りする。`--update` は全枚を触るので使わない)。
9. (sub-04 VERDICT、nit) Surface のインスペクタの『.mnav を読めない』の警告を、`NavMeshAsset::LoadByGuid` (メモリ上の登録も見る) で判定するように直す。`--nav-demo` で誤って出ている。

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
