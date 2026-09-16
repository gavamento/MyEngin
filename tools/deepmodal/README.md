# Deep-Modal データセット生成ツール (M76c)

`MyEngine.sln` の外 (`tools\collab` の Rust と同じ流儀)。学習済み 3D-CNN による
衝突音のモーダル合成 (`plans\m76-deepmodal\spec.md`) のうち、Python 側の
FEM / 固有値 / 接触励起 / Mel 圧縮 / データセット生成を担当する。

## セットアップ

```
pip install -r tools\deepmodal\requirements.txt
```

torch は上の `requirements.txt` に**含めていない** — 素朴に `pip install -r` すると
pip が CPU 版へ差し替えてしまう危険があるため。CUDA 版が要る場合は別途:

```
pip install torch --index-url https://download.pytorch.org/whl/cu128
```

環境変数 `MYE_EDITOR_EXE` で `Editor.exe` の場所を指定できる (既定は
`bin\x64\Release\Editor.exe`、無ければ `bin\x64\Debug\Editor.exe` にフォールバックする)。
`--modal-voxelize` の C++ 側実装 (M76b) を含むビルドが必要 — 古い Release バイナリの
ままだと `--modal-voxelize` が認識されず、無言で通常のエディタ GUI が起動する
(sub-03 round 1 で実際に踏んだ罠。Release を焼き直すこと)。

## データ段階の門 (spec §4.1、ユーザー決定)

データ段階は Primitive → 小規模自前 → ModelNet10 → ModelNet40 の順で拡張する。
**`train.py --overfit 16 --epochs 300` が amp MSE < 1e-3 かつ mask acc > 99% を
満たすまで、ModelNet の生成コマンド (M76h) を実行してはならない。**
このディレクトリ (`tools\deepmodal`) には ModelNet 用のダウンロード/変換スクリプトを
まだ置いていない (sub-04/sub-08 の担当)。

## ファイル

| ファイル | 役割 |
|---|---|
| `layout.py` | 共有定数 (`VOXEL_N`/`MAP_N`/`MEL_BANDS`/`CHANNELS` は `check_rules.ps1` が C++ の `ModalTypes.h` と機械照合する)、`.mvox` ヘッダのレイアウト、参照材質、Mel 帯域中心 |
| `meshio.py` | `.mvox` の読み込み (C++ が書いたバイト列をフィールド単位で読むだけ。Python は自前のボクセライザを持たない) |
| `voxelize.py` | `Editor.exe --modal-voxelize` の subprocess ラッパ (`cmd /c` 経由、200 件ごとにプロセス再起動) |
| `primitives.py` | 箱/板/円柱/球/中空箱/L字/穴あき板を寸法乱数 (seed 固定) で OBJ 生成 |
| `fem.py` | hex8 要素の Ke/Me と、占有ボクセルからのグローバル行列組み立て |
| `modal.py` | 一般化固有値問題 `K x = λ M x` の解法 (shift-invert が既定、大きすぎるメッシュは LOBPCG へ — 下記「eigsh の時間について」) |
| `contact.py` | 16^3 cell ごとの接触節点選択と励起ベクトル `a_ij` の計算 |
| `compact.py` | モード列を Mel 32 帯域 x 3 軸へ圧縮 (mask / Σ\|a\| / ln / 空帯域補間) |
| `dataset.py` | 1 メッシュ → npz の生成ドライバ (multiprocessing、再開可能、`stats.json`) |
| `calibrate.py` | 基準形状で shift-invert と LOBPCG を両方走らせ `calibration.json` に残差分布・周波数突き合わせ・取りこぼしモード数・帯域集計差を書く (下記「固有値解法の 2 経路」参照) |
| `model.py` | (M76d) `ModalUNet` — Conv3d/ConvTranspose3d/ReLU/Add だけで組んだ 3D U-Net。`widths` でチャンネル幅を一般化し、基準構成 (16,32,64,96) と fixture (4,8,8,8) の両方を同じグラフ定義から作る (下記「.dmnet と op グラフ」参照) |
| `train.py` | (M76d) npz → 学習。品質フィルタ (既定 off)、`--overfit N --epochs 300` の大規模生成の門 |
| `export.py` | (M76d) BN 畳み込み → fp16 丸め → `.dmnet` 書き出し。`--checkpoint` / `--fixture` / `--random-full` の 3 入口 |

## 実行

```
python -m pytest
python dataset.py --stage primitives --out data\stage0
```

(`pip install` 直後は `pytest.exe` が PATH に無いことがある — `python -m pytest` なら確実。
pip 自身もインストール時にその旨を警告する。)

`--stage primitives` は `tests\deepmodal\list_builtin.txt` の builtin 6 種と、
`primitives.py` が生成する形状 (既定 `--variants 6` = 7 種類 x 6 = 42 個) を
まとめて処理し、`data\stage0\<stem>.npz` と `data\stage0\stats.json` を書く。
既に `.npz` があるメッシュはスキップする (再開可能)。

npz の中身:
- `vox: uint8[32,32,32]` / `valid: uint8[16,16,16]` / `feat: float16[valid数,192]`。
  `feat` のチャンネル配置は `layout.mask_ch(j,i)` / `layout.amp_ch(j,i)`
  (j=力軸0..2, i=帯域0..31) と同じ — C++ の `ModalTypes.h` の `MaskCh`/`AmpCh` に対応する。
  mask は 0/1 (BCE の教師値そのもの、シグモイド前 logit ではない)、amp は
  **正規化前の自然対数** (Σ|a| の ln)。0..1 への正規化は sub-04 の export.py 側で
  データセット全体の統計から決める (このツールは生の対数値のまま保存する)。
  `r > RESIDUAL_DROP` のモードは feat に集計する前に除外されている。
- solver metadata: `method` (str) / `solver_params` (JSON 文字列) / `iterations` (int) /
  `converged` (bool)
- convergence quality: `residuals` (float32[元のモード数]、除外前の全モード分) /
  `residual_max` / `residual_median` / `dropped_mode_count` / `marginal_mode_count`
- **Mel-band coverage** (spec §4.1、ユーザー追加指示。round 3 で確定): `band_coverage`
  (float32[32]、帯域ごとに「この valid cell のうち何割でその帯域が鳴ったか」)、
  `coverage_ratio` (32 帯域平均、旧 `band_occupancy_ratio` を置き換える唯一の名前 —
  定義が違う 2 つの名前を持たないこと)、`coverage_high` (上位 8 帯域 = index24..31
  の平均、高域が系統的に空かを見る)、`cell_coverage` (float32[valid_cells]、`feat`
  の行と同順。cell ごとに「32 帯域のうち何割が鳴ったか」)。
  軸の畳み方は **OR** (`compact.cell_band_coverage`) — ランタイムの `BuildModes` が
  `Σ_j |k_j|·mask_j,i` と軸を足すので、1 軸でも mask が立てばその帯域は実際に鳴る。
- 診断値 (**採否・重み付けには使わない**。予算で頭打ちになったのか、そもそも
  帯域内にモードが無いのかを切り分けるためだけの値。`modal.diagnostics()`)。
  `modes_requested` (要求した非剛体モード数) / `f_top` (見つけた最高周波数) /
  `spectrum_complete` (`mode_count < modes_requested` なら True = 予算の外まで
  帯域内を拾い切った。False なら予算いっぱいまで埋まった = その先にまだ
  モードがあるかもしれない)
- その他のスカラー (再開時の stats.json 再構成にも使う): `occupied_voxels` / `ndof` /
  `assemble_seconds` / `eigsh_seconds` / `mode_count` (除外前) / `valid_cells`

sub-04 (`train.py`) は `residual_max` / `coverage_ratio` / `coverage_high` / `cell_coverage`
を読んでメッシュ単位の除外・重み付けに使う (`--quality-max-residual` /
`--quality-min-coverage` / `--quality-min-high-coverage` / `--quality-weight`)。判断は
残差と coverage の数値だけで行い、`method` や `mode_count` では分岐しない
(いずれも表示・記録専用のメタデータ)。

## 固有値解法の 2 経路と品質指標 (spec §4.1、ユーザー追加指示)

正則グリッド由来の hex8 メッシュは、占有 voxel 数が増えると shift-invert (完全 LU
分解、SuperLU) の埋め込み (fill-in) が要素数の 2 乗を超えて増える。実測:

| 占有 voxel | 自由度 | 完全 LU 分解のみ | 埋め込み (nnz(L+U)/nnz(K)) |
|---|---|---|---|
| 1305 | 5760 | 0.14 s | 6.2 倍 |
| 15979 | 54000 | 113 s | 57 倍 |

満杯の 29^3 立方体 (このリポジトリのボクセル正規化で到達できる最大占有 = 24389、
自由度 81000) では、完全 LU 分解が数 GB 級のメモリと非常に長い時間を要求する
(実行環境の空き RAM が数 GB 級の場合は危険)。

そのため `dataset.py` は **`MAX_OCCUPIED_EXACT` (既定 9000 voxel) を超えるメッシュを
`modal.solve_modes_lobpcg()` (不完全 LU 前処理の LOBPCG、メモリが有界) へ回す**。
builtin 6 種は cap を超えても常にこの経路で npz を作る (受け入れ条件 7 の
「満杯 30^3 立方体の eigsh < 600 s」の実地プローブを兼ねる — 実測 cube 279s /
cylinder 378s / sphere 205s)。primitives.py が生成する形状は cap を超えたものを
スキップし、`stats.json` の `count_skipped_cap` に数える (中央値の計算対象からも外れる)。

★**採否は `method` (solver 名) ではなく数値 (残差) で決める**。個々の固有対の
相対残差 `r_i = ‖K x̂_i − λ_i M x̂_i‖₂ / ‖λ_i M x̂_i‖₂` (x̂ は M-正規化) を
`modal.compute_residuals()` が計算し、`dataset.py` が次のしきい値で扱いを分ける:

- `r <= RESIDUAL_ACCEPT (1e-5)`: そのまま採用
- `RESIDUAL_ACCEPT < r <= RESIDUAL_DROP (1e-3)`: 採用するが `marginal_mode_count` に数える
- `r > RESIDUAL_DROP`: そのモードを Σ\|a\| (= npz の feat) から除外し `dropped_mode_count` に数える

npz と `stats.json` の両方に solver metadata (`method` / `solver_params` / `iterations` /
`converged`) と convergence quality (モードごとの `residuals` 配列 + `residual_max` /
`residual_median` / `dropped_mode_count` / `marginal_mode_count`) を保存する。

### 校正ジョブの実測結果 (calibrate.py)

```
python calibrate.py --out data\calibration.json --full
```

★**round 2 の誤診断 (訂正)**: 当初「LOBPCG が 116/150 (77%) のモードを取りこぼす」
と報告したが、これは**直接法に `k=150`、LOBPCG に既定の `m=40` (非剛体 34 本)
という違う予算を渡して比較した結果**であり、解法の質の差ではなかった
(capsule=16200DOF と sphere=45864DOF という規模の違う 2 形状で missed=116 が
完全一致したのが証拠 — 収束の失敗ならメッシュごとに違う値になるはず)。
`calibrate.py` を修正し、**両解法に同じ非剛体モード数 (既定 34) を要求する
`budget_matched` 比較**を追加した。予算を揃えた実測 (2026-09-16):

- **予算を揃えると取りこぼしは 0**: capsule/sphere どちらも `matched_count=34`,
  `missed_count=0`。周波数の相対誤差は capsule で最大 2.1e-7、sphere で最大 4.4e-4、
  中央値はどちらも 1e-12〜1e-13 — **LOBPCG が計算したモードは (見つけた範囲では)
  正しい**。
- **残差しきい値は妥当** (据え置き確定、ユーザー回答): 直接法の残差は最大 2.3e-8
  (`RESIDUAL_ACCEPT`=1e-5 より 3 桁小さい)、LOBPCG は sphere で 1 本だけ 5.2e-2 の
  悪いモードがあり `RESIDUAL_DROP`=1e-3 を優に超える — しきい値がこの 1 本を
  正しく検出・除外できることを確認した。
- **帯域集計 Σ\|a\| の相対差も予算を揃えると小さくなる**: 平均 6.2%(capsule)/7.7%
  (sphere)、最大 16.9%/22.4% (round 2 の「平均 49-61%」は予算差込みの数字だった)。
  残る数%〜20%強の差は、sphere の悪いモード 1 本の寄与と、縮退モードの基底の
  取り方の違い (§4.1 の罠) で説明がつく範囲
- **`direct_reference_large_k` (直接法だけ k=150) は capsule/sphere ともちょうど
  150 本で頭打ち** (`spectrum_complete=false`) — つまり**直接法も**「帯域内に
  まだモードがあるかもしれない」状態で、これは LOBPCG 固有の問題ではない。
  この「予算で頭打ちか、本当に帯域内が空か」を見分けるための診断値
  (`modes_requested` / `f_top` / `spectrum_complete`) を npz と stats.json に
  記録するようにした (下記「Mel-band coverage」参照) — ★**これらは採否・重み
  付けには使わない** (診断専用)
- 結論: `dataset.py` の既定 (占有 cap 超は LOBPCG、既定 `m=40`) は**このラウンドで
  変更していない**。取りこぼしは実際には起きていなかったので、m を上げる
  必要性そのものが無い (round 2 で懸念していた trade-off は前提が誤りだった)

### stage0 の帯域占有分布 (指摘3。band_coverage の実測)

`data\stage0` を再生成した `stats.json` の `coverage.band_distribution_by_method`
(38 npz、method ごとの帯域ごと平均 coverage) を見ると、**高域バンドが解法/予算に
依存して系統的に偏っている**ことが分かった:

| | n | `coverage_ratio` 平均 | `coverage_high` 平均 (index24-31) | `f_top` | `spectrum_complete` |
|---|---|---|---|---|---|
| exact (k=150) | 35 | 0.478 | 0.789 | 様々 | 32/35 が True (帯域内を拾い切った) |
| lobpcg (m=40、builtin 3 種) | 3 | 0.198 | **0.000** | 3463–4740 Hz | 3/3 が False (予算で頭打ち) |

**lobpcg 経路の 3 メッシュ (cube/cylinder/sphere) は高域バンド (24-31、9kHz 台) が
全く覆われていない** — これは「LOBPCG が高域を苦手とする」のではなく、**この 3 本は
占有 voxel 数が最大級 (自由度 45864-81000) = モード密度が非常に高く、`m=40` という
固定本数の予算では 3500-4700 Hz あたりまでしか届かない** ため (calibrate.py の
`direct_reference_large_k` でも同じ形状に対して直接法 k=150 が同様に途中で頭打ちに
なることを確認済み — 予算を大きくすれば exact でも同じことが起きる)。
低域バンド (0-10) は全体平均でも低い (0.03-0.29) が、これは spec §7 の実測メモ
(「一辺 0.13m 程度の中実アルミ塊は基本周波数が10kHz超」) と整合する物理的な傾向
(小さく硬い物体は低域の共振が少ない) で、解法とは無関係。

★この分布は **coverage のしきい値を確定させる材料**として planner に渡す
(spec §7「coverage のしきい値は未定、stage0/stage1 の分布を見て §8 で確定」)。
私の解釈: 低域が薄いのは物理的に正常、高域が lobpcg 経路だけ 0 なのは**予算依存の
偏り**なので、`--quality-min-high-coverage` を有効にする場合は cap 超のメッシュ
(= 現状 builtin 3 種) が一律に弾かれる/重み減衰することになる — これが意図どおりか
は判断が要る (下記「不安・質問」)。

## 参照材質・L_ref (spec §4.1)

初期値 (E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7, L_ref=0.3、アルミ相当) は **確定**
(spec §8 に記録済み)。`layout.py` の `REF_*` / `L_REF` 相当の定数がそれ。
stage0 の `stats.json` (`mode_count` / `coverage.mean_ratio`) を見た結果、
初期値のまま据え置いている (round 1: 帯域占有率平均 0.45 / モード数中央値 30.5、
round 3 (`coverage_ratio` 定義に切り替え後) に再生成しても平均 0.456 / モード数
中央値 30.5 とほぼ同じ — 残差フィルタや coverage 指標の追加は分布の代表値を
動かすほどではない)。

## モデル / 学習 / export (M76d)

`model.py` の `ModalUNet` は spec §4.2 / design-draft.md §F の 3D U-Net を
`widths=(stem,w1,w2,w3)` で一般化したもの (op 種は Conv3d/ConvTranspose3d/ReLU/Add
だけ — sub-05 の C++ 推論と 1:1)。基準構成 `(16,32,64,96)` で **paramCount
(BN 畳み込み後) = 1,682,448** (予算 2,000,000 以内)、fixture は `(4,8,8,8)`
(sub-04.md の「幅 4/8/8/8」)。**forward() (学習) も export.py の fp16 再計算も
同じ `ModalUNet.ops` (`OpSpec` のリスト) を辿るだけの薄い実行器
(`model._exec_op` / `model.run_graph`) を通る** — 2 つの実行経路を別々に
手書きすると必ず乖離する、というこのリポジトリの原則をここでも守っている。

### 軸並び規約 (sub-05 が実装時に合わせる契約)

入力テンソルは `(N, C=1, D=z, H=y, W=x)` — x が最終軸 (最内)。これは C++ の
`VoxelIndexOf(x,y,z) = x + n*(y+n*z)` (x が最内) と揃えるための意図的な選択で、
出力テンソル `(N, 192, 16,16,16)` も同じ規約 (`D=cz,H=cy,W=cx`)。
`model.voxel_to_tensor` / `model.output_to_cell_grid` / `model.cell_dense_to_tensor`
の 3 関数だけがこの変換を行う (2 本目を作らない)。

### `.dmnet` のバイト書式 (layout.py が正本、[追加] — sub-05 実装時に要再確認)

spec §4.2 は「256 B ヘッダ」と書いているが、列挙されているフィールドをそのまま
4 B 単位で数えると **224 B にしかならない** (`.mvox` が「64 B」→ 72 B に訂正された
sub-02 round 1 の前例と同種の数え違いの可能性がある)。256 という丸い数自体は
将来のヘッダ拡張の伸び代として意図的だろうと判断し、**`reserved` を単数の 4 B
ではなく 9 x uint32 (36 B) に広げて 256 B ちょうどへ揃えた** (`layout.DMNET_HEADER_FMT`
= `"<4I4i6f6f32fQI9I"`)。sub-05 (C++ ローダ) 実装時にこの解釈で問題ないか
確認すること。

op 表は 1 エントリ 12 x 4B = 48B (`layout.DMNET_OP_FMT`)。**weightOffset/biasOffset
はファイル先頭からの絶対バイトオフセット** (blob 境界を cin/cout/k から逆算させない
設計判断、[追加])。ファイルレイアウトは
`ヘッダ(256B) → op表(opCount×48B) → 重み blob (fp16 連続) → バイアス blob (fp32 連続)`。
ReLU/Add は重みを持たないので両オフセットとも `DMNET_OFFSET_NONE (0xFFFFFFFF)`。

### `train.py`

```
python train.py --overfit 16 --epochs 300 --data data\stage0 data\stage1
python train.py --data data\stage0 --epochs 100          # 一般学習 (Adam)
```

- npz を読み、`layout.cell_order()` で `valid`/`feat` を dense (16,16,16,192) へ
  展開し、amp チャンネルだけをデータセット統計 (mask=1 の実データのみ) の
  min/max で [0,1] へ正規化する (mask チャンネルは 0/1 のまま)。
- 品質フィルタ `--quality-max-residual` / `--quality-min-coverage` /
  `--quality-min-high-coverage` / `--quality-weight` は**既定すべて off** —
  判断軸は **band coverage と residual 品質の 2 つだけ**、`method`/`mode_count`
  では分岐しない (ユーザー指示、npz 側 (sub-03) と同じ原則)。off でも
  `load_dataset()` はこれらの値を読み、学習ログに「除外数 / 残差分布」を出す。
- **`--overfit` は Adam ではなく LBFGS で学習する** (coder 判断、[逸脱] —
  下記「overfit の実測」参照)。`--epochs` は LBFGS の外側ステップ数
  (1 ステップ = 内部で最大 `--lbfgs-max-iter` (既定 20) 回の line-search 付き反復)。
  一般学習 (`--overfit` 無し) は Adam + 20 epoch ごと半減のまま。

### `export.py`

```
python export.py --fixture --out tests\deepmodal
python export.py --random-full --out data\full_random.dmnet
python export.py --checkpoint runs\checkpoint.pt --out assets\deepmodal\deepmodal.dmnet
```

BN を畳み込みへ畳み (`model.ConvBN3d.fold()`)、**重みだけ fp16 へ丸めてから fp32 に
戻す** (バイアスは fp32 のまま、spec §4.2「fp16 重み + fp32 バイアス」)。
`--fixture` は `widths=(4,8,8,8)` の乱数重み (seed 固定、BN の running stats も
乱数で崩してから固定 — 初期値のままだと畳み込みが実質恒等になり検査価値が薄い)。
`fixture_in.mvox` は `Editor.exe --modal-voxelize --list ... builtin://cube` で
作ったものをそのままコピーする (一時ディレクトリで焼き、`tests\deepmodal` 直下には
作業ファイルを残さない)。`fixture_out.bin` は **書き出した `fixture.dmnet` を
読み戻して**推論した値 (メモリ上の net を直接使わない — 書き出しバイト列に
取りこぼしがあった場合の検出力を上げるため)。有効 cell (ボクセル占有だけから
決める — dataset.py の FEM/接触経由の判定とは別物。ランタイムの baking は
ボクセル占有さえ分かれば十分という想定、[追加]) のうち `layout.cell_order()`
順で先頭 64 個を選ぶ。

`ampScale` (spec §4.1 手順 4、「J=1 N·s の中央値ピークが -12dBFS になる値」) は
`ModalSynthRender` (C++、sub-05 未着手) を実際に鳴らして較正する必要があり、
Python 単体では計算できない。fixture/random-full/checkpoint いずれも **1.0 の
プレースホルダ**で書いている ([追加])。耳確認 (spec §2 #12、M76f/g) で確定させる。

## overfit の門 — 実測とハイパーパラメータ探索 (sub-04 round 1、2026-09-16)

round 1 の結果: `python train.py --overfit 16 --epochs 300` は amp MSE を 0.001935
まで下げたが、旧閾値 `<1e-3` にはわずかに届かず FAILED (`mask acc = 100.000%` は
`>99%` を大きく満たす)。round 1 で行った実測 (すべて 16 形状、widths=(16,32,64,96)、
Σ = amp MSE + mask BCE、当時のデータには下記の 2 バグが残っていた点に注意):

| 設定 | 300 step 相当の amp MSE (eval) | mask acc |
|---|---|---|
| Adam lr=1e-3、20 epoch ごと半減 (spec 記載の既定スケジュール) | 0.0088 (300 epoch) / 0.044 (半減あり) | 99%+ |
| Adam lr=1e-3、半減無し | 0.0088 | 99.6% |
| Adam lr=5e-3、半減無し | **0.0056** (Adam中最良) | 99.6% |
| Adam lr=1e-2 (半減無し) | 不安定 (終盤で発散、eval 0.109) | 85.8% |
| Adam、batch_size=1 (勾配ステップ数だけ増やす) | 0.033〜0.049 (BN running stats が batch=1 で劣化、train/eval 乖離大) | 88% |
| Adam、幅 4 倍 (widths=(32,64,128,192)、6.7M params) | 0.0059 (容量を上げても頭打ちはほぼ同じ) | 99.6% |
| Adam、head だけ 4 倍 (head1 出力 64→256) | 0.0052 (head 幅もボトルネックではない) | 99.6% |
| **LBFGS (max_iter=20/outer step)、300 outer step (=6000 内部反復)** | **0.001935 (最良)** | **100.000%** |

planner が round 1 の SELF_EVAL を精査し、**この頭打ちは coder の実装の不備ではなく
データ生成側の欠陥 2 件が原因**と切り分けた (詳細は spec §8 / sub-04.md フィードバック
履歴)。round 2 でその 2 件を修正した。

## round 2: データ生成のバグ 2 件と修正 (2026-09-16)

### バグ 1 (planner 指摘・must): FEM にメッシュ実寸を渡していた

`dataset.py` (旧) は FEM の要素寸法に `grid.voxel_size` (**メッシュ実寸**、AABB
最長辺/28) を渡していた。ボクセル化は AABB の最長辺で正規化する = **入力 (占有
ボクセル列) はスケール不変**なのに、FEM に実寸を渡すと固有周波数が 1/L で動くため、
**同じ入力ボクセルに異なる教師値**が生まれる。加えて、論文 §5.1「学習時は全物体を
同じ材質・同じスケールにする」+ 後処理 σ3 (spec §4.1 手順 6) の設計から、教師値に
実寸が入っていると `BuildModes` が σ3 を**二重適用**することになる (ランタイムの
正しさに直結)。

修正: `layout.py` に `VOXEL_DIVISIONS = 28` / `H_REF = L_REF / VOXEL_DIVISIONS`
を追加し、`dataset.py` の FEM 組み立てを `grid.voxel_size` → `layout.H_REF`
(全メッシュ共通) に変更した。

### バグ 2 (coder が round 2 の検証中に発見): `eigsh` の初期ベクトルが乱数だった

バグ 1 を直した直後に**指摘 3 の重複確認**をしたところ、`cylinder_0/3/5`
(ボクセル列バイト一致、占有 1305) の `feat` がまだ最大 6.9 食い違っていた
(round 1 の「最大 7.0」とほぼ同じ大きさ — **round 1 の診断はバグ 1 だけでは
説明がつかない差分を含んでいた**ことを意味する)。

原因を切り分けた結果: `modal.solve_modes()` (shift-invert `eigsh`) は `v0`
(Krylov 部分空間の初期ベクトル) を指定しないと ARPACK が内部で乱数ベクトルを使う。
同一プロセス内で同じ `K`/`M` に対して `solve_modes()` を 2 回呼ぶだけで、
周波数はほぼ一致 (`freq maxdiff ≈ 5e-5`) するのに固有ベクトルは大きく食い違う
(`vecs maxdiff ≈ 0.48`) ことを実測で確認した。円柱・球・立方体のような対称形状は
固有値が縮退するため (spec §4.1「縮退モードの罠」)、縮退部分空間内の基底が
初期ベクトル依存になり、`contact.py` が読む per-node 振幅 (ひいては `feat`) が
**実行のたびに変わる**。`solve_modes_lobpcg()` は元から `seed` 引数で `v0` を
固定していたため影響を受けない — **`solve_modes()` (exact 経路) だけの欠陥**
だった。

修正: `modal.solve_modes()` に `seed: int = 0` を追加し、
`v0 = np.random.default_rng(seed).standard_normal(ndof)` を `eigsh` に渡すように
した。修正後は同一 `K`/`M` に対して `solve_modes()` を何度呼んでも**ビット一致**
することを確認済み (`freq`/`vecs` とも maxdiff = 0.0)。

### 重複入力群の再確認 (指摘 3)

`data\stage0` を上記 2 バグの修正後に再生成し、`cylinder_0` / `cylinder_3` /
`cylinder_5` (ボクセル列がバイト一致する組) の `feat` を再確認した:

```
cylinder_0 occ=1305 valid_cells=225
cylinder_3 occ=1305 valid_cells=225 vox一致: True  feat max|diff| vs cylinder_0: 0.0
cylinder_5 occ=1305 valid_cells=225 vox一致: True  feat max|diff| vs cylinder_0: 0.0
```

**修正後は完全一致 (bit-identical) することを確認した。** 重複入力は害のない
冗長データになった。

## overfit の門 — round 2 再計測 (spec §5 #9)

旧閾値 `amp MSE < 1e-3` は撤回済み (planner の裁定: 根拠のない値で、目標場の
表現上の下限を下回っていた)。新しい門は **`mask acc > 99%` + `R² = 1 − MSE/Var(target)`**
(`train.py` の `compute_r2`)。閾値の最終確定はユーザー判断待ちなので、ここでは
再計測した値をそのまま報告する (自己判定はしない)。

`python train.py --overfit N --epochs 300 --data data\stage0 data\stage1`
(data\stage1 は無いので data\stage0 のみ使用。N=1 だけ `--epochs 120` — 早期に
収束が頭打ちになったのを確認したうえでの打ち切り、値は epoch 120 時点の eval):

| N | amp MSE (pooled) | Var(target) | R² | mask acc |
|---|---|---|---|---|
| 1 (epochs=120) | 0.000583 | 0.014639 | **0.9601** | 100.000% |
| 4 (epochs=300) | 0.000139 | 0.005082 | **0.9726** | 100.000% |
| 16 (epochs=300) | 0.001661 | 0.021765 | **0.9237** | 100.000% |

**round 1 (バグ修正前のデータ) との比較**: 16 サンプルの amp MSE (サンプル正規化、
run_overfit_lbfgs の学習ログ表示値) は 0.001935 → 0.002113 と近い水準だが、
教師データ自体 (Var(target)) がバグ修正で変わっているため単純比較はできない —
**R² で比較すべき**。round 1 で planner が実測した「R²=0.851 / 到達可能上限
≈0.849」(バグ入りデータ) に対し、round 2 は **R²=0.9237 (N=16)** まで伸びた —
2 つのバグ (サイズ漏れ + eigsh 非決定性) が実際に教師データへ「学習不能な雑音」を
持ち込んでいたことを裏づける。mask acc は round 1 から一貫して 100% に収束して
おり、mask 側は最初から問題が無かった。

N=1→4→16 で R² が 0.96→0.97→0.92 と大きくは単調でない (N=4 が最良) のは、
`select_overfit_subset` が等間隔サンプリングで選ぶ形状の構成 (N=4 は
box_0/hollowbox_1/perfplate_2/sphere、N=16 はより多様な形状を含む) に依存する
ためと考えられる — 形状が増えるほど「複数形状の共通構造の学習」の難度が上がる
ことの表れで、不健全な兆候ではないと考える。閾値の最終確定 (「十分近い」の基準) は
ユーザー判断待ち。

(ログ全文: `tools\deepmodal\runs\overfit1_v2.log` / `overfit4_v2.log` /
`overfit_gate_v2.log` — いずれも gitignore 対象)
