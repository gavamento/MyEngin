# sub-11: ABI v27 — C++ タスク・Blackboard・イベント・AnimatorPlay、C# ミラー

- 依存: sub-10
- 状態: OK (M85k としてコミット、ハッシュは台帳)
- 往復: 1

## やること
- spec 4.2 の ABI v27 を**この 1 回で全部**足す (`EngineAPI.h:13-15` の規則: 末尾追加、既存スロットは変えない)。
  - スロット: `BtGetBlackboard` / `BtSetBlackboard` / `BtSendEvent` / `BtEventCount` / `BtGetEvent` / `AnimatorPlay` / `BtRestart`。数が変わったら SELF_EVAL に書く。POD: `MyeBbValue` / `MyeBtEventPayload` / `MyeBtEvent`。
  - `MyeScriptModule` に `btTaskCount` / `btTasks` を末尾追加、`MyeBtTaskDesc` / `MyeBtTaskContext` (`ScriptTypes.h`)。`REGISTER_BT_TASK` マクロと Registrar (`ScriptAPI.h`、REGISTER_SCRIPT の作り、`ScriptAPI.h:188-215`)。`GameLogicMain.cpp` が返すモジュールに載せる。
  - Engine 側: `ScriptHost.cpp:175-316` で BT タスクの記述子を名前順に取り込み、ホットリロードで名前で引き直す (layoutHash 変化 → そのタスクを含む木をやり直し、DLL から消えた名前 → Failure + 1 回警告)。状態は BT 表のノードのインスタンスごと (spec 4.1.7、BT 節に入る)。フィールドは BT エディタのパラメータ欄 (sub-08 の自動生成) に出し `.bt.json` に名前で保存。
  - `CppTask` ノード。
  - BtSetBlackboard を Update (フェーズ 3) から呼んだ値は同じ tick の BT フェーズで監視に反映される (spec 4.1.1 の順序)。LateUpdate から書いた値は次の tick。
- C# の位置ミラー (`Interop.cs:166-356`)、`check_rules.ps1:676` の版表 (`27 = 151 + n`)、`docs\history\api-scripting-tools.md`、`EngineAPI.h` の版履歴コメント。
- GameLogic にテスト用の C++ タスク (例 `BtProbeTask`、状態に tick 数を数える) を置き、`BehaviorTreeSelfTest` から使う (既存のスクリプトを SelfTest から使う前例に合わせる。無ければ Engine 内でモジュール記述子を手で組んで試す)。

- (sub-10 VERDICT より) `BehaviorTreeComponent::activeNodeId` は SubTree 展開後の実行木の id。ABI / 文書ではそう明記する (元の木の id が要る呼び出し側向けの変換は作らない。必要になったら DisplayedIdOf を ABI に出す)。

## やらないこと (このサブでは)
- C# の BT タスクと糖衣 (sub-12)、デモ (sub-13)

## 触る場所 (planner の見立て)
- `src\Shared\EngineAPI.h`、`src\Shared\ScriptTypes.h`、`src\Shared\ScriptAPI.h`、`src\Engine\Engine\Script\EngineApiTable.cpp`、`ScriptHost.{h,cpp}`、`src\GameLogic\GameLogicMain.cpp`、`src\GameLogic\Scripts\`、`src\Scripting\Interop.cs`、`tools\check_rules.ps1`、`docs\history\api-scripting-tools.md`、`BehaviorTreeSystem.*`、`BehaviorTreeSelfTest.cpp`
- メモ `abi-bump-verification`: Interop.cs は位置ミラーで版検証が無いので順序を機械照合する (check_rules 11-a〜d)

## 受け入れ条件 (このサブ)
1. (spec 13) C++ タスクが動き、その状態が保存 → 復元 → 連続実行で一致、layoutHash 変化でやり直し、消えた名前で Failure — `BehaviorTreeSelfTest`。
2. (spec 13) ABI の 7 スロットが GameLogic から動く (BB 読み書き、イベント送受信と tick N+1 の配送、AnimatorPlay、BtRestart) — `BehaviorTreeSelfTest` か GameLogic のプローブスクリプト + ログ。
3. `check_rules.ps1` 0 件 (版表・ミラー順・引数の数・割り当て)。
4. (spec 16) ビルド 0 警告 (GameLogic と Scripting を含む)、selftest 新規 FAIL 0、replay_verify の既存ジョブ PASS (bump 後の GameLogic で)。

## 検証コマンド
- Debug / Release ビルド (`/p:MyeWarnAsError=true`、`tools\build_scripts.bat` / `tools\build_managed.bat` が要るならそれも)、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)
- ABI v27 = 158 スロット (v26 の 151 + 7)。追加スロット (末尾 append): BtGetBlackboard / BtSetBlackboard / BtSendEvent / BtEventCount / BtGetEvent / AnimatorPlay / BtRestart。POD: MyeBbValue (40 B) / MyeBtEventPayload / MyeBtEvent (48 B)。`MyeScriptModule` 末尾に btTaskCount / btTasks、`MyeBtTaskDesc` / `MyeBtTaskContext` / `MyeBtStatus` を ScriptTypes.h へ。
- C++ タスク: `REGISTER_BT_TASK` / `REGISTER_BT_TASK_NO_FIELDS` (ScriptAPI.h)。登録表 `BtTaskRegistry` (新規 BtTaskRegistry.{h,cpp}) は BehaviorTreeSystem が持ち (`Tasks()`)、ScriptHost が LoadModule / SetBehaviorTree で記述子を渡す (`PublishBtTasks`)。CppTask ノード = `BtNodeKind::CppTask` (params.task = 名前、`BtNodeDef::taskFields` = JSON の "fields")。状態は 1 ノード 128 B (BtCppTaskHeader 16 B + 状態 112 B まで) を BtInstance::extra に固定長で割り当てる (BT 節の形は変わらない)。
- 決まりごと: 入った tick に construct → fields を重ねる → OnStart (無ければ OnTick)、以降は毎 tick OnTick。OnAbort は Abort のときだけ。FIELDS に無いメンバとパディングは呼ぶたびに 0 へ戻す (ハッシュを Debug / Release で一致させるため)。layoutHash / stateSize が入ったときと違う実行中の木は OnAbort を呼ばずに状態を捨てて根からやり直す (警告 1 回)。登録に無い名前は Failure (警告は名前ごとに 1 回、登録表の更新で出し直す)。
- BtSetBlackboard / BtGetBlackboard は Update (C++ タスクのコールバック) の最中でも動く (動いている最中・処理済み・未処理の表を引く `Locate`)。BtRestart は自分の木のタスクの中から呼ぶと、タスクが返った後に Abort して根からやり直す。外からはその場で Abort。
- エディタ: BT 窓の CppTask のタスク名の選択欄とフィールド欄 (登録表の記述子から自動生成)、`BehaviorTreeEditModel::SetTaskField` (Undo に乗る)、文字列 3 本 (en / ja)。
- 検証: Debug / Release ビルド (MyeWarnAsError、sln 全体) 0 警告・0 エラー、build_managed Debug / Release 0 警告、check_rules 0 件 (11-a〜d。Interop.cs の隣接 2 スロット名を入れ替える変異で slot #155 を検出 → 復元)、Editor --selftest Debug / Release とも exit 0・FAIL 0 (Debug 1 回目は既知 flake 5 件 = Fracture weight cache 3 + net V1 2 と、更新漏れの v26 検査 1 件 + 自作テストの期待値の誤り 3 件が出て、修正後 2 回目以降 0)、Debug Server --selftest ALL PASS、replay_verify 全 16 ジョブ PASS (144.7 s)、Editor Release `--parts-demo --autoplay --frames 30` のログに `C++ task registered: BtProbeTask` (EngineLoop 経路の公開)。
- 未検証: BT 窓の CppTask 欄の目視 (画面は撮っていない。モデルの操作は BehaviorTreeEditorSelfTest が検査)、C# レーンの実走 (糖衣が無く呼べないので sub-12)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。ABI v27 = 158、check_rules と変異、実 DLL の BtProbeTask、replay_verify 16 PASS を確認。task を params へ・112B 固定長・FIELDS 以外の 0 戻し・snapshot v39 据え置きを承認。C# 実走と正規化の変異試験は sub-12 へ
