# sub-05: スクリプトの口 (ABI v35 = 202: FieldAddImpulse / FieldSample) とデモの操作役

- 依存: sub-03 (波 W1。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W1)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): ABI v35 = 202 (唯一)。`EngineAPI.h` / `Interop.cs` / `ScriptAPI.h`。デモは自分の関数 `AddFieldDemoPerson` と `BuildFieldShowcaseScene` 末尾の呼び出し 1 行。selftest は `RunDisturbanceAbiTests`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.2「ABI」、§4.1 E / G、§2 #20 / #21。ゲームが爆発・攻撃をコードから注入し、霧の濃さ・跡を読めるようにする。デモに人と爆発を足す。

1. `src\Shared\EngineAPI.h`: `MyeFieldImpulse` / `MyeFieldSample` (POD、`static_assert` でサイズ固定) と、`SetFeelOutputScale` の次に `FieldAddImpulse` / `FieldSample` の 2 スロット。`MYE_API_VERSION` 34 → 35 と版コメント (`v35 (M96d): かき乱しフィールド — FieldAddImpulse / FieldSample (SetFeelOutputScale の次の 2 本 = 202)`)。スロットの注記に spec §4.1 E (キューと消費の時点・上限 256)・G (含む範囲・高さの形・呼んだ時点の値)・C# の扱いを書く
   - `MyeFieldImpulse`: `pos` / `dir` (MyeVec3)、`radius`、`speed`、`coneDeg` (Cone のときだけ)、`densityDelta`、`trailDepth`、`ripple`、`shape` (int32: 0 Point / 1 Radial / 2 Cone)。不正な `shape`・非有限値・`radius <= 0` は 0 を返して積まない
   - `MyeFieldSample`: `velocity` (MyeVec3)、`density`、`trail`、`ripple`、`field` (MyeEntityId)
2. `src\Engine\Engine\Script\EngineApiTable.cpp`: 実装。`FieldAddImpulse` は `IsManagedLane(engine)` なら 0 (`:485-518` の PlayFeel と同じ)。中身は sub-01 の `DisturbanceQueueImpulse` / `DisturbanceSampleAt` を呼ぶだけ
3. `src\Scripting\Interop.cs`: 同じ並びでミラー + `kExpectedApiVersion = 35`。`MyeScript.cs` に読み取りの糖衣 (`FieldSample`)。書き込みの糖衣は C# に出さない
4. `src\Shared\ScriptAPI.h`: C++ の糖衣 (既定値入りの `MyeFieldImpulse` を作るヘルパと、Point / Radial / Cone の短い呼び出し)
5. デモの操作役 `src\GameLogic\Scripts\FieldDemoDriver.cpp` (FeelDemo.cpp の様式): CC の人が霧の中を決まった経路で歩き (moveInput を tick で決める = 入力に依存しない)、一定の tick ごとに前方へ Cone の「斬撃」、別の場所で Radial の「爆発」(`densityDelta = −1`)。`FieldSample` で足元の濃さを読み、濃い所では歩きを遅くする (読み取りの経路を replay で被覆するため)。`DemoContentField.cpp` の末尾に人の生成を足す
6. ABI の検証: memory `abi-bump-verification` の手順 (Interop.cs の順序の機械照合 = 規則 11、C# の一時プローブで `FieldAddImpulse` が 0・`FieldSample` が値を返すことを実走で確認してから消す)

7. (sub-01 からの申し送り) `FieldAddImpulse` の注記に「Point / Cone の `dir` は正規化しない。`u = dir × speed` なので dir の長さが速さに掛かる」と書く

## やらないこと (このサブでは)

- アニメイベント (sub-06)
- 場の設定を変える API (場のパラメータは既存の `SetComponentField` で足りる。必要になれば別途)
- 既存スロットの並び・シグネチャの変更

## 触る場所 (planner の見立て)

- `src\Shared\EngineAPI.h:13-63` (版コメント・`MYE_API_VERSION`)、`:1145-1170` 付近 (末尾のスロット)
- `src\Engine\Engine\Script\EngineApiTable.cpp:485-530`、`EngineApiTable.h`
- `src\Scripting\Interop.cs`、`src\Scripting\MyeScript.cs`、`src\Scripting\Bootstrap.cs` (版の照合)
- `src\Shared\ScriptAPI.h`
- `src\GameLogic\Scripts\FieldDemoDriver.cpp` (新規、`tools\gen_project_files.ps1`)
- ABI の版を断言している selftest (`grep -rn "MYE_API_VERSION" src --include=*SelfTest*.cpp`、例 `twodabi` の `api.version == MYE_API_VERSION` はそのまま通るはず)。スロット数を焼いた断言があれば 202 へ
- `docs\history\api-scripting-tools.md` に v35 の経緯 (EngineAPI.h の冒頭の規約)

## 受け入れ条件 (このサブ)

spec §5 の 12, 17, 18, 30。

- 17: check_rules 規則 11 PASS (順序・件数・名前・引数個数・版⇄スロット数)。C++ の一時プローブ (または GameLogic の selftest 経路) で、Update から積んだインパルスが同じ tick の step で、LateUpdate から積んだものが次の tick の step で効く。`FieldSample` が `DisturbanceSampleAt` と一致。C# の一時プローブで `FieldAddImpulse` = 0、`FieldSample` = 1 と値
- 18: Point / Radial / Cone の向き (selftest。Radial は中心のマスで 0、Cone の外で 0)
- 12: replay_verify `field` PASS (人と爆発の入ったデモで)。C# レーンは replay 被覆外なので、C# の確認は上の一時プローブで行う
- デモの絵: 人が歩いた筋・斬撃の扇・爆発の穴が写った 1 枚 (`Runtime.exe --field-demo`)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release (MyeScripting も)
- `$env:MYE_SELFTEST_ONLY='disturbance,twodabi,computeabi'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
