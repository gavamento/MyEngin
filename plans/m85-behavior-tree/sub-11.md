# sub-11: ABI v27 — C++ タスク・Blackboard・イベント・AnimatorPlay、C# ミラー

- 依存: sub-10
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
