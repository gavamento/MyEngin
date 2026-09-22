# harness 台帳: m78-project-shaders

- 依頼原文: プロジェクト側でシェーダーを追加できる機能を追加したい
- 開始: 2026-09-22 / 基点コミット: bbd11543770d2830552957ecf4122b0a55e5521d
- フェーズ: 実装

## 事前資料 (planner は必ず読むこと)
- `plans/m78-project-shaders/design-draft.md`
- `plans/m78-project-shaders/reference-unity-ue.md`

## ユーザー判断
| 論点 | 決定 | 日付 |
|---|---|---|
| 対象範囲 | ポスト + コンピュート (サーフェス外) | 2026-09-22 |
| 形式 | 生 HLSL + Properties | 2026-09-22 |
| 失敗時 | マゼンタ／エラー表示 | 2026-09-22 |
| 挿入点 | BeforeTonemap + AfterTonemap | 2026-09-22 |
| 保存 | *.fxstack.json + カメラ AssetRef | 2026-09-22 |
| コンピュート | シーン駆動 + ABI v21 (Texture 必須) | 2026-09-22 |
| dirty ツリー | 明示ステージのみで続行 | 2026-09-22 |

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 Properties DSL | OK | 1 | 27758be | nit: errno |
| sub-02 ポスト挿入 | OK | 2 | 92f372e | nit: Collect/RunPasses 二重 |
| sub-03 fxstack+Inspector | 実装中 | 0 | | |
| sub-04 CS シーン駆動 | 未着手 | 0 | | |
| sub-05 Compute ABI v21 | 未着手 | 0 | | |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- 無関係未追跡は触らない。`git add -A` 禁止。
- PackProperties: cbSizeBytes==0 なら空成功 → Runner は CB 省略可
- Header-only: `name.empty() && cbOffset==-1`
- planner: 818bdf1a… / coder sub-01: c500e245…
