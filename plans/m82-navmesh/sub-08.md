# sub-08: スクリプト API (ABI bump + C# ミラー)

- 依存: sub-07
- 状態: 未着手
- 往復: 0

## やること
spec 2. #14、4.2 (ABI)。

1. 着手時に `src\Shared\EngineAPI.h` の `MYE_API_VERSION` を確認し (M82 起票時 v23 = 131)、次の番号へ 1 回だけ bump。
2. 関数 (最小集合。増減は「仕様との差分」へ): `NavSetDestination(entity, pos)` / `NavStop(entity)` / `NavGetAgentState(entity, out)` (status / remainingDistance / pathPartial / velocity) / `NavFindPath(agentTypeId, from, to, areaMask, outCorners, maxCorners)` / `NavSamplePosition(agentTypeId, pos, extents, out)` / `NavRaycast(agentTypeId, from, to, areaMask, out)` / `NavFindRandomPoint(agentTypeId, center, radius, areaMask, out)` (World RNG) / `NavCompleteLink(entity)` (Manual の完了通知)。POD のみ。
3. `EngineApiTable.cpp` の実装、`src\Scripting\Interop.cs` の位置ミラー、C# 側の糖衣 (MyeScript、Engine は internal)、`check_rules.ps1` の版表。
4. 呼べることの確認: GameLogic の C++ から 1 回 (replay 被覆内) + C# は temp プローブで実走 (C# レーンは replay 被覆外。メモリの「ABI bump の検証レシピ」= Interop.cs の順序を機械照合)。
5. `plans\m75-ugui.md` に「ABI vNN は M82 が使った、M75h は次番号」を注記。

## やらないこと (このサブでは)
- BT ノード (M-C)

## 触る場所 (planner の見立て)
- `src\Shared\EngineAPI.h`、`src\Engine\Engine\Script\EngineApiTable.cpp`、`src\Scripting\Interop.cs` と C# 糖衣、`tools\check_rules.ps1` (rule 11 の表)、`plans\m75-ugui.md`

## 受け入れ条件 (このサブ)
1. (spec 14) `check_rules.ps1` 0 (版と slot 数一致)。C++ と C# から呼んで期待値 (固定シーンの経路コーナー数など) が返る。
2. replay_verify 全 PASS、selftest に新 FAIL なし、0 警告。

## 検証コマンド
- 両構成ビルド、`tools\build_managed.bat Debug` / `Release`、`--selftest` 両構成、`tools\check_rules.ps1`、`tools\replay_verify.bat`、C# temp プローブ (実行後に消す)

## 実装メモ (coder が追記)

## フィードバック履歴
