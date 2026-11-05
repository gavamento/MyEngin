# DeepModalModel: プロシージャル形状データセット (候補 3 の実装)

- 作成日: 2026-10-11
- 対象: `DeepModalModel\` (リポジトリ直下)、`tools\deepmodal\train.py` の軸修正
- 位置づけ: `modelnet40_eval_report.md` §5「候補 3: プロシージャル合成メッシュ群 (CSG / SDF)」の実装。
  ModelNet40 (11,911) + Thingi10K (951) + creatures (1,257) + stage0/1 (124) ≈ 14,200 件に、
  ゲーム小道具中心の 53 ファミリから **50,000 件以上** の形状を足す

## 1. 何を作ったか

| 成果物 | 内容 |
|---|---|
| `DeepModalModel\generate.py` + `shapegen\` | 形状生成器。閉じたプリミティブ (箱 / 凸角柱 / 楕円体 / 回転体 / 環) の重ね合わせで 53 ファミリを組み、`Editor.exe --modal-voxelize` で `.mvox` にして受理検査し、`dataset.py --stage small` 用の list を書く |
| `run_pilot.bat` / `run_full.bat` / `run_dataset.bat` | 1,000 件のパイロット / 50,000 件の本生成 / FEM (list を 10 分割して順に流す) |
| `DeepModalModel\tests\` | pytest 122 件 + Editor.exe 実機スモーク 5 件 |
| `tools\deepmodal\train.py` の修正 | 学習の入力ボクセルが転置されていなかった退行 (下記 §4) を直し、`tests\test_train_axes.py` を追加 |

使い方・出力・閾値の正本は `DeepModalModel\README.md`。

## 2. 有効 / 有用の定義 (要約)

**有効** = 実際のボクセライザを通した `.mvox` が、占有 300〜3,500 voxel、6 近傍で 1 連結成分、
占有 cell ≥ 40、糸状 voxel ≤ 5%、完全重複なし、16³ キーの重複 < 3 件、を満たす
(`shapegen\validate.py`)。FEM は剛体モード 6 本固定で解く (`modal.py`) ので、単一成分は必須条件。

**有用** = ゲーム内で衝突音を鳴らす物体に似たファミリ構成 (中実 8 / 板・殻・格子 23 / 容器 13 /
武器・工具 10 / 家具・建材 14 / 機械 12 / キャラ・有機 15 / 汎用 5 %)、中空には必ず開口、
ランダム回転 (軸平行 / 立方群 / 傾き / 自由) でボクセル化の階段パターンも学習させる。

## 3. 設計判断

1. **Python にボクセライザを書かない** (spec §2 #8 を維持)。連結・占有の最終判定は実物の `.mvox` で行い、
   Python 側は「ピースの重なりグラフが連結」という推定と、体積/h³ + 表面積/(2h²) の解析的な占有見積り
   (pilot 実測との log 回帰: slope 0.91〜0.95、R² 0.85〜0.93、中央値の比 1.00) だけを持つ。
2. **閉じた殻の重ね合わせ、CSG 無し**。ボクセライザの flood-fill は外から入れない領域を埋めるので、
   各ピースが閉じていれば和は正しく中実になる。凹断面や穴は矩形の和で組む (ear clipping 不要)。
3. **決定性**: 形状 i は `(seed, i)` の純関数 (SplitMix64 で seed を導出し `random.Random` の `random()` だけで
   分布を組む)。受理判定 (重複表) はインデックス昇順。並列度・中断・再開で結果が変わらない。
   `FAMILY_SET_VERSION` をファミリ定義の版とし、`run_config.json` と照合して違う設定での再開を拒む。
4. **占有分布は ModelNet40 より軽く** (中央値 ≈ 1,300、p95 ≈ 2,600)。FEM (eigsh) は占有数で急増する
   (1k〜2k: 24 s、4.5k〜6k: 136 s) ため、上限 6,000 のままだと 50,000 件が 11 並列で 67 時間級になった。
   推定占有が 1,500 を超えた形状は厚み (`ctx.fill`) と副寸法 (`ctx.lean`) を縮めて組み直す。
   相似形に縮めても占有が変わらない「塊」(岩・積層円盤・ランダム結合) は扁平・細長い構成に変えた。
5. **最小厚みは 1.6 voxel** (当初 2.0)。回転余裕と合わせると殻が 3 voxel 超になり占有が重くなったため。
   1 voxel 層 (FEM で 1 要素厚) は避けつつ、ModelNet40 の薄板に近い厚みを許す。

## 4. 別件: train.py の入力軸の退行

`ModalNpzDataset.__getitem__` と overfit 用 `load_samples` は npz の `vox` (`occ[x,y,z]`) を
転置せず `(D,H,W)=(x,y,z)` として渡していた。教師 (`target[:, cz, cy, cx]`) とモデル規約
(`model.voxel_to_tensor`、D=z,H=y,W=x)、`export.py` / C++ の fixture 照合経路は z,y,x 順なので、
**学習だけが x↔z を取り違えていた** (コミット f17ad63 で `voxel_to_tensor` 呼び出しが落ちた)。
`deepmodal_full.pt` / `deepmodal_creatures.pt` はこの状態で学習されている。2 箇所を
`model.voxel_to_tensor` 経由に戻し、`tests\test_train_axes.py` で固定した。**再学習は未実施**
(synth_v1 の npz が揃ってからユーザー判断)。

## 5. 実測 (2026-10-11、12 コア)

| 項目 | 値 |
|---|---|
| パイロット 2,000 index | 受理 1,781 (89.0%)、占有 p5/25/50/75/95 = 520/980/1309/1733/2586、非連結 0、ユニーク 16³ キー 1,769 |
| 却下理由 | pre_reject_thin 4.3%、pre_reject_est_high 3.5%、dup_exact 1.9%、occ_high 0.9% |
| 速度 | 1,000 index あたり生成 ≈ 9 s、ボクセル化 ≈ 5 s (Editor.exe 4 並列)、検査 ≈ 2 s |
| FEM 見積り | 受理 1 件あたり平均 ≈ 25 s (ModelNet40 の bin 別中央値から) → 50,000 件で 11 並列 ≈ 31 時間 |

本生成 (`data\v1`、seed 20261011) と FEM (`tools\deepmodal\data\synth_v1`) の結果は Notion の活動記録と
`DeepModalModel\data\v1\stats.json` を参照。

## 6. 未検証・申し送り

- 再学習と pooled R² への効果は未測定 (synth_v1 の npz 完了後に `train.py --data ... data\synth_v1` で確認)。
- FEM 側の健全性 (`mode_count==0` の比率、`coverage_ratio`) はファミリ接頭辞別に集計して ModelNet40 と比べること
  (`docs\test_checklists.md` の該当項目)。
- ファミリを追加・調整したら `FAMILY_SET_VERSION` を上げる (既存 run への再開を拒否するため)。
