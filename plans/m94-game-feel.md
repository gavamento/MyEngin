# M94: 手触り（ゲームフィール）を作りこみ・調整できる機能

## 再開手順

- 進捗の正本は git log（件名 `M94x: ...`、1 サブ = 1 コミット）。下の「進捗」表は最後に揃える。
- このセッションは Linux コンテナで MSVC / PowerShell が無い。検証は push 後の GitHub Actions CI。
  プロジェクトファイルは `tools/gen_project_files.ps1` と同じ規則で更新する（Windows では本物の生成器を回せば同じ結果になる）。

## 進捗

| サブ | 状態 |
|---|---|
| M94a | 実装済み（CI 未確認） |
| M94b〜M94j | 未着手 |

## Context

依頼: 「このエンジンに手触りを簡単に作りこめる調整できる機能を追加したい」。

現状（調査で確認済み）:
- 調整できるのは各スクリプトの `MYE_F_RANGE` 欄と schema component（`GameTuning` 慣例）の Inspector ドラッグ値だけ。
- ヒットストップ、時限スローモーション、カメラ揺れ・キック、振動の時間変化、画面効果のパルス、
  イージング・カーブ、ばね、先行入力、スティック応答カーブは**どれも存在しない**。
  - `TimeControl` は pause と 0..100% の tick ゲートだけ（`src/Engine/Engine/Loop/GameFlow.h:13-33`）。
  - `SetPadVibration` は保持値で、時間変化が無い（`src/Engine/Engine/Script/EngineApiTable.cpp:1150`）。
  - カメラは world 行列の逆行列のみで、差し込み口が無い（`src/Engine/Engine/Rendering/RenderSystem.cpp:705-718`）。
  - カーブは `AnimInterp { Linear, Step }` のみ（`src/Engine/Engine/Animation/Animation.h:17-18`）。
  - 入力は cur/prev 2 枚の比較だけ（`src/Engine/Platform/InputActions.h:15-19`）。

目標: 「ヒットの手触り」を **1 つのアセット（.feel.json）としてエディタで作り、スクリプト 1 行か
コンポーネントで鳴らせる**。さらに決定論・リプレイ・タイムトラベルを壊さないこと。

## ユーザー合意済みの決定

| 項目 | 決定 |
|---|---|
| 含める柱 | B 手触りプリミティブ、C イージング・応答カーブ、D 入力の手触り、E 手触りプリセット、F 画面効果パルス、G ばね・二次運動 |
| 含めない | A チューニングボード、H 計測と比較 |
| カーブ編集 | **両方**: 種類（enum）＋数値のプリセット方式と、制御点を描くカーブエディタ |
| 題材 | 汎用（アクション寄り）。ヒット演出のデモを作る |
| 進め方 | 実装して `claude/wizardly-planck-loyjc6` へ push し、GitHub Actions CI で検証 |

番号: マイルストーン **M94**、ABI **v31**、SimSnapshot **v51**、新規 TypeId **81〜**、ADR **ADR-031**。

## 設計の核

### 1. 状態の置き場所と決定論（ADR-031 で固定する）

- **何をいつ鳴らしたか** だけを sim 状態として持つ。揺れ・振動・画面効果の値は、その状態と経過時間の**純関数**で描画側が毎フレーム評価する。
  - これにより、タイムトラベル・ロールバック・What-if で揺れも正しく巻き戻る。出力レーンに巻き戻らない状態を作らない（既知の罠 `docs/rev/oversights.md:51-54` を回避）。
- 新しい sim 状態は 3 つ。すべて Scene 保持、WorldHash 対象、SimSnapshot の Scene 節に入る。`UIInteractionState` と同じ扱い。
  1. `TimeControl` の拡張: `freezeTicks`、スロー（`slowPercent / slowHoldTicks / slowRecoverTicks / slowRecoverLeft / slowRecoverEase`）。
  2. `FeelState`: 再生中インスタンス最大 16 個。各インスタンスは `MyeFeelDesc` のコピー、強さ、経過 tick を持つ。
     - 発火時に desc をコピーするので、後でアセットを編集しても再生中の結果は変わらない。状態だけで再現できる。
  3. `InputBufferState`: レーン × アクション（32）ごとの「押されてからの tick 数」。
- **既存シーンは 1 ビットも変えない**（M63 の慣例）。新しい節は「非既定値があるときだけ」ハッシュへ畳む。
  - これで既存の replay ペアと shot_verify の結果が変わらない。SimSnapshot は常に書き、version を 51 へ上げる。
- 時間効果の順序を明示する。tick N のスクリプトが要求したヒットストップは、**tick N+1 から**効く。
  - 理由は、`stepSim` がスクリプトより前に決まるため（`src/Engine/Engine/Loop/TickRunner.cpp:369`）。
- 重ね合わせの規則（決定的）:
  - freeze は残り tick の max を取る。
  - スローは低い % が勝ち、同率なら長い方が勝つ。
  - インスタンスが満杯のときは、残り時間が最短のもの（同率は添字が小さい方）を置き換える。
- 揺れのノイズは `world.Rng()` を引かない。（インスタンス番号, 発火 tick, 軸）の整数ハッシュから作る value noise を使う。既存の乱数列はずれない。
- 揺れ・振動・画面効果は**未スケールの tick** で進む。ヒットストップ中も揺れ続けるのは意図どおり。
- LoadScene で FeelState・InputBufferState・freeze/スローを明示的に消す。Play/Stop は `PlayModeController` の Time スナップショットに乗せ、残り 2 つは Play/Stop でクリアする。

### 2. 共有の数学（C: `src/Shared/FeelMath.h`、header-only の inline と POD）

- ゲームロジック DLL とエンジンが**同じ関数**を使う。sim でも使われうるので、四則演算と `MyeSinRad`（`src/Shared/ScriptAPI.h:583`）だけで組み、`std::sin` / `std::pow` は使わない。
  - Expo 系は多項式による `MyeExp2` で実装する。
- 中身:
  - `MyeEase` enum とその評価: Linear、Quad/Cubic/Quart/Quint の In・Out・InOut、Sine、Expo、Back、Elastic、Bounce。
  - `MyeCurve` POD: preset モード（ease の種類＋強さ）と custom モード（キー最大 8 個、t・値・入出接線の Hermite）。評価関数 `MyeEvalCurve`。
  - ばね: `MyeSpringStep`（周波数 Hz＋減衰比、陰的積分で無条件安定）、`MyeSmoothDamp`、float と float3 版。
  - `MyeFeelDesc` POD: 次のチャネルを持つ。
    - hitStop: tick 数
    - slow: % / hold / recover / ease
    - shake: 位置振幅、回転振幅（度）、周波数、持続、包絡カーブ
    - kick: カメラ局所の方向と距離、FOV 差分、持続、カーブ
    - rumble: low / high、持続、カーブ
    - postFx: ビネット、色収差、露出、彩度、フラッシュ色と重み、持続、カーブ

### 3. カメラ・振動・画面効果の適用（描画専用、B / F）

- `EngineLoop` はフレームごとに `FeelState` と `interpAlpha` から、カメラオフセット・振動・画面効果の差分を評価する（純関数）。結果は `RenderSystem` のフレーム入力として渡す（`interpAlpha` と同じ渡し方、`src/Engine/Engine/Rendering/RenderSystem.h:160`）。
- `ResolveCamera` の補間直後、逆行列の前でオフセットを掛ける。`RenderSystem.cpp:709-713` の間に挿入する。
  - シーンカメラ経路だけに適用し、`CameraOverride`（Scene View）には掛けない。
  - ジッタの一元化（`:738-752`）より前なので、TAA の履歴と `lastViewProjNoJitter` は揺れたカメラで一貫する。
- 画面効果のパルスは、CameraPostFx をマージした後に加算する（`RenderSystem.cpp:2214` 付近）。
- 振動は既存の出力レーン（`src/Engine/Engine/Loop/EngineLoop.cpp:2948-2953`）で合成する。
  - モーターごとに `SetPadVibration` との max を取る。抑止条件は既存と同一。
- アクセシビリティ: チャネル別の出力倍率（揺れ / 振動 / フラッシュ）を出力レーンとして持つ。sim は書くだけで読み返さない。ゲームの設定画面から変えられる。

### 4. 手触りプリセット（E: `.feel.json`）

- PhysMat の経路をそのままなぞる（`src/Engine/Engine/Physics/Rigid/PhysMatLibrary.h:33-116`）:
  - `AssetType::Feel`
  - `FeelLibrary`（AssetID と名前ハッシュの両方で引ける）
  - JSON の `ToJson` / `FromJson` / `Sanitize`
  - 起動時スキャン（`src/Engine/Engine/Demo/DemoContent.cpp:252-257`）
  - ReloadHub の種別（`src/Engine/Engine/HotReload/ReloadHub.cpp:50-80`）
  - Create メニュー（`src/Editor/Asset/AssetOps.cpp:569-589`）
  - AssetBrowser のラベル
  - StageClassifier
- アセット Inspector（`src/Editor/Windows/Scene/InspectorWindowAssets.cpp`）:
  - チャネルごとの折りたたみ見出しと、カーブのプレビューを置く。
  - Save で即時再読み込みし、Revert もできる。
  - Play 中だけ押せる「プレビュー再生」を付ける。シーン状態の編集として扱う。
- カーブエディタ（新規 `src/Editor/Widgets/CurveEditorWidget.cpp`）:
  - preset と custom を切り替えられる。
  - キーのドラッグ、ダブルクリックで追加、右クリックで削除、接線ハンドル。
  - 新しい `FieldType` は**作らない**。カーブはアセット（と actions.json）の中だけに置くので、Inspector・JSON・ハッシュ・DLL 移行の 4 系統に手を入れずに済む。
- Undo は M93f の DocOp に `UndoDocOp::Kind::Feel` を足す（`src/Editor/Undo/UndoStack.h:44-50`）。
- コードを書かずに鳴らす口として `FeelTriggerComponent` を追加する。接触の力積がしきい値を超えたら、指定した feel を鳴らす。強さは力積に比例する。

### 5. 入力の手触り（D）

- `actions.json` に任意キーを足す。無ければ既存と同一の挙動になる。
  - アクション: `bufferTicks`（先行受付の窓）。
  - 軸: `response`（MyeCurve。既定は Linear）と `outerDeadzone`。
- `InputActions::Evaluate` の後に、明示的な状態入出力として `UpdateBuffers(InputBufferState&)` を呼ぶ。
- `ActionState` の戻り値に `kActionBuffered = 1u << 3` を追加する。既存ビットは不変なので、取得側の ABI は変わらない。
- 消費（同じ先行入力で 2 回跳ばない）は新スロット `ConsumeAction`。
- Project Settings の Input 節（`src/Editor/Windows/Project/ProjectSettingsWindow.cpp:413`）に、窓の tick 数と応答カーブ（カーブエディタを再利用）を追加する。
- コヨーテタイムは接地の履歴なのでゲーム側の責務とする。`MyeTickWindow` 補助とレシピを文書化する。

### 6. ばね・二次運動（G）

- `SpringFollowComponent`: 対象エンティティへ位置・回転をばねで追従させる（カメラの追従遅れ）。
  - 速度などの状態はコンポーネント内に持ち、ハッシュ対象とする。ゲート対象なので、ヒットストップ中は止まる。
- スクワッシュ&ストレッチは `MyeSpringStep` を使うデモスクリプトで示す（エンジンに専用機構は足さない）。

### 7. スクリプト API（ABI v31、175 → 181 スロット、`SendScriptMessage` の後ろへ追加）

| スロット | 内容 |
|---|---|
| `PlayFeel(engine, nameHash, intensity) -> int32` | プリセット名で鳴らす。ハンドル、失敗時は -1 |
| `PlayFeelDesc(engine, const MyeFeelDesc*, intensity) -> int32` | コードで組んだ desc を鳴らす |
| `StopFeel(engine, handle)` | -1 なら全停止 |
| `ConsumeAction(engine, nameHash, player) -> int32` | 先行入力を消費。消費したら 1 |
| `IsSimStepping(engine) -> int32` | この tick でゲート対象が進むか。ヒットストップ中の自前移動を止める判断に使う |
| `SetFeelOutputScale(engine, channel, scale)` | 出力レーン。アクセシビリティ用 |

- C++ の糖衣を `ScriptAPI.h` に置く。`MyePlayFeel(ctx, "HeavyHit")`、`MyeHitStop`、`MyeSlowMotion`、`MyeCameraShake`、`MyeCameraKick`、`MyeRumble` は、内部で desc を組んで `PlayFeelDesc` を呼ぶ。
- C# 側は `Interop.cs` の位置ミラーと `MyeScript.cs` のラッパーを用意する（名前指定版と単機能版）。C# のイージングは範囲外。
- 同時に更新するもの: `MYE_API_VERSION`、`tools/check_rules.ps1:746` の表（`31 = 181`）、`kExpectedApiVersion`、`EngineApiTable.cpp` の充填、`src/Editor/SelfTest/PartSelfTest.cpp` の版表明と非 null 検査。

### 8. 観測（AGENTS 3.2）

- 新規 Debug ウィンドウ「手触りモニタ」を作る。次の内容を表示する。
  - 再生中インスタンス（名前、経過 / 持続、チャネル）
  - freeze とスローの残り
  - 現在のカメラオフセット、振動、画面効果の値
  - アクションごとの先行入力の経過 tick
- エディタ利用者向けに「エディタでは揺れを切る」個人設定を置く（`src/Editor/App/EditorSettings.h`）。

## サブ分割（1 サブ = 1 コミット、件名 `M94x: ...`）

| サブ | 内容 | Snapshot | ABI | TypeId |
|---|---|---|---|---|
| M94a | 計画書 `plans/m94-game-feel.md`、`src/Shared/FeelMath.h`（イージング、カーブ、ばね、ノイズ）、`FeelMathSelfTest` | - | - | - |
| M94b | TimeControl の freeze とスロー（回復カーブ付き）、ハッシュ（非既定のときだけ）、Snapshot 節、LoadScene での消去、GameFlowSelfTest の拡張 | v51 | - | - |
| M94c | `FeelState` / `FeelSystem`（発火、経過、満了、評価）、カメラ・振動・画面効果の適用、出力倍率、`FeelSystemSelfTest` | v51 | - | - |
| M94d | `.feel.json` 資産一式（Library、AssetType、Reload、Create、AssetBrowser、Inspector の数値モード、プレビュー再生）、`FeelTriggerComponent` | - | - | 81 |
| M94e | カーブエディタ widget、Feel Inspector への組み込み、DocOp Undo（Kind::Feel） | - | - | - |
| M94f | 入力: bufferTicks、応答カーブ、outerDeadzone、InputBufferState、kActionBuffered、Project Settings の UI、InputActionsSelfTest の拡張 | v51 | - | - |
| M94g | `SpringFollowComponent` とそのシステム、SelfTest | - | - | 82 |
| M94h | ABI v31 の 6 スロット、C++/C# ラッパー、PartSelfTest、check_rules の表 | - | v31 | - |
| M94i | 手触りモニタ、デモ（`FeelDemo` スクリプト、プリセット `LightHit` / `HeavyHit` / `Land`、デモシーン）、replay_verify へ feel ジョブを追加 | - | - | - |
| M94j | 文書: ADR-031、engine_spec（§5.3 / §6 / §9 / §10 / §11.3 / §11.5 / §12.2 / §12.3）、feature guide §18、README の ABI 数、docs/history、docs/test_checklists | - | - | - |

Snapshot v51 への bump は M94b で 1 回だけ行う。M94c と M94f は同じ版の中で節を足す。各サブで、版番号の照合を行う SelfTest を同時に直す。

## 主な変更ファイル（代表）

- 新規:
  - `src/Shared/FeelMath.h`
  - `src/Engine/Engine/Feel/`（FeelState.h、FeelSystem.cpp/h、FeelLibrary.cpp/h、各 SelfTest）
  - `src/Editor/Widgets/CurveEditorWidget.cpp/h`
  - `src/Editor/Windows/Debug/FeelMonitorWindow.cpp/h`
  - `src/GameLogic/Scripts/FeelDemo.cpp`
  - `assets/feel/*.feel.json`
  - `docs/adr/ADR-031-game-feel.md`
- 変更:
  - `GameFlow.h`、`Scene.h`、`TickRunner.cpp`、`EngineLoop.cpp`、`RenderSystem.cpp/h`
  - `WorldHasher.cpp`（`:466` の HashGameFlow の後ろ）
  - `SimSnapshot.cpp/h`（`kSimSnapshotVersion` を 51 へ）
  - `InputActions.cpp/h`、`Components.h/.cpp`（末尾へ追加）、`EditorComponentCatalog.cpp`
  - `AssetDatabase.cpp/h`、`ReloadHub.cpp/h`、`AssetOps.cpp`、`InspectorWindowAssets.cpp`、`UndoStack.cpp/h`、`ProjectSettingsWindow.cpp`
  - `EngineAPI.h`、`EngineApiTable.cpp/h`、`ScriptAPI.h`、`Interop.cs`、`MyeScript.cs`
  - `LocalizationTable.inl`（en と ja の両方。`###` ID は一意）、`EditorMain.cpp`（SelfTest 登録を末尾へ append）
  - `check_rules.ps1`、`replay_verify.bat`
- `build/*.vcxproj(.filters)`: このコンテナには pwsh が無い。まず PowerShell の導入を試し、可能なら `tools/gen_project_files.ps1` を実行する。不可なら、生成器と同じ規則（Kind、Rel の昇順、フィルタ GUID は MD5 由来）で手編集し、その旨を報告する。

## 検証

CI（push ごと、`.github/workflows/ci.yml`）:
- Debug / Release ビルドを警告 0 で通す（`MyeWarnAsError`）。
- `Editor.exe --selftest`（Debug / Release）。新規 SelfTest の内容:
  - イージングの端点・単調性
  - カーブの評価
  - ばねの安定性
  - freeze と スローの tick 列
  - 重ね合わせの規則
  - インスタンス満了と置換
  - 先行入力の窓と消費
  - .feel.json の往復
  - ABI v31 の非 null
- `tools\check_rules.ps1`: 規則 10（ローカライズ）、規則 11（ABI ミラー）、規則 15（D3D 型の居場所。FeelSystem は D3D 型を持たない）。
- `tools\replay_verify.bat`:
  - **既存の全ペアがそのまま一致すること**（既定値で何も変えない証拠）。
  - 新しい feel ジョブ（ヒットストップ、スロー、揺れ、先行入力を通すデモ）が、Debug 録画 → stress 付き verify → Release verify → Server verify と、タイムトラベル・What-if で一致すること。
- `tools\shot_verify.bat`: 既存のスクショが不変であること（揺れの既定値は 0）。

このセッションでは確認できないもの（ユーザーの Windows 機での確認項目として `docs/test_checklists.md` に載せる）:
- 揺れ・キック・フラッシュの見た目。
- 実パッドの振動。
- カーブエディタの操作感。
- Play 中のプレビュー再生と、Stop での復元。
- タイムトラベルのスクラブで揺れが巻き戻ること。

## 既知の制約・範囲外（報告時に明示する）

- **低い % のスローモーションはカクつく**。TimeControl が tick ゲート方式で、ゲート対象は「進む tick / 止まる tick」が交互になるため。
  - 補間でならす案は、スクリプト駆動の物体（非ゲート）と食い違うので M94 には入れない。
  - ヒットストップ（完全停止）には影響しない。
- ヒットストップは全体の停止のみ。攻撃側と被弾側だけを止める「個別ヒットストップ」は範囲外。
- 早送り（100% 超）は不可（既存仕様）。
- C# レーンはリプレイの外（既存仕様）なので、C# から鳴らした手触りは record / verify で再現されない。
- 実機での見た目確認と実パッドでの確認は、CI では代用できない。
