# sub-09: BT 窓 (2) — Undo / Redo、Blackboard 編集、検査エラー

- 依存: sub-08
- 状態: 未着手
- 往復: 0

## やること
- Undo / Redo (spec 4.3): 窓の中の独自スタック。方式は「操作の前後でアセット全体 (BT と BB) の JSON を持つ」単純なもの (シーンの `UndoStack` は使わない、`UndoStack.h:25-33` はシーン専用)。1 操作 = 1 段 (ドラッグは離した時に確定、パラメータは編集確定時)。Ctrl+Z / Ctrl+Y は BT 窓がフォーカスを持つときだけ (シーンの Undo と取り合わない)。上限 128 段。保存しても履歴は消さない。
- Blackboard パネル: 参照している `.bb.json` のキーの追加・削除・改名・型・初期値・eventName。BB を新規作成して BT に割り当てる操作。改名・削除で BT のノードが参照するキーを追従 / 未設定化 (追従は改名だけ)。BB の保存は別ボタン。
- 検査エラー (spec 4.3): SubTree の BB 不一致、LowerPriority / Both の位置、Parallel の左がタスクでない、子の数の違反、未設定キーの参照・型の不一致、C# タスクを含む (警告。sub-12 で C# タスクが入るまでは枠だけ)。赤枠と下の一覧、一覧をクリックでノードへ移動。検査はモデル層の関数にする。

## やらないこと (このサブでは)
- ライブ表示 (sub-10)

## 触る場所 (planner の見立て)
- `BehaviorTreeWindow.*`、`BehaviorTreeEditModel.*`、`BehaviorTreeEditorSelfTest.*`、`BlackboardLibrary` (保存)、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 11) Undo / Redo の往復 (追加 → 接続 → 移動 → パラメータ → 削除 を全部戻して元のバイト列と一致、やり直して後の状態と一致)、BB のキー改名の追従、各検査エラーの検出 — `BehaviorTreeEditorSelfTest`。
2. 画面: 検査エラーのある木 (赤枠と一覧) と BB パネルの画像 (一時プローブ)。
3. シーンの Undo と取り合わないこと (BT 窓にフォーカスが無いとき Ctrl+Z はシーン側) — 手順の記録。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、`tools\check_rules.ps1`、一時プローブの `--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
