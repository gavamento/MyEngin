# harness 台帳: m85-behavior-tree

- 依頼原文: M85の実装
- 開始: 2026-10-05 / 基点コミット: baf29bf0632d5daccd2b0f8496513c704b7d1237
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | 着手 | 0 | | BT の核 (.bt/.bb・コンポーネント・実行器・Composite・Wait・BT 節) |
| sub-02 | 未着手 | 0 | | Decorator 5 種と Abort |
| sub-03 | 未着手 | 0 | | Task 4 種 (MoveTo に failOnStuck、既定 false) |
| sub-04 | 未着手 | 0 | | AI ノード 4 種 |
| sub-05 | 未着手 | 0 | | 汎用イベントキューと SendEvent |
| sub-06 | 未着手 | 0 | | AnimatorPlay / PlayAnimation / SubTree |
| sub-07 | 未着手 | 0 | | 巡回ルート |
| sub-08 | 未着手 | 0 | | BT 窓 (1) |
| sub-09 | 未着手 | 0 | | BT 窓 (2) Undo・BB 編集・検査 |
| sub-10 | 未着手 | 0 | | BT 窓 (3) ライブ表示 |
| sub-11 | 未着手 | 0 | | ABI v27 |
| sub-12 | 未着手 | 0 | | C# タスク |
| sub-13 | 未着手 | 0 | | --bt-demo・replay_verify・golden |
| sub-14 | 未着手 | 0 | | ADR-025 と全体検証 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- (2026-10-05) BT の実行状態 = BehaviorTreeSystem の表 + SimSnapshot の BT 節 + ハッシュ (planner 裁定どおり)
- (2026-10-05) 根の終了後のやり直し = 次の tick から (裁定どおり)
- (2026-10-05) MoveTo の Stuck = **MoveTo にチェックボックスを持たせ、詰まったら Failure にするかをノードごとに選ばせる** (裁定「Running のまま」から変更。既定値は planner が決める)
- (2026-10-05) BT 無しの巡回 = 作らない (裁定どおり)
- (2026-10-05) ノードの表示位置 = .bt.json に入れる (裁定どおり)
- (2026-10-05) 上記を反映して spec 確定、sub-01 から着手してよい

## 申し送り (セッション跨ぎ)
- ロードマップ (`plans\ai-roadmap-m83-m86.md`) には「harness は使わず直接実装」とあるが、2026-10-05 にユーザーが `/harness M85の実装` を明示したのでハーネスで回す。
- (planner 2026-10-05) spec.md 確定 (planner 裁定)・sub-01〜14 を作成。番号の見込み: TypeId 78 BehaviorTree (sub-01) / 79 PatrolRoute (sub-07)、snapshot v34 (sub-01) から各サブで +1、ABI は sub-11 で 1 回だけ v27。調べた事実の要点は spec 2. の表に file:line 付き。
