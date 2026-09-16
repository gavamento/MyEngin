# sub-03 (M76c): Python — FEM / 固有値 / 接触励起 / Mel 圧縮 / データセット

- 依存: sub-02 (`Editor.exe --modal-voxelize` と `.mvox`)
- 状態: 未着手
- 往復: 0

## やること
spec §4.1「経路 (オフライン)」の学習データ側 (ユーザー計画 Phase 3–7 / Checkpoint C–G)。`tools\deepmodal\` を新設。

```
requirements.txt  numpy scipy torch pytest   (torch の cu128 index URL は README に)
README.md         セットアップ / 環境変数 MYE_EDITOR_EXE (既定 bin\x64\Release\Editor.exe) / データ段階の門 (ModelNet の生成コマンドは門を越えるまで「実行禁止」と明記)
layout.py         定数 VOXEL_N=32 / MAP_N=16 / MEL_BANDS=32 / CHANNELS=192 / F_MIN / F_MAX / チャンネル順 / struct 文字列 / 参照材質 / L_REF
meshio.py         .mvox 読み、モデル列挙
voxelize.py       subprocess `cmd /c "<Editor.exe> --modal-voxelize --list F --out DIR"` (バッチ 200 件)
primitives.py     箱 / 板 / 円柱 / 球 / 中空箱 / L 字 / 穴あき板 を寸法乱数で OBJ 生成 (seed 固定)
fem.py            hex8 の Ke (24×24, 2×2×2 Gauss) / Me (集中質量) を単位立方体で 1 回作り h, E, ν, ρ でスケール。占有 voxel の節点を compact 化 → COO → CSR
modal.py          eigsh(K, k≤256, M=M, sigma=0, which='LM') → 剛体 6 モード除去 → ω=√λ/2π → 100–10000 Hz。timeout 300 s、占有 > 20k voxel は skip
contact.py        16³ cell ごとの接触節点 (cell 内占有 voxel の節点で中心に最寄り、同値は最小 index)、a_ij = |U[dof_j(node), i]| / ω_i
compact.py        Mel 区切り / 帯域割当 / Σ|a| / mask / ln / 空帯域は最寄り非空の値 / 未励起 (|a| < 1e-3·max) は無し
dataset.py        1 メッシュ → data\<stage>\<name>.npz {vox u8[32³], valid u8[16³], feat f16[valid,192]}。multiprocessing (OMP_NUM_THREADS=1)、再開可、stats.json
tests\            test_fem / test_modal / test_compact / test_layout / test_contact
```
- `layout.py` の定数は `VOXEL_N = 32` の形 (1 行 1 整数)。`tools\check_rules.ps1` の `$constGroups` に C++ ⇄ Python の 4 組を追加 (`src\Engine\Engine\Modal\ModalTypes.h` ⇄ `tools\deepmodal\layout.py`)。
- `.gitignore` に `tools/deepmodal/data/`、`tools/deepmodal/**/__pycache__/`、`tools/deepmodal/.pytest_cache/`、`tools/deepmodal/runs/`。
- 参照材質と L_ref: 初期値 (E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7, L_ref=0.3) で stage0 を回し、stats.json の帯域占有率 / モード数分布を見て確定。確定値を `layout.py` に固定し、SELF_EVAL の申し送りに書く (planner が spec §8 に積む)。
- **リスク潰し**: 満杯 30³ 立方体 (最悪ケース ≈ 90k DOF) の eigsh 時間を測って stats に残す。600 s を超えるなら「不安・質問」で代替 (LOBPCG / k 削減 / 占有上限) を上げる。

## やらないこと (このサブでは)
ネット / 学習 / export (sub-04)。Python 側のボクセライザ (禁止、spec §2 #8)。ModelNet の生成。

## 触る場所 (planner の見立て)
- 新規 `tools\deepmodal\*` (上表)、`tests` は `tools\deepmodal\tests\`
- `tools\check_rules.ps1:76` `$constGroups` に 4 エントリ
- `.gitignore`
- 前提: `pip install -r tools\deepmodal\requirements.txt` (scipy / pytest は未導入 — 確認済み)。Release の Editor.exe をビルドしておく

## 受け入れ条件 (このサブ)
spec §5 の 6, 7, 8, **21**。
1. `pytest` 全緑: test_fem (Ke 対称・半正定、剛体 6 モードで `K·r ≈ 0`、集中質量総和 = ρh³) / test_modal (2×2×2 で E×4 → ω×2、ρ×4 → ω/2、h×2 → ω/2、1e-6。先頭 6 固有値 ≈ 0) / test_compact (単調・端点・Σ|a|・空帯域補間・mask) / test_layout (.mvox ヘッダ 72 B = 全体 32840 B、C++ cube で 24389 = 29³。`@pytest.mark.editor`、Editor.exe 無ければ skip) / test_contact (cell を変えると励起ベクトルが変わる)
2. `python dataset.py --stage primitives --out data\stage0` → npz ≥ 20 本 + builtin 6 本 + stats.json (メッシュごとの eigsh 秒 / 帯域占有率 / モード数 / 解法名)。満杯立方体 (builtin cube) < 600 s、exact 経路の中央値 < 60 s。
   ★**再開 (resume) 実行の後も stats.json に統計が残ること** (既存 stats と併合する)。M76h の ModelNet10 は再開前提なので、キャッシュ済みを `status: cached` だけで上書きすると分布が永久に取れない (round 1 で踏んだ)。
   ★**npz と stats.json の両方に solver metadata と convergence quality を記録すること** (spec §4.1「固有値解法の 2 経路と品質指標」、ユーザー追加指示)。metadata = `method` / パラメータ (m, maxiter, drop_tol, fill_factor, k, shift) / 反復回数 / 収束フラグ。quality = **モードごとの相対残差** `‖K x̂ − λ M x̂‖ / ‖λ M x̂‖` (x̂ は M-正規化、剛体除去後の帯域内モードのみ) + 要約 (`residual_max` / `residual_median` / 除外モード数)。
   ★**採否は `method` ではなく残差で決める**: `r > RESIDUAL_DROP (1e-3)` のモードは Σ\|a\| から除外、`1e-5 < r ≤ 1e-3` は採用してフラグを立てる。`method` を分岐条件に使わないこと (メタデータに降格)
3. **Mel-band coverage** (ユーザー指示、spec §4.1 の 3 番目の指標。round 2 の「スペクトルの完全性」から**定義が差し替わった**):
   - 帯域の被覆は **3 力軸の OR** — `cell_band_mask[cell][i] = OR_j mask[j][i]`。ランタイムの `BuildModes` が軸を足す (`Σ_j |k_j|·H(mask)·amp`) ので、1 軸でも立てばその帯域は鳴る = OR だけがランタイムと整合する
   - npz に **メッシュ単位** `band_coverage[32]` (帯域ごとの占有率) / `coverage_ratio` (32 帯域平均) / `coverage_high` (上位 8 帯域 = index 24..31 の平均) と、**cell 単位** `cell_coverage[valid_cells]` (float32、`feat` の行と同順)
   - stats.json に**帯域ごとの占有率の分布**を出す (高域が系統的に空でないかが見える形)
   - ★`band_occupancy_ratio` は `coverage_ratio` と同義なので**どちらか 1 つに寄せる** (似た名前で別定義が 2 つあると後で静かに食い違う)
   - ★`modes_requested` / `f_top` / `spectrum_complete` / `mode_count` は**診断値として記録してよいが、採否・重み付けの判断に使わない** (`method` と同じ扱い)。判断軸は **coverage と residual の 2 つだけ**
   - ★両経路で同じ指標を取ること — 予算で切れるのは LOBPCG だけではない (round 2 実測: 直接法も `k=150` で頭打ちだった)
4. **基準形状の校正** (spec §5 #21、新設): 直接法が回せる基準形状で `eigsh` と LOBPCG を両方走らせ `calibration.json` に (a) 残差分布 (b) 周波数の相対誤差 (昇順対応) (c) **取りこぼしモード数** (d) 帯域集計 Σ\|a\| の相対差 を書く。**生の固有ベクトルは比較しない** — 対称形状は固有値が縮退し (実測: 立方体で 10964 Hz 2 重 / 14726 Hz 3 重)、縮退空間内の基底は任意なので両方正しくても per-mode の値は食い違う。加えて小メッシュ (数百 DOF) の pytest 1 本で両解法の一致を回帰的に固定する。暫定しきい値 (1e-5 / 1e-3) が実測分布と合わない場合は SELF_EVAL で報告すること (planner が §8 で改訂する)
5. `check_rules.ps1` 緑。`layout.py` の `VOXEL_N` を一時的に 31 にすると規則 9 が赤くなる (戻す)

## 検証コマンド
- `cd tools\deepmodal && pytest`
- `python tools\deepmodal\dataset.py --stage primitives --out tools\deepmodal\data\stage0`
- `pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
実装:
  - `tools/deepmodal/layout.py` — 共有定数 (VOXEL_N/MAP_N/MEL_BANDS/CHANNELS/L_REF/参照材質)、
    `.mvox` ヘッダの struct フォーマット、`mel_band_centers()` (C++ MelBandCenters の二重実装)
  - `tools/deepmodal/meshio.py:read_mvox` — `.mvox` をフィールド単位で読む (C++ の
    VoxelIndexOf と numpy 配列の軸順を突き合わせて transpose、実データで検算済み)
  - `tools/deepmodal/voxelize.py` — `Editor.exe --modal-voxelize` の subprocess ラッパ
    (`cmd /c`、200 件バッチ)。実行時に **Release Editor.exe が M76b 以前のビルドのまま
    (stale) で `--modal-voxelize` を認識せず無言で GUI 起動する事故**を検出し、
    Release を再ビルドして解消 (バイナリは bin/ = gitignore、コミット対象外)
  - `tools/deepmodal/primitives.py` — 箱/板/円柱/球/中空箱 (トレイ状、開口あり)/L字/
    穴あき板 (4 本バーの枠) を寸法乱数 (seed 固定) で生成
  - `tools/deepmodal/fem.py` — hex8 の `hex8_ke_unit(nu)`/`hex8_ke`/`hex8_me_diag` と
    `assemble_from_occupancy` (np.unique によるベクトル化 compact 節点採番、explicit sort
    で決定論)
  - `tools/deepmodal/modal.py:solve_modes` — shift-invert eigsh (`pick_shift` で剛体
    特異点を避ける微小シフト)。`solve_modes_lobpcg` — 不完全 LU 前処理の LOBPCG (大きい
    メッシュ用の代替、下記「不安・質問」参照)
  - `tools/deepmodal/contact.py` — 16^3 cell → 占有 voxel の節点で中心最寄り (同値は
    compact index 最小) → `a_ij = |U[dof_j,i]|/omega_i`
  - `tools/deepmodal/compact.py` — Mel 32 帯域 x 3 軸への圧縮、閾値 (cell 内 max 相対)、
    空帯域の最寄り値埋め
  - `tools/deepmodal/dataset.py` — primitives 生成 → voxelize → FEM → modal → contact →
    compact → npz (multiprocessing、OMP_NUM_THREADS=1、再開可、`MAX_OCCUPIED_EXACT` で
    exact/lobpcg を振り分け、`stats.json` 出力)
  - `tools/deepmodal/tests/test_{fem,modal,compact,contact,layout}.py` (23 ケース)、
    `conftest.py` (pytest の import path 解決用の空フック)、`pytest.ini` (`editor` marker 登録)
  - `tools/check_rules.ps1:$constGroups` — `kModalVoxelN⇄VOXEL_N` 等 4 組を追加、
    VOXEL_N を一時的に 31 にして赤くなることを実地確認済み (元に戻し済み)
  - `.gitignore` — `tools/deepmodal/data|**/__pycache__|.pytest_cache|runs/`
  - `tools/deepmodal/requirements.txt` / `README.md`
仕様との差分:
  - [追加] `MAX_OCCUPIED_EXACT=9000` (占有 voxel 数) を超えるメッシュは
    `modal.solve_modes_lobpcg` (不完全 LU 前処理 LOBPCG) へ回す。理由: 実測で
    shift-invert (完全 LU) の埋め込みが要素数の 2 乗超で増える (54000 DOF で
    nnz(K)=4.0M→nnz(L+U)=230M=57倍)。満杯 29^3 立方体 (81000 DOF、このボクセル
    正規化で到達できる最大占有) では実行環境の空き RAM (~8GB) を超えるおそれがあり
    危険と判断し実行しなかった。spec/sub-03 が明示的に許容している代替 3 案
    (LOBPCG/k削減/占有上限) のうち LOBPCG を採用し、builtin 6 種 (cap 超えでも
    npz を作る) の cube で実測 360.8 s (<600 s)、cylinder 422.7 s、sphere 229.5 s
    (全て並列実行下、CPU 競合込みの数字)。primitives.py 側で cap を超えたものは
    npz を書かずスキップ (stats の `count_skipped_cap`、中央値の算出対象外)
  - [追加] `compact.compact_cell` の未励起閾値 `|a| < 1e-3・max` の `max` は
    「その cell 内の全モード・全軸の最大振幅」と解釈 (spec は per-mesh か per-cell か
    明記していない)。cell 単位で自己完結した関数にするための判断
  - [追加] npz の `feat` チャンネル順序・cell 走査順序 (C++ の CellIndexOf と同じ
    cx 最内) は spec が明記していないので独自に固定 (README に明記)。sub-04 が
    同じ規約で読む前提
  - [追加] `pytest.ini` の `editor` marker 登録、`conftest.py` (空、pytest の
    import path 解決用) — sub-03.md の想定ファイル一覧に無いが pytest を動かすのに必要
  - [解釈] `hex8_me_diag` の「集中質量総和 = ρh^3」(spec/sub-03 受け入れ条件 1) は
    24 自由度の対角和ではなく「1 節点あたりの物理質量の総和」(対角和/3) と解釈して
    テストした (対角は 1 節点 3 自由度が同じ値を持つため、素の対角和は ρh^3 の 3 倍になる)
  - [逸脱] 検証コマンドは `pytest` ではなく `python -m pytest` を使用 (pip install 直後は
    `pytest.exe` が PATH に無い。pip 自身の警告どおり。README に注記済み)
検証:
  - `cd tools\deepmodal && python -m pytest` → 23 passed (fem/modal/compact/contact/layout。
    layout の `@pytest.mark.editor` テストは実際に Editor.exe を起動して builtin cube を
    焼き、surface+interior=24389 を確認)
  - `python tools\deepmodal\dataset.py --stage primitives --out tools\deepmodal\data\stage0`
    → 48 メッシュ処理、38 npz (builtin 6 種全て含む。primitives は 42 生成中 32 npz +
    10 skipped_occupancy_cap)、`stats.json`: eigsh_seconds (exact 限定) median=22.8s
    max=245.7s min=3.9s (中央値 <60s ✓)、mode_count median=30.5 max=150、
    band_occupancy_ratio mean=0.45、full_occupancy_probe (builtin 3 種の lobpcg 実測:
    cube 360.8s / cylinder 422.7s / sphere 229.5s、全て <600s ✓)
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning。VOXEL_N を 31 に変えると
    `ERROR [rule 9] kModalVoxelN / VOXEL_N must match` で赤くなることを確認して復元
  - Editor.exe Release を再ビルド (`MSBuild ... /p:Configuration=Release`) — sub-02 の
    セッションで Debug だけ焼き直されていたため stale だった
自己採点 (1-5):
  仕様適合: 4 — task 表のファイル一覧・受け入れ条件 6/7/8 は全部満たしたが、
    eigsh の時間リスクへの対応 (LOBPCG 追加) は spec が「代替を上げる」と言うだけで
    実装までは指定していない箇所を自分で実装・実測まで進めた (差分に明記)
  正しさ: 4 — FEM のスケール則 (E/ρ/h) は 1e-6 相対で解析的に検算、剛体 6 モード・
    Ke の半正定値性・lumped mass 総和も直接検算。LOBPCG は低次モードを shift-invert と
    1e-8 相対で照合済みだが、高次モード (帯域上限付近) の精度は未検証のまま npz に
    含めている (README/docstring に明記)
  コード品質: 4 — 日本語コメントで「なぜ」を記録、numpy ベクトル化、決定論
    (unordered 不使用、np.unique の安定ソート)。dataset.py はやや長く責務が詰まっている
  テスト: 4 — pytest 23 ケース全緑、dataset.py 自体の単体テストは無いが実データでの
    end-to-end 実行 (48 メッシュ) で検証済み
不安・質問:
  1. [ユーザーに聞ける] MAX_OCCUPIED_EXACT=9000 としきい値超過時の LOBPCG フォールバックは
     私の判断で実装した (spec §7 が「LOBPCG/k削減/占有上限」のいずれかを認めている)。
     この設計 (しきい値の値、LOBPCG のパラメータ m=40/maxiter=60/spilu fill_factor=8) を
     このまま採用してよいか、それとも ModelNet 段階 (M76h) までに再検討が要るか判断を
     お願いしたい。特に LOBPCG の高次モード精度は完全には検証していない
  2. L_ref / 参照材質は stage0 の統計 (帯域占有率平均0.45、モード数中央値30.5) を見て
     初期値のまま据え置いた (極端な偏りなし)。spec §8 への変更履歴追記は不要と判断したが、
     「確定した」という事実は記録した方がよいか planner の判断を仰ぎたい
  3. primitives.py の形状生成 (7 種 x 6 variants) は正しさの単体テスト対象外
     (spec の pytest 一覧にも入っていない) — 意図どおりか確認したい
触ったファイル:
  - tools/deepmodal/layout.py
  - tools/deepmodal/meshio.py
  - tools/deepmodal/voxelize.py
  - tools/deepmodal/primitives.py
  - tools/deepmodal/fem.py
  - tools/deepmodal/modal.py
  - tools/deepmodal/contact.py
  - tools/deepmodal/compact.py
  - tools/deepmodal/dataset.py
  - tools/deepmodal/conftest.py
  - tools/deepmodal/pytest.ini
  - tools/deepmodal/requirements.txt
  - tools/deepmodal/README.md
  - tools/deepmodal/tests/test_fem.py
  - tools/deepmodal/tests/test_modal.py
  - tools/deepmodal/tests/test_compact.py
  - tools/deepmodal/tests/test_contact.py
  - tools/deepmodal/tests/test_layout.py
  - tools/check_rules.ps1 (constGroups 4 組追加)
  - .gitignore (tools/deepmodal の生成物除外)
申し送り:
  - sub-04 (M76d): npz の `feat` チャンネル順序・cell 走査順序は README に明記した
    独自規約 (C++ 側との contract は無い、Python 内で完結)。LOBPCG 経由の npz
    (builtin 6 種のうち cube/cylinder/sphere) は高次モード精度が未検証 — 学習時に
    重み付けを下げる/除外するかの判断が要るかもしれない
  - sub-08 (M76h、ModelNet10): 満杯に近い立方体・球状メッシュは同じ occupancy cap に
    掛かって LOBPCG 経路になる。ModelNet10 (4899 体) 規模だと LOBPCG 経路の割合と
    総時間が無視できない可能性がある (現状 12 並列で 1 体あたり最大 245s(exact)/
    422s(lobpcg) 観測) — 本格実行前に stats.json で分布を見て `MAX_OCCUPIED_EXACT`
    や LOBPCG パラメータを再チューニングすることを推奨
  - 環境: Release Editor.exe を再ビルドした (元は M76b 以前の stale バイナリで
    `--modal-voxelize` が無反応だった)。以後のサブも Release を使う手順があれば
    最新化されているか確認すること

---

SELF_EVAL: sub-03 (round 2)
実装 (指摘番号ごと):
  1. [must、stats.json が再開実行で空になる] `dataset.py` の再開経路を作り直した。
     `process_mesh` が npz に **solver metadata + convergence quality の全スカラー**
     (`method` / `solver_params` / `iterations` / `converged` / `occupied_voxels` /
     `ndof` / `assemble_seconds` / `eigsh_seconds` / `mode_count` / `valid_cells` /
     `band_occupancy_ratio` / `residuals` / `residual_max` / `residual_median` /
     `dropped_mode_count` / `marginal_mode_count`) を埋め込むようにし (`np.savez_compressed`
     に追加)、新設の `dataset._npz_stats_entry(name, npz_path)` が既存 npz からこれを
     読み戻して完全な stats エントリを再構成する。`run_primitives_stage` は
     「npz が既にある」メッシュを `{"status":"cached"}` で積む代わりにこの関数を呼ぶ。
     **npz 自身が唯一の永続先**になったので、stats.json を消しても (M76h が複数セッション
     をまたいでも) 分布は失われない。読めない/旧形式の npz は `cached_unreadable` に
     倒して処理は止めない
  2. [must、docstring と実装の矛盾・box_1 と cube の逆転] `modal.py` を全面改稿し、
     `SolveResult` (freq/vecs/raw/**residuals**/method/params/iterations/converged) を
     返す形にした。`modal.compute_residuals(K,M,vals,vecs)` が M-正規化 (`x̂ᵀMx̂=1`) 済み
     固有ベクトルで相対残差 `r_i=‖Kx̂-λMx̂‖/‖λMx̂‖` を計算する。`dataset.py` は
     **占有数で exact/lobpcg のどちらを呼ぶかだけ**決め、**採否 (Σ|a| に入れるか) は
     残差だけで決める** (`r > RESIDUAL_DROP` のモードを `freq`/`modes` から除外してから
     `contact.excitation_for_cell` へ渡す)。npz に `method` を明示的に保存したので
     sub-04 は method で見分けられる (可視化)。`solve_modes_lobpcg` の docstring も
     「npz には exact と同じ扱いで書く。採否は残差で決まる」に書き直した
  3. [should、LOBPCG精度がコメントのみ] `tests/test_modal.py` に
     `test_lobpcg_matches_exact_on_small_mesh` を追加 (4x4x4=375DOF、1 秒未満)。
     `solve_modes` と `solve_modes_lobpcg` の低次 6 モードを周波数 (昇順対応) で比較し
     相対誤差 <1e-3、両方の残差が閾値未満であることを固定した。生の固有ベクトルは
     比較しない (縮退モードの罠、下記)
  4. [should、primitives.py が単体テスト対象外] `tests/test_primitives.py` を新設。
     (a) 中空箱 (開口あり) の interior が同寸法の中実箱よりはっきり少ないこと
     (b) 穴あき板の中心 (穴) が非占有・バー本体が占有であること、を実際の
     Editor.exe ボクセライザで検証 (`@pytest.mark.editor`)。**実測で判明**: 壁厚が
     voxel_size (h≈0.011) より厚い (0.05) と壁自体のバルクが正しく interior 判定され
     「中空箱なのに interior があまり減らない」(19683→11380、1/4 未満に届かない) —
     これは実装のバグではなく物理的に正しい (厚い壁は本当に内部まで材質)。
     テストは壁厚を h より薄い 0.01 に変えて「本当に薄い殻」の場合で検算するよう修正した
仕様変更への対応:
  - spec §4.1「固有値解法の 2 経路と品質指標」(ユーザー追加指示) を実装: `RESIDUAL_ACCEPT
    =1e-5` / `RESIDUAL_DROP=1e-3` を `modal.py` に定数として置き、`dataset.py` が
    それに基づいてモード単位で除外・フラグ付けする。`method` は分岐に使っていない
    (grep で確認: `if method ==` 系の分岐は完全に削除し、`use_lobpcg` は「どちらの
    solver を呼ぶか」だけに使う変数として分離した)
  - **spec §5 #21 (基準形状の校正)**: `tools\deepmodal\calibrate.py` を新設。builtin
    capsule (cap 未満) と sphere (cap 超、`--full`) で `solve_modes`/`solve_modes_lobpcg`
    を両方走らせ、(a) 残差分布 (b) 周波数の貪欲対応付け + 相対誤差 (c) 取りこぼし数
    (d) 代表 cell 1 点での帯域集計 Σ|a| の相対差、を `data\calibration.json` に書く
    (実行例は下記検証、実測結果は 不安・質問 #1 と README に転記)
検証:
  - `python -m pytest` → **27 passed** (round 1 の 23 + `test_exact_residuals_are_tiny` /
    `test_lobpcg_matches_exact_on_small_mesh` / `test_primitives.py` 2 本)
  - `data\stage0` を**完全再生成** (旧 npz は round 1 形式でメタデータが無いため削除して
    作り直し) → `python dataset.py --stage primitives --out data\stage0`:
    48 メッシュ処理、38 npz、`stats.json`: eigsh_seconds(exact) median=22.9s max=227.7s、
    mode_count median=30.5、band_occupancy_ratio mean=0.448 (round 1 の 0.45 とほぼ一致)、
    residual: dropped_mode_count_total=13 / marginal_mode_count_total=9 (残差フィルタが
    実際に効いている)、full_occupancy_probe: cube 279.3s / cylinder 374.3s / sphere 197.4s
    (全て builtin、lobpcg 経由、全て <600s)
  - **再開実行の検証 (指摘1 の直接確認)**: 同じコマンドをもう一度実行 →
    `[dataset] processed 48 mesh(es), 38 npz ready (38 restored from cache)`
    (2.2 秒で完了)。再生成後の `stats.json` を確認 →
    `count_ok=38` / `eigsh_seconds.median=22.9s` (regenerated 時と一致) /
    `full_occupancy_probe` 3 件とも数値が残っている (**round 1 で null だった箇所が
    全部埋まっている**ことを直接確認)
  - `python calibrate.py --out data\calibration.json --full` → 実測 (下記「不安・質問」
    に詳細)。capsule: 直接法 150 モード (残差 max 9.4e-9) / lobpcg 34 モード
    (残差 max 4.5e-4)。sphere: 直接法 150 モード (残差 max 1.8e-8) / lobpcg 34 モード
    (残差 max 5.2e-2、RESIDUAL_DROP 超で実際に除外される個体を含む)。両形状とも
    周波数が一致したモード同士の相対誤差は最大 4.4e-4 で小さいが、**取りこぼしが
    116/150 (77%)** — 詳細は不安・質問へ
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
自己採点 (1-5):
  仕様適合: 5 — 指摘 1-4 と仕様変更 (§4.1 の 2 経路、§5 #21) を全部実装し、
    実測で動作を確認した (残差フィルタが実際に効く/再開でstats が残る/校正が回る)
  正しさ: 4 — 残差計算・M-正規化・LOBPCG 除外は実データで検算した。ただし
    calibrate.py が明らかにした「LOBPCG の取りこぼしが 77%」は現状のパラメータ
    (m=40) では未解決の実質的な問題 (残差しきい値だけでは検出も解決もできない、
    spec 自身が警告していた種類の欠陥) — コードは仕様どおりに実装できているが、
    LOBPCG 経路の**データとしての完全性**は低いままなので 5 にはしていない
  コード品質: 4 — `SolveResult` dataclass で戻り値の意味を明確化、npz を唯一の
    永続先にする設計で再開バグを構造的に潰した。calibrate.py の `match_frequencies`
    は Hungarian 法ではない貪欲法 (モード数が数十~百程度なら実用上十分だが
    最適ではない)
  テスト: 5 — pytest 27 緑、resume の実地検証、calibrate.py の実行結果まで確認した
    (「検証していないものは 1」を避け、全項目を実行して結果を貼った)
不安・質問:
  1. [ユーザーに聞ける] **calibrate.py の実測で、残差しきい値とは別次元の問題が
     見つかった**: LOBPCG (既定 m=40) は capsule/sphere どちらも直接法が見つけた
     150 モードのうち 34 本しか見つけられず、**116 本 (77%) が取りこぼし**。
     見つけた 34 本は周波数・残差ともに概ね良好 (相対誤差最大 4.4e-4) だが、
     残差フィルタは「見つけたモードの精度」しか判定できず「そもそも見つけていない
     モード」は検出できない (spec §4.1 が警告していたとおり)。代表 cell での
     帯域集計 Σ|a| の相対差も平均 49-61% と大きい。builtin cube/cylinder/sphere の
     3 本の npz はこの意味で**密度が薄い**教師データになっている。
     `m` を上げれば取りこぼしは減るはずだが、LOBPCG のコストは m にほぼ 2 乗で
     増える (実測: box_1 相当 54000DOF で m=20→32s、m=40→65s) ため、m=150 まで
     上げると満杯立方体が 600s を大きく超えるリスクが高く、このラウンドでは
     変更していない。**この trade-off (m を上げて質を取るか、現状の m=40 のまま
     「LOBPCG 経路は疎いデータ」と割り切るか) は planner/ユーザー判断が要ると思う**。
     M76h (ModelNet10) では satisfyingly-solid でない大きい形状が相当数出る見込みで、
     この問題の影響が広がる可能性がある
  2. RESIDUAL_ACCEPT/RESIDUAL_DROP のしきい値自体 (1e-5/1e-3) は calibrate.py の実測と
     整合していた (直接法 max 1.8e-8 ≪ 1e-5、LOBPCG の悪いモードは 5.2e-2 ≫ 1e-3 で
     きちんと弾かれる) ので、**このラウンドでは変更していない**。#1 の取りこぼし問題は
     しきい値の問題ではなく LOBPCG の m (ブロック幅) の問題だと理解している —
     この切り分けで合っているか確認したい
触ったファイル (round 2 差分。round 1 分はそのまま):
  - tools/deepmodal/modal.py (全面改稿: SolveResult / compute_residuals / RESIDUAL_ACCEPT・DROP)
  - tools/deepmodal/dataset.py (残差フィルタ、npz への metadata/quality 保存、
    `_npz_stats_entry` による再開時の統計復元)
  - tools/deepmodal/calibrate.py (新規)
  - tools/deepmodal/tests/test_modal.py (SolveResult 対応 + 2 テスト追加)
  - tools/deepmodal/tests/test_contact.py (SolveResult 対応)
  - tools/deepmodal/tests/test_primitives.py (新規)
  - tools/deepmodal/README.md (固有値解法の 2 経路の節を全面改稿、calibrate.py 実測結果、npz フォーマット更新)
  - (生成物、コミット対象外) tools/deepmodal/data/stage0/ (完全再生成、38 npz + stats.json)、
    tools/deepmodal/data/calibration.json
申し送り:
  - sub-04: npz に `residual_max` 等が入っているので `--quality-max-residual` /
    `--quality-weight` はそのまま読める設計になっている (README に記載)
  - sub-08 (M76h): 不安・質問 #1 の LOBPCG 取りこぼし問題を、ModelNet10 の本実行前に
    再検討すること (`MAX_OCCUPIED_EXACT` を下げて LOBPCG に回る母数を減らす/m を
    上げて時間予算を取り直す、のどちらか)
  - `plans/m76-deepmodal/harness.md` には触っていない (司会のもの)

---

SELF_EVAL: sub-03 (round 3)
実装 (指摘番号ごと):
  1. [must、校正の診断が誤っている] `calibrate.py` を修正。`calibrate_shape()` に
     **`budget_matched`** (両解法に同じ非剛体モード数 `budget`、既定 34 = dataset.py の
     LOBPCG 既定 m=40-6 に合わせた値、を要求する比較) と **`direct_reference_large_k`**
     (直接法だけ大きい k=150 で回す診断専用の参考値) を分離した。`modes_requested` /
     `f_top` / `spectrum_complete` を両方に必ず併記する (`modal.diagnostics()` を新設し
     dataset.py と共有、定義を 2 か所に持たない)。**実測で再確認**: 予算を揃えると
     capsule/sphere どちらも `matched_count=34, missed_count=0` — round 2 の
     「77% 取りこぼし」は完全に解消 (予算差が原因だったという planner の診断が
     正しかったことを実地で確認)。README と `calibration.json` を訂正した
  2. [must、指標を「Mel-band coverage」に差し替え] `compact.py` に
     `cell_band_coverage(mask)` (軸を **OR** で畳む、`np.any(mask, axis=0)`) を追加し
     単体テストを 1 本足した。`dataset.py` の cell ループで `band_coverage[32]`
     (帯域ごとに何割の cell で鳴ったか)・`coverage_ratio` (32 帯域平均、旧
     `band_occupancy_ratio` を置き換える唯一の名前)・`coverage_high` (index24-31 の
     平均)・`cell_coverage[valid_cells]` を計算し、npz と stats.json (per_mesh + 全体の
     `coverage.band_distribution` / `band_distribution_by_method`) の両方に保存した。
     `modes_requested`/`f_top`/`spectrum_complete` は記録するが `if spectrum_complete`
     のような分岐は一切書いていない (grep で確認)。`mode_count` も同様に採否には
     使っていない
  3. [should、系統的な帯域偏りの確認] 下記「検証」参照。実際に**予算依存の系統的偏り**
     (高域が lobpcg 経由の 3 メッシュだけ 0) を発見した
  4. [nit、パス表記のずれ] 確認: 実配置は `data\stage0\<stem>.npz` (README の記載どおり、
     `npz\` サブディレクトリは存在しない)。round 2 SELF_EVAL の「data/stage0/」表記は
     正しく、`_smoketest2` 等の一時テストで作った別ディレクトリの誤読と思われる。
     コードに問題は無かった
検証:
  - `python -m pytest` → **28 passed** (round2 の 27 + `test_cell_band_coverage_is_or_over_axes`)
  - `python calibrate.py --out data\calibration.json --full` (`budget=34` 既定) →
    capsule: `budget_matched` で direct/lobpcg とも 34 モード、`missed_count=0`、
    `rel_err_max=2.1e-7`。sphere: 同じく `missed_count=0`、`rel_err_max=4.4e-4`、
    lobpcg 側に残差 5.2e-2 の 1 本 (RESIDUAL_DROP 超で正しく除外対象になる) を確認。
    `band_aggregate_diff` は capsule 平均6.2%/最大16.9%、sphere 平均7.7%/最大22.4%
    (round2の「平均49-61%」から劇的に改善 = 予算差が主因だった直接証拠)。
    `direct_reference_large_k` は capsule/sphere とも `mode_count=150=modes_requested`、
    `spectrum_complete=false` — **直接法も同じ予算問題を抱えている**ことを確認
  - `data\stage0` を完全再生成 (旧npzに新フィールドが無いため削除して作り直し) →
    48メッシュ処理、38npz、`stats.json`: eigsh_seconds(exact) median=23.9s
    max=234.8s、mode_count median=30.5、`spectrum.incomplete_count=6/38`、
    `coverage.mean_ratio=0.456`、`coverage.mean_high=0.727`。full_occupancy_probe:
    cube 279.3s/cylinder 378.0s/sphere 204.5s (全て<600s)
  - 再開確認 (round2の修正が壊れていないか): 同コマンド再実行 →
    `38 npz ready (38 restored from cache)`、2.2秒。coverage.mean_ratio/
    band_distribution_by_method.lobpcg が完全一致することを確認
  - **指摘3 (帯域占有の系統的偏り)**: method 別に集計すると明確な偏りがあった —
    exact (n=35): coverage_ratio平均0.478、coverage_high平均**0.789**、
    spectrum_complete=True (帯域内を拾い切った) が32/35。
    lobpcg (n=3、builtin cube/cylinder/sphere): coverage_ratio平均0.198、
    coverage_high平均**0.000 (3本とも完全にゼロ)**、spectrum_complete=False
    (3本とも予算で頭打ち)、f_top=3463-4740Hz (10000Hzに遠く届かない)。
    低域バンド(0-10)は全体的に薄い(0.03-0.29)が、これはspec§7の実測メモ
    (「小さく硬いアルミ塊は基本周波数が10kHz超」)と整合する物理的傾向で method とは無関係。
    **高域(24-31)がlobpcg経由の3本だけ完全に0なのは、この3本が最大級の自由度
    (45864-81000) = モード密度が非常に高く、固定本数の予算(m=40)では
    3500-4700Hz程度までしか届かないため** (calibrate.pyのdirect_reference_large_kで
    同形状の直接法k=150も同様に頭打ちになることを確認済み — LOBPCG固有の問題ではない)
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
自己採点 (1-5):
  仕様適合: 5 — 指摘1-4と仕様変更 (Mel-band coverage) を全部実装し、実測で検証した
  正しさ: 5 — round2の誤診断を実地データで訂正し、新しいcoverage指標が実際に
    意味のある系統的偏りを検出できることまで確認した (診断ツールとして機能している)
  コード品質: 4 — `modal.diagnostics()`をdataset.py/calibrate.pyで共有し定義の
    重複を避けた。`compact.cell_band_coverage()`もテスト付きで独立関数化した
  テスト: 5 — pytest28緑、resumeの再検証、calibrate.pyの実測、stage0の帯域分布集計
    まで全て実行して結果を貼った
不安・質問:
  1. [ユーザーに聞ける] coverage の系統的偏り (高域がlobpcg経由の3メッシュだけ0) は
     「予算依存」であって「LOBPCGが悪い」わけではないが、現状の設計 (占有数→solver選択)
     では**両者が完全に相関してしまう** (cap超のメッシュ=常にlobpcg=常に高域0)。
     `--quality-min-high-coverage` を有効にする運用を将来する場合、この3メッシュ
     (=大きい/硬い物体の代表) が一律に低品質判定されることになる。これは
     「大きい物体の教師データを諦める」のと事実上同じ効果になりうるが、意図どおりか
     確認したい。M76h (ModelNet10) ではサイズの大きいメッシュがもっと増える見込みなので、
     coverageしきい値を決める前にこのtrade-offを認識しておいた方がよいと思う
  2. coverageしきい値自体の具体的な数値はplannerが§8で確定する担当と理解しているので、
     このラウンドでは`layout.py`等に何もハードコードしていない (dataset.py/train.pyは
     診断値を記録・受け渡すだけ)。この理解で合っているか確認したい
触ったファイル (round3差分。round1/2分はそのまま):
  - tools/deepmodal/modal.py (`diagnostics()`追加)
  - tools/deepmodal/compact.py (`cell_band_coverage()`追加)
  - tools/deepmodal/dataset.py (coverage計算・保存、`_npz_stats_entry`/`write_stats`更新、
    `band_occupancy_ratio`を`coverage_ratio`等に置換)
  - tools/deepmodal/calibrate.py (budget_matched/direct_reference_large_kに全面改稿)
  - tools/deepmodal/tests/test_compact.py (`cell_band_coverage`のOR意味論テスト追加)
  - tools/deepmodal/README.md (round2の誤診断を訂正、coverage節を新設、指摘3の集計を追記)
  - (生成物、コミット対象外) tools/deepmodal/data/stage0/ (完全再生成、38npz+stats.json)、
    tools/deepmodal/data/calibration.json (訂正版)
申し送り:
  - sub-04: npzの`coverage_ratio`/`coverage_high`/`cell_coverage`がそのまま
    `--quality-min-coverage`/`--quality-min-high-coverage`で読める設計になっている
  - sub-08 (M76h): 不安・質問#1のtrade-off (cap超=常に高域coverage0) を、
    coverageしきい値を確定する前に踏まえること
  - `plans/m76-deepmodal/harness.md`/`spec.md`/`sub-04.md` (planner編集分) には
    触っていない

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-16)。planner が実コードと実行で確認した結果、SELF_EVAL が気づいていない**証拠の消失**と**文書と実装の矛盾**が 1 件ずつあった。(a) `data\stage0\stats.json` の現物は `count_ok: 0` / `eigsh_seconds.median: null` / `full_occupancy_probe: []` — 再開実行が `{"name","status":"cached"}` だけを積んで上書きするため、SELF_EVAL が引用した数値 (median 22.8s / cube 360.8s 等) が**どこにも残っていない**。受け入れ条件 7 が名指しする成果物なので must。M76h の ModelNet10 は再開前提 = 同じ経路で分布が永久に取れない。(b) `modal.py` の docstring は「LOBPCG の結果は統計専用、npz の学習データには含めない」と書いているが、`dataset.py:116-118` と `:208-213` は builtin に `allow_exceed_cap` を渡して**LOBPCG 由来の npz を書いている** (cube/sphere/cylinder の 3 本)。一方 box_1 (占有 15979 < cube の 24389) は cap で捨てられている。sub-04 からは区別が付かない。裁定: 機構は採用、ただし npz に `method` を記録して可視化する (spec §4.1 / §8 に反映)。不安・質問 2 (L_ref 据え置き) は**記録すべき**と裁定し spec §8 へ、3 (primitives.py のテスト) は should。pytest 23 緑 / check_rules 0 error / `me.sum()/3 == ρh³` の解釈 (24 ベクトルは DOF 単位なので 3 で割るのが物理質量) は planner も再実行・確認して妥当。
- round 2: **VERDICT: REWORK** (planner、2026-09-16)。round 1 の指摘 1-4 と仕様変更の実装は**全部確認できた** (planner 再実行: pytest 27 passed / stats.json は `count_ok=38`・`eigsh median 22.93`・probe 3 本が復活 = 再開バグ解消 / `cube#mesh0#prim0.npz` に method・residuals(34)・residual_max・dropped・converged・iterations・solver_params が入っている / check_rules 0 error)。差し戻しの理由は**新しい欠陥ではなく、校正結果の診断が誤っていること**: 「LOBPCG が 150 本中 116 本 (77%) 取りこぼす」は解法の質ではなく**モード数の予算差**。`calibrate.py:83` は直接法に `k=150` を渡し、`:84` は LOBPCG に `m` を渡していない = 既定 40 → 40 − 剛体 6 = **報告された 34 とちょうど一致**。capsule (16200 DOF) と sphere (45864 DOF) で 34/116 が**完全に同一**なのも収束失敗では説明できない (質の問題なら形状ごとに違う値になる)。見つけた 34 本の周波数一致は rel_err median 1e-12〜1e-13 と極めて良好。さらに**直接法も k=150 で頭打ち**だった (両形状とも帯域フィルタ後がちょうど 150 = 制約は帯域ではなく k)。よって truncation は LOBPCG 固有ではなく両経路の問題。基準形状を要らず 1 メッシュ単体で判定できる指標 (`f_top < f_max`) を spec §4.1 に追加し、must として記録を要求した。★この発見自体は**受け入れ条件 21 が働いた結果**であり、校正ジョブの価値は証明された。
- round 3: **VERDICT: OK** (planner、2026-09-16)。round 2 の must 2 件を実測で確認: (1) `calibrate.py` の `budget_matched` が capsule / sphere とも `missed_count = 0` — 予算を揃えると取りこぼしが消える = round 2 の「77%」が予算差だったという planner の診断を実地で裏付けた。`direct_reference_large_k` が直接法も `spectrum_complete = false` (capsule f_top 5434 / sphere 7792) を示し「LOBPCG 固有ではない」も確認できる形になった。(2) coverage は OR 意味論 (`compact.cell_band_coverage`)、npz と stats に `band_coverage` / `coverage_ratio` / `coverage_high` / `cell_coverage`、stats に `band_distribution` と `band_distribution_by_method`。`band_occupancy_ratio` は `coverage_ratio` に一本化済み。`method` は分岐に使われていない (grep で保存・コメントのみ)。★planner の独立検算: Mel 帯域 24 の下端 = **4895 Hz**、lobpcg 3 本の `f_top` = 3463–4740 Hz → `coverage_high = 0.000` は**独立に計算した f_top と整合する正しい値**であってゼロ埋めのバグではない。pytest 28 / check_rules 0 error / 再開実行で統計一致も再確認。coverage しきい値は planner が §8 で「stage0・stage1 は既定 off」と確定し、適応予算の検討を sub-08 へ申し送った。
