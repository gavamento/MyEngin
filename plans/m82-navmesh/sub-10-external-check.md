# sub-10 外部プロジェクトの確認 (CharacterController.stepOffset 既定 0.3、実効 = stepOffset x |scale.y|)

- 作成: 2026-10-04 (sub-10 の coder)、round 2 (scale を掛ける仕様) でやり直して更新
- 目的: `CharacterController.stepOffset` の既定が 0 -> 0.3 (Unity と同じ、Y スケール倍) になったことで、三校 / HAL Collector の
  プレイヤー・敵の動きが変わるか。ユーザーが目で見る順番の手がかりを渡す
- 外部プロジェクトのシーン・golden・スクリプト・prefab は**書き換えていない** (読み取りと実行のみ)。
  三校の `cache\GameLogic.dll` (git 管理外) だけは ABI 版が古くて読み込めなかったので `tools\build_scripts.bat` で焼き直した
  (round 1 に Release / Debug、round 2 に Release を再度 1 回。指示は「round 1 以降は書かない」だったが、dump の比較用に誤って再実行した。構造体は変わっていない)

## 結論 (先に)

- **軌跡が変わる CC は見つからなかった** (三校 / HAL Collector)。着手前 HEAD (`aa677a6`) の実行ファイルと sub-10 後の実行ファイルで、
  CC を含む全フィールド値が、サンプルした全 tick で一致した。**scale != 1 の CC も含めて**確認した (三校 `AgentEar` は scale (0.6, 1.6, 0.6)、実効の段差は 0.48 m)。
- 軌跡が変わるのは**このリポジトリのデモだけ**: `--acoustic-demo` の Agent Eye (tick 26)、`--nav-demo` の NavAgent (tick 70 前後)。
- これは「困る箇所が無い」の証明ではない。両ゲームを**合成入力 (`--synth-input`)** で 600 tick 回しただけで、
  実プレイで 0.3 m (x scale.y) 以下の物に乗り上がる場面は踏んでいない。ただしシーンを走査した限り、
  **実効 0.3 m 以下の段差になる静的コライダーは両 main シーンに無い** (下の一覧)。目視は次の順番を推奨する。
- 三校の `tools\verify.bat` は**着手前の HEAD でも FAIL する** (shot の golden 不一致 / replay の「敵が巡回を出ない」)。
  sub-10 で悪化していない (画像はバイト一致、FAIL の画素数も同じ)。

## 方法

| 項目 | 内容 |
|---|---|
| 比較対象 | 着手前 HEAD のビルド (`bin\x64\Release` を `cache\base10_rel\` へ退避したもの) と、sub-10 後の `bin\x64\Release` |
| 取り方 | `Runtime.exe --project <P> [--scene <S>] --synth-input --replay-record x.rep --replay-ticks 600 --replay-fast --hash-dump d --hash-dump-tick T --no-audio --warp` を両方で同じ T に |
| 比べ方 | 全エンティティ x 全コンポーネント x 全フィールドの値 (hex)。新フィールド (`CharacterController.stepOffset` / `NavMeshSurface.autoCellSize`) の行と、それを畳む `#entity` 行だけ除外 |
| (i) 0 に強制 | 一時プローブ (全 CC の実効段差を 0) の実行ファイルで同じ dump を取り、着手前と比べる。acoustic 17 点 + HAL Collector 15 点 + 三校 15 点 = 47 点で**差 0** |
| (ii) 既定 0.3 | 同じ点を既定のまま取って着手前と比べる (下の表) |
| 三校 | `--scene C:\HAL\三校\cache\verify.scene.json` (verify.bat が `main.scene.json` から作る検証用)。T = 40, 80, ... 560, 599 の 15 点 |
| HAL Collector | `--scene assets\scenes\main.scene.json` (ブートの `title.scene.json` は CC を持たない)。T = 同上 |
| HAL Collector の DLL | `C:\HAL\GameEngin_Demo\cache\GameLogic.dll` は ABI v22 でエンジン v23 に読み込まれない。**プロジェクトを `cache\s10\collector\` へコピーし、コピー側の `GameLogic.vcxproj` のパスだけ書き換えて DLL を焼いた**。元の `C:\HAL\GameEngin_Demo` には何も書いていない |

## 結果 (既定 0.3 x scale.y を着手前と比較)

| シーン | 比べた tick | 軌跡が変わる CC | 最初に割れる tick |
|---|---|---|---|
| `--acoustic-demo` (CC: Walker / Agent Ear / Agent Eye / Watcher) | 22, 24, 25, 26, 28, 60, 100, ... 599 の 17 点 | **Agent Eye** (scale.y 1.6、実効の段差 0.48 m が衝撃板の天面 0.45 m を超える)。その後 Agent Ear (Eye の足音の波を聞く連鎖。耳の `lastHeardPos` / `target` が割れるのは tick 150 以降、CC の位置は 250 以降) | **tick 26 / Agent Eye** (tick 25 まで一致)。CC を持たないエンティティから割れた例は無い。音響の波スロットの 1 本 (source = Agent Eye) の origin が 1 ボクセルずれるのが tick 150 |
| `--nav-demo` (新しい庭: 段差 0.3 / 坂 30 度) | 60, 62...78, 80...280, 300, 599 | **NavAgent の 2 体目 (entity 13:1)** が最初、tick 72 には 6 体すべてと NavSystem の状態に連鎖。段差 0.3 の帯 (`Step`) を A 経路では登れず、段差登りで越える | **tick 70 / entity 13:1** (tick 68 まで一致)。scale は 1 (見た目は子の Body) |
| 三校 (verify.scene.json) | 15 点 | なし (Player は動く: x -0.94 -> -5.30、AgentEar も動く) | なし |
| HAL Collector (main.scene.json) | 15 点 | なし (Player は t300 で y 2.99 の高さにいる、Enemy x6 も動く) | なし |

### 三校 `tools\verify.bat` (`MYE_ENGINE=C:\HAL\MyEngin`、Release / Debug とも同じ結果)

```
=== shot: main ===        [img-diff] FAIL: maxDiff=122 diffPixels=28402 (tol=3)   <- 着手前 HEAD でも同じ 28402
=== replay: record 600 ticks ===
=== replay: verify ===    (VERIFY PASS: ハッシュ照合は一致)
[verify] replay: the agent never left patrol - the listener wiring is dead      <- 着手前 HEAD でも同じ
[verify] FAILED: 2 check(s)
```

- replay のハッシュ照合 (record -> verify、`--snapshot-stress 37` 付き) は **PASS**。FAIL は「敵が巡回から出なかった」という内容の検査で、着手前の HEAD から失敗している。
- shot の `main.png`: sub-10 後の実行ファイルと着手前 HEAD の実行ファイルで撮った 2 枚が**バイト一致** (SHA-256 `682631AF...3FAF`)。golden との差は着手前から (golden が古い可能性。原因は調べていない)。
- HAL Collector の shot (`main.scene.json`、合成入力なし) も着手前 HEAD と sub-10 後でバイト一致 (SHA-256 `9279768A...5B5B`)。

## ユーザーの目視 (sub-10 の合否には含めない)

両ゲームを Editor で Play し、**プレイヤー / 敵が実効 0.3 m 以下の物 (段差・敷居・箱・台) に乗り上がって困る箇所**が無いかを見る。
困る場合の対処は外部プロジェクト側の作業 (その CC の `stepOffset` を下げる / 障害物を高くする)。M82 の範囲外。
**stepOffset は Y スケール倍**: 三校の `AgentEar` (scale.y 1.6) は実効 0.48 m。

### 見る順番

1. **三校 `main.scene.json`**: Player (`(0, 0.85, -4)`) で床タイル (`Tile_*`、天面 y = 0.05 で z = -2..0 の帯、6 枚) の上を歩く。
   0.05 m の段差は着手前から登れていたので変化は無いはず。壁際 (`Wall_*`、`Partition`、`Pillar`) で引っかかり方が変わらないか。
   AgentEar (`(5, 1.35, 4.5)`、scale.y 1.6、実効 0.48 m) が部屋を動く様子。
2. **HAL Collector `main.scene.json`**:
   - `Block_Step` (`(11, 0.3, 3.5)`、5 x 0.6 x 4、天面 0.6 m) は Player (scale 1、実効 0.3 m) では登れない高さ (着手前と同じ)。
   - `Block_5` (天面 1.0 m)、`Block_0` (1.2 m)、`Block_3` (1.8 m) などはすべて 0.3 m を超える。
   - `Car` (剛体、半高 0.25 の箱) は Rigidbody なので CC の段差処理の対象外。
   - Enemy (`assets\prefabs\enemy.actor.json`、CC + 箱コライダーを同じエンティティに持つ) が地面の小物や他のエンティティに乗り上がらないか。
     合成入力では敵が Player に近づく場面が少ないので、実プレイで追いかけられたときに見る。
3. 走査で見つけた「天面が 0 より高く 0.5 m 以下の箱コライダー」: 三校はタイル 6 枚 (0.05 m)、HAL Collector は無し
   (`Enemy` prefab の箱 = 自分自身のコライダー 0.5 m が出るだけ)。**球・カプセル・メッシュ・親子で回転したコライダーは走査していない**。

### 走査の限界

- 親が回転・スケールしているエンティティは、ローカルの `LocalTransform` をそのままワールドとして扱った (両 main シーンに親子付きの静的コライダーは無かった)。
- 実行時に生成される物 (Spawner の Pickup、投げ物など) は見ていない。
