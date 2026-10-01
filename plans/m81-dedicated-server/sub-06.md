# sub-06: ABI v23 (レーン状態・playerId・システムイベント)

- 依存: sub-02 (sub-03〜05 と並列可。sub-05 の後に着手するなら C4 も課す)
- 状態: 未着手
- 往復: 0

## やること
spec 4.2 の ABI v23 と D3 / D14。

1. `src\Shared\EngineAPI.h`: `MYE_API_VERSION 23`、末尾に 5 スロット (`NetLaneMask` / `NetLaneState` / `NetLanePlayerId` / `NetSystemEventCount` / `NetGetSystemEvent`)、POD `MyeNetSystemEvent { uint64_t eventSeq; uint64_t playerId; uint32_t kind; uint32_t lane; }`。版の履歴コメントに v23 を足す。
   - コメントで **v13 の Net* (表示専用・機種依存) と v23 (確定入力から導く sim 値・sim から読んでよい)** の違いを明記。
   - `NetIsServer` / `NetIsClient` は**足さない** (spec D3)。
2. `EngineApiTable.cpp`: 実装は `SessionLanes` (sub-02) を読むだけ。非サーバ構成の既定値は spec 4.2。`NetGetSystemEvent` の範囲外は 0 を返し out を 0 埋め。
3. `src\Shared\ScriptAPI.h` の糖衣 (`MyeNetLaneState(ctx, lane)` 等)、`src\Scripting\Interop.cs` の位置ミラー (と必要なら MyeScript.cs の糖衣)。
4. check_rules 11-c の表 (v23 = 131)。ABI bump の検証レシピ (メモリ「ABI bump の検証レシピ」) に従い Interop.cs の順序を機械照合する。
5. GameLogic に検証用スクリプト 1 本 (既存の検証用スクリプトの置き場・命名に合わせる): 毎 tick システムイベントを読み、参加/離脱の回数と最後の eventSeq を自分のコンポーネント (ハッシュ対象) に書く。
6. selftest: (a) 非サーバ構成の既定値、(b) sub-04 のハーネスで、検証用スクリプトを載せたサーバ/クライアントのハッシュが一致 (= 全員が同じ tick に同じイベントを読んだ)。sub-04 が未完なら (b) は SessionLanes を直接駆動する単体テストで代え、sub-05 以降の server_verify で被覆されることを SELF_EVAL に書く。

## やらないこと (このサブでは)
- 役割 (Server/Client) のスロット (D3)
- C# からのネット利用 (C# レーンはネット中は止まる。ミラーは ABI の位置整合のためだけ)

## 触る場所 (planner の見立て)
- `src\Shared\EngineAPI.h`、`src\Shared\ScriptAPI.h`、`src\Scripting\Interop.cs` (`MyeScript.cs`)
- `src\Engine\Engine\Script\EngineApiTable.cpp` (実際のファイル名は coder が確認)
- `tools\check_rules.ps1` (11-c の表)
- `src\GameLogic\` の検証用スクリプト

## 受け入れ条件 (このサブ)
spec 5. の **A1, A2** と **C1, C2, C3, C5, C6** (sub-05 完了後なら C4)。

## 検証コマンド
- `pwsh -File tools\check_rules.ps1` (規則 11 の PASS 行)
- `tools\build_scripts.bat` / `tools\build_managed.bat` (GameLogic と C# の再ビルドが要る場合)
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
