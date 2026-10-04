# sub-08: スクリプト API (ABI bump + C# ミラー)

- 依存: sub-07
- 状態: OK (commit d8ff284)
- 往復: 1

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

SELF_EVAL: sub-08 (round 1)
実装:
  - EngineAPI.h: MYE_API_VERSION 23 -> 24 (131 -> 139 スロット)。NetGetSystemEvent の次に Nav 8 本 (NavSetDestination / NavStop / NavGetAgentState / NavFindPath / NavSamplePosition / NavRaycast / NavFindRandomPoint / NavCompleteLink) と POD 2 つ (MyeNavAgentState / MyeNavRaycastHit)
  - NavSystem.h/.cpp: const のクエリ 4 本 (QueryFindPath / QuerySamplePosition / QueryRaycast / QueryRandomPoint) と CompleteLink、filter 組み立て ResolveQuerySurface。Recast の型は Navigation の中に閉じた
  - EngineApiTable.cpp/.h: 8 スロットの実装、ScriptApiContext.nav。ScriptHost/ManagedHost::SetNavSystem、SimSharedServices.nav を EngineLoop / HeadlessSim から配線 (WireScriptServices)
  - Interop.cs: 構造体 2 つ + スロット 8 本の位置ミラー + Engine 静的窓口。MyeScript.cs: 糖衣 (NavSetDestination / NavStop / NavGetAgentState / NavFindPath / NavSamplePosition / NavRaycast / NavFindRandomPoint / NavCompleteLink、NavAllAreas)。ScriptAPI.h: MyeNav* ラッパー
  - check_rules.ps1: 版表に 24 = 139
  - NavDemoDriver.cpp: tick 120 に Nav クエリ全種を C++ から呼んで登録フィールドへ書き戻し (replay 被覆)、tick 300 に NavSetDestination (同値)
  - NavAgentSelfTest.cpp: 12 / 12b 節 (EngineApi テーブル越しに全スロット: 正常・失敗・RNG 決定論・RNG 非消費・Manual Link 完了)。PartSelfTest.cpp: v24 表明と充填確認
仕様との差分:
  - [追加] NavSamplePosition に areaMask 引数を追加 (sub の署名に無い) — ABI は 1 回しか上げられず、後から足せないため。他のクエリと filter が揃う
  - [追加] NavFindPath に int32_t* outPartial (null 可) — 届かない目的地かどうかが呼び出し側から判別できないため
  - [逸脱] NavFindRandomPoint は dtNavMeshQuery::findRandomPointAroundCircle を使わない。円に触れるポリゴンの任意の点を返し、広い床では半径外の点が出ることを selftest で確認 (40 回中に失敗)。代わりに World の Pcg32 で円の中の点を最大 16 回選び (一様)、水平 0.5 m・垂直 2 m で最寄りポリゴンへ吸着、半径内なら採用。center からの「到達可能性」は見ない (EngineAPI.h に明記)
  - [追加] NavCompleteLink は NavSystem の slot (linkPhase != 0 かつ linkMode == Manual) を見て linkComplete を立てる。渡っていない / Linear・Jump の最中に立てると、フラグが次の Manual の Link に持ち越されて即完了するため
  - [未実装] sub 「やること」5 (plans\m75-ugui.md の ABI 番号の注記) — 申し送りが sub-09 で行うとしているので触っていない
  - [追加] クエリは NavSystem::Update (スクリプトの後) より前の状態を見る。シーンを読んだ最初の tick は Surface が未読み込みで全クエリが 0 (selftest で確認、EngineAPI.h に明記)
検証:
  - Debug / Release MyEngine.sln ビルド (MSBuild) → 警告 0 エラー 0 (vendor 含む)。tools\build_managed.bat Debug / Release → 警告 0 エラー 0
  - Editor --selftest Debug / Release → 新 FAIL なし。NavAgent self test ALL PASS ((API) / (API Link) の全項目)、NavDeterminism PASS。FAIL は既存の Source control 2 件のみ (external cherry-pick / external revert)。PartSelfTest の v23 表明は v24 へ更新して PASS
  - tools\check_rules.ps1 → 0 error 0 warning。変異テスト (Interop.cs の NavGetAgentState と NavRaycast の名前を入れ替え) → "[rule 11] slot #134 name differs" で検出、復元済み
  - tools\replay_verify.bat → 全 15 job PASS ([PASS] replay consistency ... nav)
  - C# 実走: 一時プローブ (assets\scripts\NavProbe.cs を --nav-demo の先頭 Agent に付けて Editor --nav-demo --autoplay --frames 200) → 全 8 スロットが期待どおり返った (path corners=4 partial=False、sample y=0.11、ray hit=1 point=(0.2,-7.97)、random p=(-11.22,0.05,4.81) は半径 3 内、state Moving -> setDest 後 remaining 15.8 -> 4.2、NavStop 後 status 0=Idle、NavCompleteLink は Link 外で false)。実走後にプローブと DemoContent の一時編集を削除
  - 未実行: NavDemoDriver の tick 120 で書き戻した登録フィールドの値そのものは目視していない (replay 一致のみ)
自己採点 (1-5):
  仕様適合: 4 — 8 関数と版・ミラー・版表は揃った。areaMask / outPartial の追加と RandomPoint の方式変更を差分に書いた。m75-ugui.md の注記は申し送りどおり未実施
  正しさ: 4 — selftest の (API) 全項目 + C# 実走 + replay 全 PASS。Debug/Release 両方で selftest 同じ結果。RandomPoint の一様性と大量・多 Surface は未検証
  コード品質: 4 — Recast 型は Navigation に閉じ、Shared は POD のみ。RandomPoint は 16 回で打ち切る単純な方式
  テスト: 4 — テーブル越しの正常・失敗・RNG・Manual Link を網羅し replay 被覆の C++ 呼び出しもある。C# レーンは一時プローブのみ (恒久テストなし)
不安・質問:
  - NavFindRandomPoint の逸脱 (Detour の関数を使わない) を planner が許容するか。半径内の保証を優先した
  - 外部プロジェクト (C:\HAL\三校、C:\HAL\GameEngin_Demo) の GameLogic.dll は ABI v24 でロード拒否される (ScriptHost.cpp:191 の版不一致)。再ビルドが要る。外部には一切書いていない
触ったファイル: src\Shared\EngineAPI.h, src\Shared\ScriptAPI.h, src\Engine\Engine\Navigation\NavSystem.h, src\Engine\Engine\Navigation\NavSystem.cpp, src\Engine\Engine\Navigation\NavAgentSelfTest.cpp, src\Engine\Engine\Script\EngineApiTable.h, src\Engine\Engine\Script\EngineApiTable.cpp, src\Engine\Engine\Script\ScriptHost.h, src\Engine\Engine\Script\ManagedHost.h, src\Engine\Engine\Loop\SimInit.h, src\Engine\Engine\Loop\SimInit.cpp, src\Engine\Engine\Loop\EngineLoop.cpp, src\Engine\Engine\Loop\HeadlessSim.cpp, src\Scripting\Interop.cs, src\Scripting\MyeScript.cs, src\GameLogic\Scripts\NavDemoDriver.cpp, src\Editor\SelfTest\PartSelfTest.cpp, tools\check_rules.ps1
申し送り:
  - sub-09: plans\m75-ugui.md の ABI 注記 (v24 は M82 が使った、M75h は v25 = 139 スロットの次)、engine_spec.md 3064 行付近の ABI v23 節に v24 を追記、docs\engine-feature-guide.md の ABI 説明 (現在 v18 のまま古い) の更新
  - NavSystem のクエリは surfaces_ (前 tick の Update で確定) を見る。スクリプトが先に走る tick の頭では、同 tick の Obstacle 変更はまだ反映されない

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。areaMask と outPartial の追加、NavCompleteLink、RandomPoint の方式を採用 (spec 4.2)。m75-ugui.md の注記は sub-09 で行う (台帳の申し送りどおり)。nit: C# レーンには恒久テストが無い (一時プローブで実走を確認しただけ。ABI bump の前例どおり)。
