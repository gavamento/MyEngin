# sub-07: NavMeshLink (Off-Mesh Link の渡り)

- 依存: sub-06
- 状態: 未着手
- 往復: 0

## やること
spec 2. #12、4.1 (Link)。

1. `NavMeshLinkComponent` を末尾 append (hash 対象)。`start` / `end` (ローカル)、`width`、`bidirectional`、`area` (既定 2 = Jump)、`traversal` (Linear / Jump / Manual)、`traversalSpeed`、`jumpHeight`。
2. ベイク時は Off-Mesh Connection として `.mnav` に入れる。実行時の追加・移動・削除の反映方法 (TileCache の `dtTileCacheMeshProcess` でタイル再構築時に差し込むのが基本) を coder が決め、restore 後一致を確かめる。
3. 渡り: dtCrowd の OFFMESH 状態で NavSystem が補間を持つ (状態は Agent コンポーネントか Nav 節 = スナップショット対象)。Linear = 一定速度の直線、Jump = 放物線 (`jumpHeight`)、Manual = 端で止まり `status = OnLink` を公開し、完了の通知 (コンポーネントのフィールドを書く。ABI は sub-08) で抜ける。渡る間は CC.moveInput = 0、物理 (3.6) の後に nav が位置と CC.velocity を上書き。CC の型は変えない (変える必要が出たら `kSimSnapshotVersion` の扱いと一緒に「仕様との差分」へ)。
4. 片方向の Link は逆向きに使われない。
5. Create → 3D Object → NavMesh Link、Add Component、ギズモ (2 点と矢印)、デバッグ描画、インスペクタ、Localization。
6. `--nav-demo` に Link を足し `nav` ジョブを録り直す。

## やらないこと (このサブでは)
- アニメーション連携 (後の BT / アニメで状態を読む)、ABI

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`NavBake`、`NavSystem`、`TickRunner.cpp` (物理の後の上書き)、`SimSnapshot`、`CreateMenu.cpp`、Inspector、ギズモ、`DemoContent.cpp`、Localization

## 受け入れ条件 (このサブ)
1. (spec 10) Linear / Jump / Manual それぞれで向こう側へ渡り目的地に着く。Manual は通知まで止まる。片方向は逆向きに使われない。— SelfTest
2. 渡りの途中の tick で撮って restore → 連続実行と一致。— SelfTest
3. (spec 4) Create → NavMesh Link が Undo / Redo できる。
4. replay_verify 全 PASS (`nav` 録り直し)、golden は `nav` 以外不変 (`nav` は差分が意図どおりか画像で確認して更新)、0 警告、check_rules 0。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
