# sub-04 (M76d): Python — モデル / 学習 / export / fixture (大規模生成の門)

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること
ユーザー計画 Phase 8–9 / Checkpoint H。**ここを越えるまで ModelNet の生成を始めない** (台帳「ユーザー判断」3)。

- `model.py`: 3D U-Net 型 (sub-05 の C++ op 表と 1:1 の op 種だけ = Conv3d / ConvTranspose3d / ReLU / Add。学習時は BN あり、export で畳む)。
  基準構成: enc `conv3(1→16)+ReLU, conv3(16→16)` @32³ → `s2 (16→32)`, res(32) @16³ → `s2 (32→64)`, res(64) @8³ → `s2 (64→96)`, res(96) @4³ →
  `convT k4 s2 (96→64)` + Add(enc64), res(64) @8³ → `convT (64→32)` + Add(enc32), res(32) @16³ → head `conv3(32→64)+ReLU, conv1(64→192)` (活性なし = mask は logit、amp は 0..1 回帰)。≈1.8M params。
  **構造は sub-05 の時間計測で 1.5 s を超えたら縮める** (本学習 sub-08 の前なら自由)。
- `train.py`: Adam lr 1e-3 (`--lr` で論文 0.02)、batch 16、100 epoch、20 epoch ごと半減、loss = MSE(amp, valid cell のみ) + BCEWithLogits(mask)。
  ★**npz の品質指標を読んで除外・重み付けできること** (spec §4.1「品質指標」、ユーザー指示)。判断軸は **(1) Mel-band coverage と (2) residual 品質の 2 つだけ**:
  `--quality-max-residual R` (`residual_max > R` の npz を除外) / `--quality-min-coverage C` (`coverage_ratio < C` を除外) / `--quality-min-high-coverage C` (`coverage_high < C` を除外) / `--quality-weight` (`residual_max` と `coverage_ratio` に応じてサンプル重みを下げる)。**既定は全部 off** (= 全サンプル同じ重み) だが、off でも**読めている**ことが要件。しきい値の既定は stage0/stage1 の分布を見て planner が確定する (spec §7)。
  ★**mode count と `method` で分岐しないこと** — `modes_requested` / `f_top` / `mode_count` / `method` は診断・表示用のメタデータ (ユーザー指示: 「採否・重み付けは mode count ではなく band coverage と residual 品質で決定する」)。
  ★cell 単位の `cell_coverage` も読めるようにしておく (メッシュ単位の除外では粗すぎると分かったときに、loss のマスクへ落とせる余地を残す)。モード単位の除外は sub-03 の生成時に済んでいる (Mel 圧縮後は個々のモードが無い)。
  ★学習ログに「除外した npz 数 / 残差の分布」を 1 行出す (門の判定が品質フィルタで変わったのかを後から切り分けられるように)。
  `--overfit N --epochs 300`: N 形状に過学習させ **amp MSE < 1e-3 かつ mask acc > 99% を assert**。checkpoint は `runs\` (gitignore)。
- `export.py`: (1) BN を conv に畳む (2) fp16 に丸めて**から** fp32 で fixture 期待値を計算 (C++ と同じ重み、差は加算順だけ) (3) `paramCount ≤ 2,000,000` assert (4) `weightsHash` = FNV-1a (5) `bandCenterHz` は compact.py の値そのもの (6) ヘッダの `logAmpMin/Max` / `ampScale` (J=1 N·s の中央値ピークが −12 dBFS になる値) / 参照材質 / L_ref をデータセット統計から書く。
  `--fixture`: 幅 4/8/8/8 の小ネット (乱数重み、seed 固定) → `tests\deepmodal\fixture.dmnet` (≈50 KB) + `fixture_in.mvox` (builtin cube を sub-02 の CLI で作ったもの) + `fixture_out.bin` (有効 cell のうち固定 64 cell の index 表 + 64×192 float32)。
  `--random-full`: フルサイズ乱数重みの `.dmnet` を `data\` に出す (sub-05 の時間計測用、コミットしない)。
- `tests\test_export.py`: 往復 (書いた .dmnet を Python で読み戻して op 表と重みが一致)、paramCount assert、fixture の再現性 (同 seed → 同バイト列)。
- `.gitattributes` に `*.dmnet binary` / `tests/deepmodal/*.bin binary` (sub-02 の `*.mvox binary` と合わせる)。

## やらないこと (このサブでは)
C++ 側、ModelNet の生成、本学習。

## 触る場所 (planner の見立て)
- 新規 `tools\deepmodal\model.py` / `train.py` / `export.py` / `tests\test_export.py`
- 新規 `tests\deepmodal\fixture.dmnet` / `fixture_in.mvox` / `fixture_out.bin`
- `.gitattributes`、`tools\deepmodal\README.md` (門の記述)

## 受け入れ条件 (このサブ)
spec §5 の 9, 10。
1. **門**: `python train.py --overfit 16 --epochs 300` (stage0 + stage1 = `assets\models` 由来の形状を含む 16 形状) → amp MSE < 1e-3 かつ mask acc > 99%。ログの末尾数行を SELF_EVAL と コミット本文に残す
2. fixture 4 点がコミットされ、`pytest tests\test_export.py` 緑。`--random-full` の `.dmnet` が ≤ 4 MB
3. `pytest` 全体が緑のまま

## 検証コマンド
- `cd tools\deepmodal && pytest`
- `python train.py --overfit 16 --epochs 300 --data data\stage0 data\stage1`
- `python export.py --fixture --out ..\..\tests\deepmodal` / `python export.py --random-full --out data\full_random.dmnet`

## 実装メモ (coder が追記)

## フィードバック履歴
