# sub-12: C# タスクと C# の糖衣 (決定論の保証外)

- 依存: sub-11
- 状態: OK (M85l としてコミット、ハッシュは台帳)
- 往復: 1

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

### round 1 (SELF_EVAL の要点)
- ネイティブ: `BtManagedTaskLane` (新規 `BtManagedTaskLane.h`、`RunTask(owner, nodeIndex, className, phase, tick)`、phase = enter / tick / abort、戻り値 0/1/2 + 「クラス無し」-1)。`ManagedHost` が実装し、`MyeManagedVTable` の末尾に `BtTask` を足した。`BehaviorTreeSystem::SetManagedLane` を TickRunner が C++ スクリプトの Update より前に毎 tick 設定 (`runManaged ? &managedHost : nullptr`)。`BtNodeKind::CsTask` (params.class)、`VisitCsTask` / `ReleaseCsTask`。レーン無し・クラス名空・クラス無しは Failure + 警告 (名前ごとに 1 回、レーン停止は全体で 1 回)。BT 表へ入れる状態は無い (追加状態 0 バイト、active の印だけ)。
- C#: `MyeBtTask` (OnStart 既定 = OnTick / OnTick / OnAbort / Self / Tick / Log)、`[BtTask]`、`MyeBtStatus`、`MyeReceivedEvent`。糖衣は `MyeEntity` (public) に置き、`MyeScript` は Self への protected の一行転送。`Interop.cs` に 7 スロットのラッパ。`ScriptRuntime` に `RunBtTask` (インスタンスは (index, generation, nodeIndex) キー、Success / Failure / 例外で破棄、リロードとシーン遷移で破棄、tick のインスタンスが無ければ OnStart からやり直す、enter のたびに死んだエンティティの分を掃除)。
- エディタ: 検査の `CSharpTask` (CsTask ノードと、C# タスクを含む木を取り込む SubTree に警告)、`BtAssetUsesCsTask` (SubTree 経由を含む)、Inspector の BehaviorTree 欄の警告、文字列 `Insp_BtCSharp` (en / ja)。
- テスト: `BehaviorTreeSelfTest` に CsTask 8 項目 (代役レーン) + パディング状態の項目。BtTaskCanonicalizeState を外す変異で「状態のパディング」と既存の「FIELDS に無いメンバ」の 2 項目が FAIL することを確かめ、戻した。
- 一時プローブ (Runtime.exe、Debug / Release、削除済み): C# スクリプト + `ZzChase` タスクの木。ログ: `OnStart tick=24` → `OnTick n=1..3 bb.counter=1..3` → `Success` → 全体宛てイベント受信 (done=True broadcast=True value=1.5 int=42)。7 スロット: BtSetBlackboard / BtGetBlackboard (Bool / Vector / Entity の往復と型違いの Set が false)、BtSendEvent + BtEventCount / BtGetEvent (自分宛て direct を翌 tick に受信)、AnimatorPlay (Animator が無いので false = 呼び出しは届く)、BtRestart (CsTask 実行中に呼ぶと `OnAbort` → 次の tick に OnStart から)。記録 (`--replay-record`) は `[WARN] C# task 'ZzChase': the C# lane is not running ...` が 1 回だけ、`--replay-verify` も PASS (600 tick)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。C# 7 スロットの実走、記録中の Failure + 警告 1 回と verify PASS、正規化の変異試験 (2 項目 FAIL) を確認。fields とピッカーは後回し (spec 2. #21)
