# sub-05: スクリプトの口 (ABI v35 = 202: FieldAddImpulse / FieldSample) とデモの操作役

- 依存: sub-03 (波 W1。spec §6)
- 状態: OK (統合待ち。W1 の順 04 → 05 → 15 → 12)
- 往復: 2


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
5. デモの操作役 `src\GameLogic\Scripts\ZZFieldDemoDriver.cpp` (名前は ZZ 始まり: スクリプトの TypeId は名前順なので、既存のスクリプトの TypeId を動かさないため。`ZTwoDHero` と同じ。round 1 で確定) (FeelDemo.cpp の様式): CC の人が霧の中を決まった経路で歩き (moveInput を tick で決める = 入力に依存しない)、一定の tick ごとに前方へ Cone の「斬撃」、別の場所で Radial の「爆発」(`densityDelta = −1`)。`FieldSample` で足元の濃さを読み、濃い所では歩きを遅くする (読み取りの経路を replay で被覆するため)。`DemoContentField.cpp` の末尾に人の生成を足す
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

SELF_EVAL: sub-05 (round 1) — 全文は司会への返答を参照。要点: ABI v35 = 202、RunDisturbanceAbiTests (未実行)、デモ ZZFieldDemoDriver、Debug / Release ビルド・check_rules 0 error。C# 実走プローブ・selftest・replay_verify は未実行。スクショ: scratchpad\m96-shots\sub05_t400.png

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-08)。ABI・ミラー・糖衣・拒否・デモの実装と規則 11 PASS を確認。ZZ 命名は受け入れ (spec 側を直した)。must: C# の一時プローブの実走 (受け入れ 17。selftest / replay_verify ではないので、まとめ実行の対象外)。should: メインの作業ツリーの git status、斬撃の扇が写る絵
- round 2: VERDICT OK (planner、2026-11-08)。C# の一時プローブ実走で (a) AddImpulse = 0 (b) Sample の全欄が C++ と一致 (2 位置 × 2 tick) (c) ABI v35 の照合を確認、プローブは撤去済み。メインの git status にこのサブのファイルなし。斬撃の扇が絵で断定しづらい件は minor (形は受け入れ 18 の selftest が受け持つ。絵は sub-17 の手動確認へ)

## 中断メモ (2026-11-08)

司会の指示 (ユーザーの一時停止) で中断。worktree = C:\HAL\MyEngin\.claude\worktrees\agent-adc78e94e6a5e1d68、ブランチ = worktree-agent-adc78e94e6a5e1d68。コミットなし。

- 前提の修正: worktree は古い aa3288e から切られていたため、作業の最初に `git reset --hard master` (b7cb1fa、clean だったので失うものなし) で master に合わせた
- 終わったこと (やること 1〜4・7):
  - EngineAPI.h (MyeFieldImpulse 52 B / MyeFieldSample 32 B、スロット 2 本、v35、注記)、EngineApiTable.cpp (FillDisturbanceSlots。C# レーンは積めず 0)、Interop.cs (ミラー + kExpectedApiVersion = 35 + FieldSample)、MyeScript.cs (FieldSample 糖衣)、ScriptAPI.h (MyeFieldPoint / Radial / Cone / MyeFieldSampleAt)、check_rules.ps1 の版表 (35 = 202)、PartSelfTest.cpp の版の表明 34 → 35、docs\history\api-scripting-tools.md の v35 行
  - selftest コード: DisturbanceFieldSelfTest.cpp に `RunDisturbanceAbiTests` (Fast 層の末尾から 1 行で呼ぶ。未実行)
  - デモ: src\GameLogic\Scripts\ZZFieldDemoDriver.cpp (新規。TypeId を動かさないため 'ZZ' 始まり。sub 記載の FieldDemoDriver から逸脱)、DemoContentField.cpp に AddFieldDemoPerson + 末尾 1 行
  - gen_project_files 実行済み (build\GameLogic.vcxproj / .filters が変わった)
- 検証済み: Debug / Release の MyEngine.sln ビルド成功 (デモ位置調整前の版)、MyeScripting Debug / Release 成功、check_rules 0 error、Runtime `--field-demo` のスクショ撮影 (cache\field_t300.png, cache\field_r400.png。球の筋は見えるが人の筋・爆発の穴は遠くて薄い)
- 途中のこと: デモの人と爆発を手前へ寄せる調整 (人 (5, 0.9, -1)、爆発 (-7, 0.3, 1)、半径 4 / 斬撃 3) の後の Release 再ビルドが背景で走っていた (結果未確認)。調整後のスクショは未撮影
- 次にやること: Release 再ビルドの成否確認 → `--field-demo --screenshot cache\field_r400.png --shot-frame 400` を撮り直して人の筋・斬撃・爆発の穴を確認 → Debug 再ビルド → C# 一時プローブ (FieldAddImpulse = 0 / FieldSample = 値) は未実施 → selftest / replay_verify は W2 後にまとめて (未実行) → SELF_EVAL を返す
- 触ったファイル: src\Shared\EngineAPI.h、src\Shared\ScriptAPI.h、src\Engine\Engine\Script\EngineApiTable.cpp、src\Scripting\Interop.cs、src\Scripting\MyeScript.cs、tools\check_rules.ps1、src\Editor\SelfTest\PartSelfTest.cpp、src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp、src\Engine\Engine\Demo\DemoContentField.cpp、src\GameLogic\Scripts\ZZFieldDemoDriver.cpp (新規)、docs\history\api-scripting-tools.md、build\GameLogic.vcxproj(.filters) (生成物)
