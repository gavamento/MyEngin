# harness 台帳: m78-project-shaders

- 依頼原文: プロジェクト側でシェーダーを追加できる機能を追加したい
- 開始: 2026-09-22 / 基点コミット: bbd11543770d2830552957ecf4122b0a55e5521d
- フェーズ: 実装

## 事前資料 (planner は必ず読むこと)
- `plans/m78-project-shaders/design-draft.md` — 現状コードの事前調査 + ユーザーとの合意事項
- `plans/m78-project-shaders/reference-unity-ue.md` — Unity/UE の方式調査 (一次資料ベース)

## ユーザー判断 (策定前に確定済み)
| 論点 | 決定 | 日付 |
|---|---|---|
| 対象範囲 (初回) | 3 系統すべて: サーフェス / ポストエフェクト / コンピュート | 2026-09-22 |
| **対象範囲 (改定)** | **ポストエフェクト + コンピュートのみ。サーフェス／マテリアル差し替えはスコープ外** | 2026-09-22 |
| 独自パラメータ | 持たせる (Unity の Properties 相当。宣言を解析して Inspector に自動露出) | 2026-09-22 |
| 作者が書く形式 | 生 HLSL + 属性／コメントで Properties (案 A) | 2026-09-22 |
| コンパイル失敗時 | エラー／マゼンタ相当で気づける表示 (案 A) | 2026-09-22 |
| 対応パス (旧・サーフェス向け) | Forward + Deferred GBuffer — **改定によりサーフェス外のため本マイルで無効** | 2026-09-22 |
| 開始地点 | 先行していた bool 化スイープを `bbd1154` としてコミットしてから着手 | 2026-09-22 |
| dirty ツリー | 無関係未追跡は触らず、コミットは明示ステージのみで進める (ユーザー選択 1) | 2026-09-22 |
| コンピュート境界 | **ABI v21 も本マイルに含める** (planner 裁定「シーン駆動のみ」を覆す) | 2026-09-22 |
| 挿入点 | BeforeTonemap + AfterTonemap のみ (裁定どおり) | 2026-09-22 |
| 保存形式 | `*.fxstack.json` + カメラ AssetRef (裁定どおり) | 2026-09-22 |
| 実装開始 | 受け入れ条件どおり実装開始可 | 2026-09-22 |
| SetComputeTextureFromAsset | **v21 最小セットに必須** (ユーザー B。planner「削可」を覆す) | 2026-09-22 |

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 Properties DSL | 実装中 | 0 | | |
| sub-02 ポスト挿入 | 未着手 | 0 | | 依存: sub-01 |
| sub-03 fxstack+Inspector | 未着手 | 0 | | 依存: sub-01,02 |
| sub-04 CS シーン駆動 | 未着手 | 0 | | 依存: sub-01,03 |
| sub-05 Compute ABI v21 | 未着手 | 0 | | 依存: sub-04 / Texture 必須 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- 作業ツリーに未追跡の雑多ファイルがある (`.agents/` `SKILL.md` `_agent_diff1.patch`
  `_agent_diff2.patch` `scratch_vox/` `tools/deepmodal/*.txt`)。M78 とは無関係。
  コミットは常にファイル名を明示してステージすること (`git add -A` 禁止)。
- `plans/m78-project-shaders/` 自体も未追跡。仕様・台帳は明示ステージ。
- 現ブランチは `GY-PS_Kadai` (master ではない)。
- 仕様確定: ポスト+コンピュート、生 HLSL Properties、マゼンタ失敗、fxstack、
  挿入 Before/After Tonemap、ABI v21+SetComputeTextureFromAsset 必須。
- planner ID: 818bdf1a-f6ad-4fe7-b1ed-028a65eab6f7
