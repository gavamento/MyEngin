# sub-05: NavMeshObstacle (TileCache の切り抜き)

- 依存: sub-03 (sub-04 の後に直列で回す。どちらも `--nav-demo` と golden `nav` を触るため)
- 状態: OK (コミット待ち)
- 往復: 2

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

### round 1 (SELF_EVAL の要点)
- Obstacle: `NavMeshObstacleComponent` (TypeId 73、末尾 append)。`NavSystem::SyncObstacles` が SyncSurfaces の直後に、carve の立った Obstacle のワールド形 (`NavMakeObstacleSpec`) を store の障害物 (キー順、復元済みの状態そのもの) と突き合わせ、外す → 付ける → `Commit`。NavSystem 自身は前回の記録を持たない = restore 直後でも二重追加・消し忘れが起きない。
- 判断: `carve=false` は何もしない (dtCrowd に動く障害物の回避が無いため)。Box は回転後の外接 AABB、Cylinder は y 回転を無視。動きの閾値は 5 cm (`kObstacleMoveThreshold`)。障害物は読み込み済みの全 Surface に付ける (範囲外のものは TileCache に触れない)。
- 計測 (324 タイル、障害物 8 個を毎 tick 動かす): Release 平均 857 us / 最悪 1258 us、うち dtNavMesh の入れ直し (正規化) 約 266 us。Debug は平均 4239 us。`Commit` は作り直されたタイルが無い tick では入れ直さない (中身は同じ形になるため)。履歴依存を消す性質は復元一致の SelfTest で確認。最適化はこれ以上不要と判断。
- Stuck: `NavAgentSlot` に `stuck` / `noProgressTicks` / `bestRemaining` を追加 (Nav 節に入る)。60 tick の間、残り距離が基準から max(radius/4, 1 cm) 以上動かなければ止める (基準より遠ざかったときも基準を取り直す)。目的地の変更・取り消し・到着で解除。status は末尾に `kStuck = 6`。
- 表示: `NavTileStore::Generation` (プロセス内で一意) を鍵に、`NavDebugView::Refresh(world, nav)` が Play 中は NavSystem のナビメッシュから作り直す。編集中は従来どおり .mnav。Inspector の誤警告は `LoadByGuid` 判定に直した。
- `RestoreSimSnapshot` は Nav 節の `ApplySnapshot` の戻り値を最後に返す (World は戻せないので他の外部状態は当て切る)。
- `--nav-demo` に Obstacle (tick 150 で carve を倒す、210 で z+5 へ動かして戻す: `NavObstacleDriver`) を足し、golden `tests\golden
av.png` だけを撮り直した。
- 検証: Debug / Release ビルド警告 0、`--selftest` 両構成 (Nav 系 ALL PASS、既知の FAIL は Source control 2 件のみ)、Server.exe --selftest PASS、`replay_verify.bat` 15 ジョブ全 PASS、`shot_verify.bat` は既知 5 枚のみ FAIL (数値は着手前と同一) で nav は PASS、`check_rules.ps1` 0。

### round 2 (SELF_EVAL の要点)
- #1 部分経路: 前進が 60 tick 止まり、`ag->partial` かつ終点まで radius (`kPartialArriveRadiusScale` = 1.0) 以内なら `Arrived` + `pathPartial` (WARN なし)。Stuck は完全な経路の途中、または部分経路の終点から遠い所だけ。0.12 m の原因: crowd 自身の位置が終点 (1.55) の手前 1.428 で速度 0 になる (CC の停止位置ではない。crowd x 1.428 = CC x 1.43)。経路の終点は最寄り点の問い合わせ結果で、dtPathCorridor が位置を置けるポリゴンの縁と数 cm ずれる、という見立て (未追跡)。
- #2 `kSimSnapshotVersion` 26 → 27。AcousticAudioSelfTest の期待値を追随 (SimSnapshotSelfTest は version-1 で自動追随)。
- #4 障害物は Surface のワールド AABB (`NavMakeBakeConfig`) と重なる Surface にだけ付ける。#5 y 回転だけの箱は `addBoxObstacle(center, half, yaw)` (store の type = ORIENTED_BOX、状態の保存形式 v2 と HashObstacles に yaw)、x/z に傾いた箱は外接 AABB。
- 試験の注意: `findNearestPoly` は穴の縁のポリゴンも拾うので、`HasPolyAt` は最寄り点が 5 cm 以内のときだけ「歩ける」とする。
- 再焼き: ヤード E1F88B1FFFF5635C、NavDeterminism capture.A/B.store (Debug = Release 一致)。golden nav だけ撮り直し。画像: cache\s05\inspector_obstacle.png (Inspector の Obstacle 節、carve=true)。carve=false の注意書きと Stuck の表示は画像未取得。
- 検証: 両構成ビルド警告 0、Debug / Release の Editor --selftest (FAIL は Source control 2 件のみ)、Server.exe --selftest 両構成 PASS、replay_verify 15 ジョブ PASS、shot_verify は既知 5 枚のみ FAIL、check_rules 0。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-04)。must: (1) 部分経路の終点の近くで止まった Agent を Stuck ではなく Arrived + pathPartial にする (spec 4.1) (2) Nav 節の書式変更に合わせて kSimSnapshotVersion を 26 → 27 にする (3) ハッシュを焼き直した後のビルドで Debug の selftest を通しで流し直す。should: Obstacle を付ける Surface をワールド AABB で絞る / y 回転した Box を `addBoxObstacle(center, halfExtents, yRadians)` (vendor 版にある) で切り抜く / Inspector の Obstacle 節の画像を撮る。採用した点: carve=false は何もしない、5 cm の閾値、状態を持たない突き合わせ、Generation による表示の作り直し、HashLayers を層ハッシュで畳む、Stuck で基準を取り直す方式。
- round 2: VERDICT OK (planner、2026-10-04)。must 1〜3 と should 4・5 は解消。should 6 は一部 (carve=false の注意書きと Stuck の表示の画像が無い) → reviewer。既知の限界として受け入れた点: 部分経路の Arrived は無進捗の 60 tick を待ってから確定する (1 秒遅れる)。終点の 0.12 m のずれは dtCrowd 側で、原因は見立てまで。どちらも ADR に書く (sub-09)。
