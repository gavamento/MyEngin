# harness 台帳: m78-project-shaders

- 依頼原文: プロジェクト側でシェーダーを追加できる機能を追加したい
- 開始: 2026-09-22 / 基点コミット: bbd11543770d2830552957ecf4122b0a55e5521d
- フェーズ: 完了
- 完了: 2026-09-22

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
| sub-02 ポスト挿入 | OK | 3 | 92f372e / c1bbe86 | 上限8 fix |
| sub-03 fxstack+Inspector | OK | 3 | 1758116 / a96f015 | Override+サンプル |
| sub-04 CS シーン駆動 | OK | 3 | 062cd01 / 4595ccb | Override 確認 |
| sub-05 Compute ABI v21 | OK | 1 | 0bcc5dd | |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 3/3/N/A/4 | #1 major CameraOverride |
| 2 | PASS | 4/4/N/A/4 | なし (minor 0) |

## 申し送り (残 minor / 手動)
- Tint/マゼンタ/CS→ポストの GUI 目視は未実施 (reviewer N/A)
- gray/black/bump 専用 SRV は後続可 (WARN+docs 済み)
- Part selftest CesiumMan 不足は環境要因
- GameLogic.dll は MYE_API_VERSION 21 で再ビルド必須
- 反対意見記録: ABI を今マイルに含める (1B)、TextureFromAsset 必須 (B) — planner 反対を押し切り
