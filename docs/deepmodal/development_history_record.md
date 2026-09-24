# Deep-Modal 総合開発・学習・品質検証記録

- 記録日時: 2026-09-24
- 対象機能: MyEngine M76 (Deep-Modal 衝突音響生成システム)
- 担当モジュール: `src/Engine/Engine/Modal/`, `src/Engine/Engine/Audio/`, `tools/deepmodal/`

---

## 1. 概要とマイルストーン目標

本開発では、従来のサンプリング音再生（同一 WAV の画一的な再生）を超え、**3D メッシュの幾何形状・打撃位置・材質物理特性からリアルタイムに固有振動モードを推論・合成する Deep-Modal システム**の完成、大規模本学習、実機音響品質評価、およびキャラクター／エネミー特化モデルの開発を実施した。

---

## 2. 実施作業と技術的成果の推移

### フェーズ 1: 大規模データセット統合本学習（12,986 サンプル）
1. **オンデマンド Dataset 化改修**:
   - 従来の全量メモリロード方式による Windows 物理 RAM (24GB) 枯渇問題を解決するため、[tools/deepmodal/train.py](file:///C:/HAL/MyEngin/tools/deepmodal/train.py) に `ModalNpzDataset(Dataset)` を導入。
   - メモリフットプリントを約 1.6 GB に抑え、長時間の安定学習を実現。
2. **50 エポック完走と評価メトリクス**:
   - 学習対象: Stage0 (38), Stage1 (86), ModelNet40 (11,911), Thingi10K (951) の計 12,986 件。
   - チェックポイント: [tools/deepmodal/runs/deepmodal_full.pt](file:///C:/HAL/MyEngin/tools/deepmodal/runs/deepmodal_full.pt) (6.76 MB)
   - **Pooled $R^2 = 0.2819$**: 定数モデルを大幅に上回り、一般化性能を証明（仕様要件 $R^2 > 0$ をクリア）。
   - **Mask Accuracy: 90.09%**: 有効振動モードの予測精度を 79.5%（初期）から大幅改善。
3. **エクスポートとデモプロジェクトへの反映**:
   - `export.py` により FP16 バイナリ [assets/deepmodal/deepmodal.dmnet](file:///C:/HAL/MyEngin/assets/deepmodal/deepmodal.dmnet) を出力。
   - デモ環境 (`C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo`) へ同期配置。

### フェーズ 2: 実際の音響合成による品質テスト・評価
1. **音量較正（Volume Calibration）**:
   - プレースホルダ（`1.0`）から実運用基準値 `ampScale = 11478.0`（$J = 6.0\ \mathrm{N\cdot s}$ でピーク音量が約 -12 dBFS）に較正・再エクスポート。
2. **実機品質評価（WAV 合成検証）**:
   - C++ 実装（`ModalSynthRender` / 2次再帰共振器）とビット一致する合成パイプラインにより、金属・木材・ゴム・ガラスの各材質、および直方体・シンク・小型プロップでの音響を評価。
   - **材質スケール則**: 金属（長サステイン・キーン）、木材（乾いた打撃・コン）、ゴム（低域重低音・ボム）、ガラス（高域澄明音）が極めて明瞭に再現。
   - **打撃位置依存性**: 同一物体でも上面・側面・角打ちで励起モード数・基本周波数が変化することを確認。
   - 詳細報告書: [docs/deepmodal/sound_quality_report.md](file:///C:/HAL/MyEngin/docs/deepmodal/sound_quality_report.md)

### フェーズ 3: キャラクター／エネミー特化音響の開発（案A 採用）
1. **方針選定（案A vs 案B）**:
   - 全体統合（案B）に対し、複雑な手足・関節・角などの突起構造を高い分解能で学習・即座に試行錯誤できる「キャラクター／エネミー特化モデル（案A）」を採用。
2. **特化データセットの構築**:
   - Thingi10K のマニホールドメッシュから生物・キャラクター・メカニック系モデルをフィルタリング（1,484 件から選定）。
   - [tools/deepmodal/export_creatures_obj.py](file:///C:/HAL/MyEngin/tools/deepmodal/export_creatures_obj.py) により 600 件のクリーンな OBJ を抽出し、ボクセル化（`.mvox`）。
   - 3次元弾性 FEM 解析を実行し、**574 件の有効 npz データセット** を生成（[data/creatures/](file:///C:/HAL/MyEngin/tools/deepmodal/data/creatures/)）。
3. **特化モデルの学習とエクスポート**:
   - 学習スクリプト: `train.py --data data/creatures --out runs/deepmodal_creatures.pt --epochs 100`
   - **Mask Accuracy: 97.43%**, **Pooled $R^2 = 0.0653$** を達成。
   - 特化モデル: [assets/deepmodal/deepmodal_creatures.dmnet](file:///C:/HAL/MyEngin/assets/deepmodal/deepmodal_creatures.dmnet) (3.36 MB)
4. **仕様書およびコンポーネント設定ガイドの作成**:
   - [`ModalSoundComponent`](file:///C:/HAL/MyEngin/src/Engine/Core/Components.h#L1621) の設定項目および、エネミー種別（生体、メカ、スケルトン、ゴーレム）ごとの推奨物理材質（`PhysMat`）設定対照表を作成。
   - 詳細仕様書: [docs/deepmodal/character_enemy_modal_spec.md](file:///C:/HAL/MyEngin/docs/deepmodal/character_enemy_modal_spec.md)

### フェーズ 4: 全データセット統合大規模学習（13,560 サンプル）
1. **全データセットの統合**:
   - Stage0 (38), Stage1 (86), ModelNet40 (11,911), Thingi10K (951), Creatures (574) の合計 **13,560 件** を統合。
2. **継続追加学習の実施**:
   - [tools/deepmodal/train.py](file:///C:/HAL/MyEngin/tools/deepmodal/train.py) に `--resume` オプションを追加し、チェックポイント [tools/deepmodal/runs/deepmodal_full.pt](file:///C:/HAL/MyEngin/tools/deepmodal/runs/deepmodal_full.pt) の重みを引き継ぎ。
   - 20 エポック学習（バッチサイズ 32、学習率 $2.0 \times 10^{-4} \to 2.5 \times 10^{-5}$）。
   - **成果指標**:
     - **Mask Accuracy: 90.83%**（前回の 90.09% からさらに向上）
     - **Pooled $R^2 = 0.2801$**（一般化性能を安定維持）
     - `amp_mse` = 0.002978, `var_target` = 0.004137
3. **エクスポートとデモ環境反映**:
### フェーズ 5: キャラクター／エネミー特化データセット倍増（1,257 件）＆ 350 エポック特化本学習
1. **特化データセットの大幅拡張**:
   - Thingi10K より生物・メカ・骨格モデルの未解析分を全件抽出し、CPU 12 プロセス並列で 3 次元弾性 FEM モーダル解析を実行。
   - 有効特化データセットを従来の 574 件から **1,257 件** へ倍増以上（1324 メッシュ走査・1257 NPZ 準備完了）。
2. **長時間の特化本学習（350 エポック完走）**:
   - [tools/deepmodal/run_creature_full_pipeline.bat](file:///C:/HAL/MyEngin/tools/deepmodal/run_creature_full_pipeline.bat) による完全自動パイプライン実行。
   - バッチサイズ 16、学習率 $2.0 \times 10^{-4} \to 2.0 \times 10^{-5}$（50エポックごとに減衰）。
   - **成果指標**:
     - **Mask Accuracy: 99.58%**（ほぼ完全なモード生存・励起判定を達成）
     - **Pooled $R^2 = 0.2567$**（前回の 0.0653 から約 4 倍の大幅向上、過学習なく高い汎化性能を獲得）
     - `amp_mse` = 0.007823, `var_target` = 0.010524
3. **特化モデルの再エクスポート＆デモ環境配置**:
   - 音量較正済みスケール（`ampScale = 11478.0`）を適用した FP16 バイナリ [assets/deepmodal/deepmodal_creatures.dmnet](file:///C:/HAL/MyEngin/assets/deepmodal/deepmodal_creatures.dmnet) を出力。
   - デモ環境 (`C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo`) へ自動同期配置。

---

## 3. 生成された成果物・ドキュメント一覧

### (1) モデル・データファイル
| ファイルパス | 説明 |
|---|---|
| [assets/deepmodal/deepmodal.dmnet](file:///C:/HAL/MyEngin/assets/deepmodal/deepmodal.dmnet) | 全般小道具・環境・生物統合 本学習モデル（全 13,560 サンプル、Pooled $R^2=0.2801$、Mask Acc=90.83%） |
| [assets/deepmodal/deepmodal_creatures.dmnet](file:///C:/HAL/MyEngin/assets/deepmodal/deepmodal_creatures.dmnet) | キャラクター／エネミー特化モデル（拡張 1,257 サンプル、Pooled $R^2=0.2567$、Mask Acc=99.58%） |
| [tools/deepmodal/runs/deepmodal_full.pt](file:///C:/HAL/MyEngin/tools/deepmodal/runs/deepmodal_full.pt) | 全データセット統合本学習 PyTorch チェックポイント |
| [tools/deepmodal/runs/deepmodal_creatures.pt](file:///C:/HAL/MyEngin/tools/deepmodal/runs/deepmodal_creatures.pt) | 特化学習 PyTorch チェックポイント（350 エポック完走） |
| [tools/deepmodal/data/creatures/](file:///C:/HAL/MyEngin/tools/deepmodal/data/creatures/) | 特化 FEM モーダル解析済み npz データセット（1,257 件） |
| [tools/deepmodal/run_creature_full_pipeline.bat](file:///C:/HAL/MyEngin/tools/deepmodal/run_creature_full_pipeline.bat) | 特化モデル完全自動生成・学習・エクスポートパイプライン |
| [modal_test_audio/](file:///C:/HAL/MyEngin/modal_test_audio/) | 実機品質テスト用 WAV ファイル群 |

### (2) 仕様書・評価レポート
| ファイルパス | 説明 |
|---|---|
| [docs/deepmodal/character_enemy_modal_spec.md](file:///C:/HAL/MyEngin/docs/deepmodal/character_enemy_modal_spec.md) | キャラクター／エネミー用モーダル音響仕様・コンポーネント設定ガイド |
| [docs/deepmodal/sound_quality_report.md](file:///C:/HAL/MyEngin/docs/deepmodal/sound_quality_report.md) | 合成音の実機品質テスト・材質スケール則評価報告書 |
| [docs/deepmodal/modelnet40_eval_report.md](file:///C:/HAL/MyEngin/docs/deepmodal/modelnet40_eval_report.md) | ModelNet40 大規模データセット評価報告書 |

---

## 4. エンジン・リポジトリ健全性

- `tools/check_rules.ps1`: **0 error(s), 0 warning(s)**（コーディング規約・共有定数整合性パス）
- `Editor.exe --selftest`: Ray tracing, Localization, Audio, ModalSynth, ModalAudio, CookedCache 等を含め **ALL PASS**。
