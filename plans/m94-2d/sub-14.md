# sub-14: ABI (10 本) + C# ミラー + MyeScript

- 依存: sub-08, sub-11
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (特に 4.1 の ABI 規約)。spec: 2. #18、4.1.10、7. R5

## やること
1. **版とスロット数は着手時の `EngineAPI.h` から取る** (今 v30 = 178。M75h が先に v31 を使っていれば v32)。`MyeEngineApi` 末尾に `// ---- vNN (M94n): 2D ----` 節で spec 4.1.10 の 10 本をこの順で追加。`EngineAPI.h:15-55` に版コメント。
2. `EngineApiTable.cpp`: `FillTwoDSlots(out, ctx)` を `FillComputeSlots` の隣に置き、全スロットを `out.<Name> =` で充填。Tilemap 系は `ScriptApiContext` から `TilemapSystem` に届く経路 (TickServices 経由。無ければ 0 / -1 を返す)。`SpriteByName` は `SpriteLibrary::FindByName`。`OverlapBoxMasked` / `BoxCast` は sub-08 の C++ 関数。`MyeRaycastHit` を流用、新しい POD は `static_assert(is_trivially_copyable)`。
3. `src\Scripting\Interop.cs`: `MyeEngineApi` 末尾に同じ順で `delegate* unmanaged<...>`、`Engine` ラッパ。`MyeScript.cs`: `Tilemap.GetCell(...)` 等の糖衣 (static クラス `Tilemap2D` と `Physics2D` 相当の名前。既存の命名規約に合わせる)。`ScriptAPI.h`: C++ 糖衣 `MyeTilemapGetCell(ctx, ...)` 等。
4. `check_rules.ps1:743` の `$apiVersionSlots` に `NN = スロット数`。`PartSelfTest.cpp:178` の版固定と非 null 確認を更新。`docs\history\api-scripting-tools.md` と `engine_spec.md:3475` 付近の Scripting 節に 1 段落。
6. (spec R12 (b)、sub-11 の申し送り) `PhysicsQueries.cpp` の `CollectTargets` / `RaycastWorld` / `OverlapSphereWorld` / `OverlapBoxWorld` / `SphereCastWorld` / `BoxCastWorld` が、PhysicsSystem に登録された `StaticShapeSource` の `Collect` (絞り込み無しの全体) をコライダーの収集に足す (owner = Tilemap エンティティ、layer / mask は供給元の col)。selftest: タイル床へ `RaycastWorld` が当たり owner が Tilemap、`OverlapBoxMasked` がマスクで除外、ABI の `Raycast` / `OverlapBoxMasked` でも同じ。
5. selftest: `TwoDAbiSelfTest` (新規、`ComputeAbiSelfTest` の流儀): 10 スロットが非 null、`TilemapSetCell` → `TilemapGetCell`、座標変換の往復、`OverlapBoxMasked` のマスク、`BoxCast` のヒット。C# 側は `tools\build_managed.bat` でビルドが通ること + temp の C# スクリプト 1 本で `Tilemap2D.SetCell` → 次 tick で箔が落ちる (ログを SELF_EVAL に。`abi-bump-verification` の「C# レーンは replay 被覆外なので temp プローブで実走確認」)。

## やらないこと (このサブでは)
- 2 回目の bump。これ以降のサブで ABI を触る必要が出たら「仕様との差分」に出して planner が判断する

## 触る場所 (planner の見立て)
- `src\Shared\EngineAPI.h` (:56 版、:283-1036 テーブル)、`src\Shared\ScriptAPI.h`
- `src\Engine\Engine\Script\EngineApiTable.cpp` (:104 FillComputeSlots、:327 BuildEngineApi、:1792)
- `src\Scripting\Interop.cs` (:267-487, :531-)、`src\Scripting\MyeScript.cs`
- `tools\check_rules.ps1:743`、`src\Editor\SelfTest\PartSelfTest.cpp:178`
- 新規 `src\Engine\Engine\Script\TwoDAbiSelfTest.{h,cpp}`、`EditorMain.cpp`
- `engine_spec.md`、`docs\history\api-scripting-tools.md`
- 参考: コミット `1561ac3` (M93c、v30) の 13 ファイル

## 受け入れ条件 (このサブ)
1. (spec 26) `check_rules.ps1` 規則 11-a〜d が 0、`PartSelfTest` / `TwoDAbiSelfTest` PASS、`tools\build_managed.bat` 成功、C# temp プローブのログ。
2. GameLogic.dll の既存スクリプト (`src\GameLogic\Scripts\*.cpp`) がリビルドだけで動く (`--demo` を `--replay-record` → `--hash-diff` で 1 本。replay_verify 全体は sub-15)。
3. (spec 1-3) 0 警告、check_rules 0 (規則 11)、`MYE_SELFTEST_ONLY=m94` に `TwoDAbiSelfTest` / `PartSelfTest` を足して PASS、ABI +1 のみ (snapshot は触らない)。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`tools\build_managed.bat`、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- `Runtime.exe --demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-15)。

## 実装メモ (coder が追記)

## フィードバック履歴
