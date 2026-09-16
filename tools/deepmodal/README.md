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
