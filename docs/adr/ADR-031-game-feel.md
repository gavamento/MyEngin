# ADR-031: 手触り (ゲームフィール) — 「何をいつ鳴らしたか」だけを sim 状態に持ち、揺れ・振動・画面効果はその純関数で描く

- 状態: **確定** (2026-11-04、M94a〜M94j)
- 出所: M94。計画は `plans/m94-game-feel.md` (ユーザー合意済みの範囲 B / C / D / E / F / G)。
- 実体:
  - 共有の数学: `src/Shared/FeelMath.h` (イージング 29 種、`MyeCurve`、ばね、値ノイズ、`MyeFeelDesc`)、`src/Engine/Core/Util/CurveJson.{h,cpp}`。
  - 時間効果: `src/Engine/Engine/Loop/GameFlow.h` (`TimeControl` の freeze / 時限スロー)。
  - 再生中の手触り: `src/Engine/Engine/Feel/FeelState.{h,cpp}` (`FeelState`、`EvaluateFeel`)、`FeelTriggerSystem.{h,cpp}`、`SpringFollowSystem.{h,cpp}`。
  - プリセット資産: `src/Engine/Engine/Feel/FeelLibrary.{h,cpp}` (`.feel.json`)、`assets/feel/*.feel.json`。
  - 入力: `src/Engine/Platform/InputActions.{h,cpp}` (`bufferTicks`、`InputBufferState`、応答カーブ、外側の遮断)。
  - 適用: `src/Engine/Engine/Loop/EngineLoop.cpp` (評価と振動の合成)、`src/Engine/Engine/Rendering/RenderSystem.cpp` (カメラとポスト)。
  - エディタ: `src/Editor/Windows/Scene/FeelAssetInspector.{h,cpp}`、`src/Editor/Widgets/CurveEditorWidget.{h,cpp}`、`src/Editor/Windows/Debug/FeelMonitorWindow.{h,cpp}`、Project Settings の入力節。
  - API: `src/Shared/EngineAPI.h` (ABI v31)、`src/Shared/ScriptAPI.h` (`MyePlayFeel` ほか)、`src/Scripting/Interop.cs` / `MyeScript.cs`。
  - デモ・検証: `--feel-demo` (`DemoContentFeel.cpp`、`src/GameLogic/Scripts/FeelDemo.cpp`)、`tools/replay_verify.bat` の `feel` ジョブ、`Feel{Math,System,Library}SelfTest`、`SpringFollowSelfTest`。
- 番号: ABI **v30 → v31** (175 → **181** スロット)、SimSnapshot **v50 → v51** (Scene 節)、TypeId **81** (`FeelTrigger`) / **82** (`SpringFollow`)、`AssetType::Feel`。

## 背景

「手触りを簡単に作りこめる・調整できる」ために、ヒットストップ・時限スロー・カメラの揺れとキック・時間で減衰する振動・
画面効果のパルス・イージング / カーブ・ばね・先行入力・スティックの応答カーブを、エンジンの機能として持つ。

制約は 3 つ。

1. 決定論 (AGENTS 4.2、engine_spec §11): Debug / Release / Server で同じ入力から同じ結果になること。
2. タイムトラベル・What-if・ロールバックで、揺れも振動も正しく巻き戻ること。出力レーンに巻き戻らない状態を作ると、
   スクラブした先で揺れが続いたままになる (`docs/rev/oversights.md` の出力レーンの指摘と同型)。
3. 手触りを使わないシーンのハッシュ列と見た目を 1 ビットも変えないこと (既存の replay ペアと shot_verify を守る)。

## 決定の要約

### D1. sim に持つのは「何をいつ鳴らしたか」だけ

- `FeelState` (Scene 保持) は再生中の最大 16 件を持つ。各件は**発火時にコピーした** `MyeFeelDesc`、強さ、経過 tick、通し番号。
  後からアセットを編集しても再生中の結果は変わらず、状態だけで描画を再現できる。
- 揺れ・キック・振動・画面効果の値は、`EvaluateFeel(state, interpAlpha, outputScale)` で**毎フレーム評価する純関数**。
  描画専用の値を状態として持たないので、スナップショットを戻せば揺れも戻る。
- 揺れの乱れは `world.Rng()` を引かず、(通し番号, 軸, 時刻) の整数ハッシュによる値ノイズで作る。既存の乱数列はずれない。

### D2. 時間効果は既存の tick ゲートに載せる

- ヒットストップと時限スローは `TimeControl` の欄として持つ。dt は変えず、「この tick でゲート対象を進めるか」で表す (M51g と同じ)。
- 重ね合わせ: freeze は残りの max (足さない)、スローは遅い方が勝ち、同率は残りが長い方が勝つ。
- スローの tick はヒットストップが明けてから数える。実効の速さは `min(scalePercent, スローの現在値)`。
- tick N のスクリプトが要求した時間効果は **tick N+1 から**効く (`stepSim` はスクリプト層より前に決まる)。
- 揺れ・振動の経過は**ゲートしない**。ヒットストップで止まった世界が震えるのが手触りの定番だから。

### D3. 既定値では何も畳まない (内容ゲート)

- `TimeControl` の 6 欄は効果が残っているときだけ、`FeelState` は一度でも鳴らしたら、`InputBufferState` は窓の中のものがあるときだけハッシュへ畳む。
- SimSnapshot は常に書く (レイアウト固定、v51)。

### D4. カメラは補間の後・逆行列の前、ポストはマージの後

- `RenderSystem::ResolveCamera` でシーンカメラの world 行列へカメラ局所の回転 + 平行移動を掛ける。
  ジッタの一元化・TAA の再投影・`lastViewProjNoJitter` は揺れたカメラを本当に動いたカメラとして扱う。
- Scene View などの `CameraOverride` には掛けない。画面効果は CameraPostFx をマージした実効値への加算で、設定値は書き換えない。
- 振動はモーターごとに `SetPadVibration` との max。抑止条件 (record / verify / スクラブ / 非フォーカス) は既存の出力レーンと同じ。

### D5. カーブは 2 方式、新しい FieldType は作らない

- `MyeCurve` は preset (イージングの種類 + 始め / 終わり / 行き過ぎ量) と custom (制御点 8 個までの Hermite) を持つ固定長 POD。
- カーブは `.feel.json` と `actions.json` の中だけに置く。Inspector の汎用欄・シーン JSON・ハッシュ・DLL 移行の 4 系統に新しい型を足さない。
- sim から呼ばれうるので、評価は四則演算と固定次数の多項式だけで組む (CRT の sin / pow / exp を使わない)。

### D6. プリセットは資産、エディタではライブ編集

- `.feel.json` は PhysMat と同じ「再生時に同じ資産がある」前提の資産クラス。SimLibraries として EngineLoop と HeadlessSim の両方に注入する。
- アセット Inspector は FeelLibrary 上のプリセットを直接編集する。スライダを動かすと次に鳴らした分から効く。ディスクへは Save でだけ書く。
  編集は文書エントリ (`UndoDocOp::Kind::Feel`) として Undo できる。
- Inspector の「プレビュー」は sim の表へは鳴らさず、EngineLoop が持つ**描画専用のプレビュー表** (`EngineContext::feelPreview`) へ、
  時間効果を外した desc (`PresentationOnly`) を鳴らす。描画フレームから hash 対象の状態を書くと、録画・検証・
  タイムトラベルの再シミュレーションで再現できないため (PR レビューの指摘で M94 中に変更)。評価は sim の表と足し合わせる。

### D7. スクリプトと C# レーン

- C++ スクリプトは `MyePlayFeel(ctx, "HeavyHit")` の 1 行か、`MyeHitStop` / `MyeSlowMotion` / `MyeCameraShake` / `MyeCameraKick` / `MyeRumble` で鳴らす。
- コードを書かない口として `FeelTrigger` (接触の力積がしきい値を超えたら鳴らす) を持つ。
- 鳴らす・止める・先行入力の消費は hash 対象の状態を書くので、**C# レーンからは受けない** (v30 の `SendScriptMessage` と同じ規則)。
  C# からは出力倍率 (`SetFeelOutputScale`、出力レーン) と `IsSimStepping` だけを使える。

### D8. 入力の手触りは actions.json の任意キー

- アクションの `bufferTicks`、軸の `response` と `outerDeadzone`。無ければ従来と同一で、保存しても新しいキーは書かない。
- 先行入力の窓 (`InputBufferState`) は Scene 保持の sim 状態。`kActionBuffered` は問い合わせ時に窓から引くので、同じ tick に消費した後は立たない。
- 応答カーブはパッド成分 (内側の遮断の後) にだけ掛け、恒等なら評価を飛ばす。

### D9. ばね追従は LocalTransform から組む

- `SpringFollow` は追従先の姿勢を `WorldMatrix` ではなく LocalTransform の親子連鎖から組む。
  `WorldMatrix` は描画時にも更新される派生値で、読むと「描画したかどうか」で sim が変わるため。
- フェーズ 4 の Transform 確定の直前に、entity index 昇順で評価する。物理と同じゲート対象。

## 採らなかった案

- **dt をスケールするスローモーション**: 固定 tick の前提が全システムで壊れる (M51g の判断のまま)。
- **低い % のスローを補間でならす**: ゲート対象と非ゲートのスクリプト駆動の物体で補間の前提が食い違う。M94 では入れない (既知の制約)。
- **揺れのオフセットを出力レーンの状態として持つ**: 巻き戻らない状態になり、スクラブや What-if で揺れが残る。
- **カーブを新しい FieldType にする**: Inspector・シーン JSON・ハッシュ・DLL 移行の 4 系統を同時に変える必要があり、範囲に見合わない。
- **個別ヒットストップ (攻撃側と被弾側だけ止める)**: エンティティ単位のゲートが要る。今回は全体の停止だけ。

## 既知の制約

- 低い % のスローモーションは、ゲート対象が「進む tick / 止まる tick」の交互になるのでカクつく。
- 早送り (100% 超) は不可 (既存の仕様)。
- C# レーンからは手触りを鳴らせない (D7)。
- `bufferTicks` を持てるのはアクション定義の先頭 32 本まで。
- 先行入力は replay の feel ジョブでは被覆していない (台本で入力を作らないため)。InputActionsSelfTest と SimSnapshotSelfTest が被覆する。
- 見た目 (揺れ・キック・フラッシュ) と実パッドの振動は CI では確認できない (`docs/test_checklists.md` の M94 節で手動確認する)。
