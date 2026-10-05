# sub-12: C# タスクと C# の糖衣 (決定論の保証外)

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## やること
- `MyeScript.cs` に糖衣: `GetBlackboard*` / `SetBlackboard*` / `SendEvent` / `ReceivedEvents` (BtEventCount / BtGetEvent) / `PlayAnimation` (AnimatorPlay) / `RestartBehaviorTree`。
- C# の BT タスク: `[BtTask]` 属性の基底クラス (`OnStart` / `OnTick` / `OnAbort`、戻り値 Running / Success / Failure)。ManagedHost にタスクの一覧と呼び出し口を足す (`ManagedHost.cpp:340-390` の vtable の流儀)。`.bt.json` のノード `CsTask` (クラス名は CppTask の task と同じく `params.class` の String パラメータ。sub-11 VERDICT)。
- C# レーンが止まる場面 (`TickRunner.cpp:333-341` の runManaged が偽: 記録・検証・Net・再シム) では CsTask は即 Failure + 1 回警告 (spec 2. #8)。BT 窓の検査に「C# タスクを含む = 決定論の保証外」の警告 (sub-09 の枠を埋める)、Inspector の BehaviorTreeComponent 欄にも同じ警告。
- C# タスクのインスタンスは managed 側に置く (World の外)。巻き戻し後のリセットは既存の C# スクリプトと同じ扱い (`SimSnapshot.h:39-46`)。

- (sub-11 VERDICT より) C# の位置ミラー (sub-11) の実走確認もこのサブの一時プローブで行う (7 スロットを C# から 1 回ずつ呼ぶ)。
- (sub-11 VERDICT より) C++ タスクの状態の正規化 (`BtTaskCanonicalizeState`) を一時的に外すと、パディングを持つ状態型のテストが FAIL することを 1 回確かめる変異試験 (テストが無ければ 1 項目足す)。

## やらないこと (このサブでは)
- C# タスクの状態の決定論化 (保証外のまま)

## 触る場所 (planner の見立て)
- `src\Scripting\MyeScript.cs`、`src\Scripting\` の Bootstrap / vtable、`src\Engine\Engine\Script\ManagedHost.{h,cpp}`、`BehaviorTreeSystem.cpp`、`BehaviorTreeWindow.*`、`InspectorWindow.cpp`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 14) 一時プローブ (C# スクリプト + C# タスクの木) で Play 中に C# タスクが Running → Success を返し、BB の読み書きとイベントの受信が動く — ログの写しを SELF_EVAL に (恒久テストは無し。プローブは消す)。
2. (spec 14) 記録中 (`--replay-record`) は CsTask が Failure になり警告が 1 回 — 同じプローブのログ。
3. BT 窓・Inspector の警告の画像。
4. (spec 16) ビルド 0 警告 (managed を含む)、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、`tools\build_managed.bat`、Editor `--selftest` 両構成、`tools\check_rules.ps1`、一時プローブの実行

## 実装メモ (coder が追記)

## フィードバック履歴
