# sub-08 (M76h): 本学習 (stage1) と文書

- 依存: sub-06, sub-07
- 状態: 未着手
- 往復: 0

## やること
spec §2 #14 の裁定どおり、**stage1 (小規模自前 ≤ 100 形状) で端から端まで通して `.dmnet` をコミット**し、ModelNet10 以降はユーザーが README の手順で回せる状態にする。

- データ: stage1 = `assets\models` + 三校 / HAL Collector のモデル (合計 ≤ 100)。`dataset.py --stage small --list <一覧>`。
- 学習: `train.py --epochs 100` (stage0 + stage1) → `export.py --out assets\deepmodal\deepmodal.dmnet` (≤ 4 MB) → `Editor.exe --modal-bake` → `--modal-demo` で `ampScale` / physmat の α, β を耳で詰める (値の変更は physmat JSON と export の統計。コードは触らない)。
- README: ModelNet10 の手順 (`dataset.py --stage modelnet10 <dir> --jobs 12` ≈ 4.5 h 見込み / 再開方法 / 門を越えた証拠 (sub-04 のログ) が無ければ実行禁止 / ModelNet40 は同手順)。
- 文書:
  - `engine_spec.md` §10.7 (経路図 / `.mvox` `.msfm` `.dmnet` の版と配置 / 後処理順 / レート制限 / バックエンド / CLI 6 本)
  - `docs\adr\ADR-0NN-deep-modal.md` (次の空き番号。現時点で ADR-019 が末尾): per-collision 合成 vs ストリーミング / CPU 推論 + バックエンド抽象 vs ONNX・DirectML / C++ 単一ボクセライザ / |k| と Σ|a| (論文式 9 からの逸脱) / 絶対音量 / wave 口封じの段階移行 / 参照材質と L_ref / poissonRatio は PhysMat に保持するがランタイムは読まない (ユーザー判断 spec §2 #4、将来 ν を考慮するモデルへの拡張口) / データ段階の門
  - `README.md` (機能概要に 1 節)
  - `CLAUDE.md`: 末尾 TypeId **61 = ModalSound** (Cloth/SoftBody 予約は 62/63 へ)、CLI 6 本 (`--modal-voxelize` / `--modal-bake` / `--modal-backend` / `--modal-audio-log` / `--modal-sync-bake` / `--modal-demo`)、検証表に「`--modal-audio-log` の 2 run 一致」、横断チェックリストに「`.dmnet` を差し替えたら `--modal-bake`」「constGroups に C++ ⇄ Python の組がある」、include の向き (Audio → Modal)
  - `plans\m76-deepmodal.md` (design-draft の「実装開始時」の指示どおり計画を複写し、申し送りを書く)
- 共通検証を全部回す (shot_verify を含む — golden は触っていないことの確認)。

## やらないこと (このサブでは)
ModelNet10 / 40 の実走 (ユーザーが回す。`[ユーザーに聞ける]`)。`D3d11ModalBackend`。合成のワーカー化。

## 触る場所 (planner の見立て)
- `assets\deepmodal\deepmodal.dmnet` (新規、≤ 4 MB)、`assets\physmats\*.physmat.json` (耳合わせの値)
- `tools\deepmodal\README.md`
- `engine_spec.md` (§10.6 の後、1703 以降)、`docs\adr\`、`README.md`、`CLAUDE.md`、`plans\m76-deepmodal.md`

## 受け入れ条件 (このサブ)
spec §5 の 19, 20, 17。
1. stage1 で `dataset → train → export → --modal-bake (exit 0) → --modal-demo (played > 0)` が通り、`.dmnet` がコミットされている。学習ログの最終 loss を SELF_EVAL に
2. README に ModelNet10 の手順 / 時間見積もり / 実行禁止の門 / 再開方法
3. 文書 4 点 + CLAUDE.md の更新。`check_rules.ps1` 緑
4. 共通検証: Debug/Release 0 警告 → `--selftest` → `check_rules.ps1` → `replay_verify.bat` → `shot_verify.bat` (24 枚不変)

## 検証コマンド
- `cd tools\deepmodal && python dataset.py --stage small --list small.txt --out data\stage1 && python train.py --epochs 100 --data data\stage0 data\stage1 && python export.py --out ..\..\assets\deepmodal\deepmodal.dmnet`
- `cmd /c "bin\x64\Release\Editor.exe --modal-bake"`
- 共通検証 4 本 + `tools\shot_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴

## round 3 からの申し送り (sub-03、2026-09-16)
- **適応予算の検討** (spec §8): ModelNet10 は大きいメッシュが増えるので、固定モード数 (直接法 k=150 / LOBPCG m=40) だと高域が系統的に欠ける。stage0 実測で lobpcg 経路 3 本の `coverage_high` が一律 0.000 (f_top 3463–4740 Hz < 帯域 24 の下端 4895 Hz)。本実行の前に「`f_top ≥ f_max` に達するまで、または実時間上限まで m / k を上げる」適応予算を入れるかを決めること。入れない場合は coverage しきい値でどう扱うかを決める (既定 off のままだと高域が薄い教師データが混ざる)。
- coverage しきい値は stage0/stage1 では既定 off で確定済み。M76h で分布を見て再検討する。
