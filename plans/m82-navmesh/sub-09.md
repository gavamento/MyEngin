# sub-09: 文書と全体検証

- 依存: sub-08 (sub-04 も OK であること)
- 状態: 未着手
- 往復: 0

## やること
spec 5. の 11, 12, 15, 16, 17, 18 (c)。

1. `docs\adr\ADR-023-navmesh.md` を仕上げる (sub-01 の下書き + 各サブの決定: 復元方式、**将来の実行時再ベイクの足し方 (どの関数を・どの tick のどのステップで呼ぶか・何が未実装か)**、塗りの描画レーン、TypeId、`.mnav`、エリアコストを Surface に置いた理由、CC 必須、Link の渡り、Obstacle の同期確定、除外した案と理由、AcousticNav との役割分担)。
   sub-10 から: CC の stepOffset (既定 0.3、|scale.y| 倍、上限は実効の全高)、cellSize / cellHeight の自動決定とその丸め、段差判定の量子化のずれ (1 セル未満で、残りは Stuck で受ける)、登る tick に 0.13〜0.16 m 前へ出る跳び、acoustic の golden が Agent Eye の乗り上がりを写さないこと。 sub-05 から: Stuck の仕様 (60 tick、radius/4、解除の条件、部分経路の到着との使い分け)、carve=false が何もしない理由、Box の切り抜き方、5 cm の閾値、表示を Generation で作り直すこと、Commit で入れ直しを省く条件、計測 (324 タイル・障害物 8 個で Release 平均 857 µs)。plans の m75-ugui.md の TypeId 注記 (71 / 72 / 73 を M82 が使用)。 sub-05 round 2 から: 部分経路の Arrived が 60 tick 遅れること、dtCrowd の位置が終点の手前で止まること、回転 Box (yaw)、Surface の AABB による絞り込み、kSimSnapshotVersion 27 / NavTileStore の状態 v2、HasPolyAt は最寄り点の距離で判定する (穴の縁のポリゴンを拾う罠)。 sub-06 から: Recast パッチ 3 (paint と priority による塗り順、areaId があると通行不可を復活させない)、ポリゴンのフラグ = 1 << area、OPTIMIZE_VIS をコスト上げ時に外す理由 (実測)、Modifier をベイクに焼き込まない理由、filter の割り当て (16 種まで)、kStateVersion 3 / kSimSnapshotVersion 28、TypeId 74。 sub-07 から: Recast パッチ 4、Link は .mnav に入れず World から差し込む設計、出口は入口のタイルと同じか隣まで、入口の y を寄せる処理、渡りの状態は Nav 節の NavAgentSlot、渡っている途中で Link が消えたときの振る舞い、TypeId 75、版 29 / 状態 v4。
   sub-03 から: 坂の実効上限 atan(maxClimb / (2·cellSize)) と CC の登れる段差の実測 (spec 2. #19 / #5)、`targetPathqRef` を tick 末に `DT_PATHQ_INVALID` へ正規化する理由 (dtPathQueue の連番は復元で 1 からになる)、回避なし = `collisionQueryRange` 0.01 で近傍探索ごと止める理由、Nav 節の実測 (6 体 2,894 B / 128 体 38,782 B、capture +64 µs、restore 58 µs、ロールバック 148 KB に対し 128 体で +26% → 差分化しない)、最悪 tick の `dtCrowd::update` (128 体一斉要求で 0.76-0.87 ms、dtPathQueue 容量 8 件が自然に絞るので件数制限を足さない)。
2. `engine_spec.md` に NavMesh 節 (コンポーネント、Tick 位置、スナップショット、ABI 表の版)。`docs\engine-feature-guide.md` 9.3 の「NavMesh ベイクとは区別します」を書き換え。`AcousticNav.h:22-24` のコメントを「音響ナビは NavMesh を使わない (汎用移動は ADR-023 の NavMesh)」の意味に整える。`docs\test_checklists.md` に NavMesh の手動確認項目。`CLAUDE.md` / `AGENTS.md` に末尾 TypeId や検証表の記述があれば更新 (無ければ触らない)。`plans\m75-ugui.md` の TypeId 注記。
3. 全体検証: 両構成 `/p:MyeWarnAsError=true`、`--selftest` 両構成、`check_rules`、`replay_verify` (全ジョブ)、`shot_verify`、Runtime.exe `--nav-demo` 実走 + スクショ、エディタで Create → 4 項目 → Bake → 再生で Agent が歩くのを `--screenshot` で確認。

## やらないこと (このサブでは)
- 機能追加

## 触る場所 (planner の見立て)
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`、`src\Engine\Engine\Acoustic\AcousticNav.h` (コメントのみ)、`plans\m75-ugui.md`

## 受け入れ条件 (このサブ)
1. (spec 16) 文書の差分が spec 4. と実装に一致 (参照先のパス・番号が実在)。
2. (spec 11, 12, 15, 17) 全検証 PASS (shot_verify の golden `nav` を含む)。未実施があれば理由とリスクを書く。

## 検証コマンド
- 上記 3. の全部

## 実装メモ (coder が追記)

## フィードバック履歴
