# DeepModalModel — DeepModal 用の形状生成器

DeepModal (M76、`tools\deepmodal`) の学習データを増やすための **プロシージャル形状生成器**。
`docs\deepmodal\modelnet40_eval_report.md` §5 の「候補 3: プロシージャル合成メッシュ群」の実装で、
53 種類のファミリ (小道具・容器・武器・家具・建材・機械部品・クリーチャー・岩など) から
**50,000 件以上の有効な形状** を決定的に生成し、実際の `Editor.exe --modal-voxelize` で
ボクセル化して検査した OBJ / `.mvox` と、`dataset.py --stage small` へ渡す list を出力する。

`MyEngine.sln` の外 (`tools\deepmodal` と同じ流儀)。`tools\deepmodal` のコードは変更せず、
`layout.py` / `meshio.py` / `voxelize.py` を import して使う。

## 前提

- Python 3.11 と `pip install -r DeepModalModel\requirements.txt` (numpy / scipy / pytest のみ)
- `bin\x64\Release\Editor.exe` (無ければ Debug) が `--modal-voxelize` を認識するビルドであること。
  古いバイナリだと無言で GUI が起動する (tools\deepmodal\README.md の罠)。`MYE_EDITOR_EXE` または
  `--editor-exe` で指定できる
- FEM (npz 化) は `tools\deepmodal\dataset.py` に任せる (このフォルダは形状と list までを担当)

## クイックスタート

```
cd DeepModalModel
python -m pytest                      # 単体テスト (Editor.exe があれば実機スモークも走る)
run_pilot.bat                         # 1,000 件のパイロット → data\pilot\stats.json と qa\*.png を見る
run_full.bat                          # 50,000 件の本生成 → data\v1 (20〜40 分、中断は Ctrl+C、再開は同コマンド)
run_dataset.bat                       # FEM: list_part00..09 を順に tools\deepmodal\data\synth_v1 へ (35〜40 時間)
```

学習は `tools\deepmodal\train.py --data data\stage0 data\stage1 data\modelnet40 data\thingi10k data\creatures data\synth_v1 ...`
(手順は `tools\deepmodal\README.md`)。

### CLI (`generate.py`)

| コマンド | 役割 |
|---|---|
| `run --out DIR --target N --seed S [--jobs 10] [--editor-jobs 4] [--chunk 2000] [--batch 500] [--occ-min 300] [--occ-max 3500] [--profile small\|modelnet] [--rotation-mix default\|aligned\|free] [--family NAME ...] [--keep-rejected] [--editor-exe PATH] [--force]` | 目標件数まで生成。既存 DIR は自動再開 (設定不一致はエラー、`--force` で作り直し) |
| `finalize --out DIR [--target N]` | manifest から `list.txt` / `list_part00..09.txt` / `stats.json` / `qa\` を作り直す (run の最後に自動実行) |
| `preview --family NAME --count 24 --out DIR --seed S` | 1 ファミリだけ小さく回して統計とシートを見る (調整用) |
| `stats --out DIR` | 統計をコンソール表示 |
| `regen --out DIR --index I [--obj PATH]` | 1 形状を再生成して OBJ を書く (デバッグ) |

## 出力 (`data\<run>\`)

```
run_config.json   設定 + Editor.exe の指紋 + Python/numpy/scipy 版 (再開時の整合検査に使う)
progress.json     next_index / accepted (ラウンドごとに原子更新)
manifest.jsonl    1 行 1 形状 (受理も却下も)。index / name / family / category / rot_mode / seed /
                  status / reason / occ / surface / interior / cells / filament_ratio / components /
                  occ_hash / coarse_key / est_occ / n_pieces / n_tris / L / min_feature_vox / params
list.txt          受理した OBJ の絶対パス (index 昇順、target 件)。dataset.py --stage small --list に渡す
list_partNN.txt   list.txt の 10 分割 (run_dataset.bat 用)
stats.json        受理率・却下理由・ファミリ別 (件数 / 受理率 / 占有分位 / ユニーク 16³ キー数) /
                  占有ヒストグラム / 回転別 / 推定と実測の log 回帰 / FEM 時間の見積り
obj\<name>.obj    受理した形状 (先頭コメントに family / index / seed / params)
vox\<name>#mesh0#prim0.mvox   検査済みの占有 (Editor.exe の出力そのまま)
qa\sheet_random.png, sheet_<family>.png (+ .txt = タイルと名前の対応)   3 軸深度投影のコンタクトシート
```

名前は `<family>_<index:06d>`。形状 i は `(seed, i)` の純関数 (`shapegen\rng.py`) なので、
`--jobs` や中断・再開に関係なく同じ index は同じ形状・同じ受理結果になる。

## 有効 / 有用の定義

**有効** = 実際のボクセライザを通した `.mvox` が次を全部満たす (`shapegen\validate.py`、閾値は `shapegen\constants.py`):

| 順 | 検査 | 却下理由 |
|---|---|---|
| 1 | `.mvox` が読める | `vox_error` |
| 2 | surface+interior が占有数と一致、外周 1 層が空、最長軸 ≈ 29 voxel | `vox_anomaly` |
| 3 | `OCC_MIN (300) ≤ 占有 ≤ OCC_MAX (3500)` | `occ_low` / `occ_high` |
| 4 | 6 近傍で **1 連結成分** (FEM は剛体モード 6 本固定 = 単一成分が前提) | `disconnected` |
| 5 | 占有を含む 16³ cell が `CELLS_MIN (40)` 以上 | `cells_low` |
| 6 | 6 近傍の占有が 1 以下の voxel の比率 ≤ `FILAMENT_RATIO_MAX (5%)` (1 voxel 幅の糸) | `filament` |
| 7 | 占有列のハッシュが未出 | `dup_exact` |
| 8 | 16³ max-pool キー (ネット出力解像度) の出現が `COARSE_DUP_LIMIT (3)` 未満 | `dup_coarse` |

ボクセル化前の事前検査 (`pre_reject_*`): 全ピースが閉じている (`mesh.check_closed`)、
最小厚み ≥ `MIN_FEATURE_VOXELS (1.6)` voxel、三角形数 ≤ `TRI_MAX`、解析的な占有見積りが
`EST_LOW_FACTOR`〜`EST_HIGH_FACTOR` の範囲、重なりグラフが連結。

**OCC_MAX を 3500 にした理由**: FEM (eigsh) の時間は占有数で急増する (ModelNet40 実測の中央値:
1k〜2k で 24 s、2k〜3k で 47 s、4.5k〜6k で 136 s)。pilot で占有中央値 2414 のときは 50,000 件が
11 並列で 67 時間になった。推定占有が `EST_SOFT_HIGH (1500)` を超えた形状は厚み (`ctx.fill`) と
副寸法 (`ctx.lean`) を縮めて組み直し、分布を中央値 ≈ 1,450 / p95 ≈ 3,000 (推定 FEM 35〜40 時間) に寄せている。
`--occ-max` で緩められるが、FEM 時間は `stats.json` の `fem_hours_est_jobs11` で確認すること。

**有用** = ファミリ設計で担保: ゲーム内で衝突音を鳴らす物体に似た 53 ファミリ (重みはカテゴリ別に
中実 8 / 板・殻・格子 23 / 容器 13 / 武器・工具 10 / 家具・建材 14 / 機械 12 / キャラ・有機 15 / 汎用 5 %)、
中実・中空 (開口つき)・薄板・枝状・穴あき・格子・突起・非対称を網羅し、ランダム回転
(identity / 立方群 / 3〜20° の傾き / 一様な自由回転) でボクセル化の階段パターンも学習させる。

## 設計の要点 (`shapegen\`)

- **閉じたプリミティブの重ね合わせ** (`pieces.py`: Box / ConvexPrism / Ellipsoid / Revolve / RevolveRing)。
  ボクセライザの flood-fill は外から入れない領域を中実にするので CSG は不要。中空は必ず開口を持たせる
  (閉じた殻は埋まる)。凹断面 (U/I/H/Z、穴あき板、格子) は矩形の和で組む
- **連結の規則** (`assembly.py`): 2 ピースが「重なる」= 一方の内部点 (sdf ≤ −m) が他方にも深さ m 以上入る
  (双方向、m = `MIN_OVERLAP_VOXELS (0.3)` × h)。重なり点を含む voxel は両ピースの占有に入るので、
  各ピースの占有が連結なら全体も連結。形状全体は重なりグラフ (AABB で枝刈り) が 1 成分であること。
  これは Python 側の推定で、最終判定は `.mvox` の 6 連結検査。pilot (1,400 件) で非連結 0
- **最小厚み**: 厚み・半径・肉厚はすべて `ctx.thick()` 経由 (= `MIN_FEATURE_VOXELS × h_design × 回転余裕`)。
  開口・隙間は `ctx.opening` (3 voxel) 以上
- **決定性**: `derive_seed(seed, index)` (SplitMix64)、`random.Random` の `random()` だけで分布を組む。
  ファミリ選択は最初の 1 回。受理判定 (重複表) はインデックス昇順
- **Python にボクセライザを書かない** (spec §2 #8)。`estimate.py` は体積/h³ + 表面積/(2h²) の解析値
  (pilot 実測との log 回帰で slope 0.93 / R² 0.92 / 比 1.0) で、事前却下と FEM 見積りにだけ使う

## 決定性を壊すもの (再開・再現の前提)

- `--seed`、`FAMILY_SET_VERSION` (ファミリの追加・削除・重み・閾値を変えたら上げる)、`--profile`、
  `--rotation-mix`、`--family`、`--occ-min/max` のどれかが違えば別のデータセット (`run_config.json` で検査)
- `Editor.exe` のボクセライザが変わると `.mvox` が変わりうる (指紋を `run_config.json` に記録)
- Python / numpy / scipy の版 (`random.Random` のストリームは CPython が互換を保証、numpy は乱数に使わない)

## 時間とディスクの目安 (12 コア、pilot 実測から)

| 工程 | 目安 |
|---|---|
| 生成 + ボクセル化 + 検査 | 2,000 件/ラウンドで gen ≈ 10 s、vox ≈ 10 s、検査 ≈ 2 s。50,000 件 (受理率 ≈ 65% → 約 77,000 index) で 20〜40 分 |
| ディスク | obj ≈ 100 KB × 50k ≈ 5 GB、mvox 32 KB × 50k ≈ 1.6 GB (`data\` は gitignore) |
| FEM (`run_dataset.bat`, jobs 11) | 占有中央値 ≈ 1,450 で 35〜40 時間 (`stats.json` の `fem_hours_est_jobs11`) |

## トラブルシューティング

- `run_config.json と設定が食い違います`: 既存 `--out` と違う設定で再開しようとした。別の `--out` にするか `--force`
- 受理 0 のラウンドが続いて中断: `--family` の絞り込みで重複上限に当たった、`--occ-max` が小さすぎる等
- `vox_error` が多い: `work\round_*\voxelize_*.log` (ラウンド中) に Editor.exe の stderr が残る。
  `--keep-rejected` で却下分の OBJ/mvox を `rejected\` に残せる
- 特定ファミリの受理率が低い: `preview --family NAME` で統計とシートを見て寸法を調整し、`FAMILY_SET_VERSION` を上げる
