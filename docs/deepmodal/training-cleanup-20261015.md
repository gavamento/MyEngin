# Deep-Modal 再学習と容量整理（2026-10-15）

## 目的と現在の状態

ユーザー依頼: 学習を済ませ、不要になった元データを片付ける。

学習開始時のGit作業ツリーはクリーン。学習後、ユーザーから「エンジンへの反映と削除して」の指示を受け、音量較正・モデル反映・承認済み展開データの削除まで完了した。
学習処理はバックグラウンドで実行した（開始時PID: 7700）。両モデルの学習・書き出し検証は完了し、ルートの `completed.json` と各モデルの `completed.json`、ログの `[completed] full` を確認した。PID 7700のプロセスも終了している。
反映作業開始時に存在した `src/Editor/App/StartupSplash.cpp` の未コミット変更は本作業の対象外であり、変更していない。

開始後の確認: 37,370件のNPZの全件検査は成功。生物モデルのepoch 1と `last.pt` / `best.pt` の保存を確認した。
生物全1,257件のpooled評価は、初期R²=0.1357446・mask accuracy=0.7966916から、epoch 1でR²=0.1972711・mask accuracy=0.8205552へ変化した。予定200 epochの完了ではない。

作業ディレクトリ: `C:\HAL\MyEngin\cache\deepmodal_finish_20261015`

## 再学習が必要な理由

`docs/deepmodal/synthetic_shapes.md` §4に、既存の汎用・生物モデルが入力x/z軸の退行を含む学習経路で作成され、修正後の再学習は未実施と記録されている。
現在の `train.py` は `model.voxel_to_tensor()` を通す修正済み経路。
修正後の経路で既存生物モデルを等間隔抽出64件に適用した今回の参考値は、pooled R²=0.1122、mask accuracy=78.78%。全件評価や未知形状の評価ではない。

## 実行方法と成果物

既存 `train.run_epoch_loader()` と `train.evaluate_pooled_loader()` を呼び出す専用ランナー:
`C:\HAL\MyEngin\cache\deepmodal_finish_20261015\finish_training.py`

- 事前検査: NPZ全メンバーを読み、ZIP CRC、vox/valid/feat形状、有限値を確認。NPZのSHA-256一覧と品質集計を保存。
- 生物: `data/creatures` の1,257件、200 epoch、batch 16、lr=2e-4、40 epochごと半減、下限2e-5。
- 汎用: stage0 / stage1 / modelnet40 / thingi10k / creatures / synth_v1 の37,370件、50 epoch、batch 16、lr=2e-4、10 epochごと半減、下限2e-5。
- 既存 `runs/deepmodal_creatures.pt` / `runs/deepmodal_full.pt` から重みを引き継ぐ。正規化範囲は各対象データ全体から計算。
- 各epochの最後に専用出力先の `last.pt` へ重み・optimizer・RNG状態を保存。1 epoch目と5 epochごと、および最終epochに全件評価。
- 評価したepochのうちpooled R²が最良のものを `best.pt` に保存。
- 評価は学習データ上の指標。未知形状への汎化性能の証明には使わない。
- 完走後に各モデルを `.dmnet` へ書き出し、重みハッシュ、FP16丸め後の重み・biasのビット一致、有限な推論出力を確認。
- 新しい `.dmnet` のampScaleは未較正の1.0。既存の11478.0を再較正済みと扱わない。エンジン資産へ自動配置しない。

ログ・状態ファイル（上記作業ディレクトリ内）:

| パス | 用途 |
|---|---|
| `training.log` / `training.stderr.log` | 標準出力・標準エラー |
| `dataset_audit.json` / `dataset_manifest.json` | 全件検査集計・入力一覧とハッシュ |
| `source_hashes.json` | 学習時のPythonソースのハッシュ |
| `creatures/` / `full/` | 設定、初期評価、途中保存、最良モデル、評価履歴 |
| 各モデルの `completed.json` | 学習完走とPython側の書き出し検証結果 |
| ルートの `completed.json` | 両モデルの処理完走。削除済みを意味しない |
| `failure.txt` | 異常終了した場合の例外 |

学習開始前の64件の試行では学習約1.09秒、GPUピーク割当約0.62 GiB（RTX 3060 12 GiB、PyTorch 2.11.0+cu128）。単純換算は両モデル合計約10時間だが、I/O・評価・保存等を含まない見積り。

停止するときは作業ディレクトリに `STOP` ファイルを作ると次のミニバッチ前に終了する。最後に完了したepochまで保存される。
再開は停止原因を解消し、同じランナーを再実行する。`STOP` を作った場合はファイル削除規則に従って解除すること。
再開前に元プロセスが終了していることを確認し、同じ出力先で二重起動しない。

## 容量と整理方針

読み取り時点のファイル論理サイズ:

| 対象 | 容量 | 方針 |
|---|---:|---|
| `tools/deepmodal/data/raw/ModelNet40` | 9,772,825,448 bytes（約9.10 GiB） | ZIPとの全件一致を再確認し、承認を受けて削除済み |
| `tools/deepmodal/data/raw/ModelNet40.zip` | 2,039,180,837 bytes（約1.90 GiB） | 復元元として保持 |
| `tools/deepmodal/data/raw/creatures` | 207,924,343 bytes | 保持 |
| `tools/deepmodal/data/raw/thingi10k` | 210,630,300 bytes | 保持 |
| `data` のraw以外 | 約2.68 GiB | NPZ・検査情報を保持し、再学習可能性を残す |
| 既存 `.pt` とエンジンの `.dmnet` | 各数MiB | 保持 |

復元確認スクリプト: `cache/deepmodal_finish_20261015/verify_raw_backup.py`。
成功時に `verified_modelnet40_backup.json` を出力する。ZIPと展開済みフォルダーのファイル集合・サイズ・全バイトを比較し、ZIP全メンバーのCRCを検証。元ZIPと各展開済みファイルのSHA-256も記録する。

今回の照合は成功: 12,311ファイル、9,772,825,448 bytesすべて一致。保持するZIPのSHA-256は `42dc3e656932e387f554e25a4eb2cc0e1a1bd3ab54606e2a9eae444c60e536ac`。

削除候補の絶対パス: `C:\HAL\MyEngin\tools\deepmodal\data\raw\ModelNet40`

このフォルダーを削除すると、その `.off` を参照する一覧からのボクセル化・FEM再生成にはZIPの再展開が必要になる。保持するNPZからの学習と既存 `.dmnet` の推論は展開済みメッシュを読まない。
削除前に全ファイルを再照合し、対象の絶対パス、ファイル数・サイズ、保持するZIPのSHA-256を再検証した。対象ツリーにreparse pointがないことも確認し、ユーザーが承認した展開済みフォルダーだけを削除した。

## 音量較正・エンジン反映の結果

実行スクリプト: `cache/deepmodal_finish_20261015/deploy_models.py`。
`bin/x64/Release/Runtime.exe` を隔離した検証用プロジェクトで実行し、`--modal-demo --modal-sync-bake --modal-backend cpu --modal-face-probe --modal-wav-dump` による実PCMで較正した。
`MYE_MODAL_PROBE_IMPULSE=6` の6面のピークを測り、昇順index 3（median_high）が−12 dBFSになる倍率を計算して再測定した。

| モデル | 反映先 | ampScale | J=6のピーク中央値 |
|---|---|---:|---:|
| 汎用（epoch 30） | `assets/deepmodal/deepmodal.dmnet` | 22869.02734375 | −12.0007369 dBFS |
| 生物（epoch 200） | `assets/deepmodal/deepmodal_creatures.dmnet` | 8740.9287109375 | −12.0007369 dBFS |

- 両モデルでJ=0.35 / 1 / 3 / 6 / 30 / 100のピーク中央値が単調に増加することを確認。概略−24.34 / −19.78 / −15.01 / −12.00 / −5.01 / −0.34 dBFS。
- J=6では両モデルの全6面が無音・フルスケール飽和ではないことを確認。重みハッシュは未較正の学習成果物と一致し、較正が重みを変更しないことも確認。
- `.dmnet.meta` は維持。実際の配置ファイルのSHA-256が較正済み候補と一致することを確認。
- `runs/deepmodal_full.pt` と `runs/deepmodal_creatures.pt` も今回の `best.pt` へ更新し、ハッシュ一致を確認。旧モデル2個と旧チェックポイント2個は `cache/deepmodal_finish_20261015/deployment/backups/` に保持。
- プロジェクト側にモデルを置かない検証でも、Runtimeログが `C:\HAL\MyEngin\assets\deepmodal\deepmodal.dmnet` の読み込みを示し、同じ−12.00 dBFSを再現。20 impacts / 20 played、notReady / dropped / playFailedはいずれも0。
- エンジンの既定解決名は `deepmodal.dmnet`。生物用ファイルは同時に配置した別モデル資産であり、自動切替機能を追加したものではない。
- 隔離したプロジェクトではGameLogic DLLと既定テストテクスチャを置かなかったため、その欠落ログは出る。今回のコード生成デモはEngine側で構築され、物理衝突・推論・PCM合成の成功を実測した。実ゲームのスクリプトや聴感品質の保証には使わない。

詳細: `deployment/calibration.json`、`deployment/completed.json`、`deployment/installed_assets.json`、`deployment/installed_checkpoints.json`。各検証ディレクトリにRuntimeログとWAV、PCMピーク集計を保存。

## 削除結果

2026-10-15 22:26 JST、`C:\HAL\MyEngin\tools\deepmodal\data\raw\ModelNet40` の12,311ファイル、9,772,825,448 bytesを削除。削除後のパス不存在を確認。
削除直前・直後のCドライブ空き容量は130,762,113,024 → 140,560,977,920 bytes。差分9,798,864,896 bytesにはファイル割当単位と他処理の影響も含むため、削除ファイルの論理サイズとは区別する。
保持するZIPのSHA-256は前記の値と一致。ZIP・NPZ・学習済みモデル・チェックポイントは保持した。復元には `ModelNet40.zip` を `data/raw` へ再展開する。
機械可読記録: `cache/deepmodal_finish_20261015/cleanup_completed.json`。

## 残る確認

- 学習完走・書き出し検証は完了。生物200 epoch、汎用50 epoch。FP16重みの往復ビット一致、重みハッシュ、有限な推論出力の確認は両方成功。
- 生物の採用候補はepoch 200: pooled R²=0.2926574、mask accuracy=99.6450%。汎用の採用候補はepoch 30: pooled R²=0.2949529、mask accuracy=88.8371%。汎用epoch 50のR²=0.2928902より良かったepoch 30を保存。いずれも学習データ上の評価であり、汎化性能・音響品質を保証しない。
- 音量較正、Runtimeの読み込み・PCM生成、削除・空き容量の確認は完了。人による試聴と実ゲームでのプレイテストは未実施。

エンジンソースの変更・ビルド・Replay検証は本作業では実施していない。
