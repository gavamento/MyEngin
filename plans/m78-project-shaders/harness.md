# harness 台帳: m78-project-shaders

- 依頼原文: プロジェクト側でシェーダーを追加できる機能を追加したい
- 開始: 2026-09-22 / 基点コミット: bbd11543770d2830552957ecf4122b0a55e5521d
- フェーズ: 実装

## ユーザー判断
| 論点 | 決定 |
|---|---|
| 対象 | ポスト + コンピュート (サーフェス外) |
| 形式 | 生 HLSL + Properties |
| 失敗 | マゼンタ／エラー |
| 挿入点 | BeforeTonemap + AfterTonemap |
| 保存 | *.fxstack.json + カメラ AssetRef |
| ABI | v21 + SetComputeTextureFromAsset 必須 |

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 Properties DSL | OK | 1 | 27758be | |
| sub-02 ポスト挿入 | OK | 2 | 92f372e | selftest ok&= |
| sub-03 fxstack+Inspector | OK | 2 | 1758116 | Tex2D must |
| sub-04 CS シーン駆動 | OK | 2 | 062cd01 | Godray後・GetOutputSRV |
| sub-05 Compute ABI v21 | 実装中 | 0 | | coder 背景実行中 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り
- 無関係未追跡は触らない。`git add -A` 禁止。
- planner: 818bdf1a… / sub-05 coder: 1e33f9cc…
- _st_clean.err/.out は selftest 残骸。コミットしない。
