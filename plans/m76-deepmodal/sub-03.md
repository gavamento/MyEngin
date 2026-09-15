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
spec §5 の 6, 7, 8。
1. `pytest` 全緑: test_fem (Ke 対称・半正定、剛体 6 モードで `K·r ≈ 0`、集中質量総和 = ρh³) / test_modal (2×2×2 で E×4 → ω×2、ρ×4 → ω/2、h×2 → ω/2、1e-6。先頭 6 固有値 ≈ 0) / test_compact (単調・端点・Σ|a|・空帯域補間・mask) / test_layout (.mvox 64 B、C++ cube で 27000。`@pytest.mark.editor`、Editor.exe 無ければ skip) / test_contact (cell を変えると励起ベクトルが変わる)
2. `python dataset.py --stage primitives --out data\stage0` → npz ≥ 20 本 + builtin 6 本 + stats.json (メッシュごとの eigsh 秒 / 帯域占有率 / モード数)。満杯立方体 < 600 s、primitive 中央値 < 60 s
3. `check_rules.ps1` 緑。`layout.py` の `VOXEL_N` を一時的に 31 にすると規則 9 が赤くなる (戻す)

## 検証コマンド
- `cd tools\deepmodal && pytest`
- `python tools\deepmodal\dataset.py --stage primitives --out tools\deepmodal\data\stage0`
- `pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
