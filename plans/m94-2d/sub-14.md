# sub-14: ABI (10 本) + C# ミラー + MyeScript

- 依存: sub-08, sub-11
- 状態: OK (commit 4fe503f)
- 往復: 1
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

SELF_EVAL: sub-14 (round 1)
実装:
  - src\Shared\EngineAPI.h — MYE_API_VERSION 30 → 31 (版コメント)。MyeEngineApi 末尾に spec 4.1.10 の 10 本を順序どおり (OverlapBoxMasked / BoxCast / SpriteByName / TilemapGetCell / SetCell / FillRect / DamageCell / WorldToCell / CellToWorld / GetBounds)。docstring に BoxCast の 27 点近似の限界、セルのビット配置、失敗値を記載
  - src\Engine\Engine\Script\EngineApiTable.cpp:FillTwoDSlots — 10 スロット全部を `out.<Name> =` で充填 (BuildEngineApi の FillComputeSlots の次)。タイルは ScriptApiContext.tilemap の TilemapSystem に 1 対 1、Tilemap 無し / layer 範囲外 / 座標範囲外は失敗値 (Get 0 / Set 0 / Fill 0 / Damage -1 / W2C・C2W・Bounds 0)。OverlapBoxMasked = OverlapBoxWorld (無回転 + mask)、BoxCast = BoxCastWorld (sub-08)、SpriteByName = SpriteLibrary::FindIdByName
  - src\Engine\Engine\Script\EngineApiTable.h / ScriptHost.h / ManagedHost.h / src\Engine\Engine\Loop\SimInit.{h,cpp} / EngineLoop.cpp / HeadlessSim.cpp — ScriptApiContext に `tilemap` と `assetsRoot` を追加。`SetTilemapService(tilemap, assetsRoot)` を両ホストに足し、SimSharedServices (tilemap / assetsRoot) → WireScriptServices で配線 (SetNavSystem と同じ流儀)
  - src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}:GetLayerBounds — layer の空でない実効セル (タイル id != 0、override 込み) の外接矩形。GetBounds ABI の実体
  - src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.{h,cpp}:FindIdByName — FindByName と同じ検索で AssetID を返す (FindByName はこれを経由)
  - やること 6: src\Engine\Engine\Physics\Rigid\PhysicsSystem.h:physicsquery::Install / CollectStaticShapes + PhysicsSystem::StaticShapeSources() (登録順の読み出し)。PhysicsQueries.cpp の CollectTargets (Overlap 系 / SphereCast / BoxCast 共通) と PhysicsSystem.cpp の RaycastWorld が、供給口の Collect 全体 (絞り込み無し) を mask (col->layer) で絞って通常のコライダーと同じ targets に足す (owner = Tilemap エンティティ、skip も効く)。注入は SimInit の InstallSimLibraries (SimLibraries.physics) / UninstallSimLibraries — EngineLoop と HeadlessSim (Activate 含む) が自分の physicsSystem を渡す
  - src\Scripting\Interop.cs — MyeEngineApi 末尾へ同じ順の delegate* unmanaged 10 本 + Engine 静的ラッパ。src\Scripting\MyeScript.cs — protected static の糖衣 (OverlapBoxMasked / BoxCast / SpriteByName / TilemapGetCell ... TilemapGetBounds)。src\Shared\ScriptAPI.h — C++ 糖衣 MyeOverlapBoxMasked / MyeBoxCast / MyeSpriteByName / MyeTilemap*
  - tools\check_rules.ps1 — $apiVersionSlots に 31 = 188。src\Editor\SelfTest\PartSelfTest.cpp — 版固定を v31 へ + 10 スロットの非 null 確認
  - src\Engine\Engine\Script\TwoDAbiSelfTest.{h,cpp} (新規) + EditorMain.cpp (m94 suite と全体 suite に登録、m94 に PartSelfTest も追加) — 52 項目
  - engine_spec.md (ABI v31 段落、M75h は v32)、docs\history\api-scripting-tools.md (v31 の 1 項)
仕様との差分:
  - [逸脱] TilemapCellToWorld の戻りを `Vec3` 値返しでなく `int (*)(engine, e, cx, cy, MyeVec3* out)` (1 = 成功) にした — 既存テーブルに構造体の値返しが 1 本も無く (ベクトル結果は GetVelocity 等すべて out ポインタ)、C# の delegate* unmanaged での値返しは未検証の経路になるため。失敗 (Tilemap 無し) を戻り値で区別できる利点もある。spec 4.1.10 の表記は `→ Vec3`
  - [追加] 空間クエリが供給口を見るための注入口 `physicsquery::Install` (SimLibraries.physics 経由)。クエリが World しか取らないため、PhysicsSystem への経路が他に無い (引数を 5 関数 × 全呼び元に足す案は採らなかった)。sub の見立てにない SimInit.{h,cpp} / EngineLoop.cpp / HeadlessSim.cpp を触った
  - [追加] ScriptApiContext.assetsRoot (SpriteByName が assets 相対のパスを絶対化する根。PlayEffect のプレハブキーと同じ規則: ':' を含まなければ根を前置)。spec の `SpriteByName(texturePath, spriteName)` の path 解釈を決めた
  - [追加] TilemapSystem::GetLayerBounds と SpriteLibrary::FindIdByName (どちらも ABI の実体に必要な小関数)
  - [追加] GetBounds の定義: 「タイル id != 0 のセルの外接矩形 (両端を含む)」。spec は意味を定めていない。Camera2DFollow.boundsFromTilemap (sub-22) が衝突レイヤーの AABB を別に求めるなら、そちらは別 API
  - [追加] FillRect の戻りは書いたセル数 (|x1-x0|+1)*(|y1-y0|+1)。失敗は 0。TilemapDamageCell の -1 は Tilemap 無し / layer 範囲外 / 座標範囲外 (TilemapSystem の kNone は空・壊れない・amount<=0 と対象外を区別しないので ABI 側で判定)
  - [追加] FillTwoDSlots のシグネチャは `(out)` (sub は `(out, ctx)`) — ラムダは engine ポインタから ctx を得るので FillComputeSlots と同じ形にした
  - 版の確認: 着手時の MYE_API_VERSION は 30 (M75h 未着手) で v31 = 178 → 188 を使用。kSimSnapshotVersion (57) / kDocVersion / kCookVersion は触っていない。hash 対象フィールドの append も無い (焼いた WorldHash 定数の再採取は不要。PhysicsSelfTest / Tilemap suite の既存の焼いた値が両構成で PASS)
検証:
  - Debug / Release `/p:MyeWarnAsError=true` 0 警告 → 通った (Debug: build_debug6、Release: build_release6。どちらも SelfTestUnused.kernel.hlsl の C4100 の既存癖で最初の数回は落ち、非 WX ビルド → WX を重ねて通した。ログ cache\m94\s14\build_*.log。私の変更起因の警告・エラーは 0)
  - `tools\build_managed.bat Debug` → 成功 (0 警告 0 エラー)。Release 構成の build_managed は未実行 (Interop.cs / MyeScript.cs の変更は Debug と同一ソースで、sln の Release ビルドが GameLogic まで通っている)
  - `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり。規則 11 は 0)。変異テスト: Interop.cs の TilemapWorldToCell / TilemapCellToWorld の名前を入れ替え → `ERROR [rule 11] slot #186 name differs` を検出 → 復元して 0 を確認 (cmp で元と同一)
  - `MYE_SELFTEST_ONLY=m94 Editor.exe --selftest` Debug → exit 0 (250 秒)、Release → exit 0 (24 秒)。TwoDAbiSelfTest は両構成 ALL PASS (52 項目: 10 スロットの充填、GetCell / GetBounds / W2C / C2W の往復 (原点 (10,20,0))、Raycast / ABI Raycast / RaycastMasked がタイルの床と木箱に当たり owner = Tilemap、通常の球と同じプールで順序が index 昇順、mask でタイル層を除外、OverlapBoxMasked のマスク除外・複数の箱を持ち主 1 回で数える・null out は数えるだけ、BoxCast 距離 3.75 / 2.75 / 始点重なり 0 / ミス 3 種、SphereCast / OverlapSphere、Install(nullptr) の対照で見えなくなる、DamageCell 0/1/2/-1、破壊後の瓦礫が衝突しない、SetCell / FillRect の戻りと拒否、書き戻しで override が空になる、tilemap = nullptr の文脈の失敗値、SpriteByName の相対 / 絶対 / 不明名 / 欠けたテクスチャ / null / 未接続)。PartSelfTest の v31 固定と充填確認も PASS。m94 に入っている PhysicsSelfTest (焼いた値) / Tilemap suite も両構成 PASS
  - Tilemap の無いシーンで結果が変わらないこと: `Runtime.exe --demo --replay-record --replay-ticks 600 --hash-dump (tick 599)` (Release / WARP) → `Editor.exe --hash-diff cache\m94\s11\after_demo.dump cache\m94\s14\after_demo.dump` → identical (7596 行、total 6834D7A80D7B38D8 = sub-11 時点の値)。同じ .rep を `--replay-verify` → 600 tick hash-identical PASS。GameLogic.dll は再ビルドだけで読み込まれた (`[dll] script registered` 5 本)
  - C# temp プローブ (Runtime.exe Debug、temp シーン + temp .tilemap/.tilepalette + assets\scripts\S14Probe.cs、すべて削除済み。ログ cache\m94\s14\probe_csharp2.log): `Tilemap` の床の上に球 (plane2D) → t60 ballY=1.500 / TilemapGetCell(0,0)=1 / BoxCast ヒット dist=2.7505 (球の上面) / WorldToCell(2.5,0.5)=(2,0) / CellToWorld(2,0)=(2.5,0.5) / GetBounds=(-16,0)-(15,0) / SpriteByName の未知パスと空文字 = 0。t90 に `TilemapFillRect(-1..1, 0, 0)` = 3 → 次 tick から球が落下 (t91 y=1.498 → t120 y=0.263 → t180 y=-9.567)。TilemapDamageCell(floor) = 0。→ C# の delegate* unmanaged 経由で書き込み・読み出し・クエリが実走した
  - 未実行: replay_verify 全体 / 全体 selftest / shot_verify (チェックポイント sub-15 に回す)。ABI v31 での Release の build_managed。実エディタ上での操作確認 (該当 UI なし)
自己採点 (1-5):
  仕様適合: 4 — 10 スロット・C# ミラー・糖衣・文書・やること 6 は仕様どおりで検証済み。CellToWorld の値返し → out ポインタの逸脱と注入口の追加が差分として残る
  正しさ: 4 — 両構成の selftest 54 項目と C# 実走、demo の hash-diff identical で確認。クエリ側のコストと、Perception / FootIk / Acoustic が RaycastWorld 経由でタイルを見るようになった副作用 (タイルのあるシーンのみ) は未計測
  コード品質: 4 — 既存の SetNavSystem / FillComputeSlots / モジュール注入の流儀に揃えた。FillTwoDSlots が 130 行でやや長い
  テスト: 4 — 新規 suite は主要経路と失敗値を網羅。Tilemap のあるシーンの replay / 実編集経路 (Editor の Play 中) は sub-23 のデモ待ち
不安・質問:
  - クエリの供給口は PhysicsSystem 全体ではなくモジュール注入 (physicsquery::Install) にした。planner が「クエリ関数へ引数で渡す」ほうを望むなら差し替え可 (5 関数 + FootIk / AcousticField の呼び元に引数が増える)
  - 供給口の Collect は絞り込み無しで毎クエリ呼ぶ (チャンクの箱はキャッシュされるが、箱のベクトルは毎回組み直す)。大きな Tilemap で 1 tick に Raycast を数十本撃つゲームでは重くなりうる。計測は sub-22 / sub-23 のデモで (今は未計測)。重ければ Collect の結果を tick 単位でキャッシュする手がある
  - AcousticField (床への Raycast) と FootIk の接地レイも RaycastWorld 経由でタイルの箱に当たるようになった (Tilemap のあるシーンだけ。無いシーンは hash-diff identical)。意図どおりか
  - GetBounds を「非空セルの外接矩形」と決めたが、Camera2DFollow の境界 (sub-22) が衝突レイヤーの AABB を求めるなら別の口が要る
  - TilemapCellToWorld を out ポインタにした逸脱 (上記) を了承してほしい。値返しに戻すなら EngineAPI.h / Interop.cs / ScriptAPI.h / EngineApiTable.cpp の 4 箇所だけで、スロット数は変わらない
触ったファイル:
  - C:\HAL\MyEngin\src\Shared\EngineAPI.h
  - C:\HAL\MyEngin\src\Shared\ScriptAPI.h
  - C:\HAL\MyEngin\src\Scripting\Interop.cs
  - C:\HAL\MyEngin\src\Scripting\MyeScript.cs
  - C:\HAL\MyEngin\src\Engine\Engine\Script\EngineApiTable.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Script\EngineApiTable.h
  - C:\HAL\MyEngin\src\Engine\Engine\Script\ScriptHost.h
  - C:\HAL\MyEngin\src\Engine\Engine\Script\ManagedHost.h
  - C:\HAL\MyEngin\src\Engine\Engine\Script\TwoDAbiSelfTest.h (新規)
  - C:\HAL\MyEngin\src\Engine\Engine\Script\TwoDAbiSelfTest.cpp (新規)
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\SimInit.h
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\SimInit.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Loop\HeadlessSim.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSystem.h
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsQueries.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapSystem.h
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.h
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.cpp
  - C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp
  - C:\HAL\MyEngin\src\Editor\SelfTest\PartSelfTest.cpp
  - C:\HAL\MyEngin\tools\check_rules.ps1
  - C:\HAL\MyEngin\engine_spec.md
  - C:\HAL\MyEngin\docs\history\api-scripting-tools.md
  - C:\HAL\MyEngin\plans\m94-2d\sub-14.md (実装メモ)
  - 生成物: C:\HAL\MyEngin\build\Engine.vcxproj / C:\HAL\MyEngin\build\Engine.vcxproj.filters (tools\gen_project_files.ps1 の出力。TwoDAbiSelfTest の追加)
申し送り:
  - (司会) メモリの M75h 予約を更新: M75h は v32 以降 (v31 は M94n/sub-14 が使用、178 → 188 スロット)。engine_spec.md は反映済み
  - (sub-15 チェックポイント) PhysicsQueries を触ったので全体 selftest / replay_verify の個別 PASS 行を確認。今回の確認範囲は m94 suite 両構成 + demo 600 tick の hash-diff identical まで
  - (sub-21) Perception の LOS (CollectLosColliders) は `physicsquery::CollectStaticShapes(world, mask, out)` を呼べば同じ口でタイルを見られる (mask は col->layer で絞り済み)
  - (sub-22 / 23) タイルの供給口 Collect を毎クエリ呼ぶコストは未計測。デモの Raycast 本数が多ければ計測する
  - (sub-24) engine_spec の限界欄: BoxCast は 27 点の近似 (突端の取りこぼし)、クエリは絞り込み無しの全箱、SpriteByName の初回は .meta を読む、GetBounds の定義、CellToWorld は out ポインタ
  - C# からの SpriteByName は未知パス / 空文字の 0 しか実走していない (実在スライスの C++ 側の往復は TwoDAbiSelfTest で確認)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-21)。v31 = 188、規則 11 が 0 (変異テストで検出も確認)、TwoDAbiSelfTest 52 項目両構成 PASS、C# プローブで書き込み → 次 tick で球が落ちる実走、`--demo` の hash-diff identical。CellToWorld の out ポインタ化・`physicsquery::Install`・assetsRoot・GetBounds の定義は採用 (spec 4.1.10)。Raycast の既存消費者がタイルに当たるのは意図どおり。コストは sub-23 やること 8、Camera2DFollow の境界は sub-22 やること 0。
