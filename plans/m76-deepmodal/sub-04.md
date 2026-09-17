# sub-04 (M76d): Python — モデル / 学習 / export / fixture (大規模生成の門)

- 依存: sub-03
- 状態: OK (commit 4239ce3)
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
  `--overfit N --epochs 300`: N 形状に過学習させ **mask acc > 99% と amp の説明率 `R² = 1 − MSE/var(target)` を assert** (旧「amp MSE < 1e-3」は**撤回**。planner が根拠なく置いた値で表現上の下限 ≈ 0.002 を下回っていた — §8 / round 1 の切り分け)。checkpoint は `runs\` (gitignore)。
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
1. **門**: `python train.py --overfit 16 --epochs 300` → **mask acc > 99%** かつ **amp の説明率 `R² = 1 − MSE/var(target)`** が実測の表現上限に十分近いこと。閾値は**サイズ漏れ修正後に再計測して planner が §8 で確定**する。
   ★**前提**: `dataset.py` のサイズ漏れ修正 (FEM に `h_ref = L_REF/28` を渡す、spec §4.1) と `data\stage0` の再生成。修正前のデータは「同じ入力に異なる教師値」を含むので門の数値に意味が無い。
   ★ログに `MSE` / `var(target)` / `R²` と **`--overfit 1` の下限**を併記する (構造的下限のどれだけ手前かが分かる形)。旧閾値 `amp MSE < 1e-3` は**追いかけないこと**
2. fixture 4 点がコミットされ、`pytest tests\test_export.py` 緑。`--random-full` の `.dmnet` が ≤ 4 MB
3. `pytest` 全体が緑のまま

## 検証コマンド
- `cd tools\deepmodal && pytest`
- `python train.py --overfit 16 --epochs 300 --data data\stage0 data\stage1`
- `python export.py --fixture --out ..\..\tests\deepmodal` / `python export.py --random-full --out data\full_random.dmnet`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - `tools\deepmodal\model.py` (新規) — `ModalUNet` (Conv3d/ConvTranspose3d/ReLU/Add のみの
    3D U-Net、`widths=(stem,w1,w2,w3)` で一般化。基準構成 `(16,32,64,96)` で
    paramCount(BN畳み込み後)=1,682,448、fixture 用 `(4,8,8,8)` で 34,436)。
    `ConvBN3d.fold()` (BN を畳み込みへ畳む)、`OpSpec`/`_exec_op`/`run_graph` (forward
    学習経路と export の fp16 再計算経路が**同じ薄い実行器**を通る設計)、
    `voxel_to_tensor`/`output_to_cell_grid`/`cell_dense_to_tensor` (軸並び規約の
    唯一の変換点)、`round_folded_to_fp16` (重みだけ fp16 丸め、バイアスは fp32 のまま)。
  - `tools\deepmodal\train.py` (新規) — npz → dense 展開 (`densify`、`layout.cell_order()`
    使用) → amp 正規化 (`compute_amp_stats`/`build_target`、mask=1 の実データのみで
    min/max) → 品質フィルタ (`load_dataset` の `--quality-max-residual` /
    `--quality-min-coverage` / `--quality-min-high-coverage` / `--quality-weight`、
    既定 off、判断軸は band coverage と residual のみで method/mode_count では
    分岐しない) → 学習 (`--overfit` は LBFGS、それ以外は Adam + 20 epoch ごと半減) →
    checkpoint 保存。
  - `tools\deepmodal\export.py` (新規) — `build_dmnet_bytes` (ヘッダ/op表/重みblob/
    バイアスblob の組み立て、`fnv1a64` で weightsHash)、`read_dmnet`/`load_op_weights`/
    `ops_from_header` (往復)、`export_checkpoint`/`export_random_full`/`export_fixture`
    の 3 入口。fixture は builtin://cube を Editor.exe で焼き (一時ディレクトリ使用、
    `tests\deepmodal` 直下を汚さない)、書き出した `.dmnet` を**読み戻して** fp16 丸め
    後の重みで推論し、有効 64 cell (先頭から `layout.cell_order()` 順) の値を
    `fixture_out.bin` に書く。
  - `tools\deepmodal\layout.py` (既存を拡張) — `cell_order()` (dataset.py から引き上げ、
    train.py と共有)、`.dmnet` の `DMNET_*` 定数群 (ヘッダ 256B フォーマット文字列、
    op 表 48B フォーマット文字列、op 種別定数)。
  - `tools\deepmodal\dataset.py` — `_cell_order()` の private 実装を削除し
    `layout.cell_order()` を呼ぶだけに変更 (2 本目の順序定義を作らないため。
    既存 28 pytest は無変更で緑のまま = 挙動不変を確認済み)。
  - `tools\deepmodal\tests\test_export.py` (新規) — FNV-1a 既知ベクタ、ヘッダ 256B、
    random-full の往復 (op 表 + fp16 丸め後重みのビット一致)、op 表とグラフ定義の
    1 対 1 対応、paramCount 予算超過の assert、`--random-full` の 4MB 以内、
    同 seed 再現性、fixture 4 点の往復 (`@pytest.mark.editor`、Editor.exe 前提)。
  - `.gitattributes` — `*.dmnet binary` / `tests/deepmodal/*.bin binary` を追加。
  - `tools\deepmodal\README.md` — model/train/export の使い方、`.dmnet` バイト書式の
    [追加] 事項、overfit の門の実測ハイパーパラメータ探索結果を追記。
  - `tools\deepmodal\conftest.py` — **バグ修正**: `test_export.py` (torch を import) を
    先に集めると `test_modal.py::test_lobpcg_matches_exact_on_small_mesh` が
    ファイル収集順序依存で flaky に落ちることを発見 (torch 同梱の MKL/OpenMP が
    numpy/scipy のスレッド数を変え、shift-invert の残差が境界値をわずかに超える)。
    `dataset.py` が multiprocessing worker に対して既にやっている
    `OMP_NUM_THREADS=1` 等の固定を、conftest.py で pytest セッション全体
    (どのテストファイルが numpy/torch を import するより前) に適用して解消。
    3 回連続実行 + ファイル順序を入れ替えた実行で再現しないことを確認済み。
  - `tests\deepmodal\fixture.dmnet` / `fixture_in.mvox` / `fixture_out.bin` (生成物だが
    コミット対象。71304B / 32840B / 49408B)。

仕様との差分:
  - [追加] `.dmnet` ヘッダ: spec §4.2 は「256 B」と書くが、列挙フィールドを素直に足すと
    224B にしかならない (`.mvox` 64B→72B の前例と同種の数え違いの可能性)。256 という
    丸い数を意図的な伸び代と判断し、`reserved` を単数 4B ではなく 9×uint32 (36B) に
    広げて 256B ちょうどへ揃えた (`layout.DMNET_HEADER_FMT`)。sub-05 実装時に要確認。
  - [追加] op 表の `weightOffset`/`biasOffset` を「ファイル先頭からの絶対バイトオフセット」
    と定義した (spec は「blob 先頭から」としか書いていない)。blob 境界を cin/cout/k
    から逆算させない設計判断 — sub-05 のローダはこの前提で実装すること。
  - [追加] fixture の cell 有効性判定 (`export._cell_valid_from_occupancy`) を
    「2x2x2 voxel ブロックにいずれか占有があれば有効」というボクセル占有だけの規則にした
    (dataset.py の FEM/接触経由の判定とは別物 — ランタイムの baking は FEM を持たない
    ため、sub-05 の実際の有効性判定もこれと同じになる想定で作った仮定)。sub-05 実装時に
    要確認。
  - [追加] `ampScale`/`logAmpMin`/`logAmpMax` は fixture/random-full/checkpoint いずれも
    プレースホルダ (1.0 / -20.0 / 0.0 等)。`ampScale` の本来の較正 (J=1 N·s の中央値
    ピークが -12dBFS) は `ModalSynthRender` (C++、未着手) が無いと Python だけでは
    計算できない。M76f/g の耳確認まで持ち越し (spec §2 #12 が既にこの遅延較正を許容)。
  - [追加] `dataset.py` の `_cell_order()` を `layout.cell_order()` へ引き上げ (sub-03 の
    ファイルへの変更。動作は不変、既存 pytest 28 本が緑のまま)。
  - [追加] `train.py` に `--no-bn` / `--widths` / `--lbfgs-max-iter` の CLI を追加
    (sub-04.md に明記は無いが、overfit の収束実験に必要だった。既定値は spec の
    「基準構成」と整合し、指定しなければ従来どおり)。
  - [逸脱・要相談] **`--overfit` を Adam ではなく LBFGS で学習する** (README「overfit の
    実測」参照。Adam はどの学習率/減衰スケジュール/バッチサイズでも 300 step 前後で
    amp MSE が 0.0056〜0.044 に頭打ちしたが、LBFGS は 0.0019 まで下げられた)。
  - **[未達] 受け入れ条件 #1 (overfit の門) が閾値を満たしていない** — 詳細を
    「不安・質問」に記載。

検証:
  - `cd tools\deepmodal && pytest` → **36 passed** (既存 28 本 + 新規 8 本、全緑。
    `test_fixture_files_present_and_valid` は Editor.exe 実機で実行し PASS、skip では
    ない)。3 回連続実行、および `test_export.py`→`test_modal.py` の順序を入れ替えた
    実行の両方で安定して緑になることを確認 (conftest.py のスレッド数固定を参照)
  - `python train.py --overfit 16 --epochs 300 --data data\stage0 data\stage1`
    (data\stage1 は無いので `[train] WARNING: --data ディレクトリが見つかりません`
    を出して data\stage0 の 38 本のみ使用) →
    **amp_mse=0.001935 (要 <1e-3、FAILED)、mask_acc=100.000% (要 >99%、PASS)。
    exit code 1**。ログ全文は `tools\deepmodal\runs\overfit_gate.log`
    (gitignore 対象、コミットされない。「実装メモ」に主要行を転記済み)
  - `python export.py --fixture --out ..\..\tests\deepmodal` → fixture 4 点を生成、
    exit 0。手動で読み戻し検査 (書き出した .dmnet を読み戻して推論した値が
    fixture_out.bin と 64 cell 全て 1e-6 未満で一致) を実施し PASS
  - `python export.py --random-full --out data\full_random.dmnet` → 3,368,464 B
    (3.21 MiB、≤4MB 予算内)、exit 0
  - `python export.py --checkpoint runs\overfit_gate.pt --out runs\checkpoint_test.dmnet`
    (sub-04.md の必須検証コマンドには無いが「やること」記載の経路なので疎通確認) →
    exit 0、3.21 MiB
  - ハイパーパラメータ探索 (Adam 各種学習率/スケジュール/batch/容量、LBFGS、
    per-channel 正規化 vs global 正規化) の実測ログは README「overfit の門 — 実測と
    ハイパーパラメータ探索」節に表でまとめた

自己採点 (1-5):
  仕様適合: 2 — 受け入れ条件の核心 (overfit の門: amp MSE<1e-3 かつ mask acc>99%) の
    片方 (amp MSE) が未達 (0.001935 > 1e-3)。他の受け入れ条件 (fixture 4 点、
    paramCount 予算、pytest 全緑) はすべて満たしている
  正しさ: 3 — データ読み込み・チャンネル配置・BN 畳み込み・fp16 丸め・.dmnet 往復・
    軸規約はすべて独立に検証済みで矛盾は見つかっていない (round-trip 全一致、
    forward()とexport再計算の差が 1e-6 未満)。未達の amp MSE は「バグ」ではなく
    「この構成の収束限界」と判断した根拠 (容量 1x/4x/head4x、LR 1e-4〜2e-2、
    batch 1/4/16、global/per-channel 正規化、対称形状の有無を変えても floor が
    ほぼ同じ) を README に残したが、根本原因 (なぜ 0.002 付近で頭打ちになるか) は
    未解明のまま
  コード品質: 4 — forward/export が同じグラフ実行器を通る設計、軸規約を 1 箇所に
    集約、layout.py への定数集約など、このリポジトリの「規則は 1 本」原則に沿って
    書いた。LBFGS 分岐の追加で train.py の main() がやや複雑になった点は nit
  テスト: 4 — test_export.py 8 本 (FNV 既知ベクタ、ヘッダサイズ、往復、グラフ対応、
    予算超過、4MB 予算、再現性、fixture 実機往復) を追加、全 36 本緑。overfit の
    門自体は「テスト」ではなく受け入れ条件の実行ログとして記録

不安・質問:
  - **最重要: 受け入れ条件 #1 (overfit の門) の閾値 `amp MSE < 1e-3` が達成できていない。
    planner の判断を仰ぎたい。** 実測 (README 詳細):
    - Adam (学習率 1e-4〜2e-2、半減あり/なし、batch_size 1/4/16、容量 1x/4x、
      head 幅 4x のいずれの組み合わせでも) は 300 step 前後で amp MSE が
      0.0056〜0.044 に頭打ち。mask (BCE) 側はどの設定でもほぼ 99%+ に収束する
      — 頭打ちは amp 回帰だけの現象
    - LBFGS (2 次法、決定論的フルバッチ) に替えると Adam の最良値の半分以下
      (300 step=6000 内部反復で 0.001935、400 step=8000 内部反復の別実験で
      0.00225) まで下がるが、それでも 1e-3 の約 2 倍で足踏みする。step を
      増やすほど下がってはいるが減少幅は指数的に縮小しており (240→300 step の
      Δ が 0.000064→0.000015 と 1/4 ずつ縮む)、同じ設定のまま多少 step を
      増やしても実務的な時間で 1e-3 に届く保証がない (漸近的収束に見える)
    - 対称形状 (cube/sphere/cylinder) を除いた 16 形状でも floor はほぼ同じ
      (0.0035 前後) — 縮退モードのノイズが主因ではなさそう
    - 正規化方式 (帯域ごとの global min/max → 帯域ごとの per-channel min/max) を
      変えても改善しない (per-channel はむしろ悪化)
    - 提案 (planner の裁定を仰ぎたい): (a) 閾値を実測ベースで緩める
      (例: `<2e-3`、LBFGS 300 step で安定して満たせる水準) (b) `--epochs` を
      増やす方向で緩める (ただし上記の漸近的収束から効果は不確実) (c) 別の
      技術的方向 (loss 定式化・ネット構造・正規化方式の再検討) を sub-04 round 2
      として仕切り直す (d) このサブでは pipeline 疎通の検証(=完了)を主目的とし、
      閾値達成は M76h (実データでの本学習) まで持ち越す、を明記して次サブへ進める
      のいずれか。**筆者の見立ては (a) または (d)** — 仕様の趣旨 (「配管の正しさを
      確認してから大規模生成に進む」) は pytest 全緑・fixture 往復・LBFGS の
      100%mask精度で十分示せていると考えるが、`<1e-3` という具体的数値そのものの
      根拠は spec に無く (実測で決めた値ではない可能性が高い)、その意味で判断が
      planner 側にあると考えた
  - `.dmnet` ヘッダ 256B の解釈 ([追加] 参照) — sub-05 着手時に確定させてほしい
  - weightOffset/biasOffset の絶対オフセット方式 ([追加] 参照) — 同上
  - fixture の cell 有効性判定 (ボクセル占有のみ) が sub-05 の実際の baking 実装と
    一致するか — 同上
  - `ampScale` 較正 (プレースホルダ 1.0) は M76f/g 待ちのままでよいか (spec §2 #12
    が既に許容しているという理解だが、念のため)

触ったファイル:
  - tools\deepmodal\model.py (新規)
  - tools\deepmodal\train.py (新規)
  - tools\deepmodal\export.py (新規)
  - tools\deepmodal\layout.py
  - tools\deepmodal\dataset.py
  - tools\deepmodal\conftest.py (flaky テストのバグ修正)
  - tools\deepmodal\tests\test_export.py (新規)
  - tools\deepmodal\README.md
  - .gitattributes
  - tests\deepmodal\fixture.dmnet (生成物だがコミット対象)
  - tests\deepmodal\fixture_in.mvox (生成物だがコミット対象)
  - tests\deepmodal\fixture_out.bin (生成物だがコミット対象)

申し送り:
  - overfit の門が数値未達のまま。次サブ (sub-05) の依存関係上は model.py/export.py の
    インターフェース (`.dmnet` 書式、`ModalUNet`) はこのまま使える設計にしてあるので、
    planner が閾値/方針を決め次第、必要なら「sub-04 round 2」で train.py の学習方式
    だけ差し替えれば sub-05 以降への影響は無い想定
  - `data\stage1` が存在しない状態で `--data data\stage0 data\stage1` を実行すると
    警告を出して data\stage0 のみで進む実装にした (sub-03 の申し送りどおり、stage1
    生成はこのサブの範囲外)
  - `tools\deepmodal\runs\*.log` / `*.pt` に今回のハイパーパラメータ探索の生ログを
    残してある (gitignore 対象なのでコミットされないが、次に読む人が再現できるよう
    ファイル名はそのまま): `overfit_gate.log` (本命)、`lbfgs_long.log` (400 step 実験)、
    `perchannel_test.log` (正規化方式の比較)

---

SELF_EVAL: sub-04 (round 2)
実装 (指摘番号ごと):
  #1 (must、サイズ漏れ): `layout.py` に `VOXEL_DIVISIONS = 28` / `H_REF = L_REF / 28`
     を追加し、`dataset.py` の `fem.assemble_from_occupancy(grid.occ, grid.voxel_size, …)`
     → `fem.assemble_from_occupancy(grid.occ, layout.H_REF, …)` に変更 (全メッシュ共通の
     参照サイズで FEM を組む)。`data\stage0` を削除・再生成した (38 npz)。
  #2 (must、門の閾値撤回): `train.py` に `compute_r2()` (`R² = 1 − MSE/Var(target)`、
     amp チャンネル・有効 cell だけをプールした素朴な MSE/分散) を追加。
     `run_overfit_lbfgs()` の戻り値に `var_target`/`r2` を追加し、ログに
     `amp_mse` / `var_target` / `R²` / `mask_acc` を毎回併記するようにした。
     `main()` の門判定は **mask_acc>99% だけを assert** し、R² は自己判定せず
     report only にした (司会指示どおり)。旧 `amp MSE<1e-3` の assert は削除。
     `--overfit 1` (epochs=120) を実行し「下限」として README/ここに記録。
  #3 (should、重複確認): `cylinder_0`/`cylinder_3`/`cylinder_5` (ボクセル列バイト
     一致) の `feat` を再確認 → **後述のバグ 2 を直すまでは依然不一致 (最大 6.9)**
     だったため追加調査し、バグ 2 を修正してから再確認して**完全一致
     (bit-identical、max|diff|=0.0)** を確認した (下記「[追加] バグ 2」参照)。
  #4 (nit、.dmnet 書式は採用): 変更なし (README の記述もそのまま維持)。
  #5 (nit、conftest flaky 修正): 変更なし (README「固有値解法の 2 経路」節ではなく
     round 1 の実装メモに原因と対処を記載済み。今回追加で
     `test_solve_modes_is_deterministic_for_symmetric_mesh` という関連テストを
     `test_modal.py` に足した)。

  ★[追加] バグ 2 (指摘 3 の確認中に coder が発見。FIX_REQUEST には無かったが
  指摘 3 を満たすために必須だった): #1 を直した直後に cylinder_0/3/5 を再確認しても
  `feat` が最大 6.9 食い違ったままだった (round 1 の「最大 7.0」とほぼ同じ大きさ =
  round 1 の診断はサイズ漏れだけでは説明がつかない差分を含んでいたことになる)。
  切り分け: 同一プロセス内で同じ `K`/`M` に対し `modal.solve_modes()` を 2 回
  呼ぶだけで `vecs` が大きく食い違う (`freq maxdiff≈5e-5`、`vecs maxdiff≈0.48`) こと
  を実測。原因は `eigsh` が `v0` (Krylov 初期ベクトル) 省略時に乱数を使うこと —
  円柱等の対称形状は固有値が縮退するため、初期ベクトルが変わると縮退部分空間内の
  基底が変わり、`contact.py` が読む per-node 振幅 (npz の `feat`) が実行のたびに
  変わる。`solve_modes_lobpcg()` は既に `seed` で `v0` を固定していたため無傷
  だった (= exact 経路だけの欠陥)。
  修正: `modal.solve_modes()` に `seed: int = 0` を追加し、
  `v0 = np.random.default_rng(seed).standard_normal(ndof)` を `eigsh` に渡すように
  した (既存呼び出し元はすべてキーワード引数で `f_min`/`f_max` を渡しており
  `seed` は末尾のデフォルト引数なので後方互換)。`data\stage0` をこの修正後に
  もう一度再生成し、指摘 3 の完全一致を確認した。回帰テストとして
  `test_modal.py::test_solve_modes_is_deterministic_for_symmetric_mesh` を追加。

仕様との差分:
  - [追加] バグ 2 (`eigsh` の `v0` 非決定性) は FIX_REQUEST に無かった追加修正。
    calibrate.py も同じ `modal.solve_modes()` を呼んでいるが、`seed` はデフォルト
    引数なので**呼び出し元を変更せずに**恩恵を受ける (calibrate.py 自体は
    「同一ジョブ内で exact と LOBPCG を比較する」用途なのでクロスラン再現性は
    元々問題にならないが、副次的に決定論的になった)。calibrate.py 自体には
    手を入れていない (FIX_REQUEST の対象外、スコープを広げないため)。
  - [nit・要確認] `data\stage0` 再生成後の統計が変わった: `eigsh_seconds` 中央値
    20.1s (旧 ~60s 台)、`mode_count` 中央値 14 (旧 30.5)、`coverage.mean_ratio` 0.336
    (旧 0.45)。全メッシュを参照サイズ (0.3m) で解くようになったため、周波数帯域
    100-10000Hz に入るモード数の分布が変わったことによる自然な帰結 (バグではない)。
    ただし spec §8 の「L_REF/fMax は stage0 統計 (旧バグ入りデータ) で確定」の
    根拠がこの新しい分布とは食い違うため、**M76h 前に L_REF/fMax を再検討するか
    planner に確認してほしい**(不安・質問へ記載)。
  - [追加] `dataset.py` regen コマンドを Bash ツールから実行する際、
    `--out data\stage0` (バックスラッシュ) を渡すと Bash がバックスラッシュを
    1 段潰して `datastage0` という**別ディレクトリ**に書いてしまう事故を実際に
    踏んだ (script-editing-traps と同型の罠。今回は `/` 区切りに直してから
    再実行し、誤って書かれた 38 npz は正しい場所へ移動してから誤ディレクトリを
    削除した — 実際の計算結果は流用できた。以後 dataset.py 系のコマンドは
    forward slash で統一する)。

検証:
  - `cd tools\deepmodal && pytest` → **37 passed** (36 + 新規回帰テスト 1 本)
  - 重複確認 (指摘 3): `cylinder_0/3/5` (occ=1305, valid_cells=225) の `vox` は
    3 本とも bit-identical。**`feat` も 3 本とも bit-identical (max|diff|=0.0)**
    (バグ 1+2 修正後)
  - `python train.py --overfit 1 --epochs 120 --data data\stage0 data\stage1`
    → `data\stage1` 無しで警告、data\stage0 のみ使用。
    **amp_mse=0.000583、var_target=0.014639、R²=0.9601、mask_acc=100.000%**、exit 0
  - `python train.py --overfit 4 --epochs 300 --data data\stage0 data\stage1`
    → **amp_mse=0.000139、var_target=0.005082、R²=0.9726、mask_acc=100.000%**、exit 0
  - `python train.py --overfit 16 --epochs 300 --data data\stage0 data\stage1`
    (受け入れ条件の本命コマンド) →
    **amp_mse=0.001661 (pooled)、var_target=0.021765、R²=0.9237、
    mask_acc=100.000%**、exit 0 (mask acc 条件のみ assert、R² は report only)
  - `python export.py --fixture` / `--random-full`: round 1 から変更していないため
    再実行せず (fixture/random-full は乱数重みでデータセットに依存しないため
    今回の修正の影響を受けない)。`pytest tests\test_export.py` (8 本) は上の
    37 本に含めて確認済み

自己採点 (1-5):
  仕様適合: 4 — 指摘 1・2・3 をすべて実装し、mask acc>99% は 3 つの N すべてで
    100% 達成。R² (0.92〜0.97) は「実測の表現上限に十分近いか」の最終判断が
    ユーザー待ちなので 5 にはしない
  正しさ: 4 — 重複確認が bit-identical まで到達し、`solve_modes` の決定性を
    独立に実測・回帰テスト化した。round 1 で「原因不明」としていた残差
    (バグ 2) を実際に特定・修正できたことで、データパイプラインの正しさへの
    確信度が上がった。5 にしない理由: L_REF/fMax の再検討要否が未解決のまま
    残っている (上記 [nit・要確認])
  コード品質: 4 — 修正は最小限 (2 箇所の値渡し変更 + 1 引数追加)、コメントに
    実測根拠と失敗した誤診断の経緯を残した
  テスト: 4 — 決定性の回帰テストを追加。R² 計算自体の単体テストは追加していない
    (train.py の関数なので tools\deepmodal\tests 配下に置く先例が無く、
    今回は実行ログでの検証にとどめた — 必要なら nit として指摘してほしい)

不安・質問:
  - L_REF (0.3m) / fMax (10000Hz) は spec §8 で「stage0 統計 (旧バグ入りデータ:
    モード数中央値 30.5、coverage 平均 0.45) を見て確定」となっているが、
    バグ修正後の新しい統計はモード数中央値 14、coverage 平均 0.336 と有意に
    下がっている。M76h (ModelNet10) に進む前に L_REF/fMax を再検討すべきか
    planner の判断を仰ぎたい (このサブでは再検討していない — sub-04 の
    スコープ外と判断したが、確信は無い)
  - R² の最終閾値確定に向けて、参考値をもう少し増やすべきか (例: `--overfit 8`)。
    今回は 1/4/16 の 3 点で足りると判断したが、要望があれば追加で計測する
  - `--overfit` の N によって R² が単調でない (0.96→0.97→0.92、N=4 が最良) のは
    `select_overfit_subset` の等間隔サンプリングが選ぶ形状構成に依存するため
    と考えているが、他の要因 (LBFGS の収束の run-to-run ばらつき等) を完全には
    排除できていない — 大きな懸念ではないが記録しておく

触ったファイル (round 2 で追加/変更、round 1 分は上記参照):
  - tools\deepmodal\dataset.py (バグ 1 修正)
  - tools\deepmodal\layout.py (H_REF/VOXEL_DIVISIONS 追加)
  - tools\deepmodal\modal.py (バグ 2 修正: solve_modes に seed/v0 追加)
  - tools\deepmodal\tests\test_modal.py (決定性の回帰テスト追加)
  - tools\deepmodal\train.py (R² 計算・報告、門判定の組み替え)
  - tools\deepmodal\README.md (バグ 2 件の記録、round 2 の再計測値)
  - plans\m76-deepmodal\sub-04.md (このメモ)

申し送り:
  - `data\stage0` は 2 度目の再生成 (バグ 1+2 修正後) の状態でディスクに残っている
    (gitignore 対象、コミットされない)。sub-08 (M76h) が ModelNet10 を回す前に、
    上記の L_REF/fMax 再検討の要否を確認してほしい
  - calibrate.py は今回触っていないが、`modal.solve_modes()` の `seed` 既定値
    追加により副次的に決定論的になった。校正ジョブを再実行する場合、round 1 の
    `calibration.json` の数値とは (方向性は変わらないはずだが) 完全一致しない
    可能性がある

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-16)。門が未達なのは coder の実装の不備ではなく、**(1) データ生成のサイズ漏れ**と**(2) planner が置いた閾値の誤り**の 2 つが原因、と実測で切り分けた。
  (1) `dataset.py:83` が FEM に `grid.voxel_size` (実寸) を渡していた。ボクセル化は最長辺で正規化する = 入力はスケール不変なので、同じ入力に違う教師値が生まれる。実測で `cylinder_0/3/5` のボクセル列が**バイト一致** (占有 1305) なのに L=0.330/0.373/0.566 で `feat` が最大 7.0 違うことを確認。論文 §5.1 (学習時は同じスケール) + 後処理 σ3 の設計から、**ランタイムが σ3 を二重適用する**欠陥でもある (sub-05/sub-06 の正しさに直結)。→ spec §4.1 に `h_ref = L_REF/28` を明記。
  (2) 門の閾値 `amp MSE < 1e-3` は planner が根拠なく置いた値で、**表現上の下限を下回っていた**。切り分け: 重複入力は選ばれた 16 本に**含まれない** (原因ではない) / **1 サンプルのみ** (240 cell × 96 = 23k 値を 1.68M パラメータ = 73 倍の過剰パラメータ) でも 0.00109 で頭打ち = 容量でも最適化でも標本数でもなく構造的 / 目標場の隣接 cell 間二乗差 0.00392 → cell 単位の予測不能成分の分散 ≈ 0.00196 が **観測下限 0.001935 とほぼ一致**。畳み込みは並進同変なので、受容野の中身が同じ内部 cell を区別できない (`contact.py` が cell ごとに代表節点を 1 つ選ぶ = 目標場に cell 単位のジッタが乗る)。**現状のネットは R²=0.851 / 到達可能上限 ≈0.849 = 学習可能な信号をほぼ全部取れている**。→ 門を「mask acc + amp の R²」に組み替え、閾値はサイズ修正後に再計測して確定。
  coder の提案 (a)(d) は方向として正しかったが、**先に直すべきデータ欠陥があった**ので round 2 で両方やる。LBFGS 採用・conftest の flaky 修正・fixture・paramCount・.dmnet 往復は問題なし。
- round 2: **VERDICT: OK** (planner、2026-09-16)。round 1 の must 2 件を実測で消し込み、**門を確定**した (spec §5 #9: mask acc > 99% かつ R² ≥ 0.90)。planner の再実行・独立確認: `cylinder_0/3/5` の `feat` が **bit-identical (max|diff| = 0.0)** = サイズ漏れと `v0` 乱数の両方が消えた / `dataset.py:93` が `layout.H_REF` を渡している / `modal.py:150,156,160` が `seed` を `params` に記録し `v0` を固定 / pytest **37 passed** / `data\stage0` 全 38 npz の exact 経路の seed は 0 で一貫。★**planner の round 1 の主張を訂正した**: 「下限 ≈0.002 は構造的でネットは上限にいる」は誤りで、clean データでは実測 MSE が見積もり下限を 3 例とも下回った (N=1 0.000583 / N=4 0.000139 / N=16 0.001661)。頭打ちの真因は表現力ではなく**データ欠陥 2 件**だった。1e-3 撤回の結論は維持 (N=16 は clean でも 0.001661 > 1e-3) だが理由が変わったので §8 に明記した。coder が FIX_REQUEST に無かった**バグ 2 (`eigsh` の `v0` 乱数) を自力で発見・修正**した点は、決定論契約 (規則 8) に触れる欠陥をオフライン側で塞いだ good catch。
