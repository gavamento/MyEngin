# sub-09: BT 窓 (2) — Undo / Redo、Blackboard 編集、検査エラー

- 依存: sub-08
- 状態: OK (M85i としてコミット、ハッシュは台帳)
- 往復: 1

## やること
- Undo / Redo (spec 4.3): 窓の中の独自スタック。方式は「操作の前後でアセット全体 (BT と BB) の JSON を持つ」単純なもの (シーンの `UndoStack` は使わない、`UndoStack.h:25-33` はシーン専用)。1 操作 = 1 段 (ドラッグは離した時に確定、パラメータは編集確定時)。Ctrl+Z / Ctrl+Y は BT 窓がフォーカスを持つときだけ (シーンの Undo と取り合わない)。上限 128 段。保存しても履歴は消さない。
- Blackboard パネル: 参照している `.bb.json` のキーの追加・削除・改名・型・初期値・eventName。BB を新規作成して BT に割り当てる操作。改名・削除で BT のノードが参照するキーを追従 / 未設定化 (追従は改名だけ)。BB の保存は別ボタン。
- 検査エラー (spec 4.3): SubTree の BB 不一致、LowerPriority / Both の位置、Parallel の左がタスクでない、子の数の違反、未設定キーの参照・型の不一致、C# タスクを含む (警告。sub-12 で C# タスクが入るまでは枠だけ)。赤枠と下の一覧、一覧をクリックでノードへ移動。検査はモデル層の関数にする。

- (sub-08 VERDICT で追加)
  - Undo の 1 段: ノードのドラッグはマウスを押した時点で 1 回だけ撮り、離した時点で確定。DragInt / DragFloat のパラメータも編集確定 (IsItemDeactivatedAfterEdit) でまとめる。Ctrl+Z / Ctrl+Y は Delete と同じく BT 窓にフォーカスがある間だけ窓が握る (SetKeyOwner)。
  - 未保存の印: Undo / Redo で保存時の内容へ戻ったら印を消す (保存時の JSON と比較、または保存時の版番号)。
  - ReloadHub の `ReloadBehaviorTree` / `ReloadBlackboard`: 読んだ内容が登録済みと同じなら置き換えない (BT 窓の保存の直接登録の直後にファイル監視が同じ内容を読み直し、走っている木が 2 回やり直すのを防ぐ)。比較は正規化した JSON か内容ハッシュ。ReloadHubSelfTest か BehaviorTreeSelfTest で「同じ内容の再読込で木がやり直さない」を確かめる。
  - 手書きの木に残る型違いのキー参照も検査エラーで拾う。
  - BehaviorTreeWindow.cpp の DrawCanvas を入力・描画・メニューに分けてから足す (sub-10 のライブ表示も足しやすくなる)。

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

SELF_EVAL: sub-09 (round 1)
- 実装: モデル層 (`BehaviorTreeEditModel`) に Undo / Redo (128 段、操作前後の状態コピー、ジェスチャ)・BB の作業用コピーと編集 (追加 / 削除 / 改名の追従 / 型 / 初期値 / eventName / 新規作成 / 保存)・検査 `Inspect()` を追加。窓は DrawCanvas を入力 / 描画 / メニューに分け (CanvasView で座標変換を共有)、Undo / Redo ボタンと Ctrl+Z / Ctrl+Y、BB パネル、赤枠と検査一覧を追加。Engine 側は `BehaviorTreeLibrary` / `BlackboardLibrary` の `LoadFromFile(path, &unchanged)` と ReloadHub の「内容が同じなら置き換えない」。
- 検証: Debug / Release ビルド 0 警告、Editor `--selftest` Debug / Release 0 件 (Debug 1 回目に既知 flake の Fracture weight cache 3 件)、Debug Server `--selftest` 0 件、check_rules 0 件、replay_verify 16 ジョブ PASS (最後の変更後に再実行)。画面は一時プローブ + 実マウス / キー入力で確認 (詳細は司会への SELF_EVAL)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。Undo の保持方式 (コピー) の逸脱を承認 (spec 8.)。ReloadHub の同一内容スキップは変異テストと GUI ログで確認、replay_verify 全 PASS。BB 共有時の追従範囲と KeyRequired の定義を承認、限界は sub-14 の ADR へ
