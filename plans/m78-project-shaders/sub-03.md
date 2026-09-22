# sub-03: fxstack アセット＋Inspector Properties UI

- 依存: sub-01, sub-02
- 状態: 判定待ち (round 3 VERDICT OK — コミット待ち)
- 往復: 3

## やること

`*.fxstack.json` のロード／保存と、カメラからの AssetRef 接続を実装する。Inspector で:

- スタック参照
- パス一覧 (enabled / kind / insertion|dispatchPoint / priority / shader)
- 選択中ポストの Properties を sub-01 スキーマから自動ウィジェット化 (Slider / ColorEdit / Vector / テクスチャ選択)

値変更はスタック JSON の `properties` に書き戻し、実行中 Runner に反映する (保存前プレビューは既存マテリアルプレビューほど厳密でなくてよいが、Play 中は毎フレームまたはダーティ時に反映)。

AssetDatabase の分類・ブラウザ表示に `fxstack` を載せる (既存 ClassifyPath の流儀に合わせる)。

## やらないこと (このサブでは)

- コンピュート Runner 本体 (エントリ型は JSON に含めてよいが実行は sub-04)
- ABI、サーフェス
- Volume 空間ブレンド

## 触る場所 (planner の見立て)

- 新規: fxstack ローダ／型 (`Engine/Asset` または `Renderer` 近傍)
- `Components.h` — 薄コンポーネントまたは Camera 近傍の `AssetID fxStack` (末尾 append・既存シーン互換)
- `InspectorWindow.cpp` — スタック＋Properties UI
- AssetDatabase 分類、必要なら `.fxstack.json.meta` 慣例
- シリアライズ: FieldDesc の AssetRef ＋ JSON ファイル本体

## 受け入れ条件 (このサブ)

1. fxstack をカメラに割り当て、プロジェクト `*.post.hlsl` が挿入点どおり走る  
   — 手動手順 (実装メモに記載)
2. Properties 変更が Inspector からでき、画面に反映される  
   — 手動
3. fxstack 未設定シーンは従来どおり  
   — 手動／既存テスト
4. ローカライズ規約を破る生文字列を増やしすぎない (`MYE_JP` 等)  
   — レビュー

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: サンプル fxstack + Tint ポストで Inspector 操作。

## 実装メモ (coder が追記)

---

SELF_EVAL: sub-03 (round 3)
実装:
  - #1: src/Engine/Engine/RenderSystem.cpp:ResolvePost — CameraOverride 時に projectEffectRunner_/projectComputeRunner_ を ClearPasses; BeforePost Dispatch と postFx_.Resolve へは nullptr (ShouldInjectProjectFxStack)
  - #1: src/Engine/Renderer/ProjectFxStackPolicy.h — ShouldInjectProjectFxStack(bool cameraOverrideActive)
  - #1: src/Engine/Renderer/FxStackSelfTest.cpp — TestProjectFxStackInjectionPolicy (テスト 8)
  - #3: RenderSystem Tex2D リゾルバ — gray/black/bump を white フォールバック＋初回 MYE_LOG_WARN; docs/project-shaders-tex2d-defaults.md; ProjectPostCommon.hlsli コメント
  - #5: assets/shaders/MyTint.post.hlsl + .meta, assets/MyTint.fxstack.json + .meta (手動 Tint 手順と spec 例に一致)

仕様との差分:
  - [追加] CameraOverride 時の Compute ClearPasses/nullptr は sub-04 本丸だが、Scene View で stale CS が走らないよう本サブで BeforePost もガード (sub-04 と役割重複を申し送り)
  - [追加] black/bump/gray 専用 SRV は未実装 (planner/review 裁定どおり WARN＋docs のみ)

検証:
  - MSBuild Engine+Editor (Debug|x64, VS18 MSBuild) → 成功 (exit 0)
  - Editor.exe --selftest → exit 0 (FxStack テスト 8 CameraOverride policy PASS 含む)
  - tools\check_rules.ps1 → 0 errors, 0 warnings

自己採点 (1-5):
  仕様適合: 5 — review-1 #1/#3/#5 と spec §4.1 CameraOverride=Runner 無効を反映
  正しさ: 4 — ポリシー SelfTest PASS。Scene View 実画面／Play→停止の GUI 再現は未実施
  コード品質: 4 — 既存 CameraPostFx と同規則の分岐。Policy を Renderer ヘッダに分離
  テスト: 4 — 注入ポリシー 2 アサーション。RenderSystem 統合のヘッドレステストは無し

不安・質問: なし

触ったファイル:
  - src/Engine/Engine/RenderSystem.cpp
  - src/Engine/Renderer/ProjectFxStackPolicy.h
  - src/Engine/Renderer/FxStackSelfTest.cpp
  - assets/shaders/MyTint.post.hlsl
  - assets/shaders/MyTint.post.hlsl.meta
  - assets/MyTint.fxstack.json
  - assets/MyTint.fxstack.json.meta
  - assets/shaders/ProjectPostCommon.hlsli
  - docs/project-shaders-tex2d-defaults.md

申し送り:
  - sub-04: CameraOverride 時の projectComputeRunner_ 更新停止は本修正で ClearPasses＋Dispatch ガード済み。sub-04 で Compute 側の仕様テスト・ドキュメントを一本化すると二重記述を減らせる
  - review-1 #2 (ポスト本数上限) は sub-02/round2 で ProjectEffectRunner::kMaxPostPasses 実装済み — reviewer 再確認用
  - 手動: Play で MyTint.fxstack 割当→停止→Scene View でユーザーポストが消えることを目視 (sub-03 手動手順 1–6)

---

SELF_EVAL: sub-03 (round 2)
実装:
  - src/Engine/Renderer/ProjectShaderProperties.h : PropValue — `std::string` を第3 variant として追加 (Tex2D のアセット名 / GUID hex)
  - src/Engine/Renderer/FxStackAsset.cpp : ParseProperties(local), SerializeProperties — JSON 文字列値 (Tex2D) のロード・保存を追加
  - src/Engine/Renderer/FxStackSelfTest.cpp : TestTex2DRoundTrip — Tex2D プロパティ (ビルトイン名 / GUID hex 文字列) の load/save round-trip テスト追加 (テスト 7)
  - src/Engine/Renderer/ProjectEffectRunner.h : TextureResolver, SetTextureResolver, DrawFullscreen (userTexSRVs 追加), texResolver_ メンバ
  - src/Engine/Renderer/ProjectEffectRunner.cpp : DrawFullscreen — t2+ スロットへ userTexSRVs バインド + 後始末; RunPasses — schema の Tex2D 行を収集して texResolver_ で SRV 解決し DrawFullscreen に渡す
  - src/Engine/Engine/RenderSystem.cpp : ResolvePost — フレームごとに SetTextureResolver を設定 (builtin 名 / GUID hex → Texture SRV)
  - src/Editor/Windows/InspectorWindow.h : FxStackEditState::schemaCache 追加, DrawFxStackInspector 引数に EngineContext& ctx 追加, ProjectShaderProperties.h / unordered_map インクルード追加
  - src/Editor/Windows/InspectorWindow.cpp : DrawFxStackInspector — スキーマ駆動 UI に全面改修 (Header/HideInInspector/Range/Float/Color/Vector/Tex2D ピッカー); フォールバックとして JSON キー列挙を維持; ShaderManager.h / ProjectShaderProperties.h インクルード追加

仕様との差分:
  - [追加] FxStackSelfTest.cpp の MYE_LOG_INFO 文字列は英語表記 (round 1 からの継続)
  - [追加] Inspector の保存前プレビューは save-on-apply (round 1 から継続。planner に受理済み)
  - [追加] Tex2D の string 値はビルトイン名 ("white"/"black"/"gray"/"bump") または GUID hex 16 文字列。GUID hex 以外の任意パス文字列は現状 white にフォールバック (round 2 の最小実装)
  - [未実装] コンピュート Runner 本体は sub-04 として申し送り (継続)

検証:
  - MSBuild Engine (Debug|x64) → 成功 (exit 0)
  - MSBuild Editor (Debug|x64) → 成功 (exit 0、LNK4204 警告は従来からのもの)
  - Editor.exe --selftest → exit 0
      FxStack SelfTest (M78c): 全ケース PASS (テスト 7 Tex2D round-trip 含む)
      ProjectEffectRunner SelfTest: ALL PASS (既存テスト)
  - tools\check_rules.ps1 → 0 errors, 0 warnings

自己採点 (1-5):
  仕様適合: 5 — スキーマ駆動 Inspector (Header/Range/Float/Color/Vector/Tex2D) 完備。Tex2D 値・Inspector・Runner バインドをすべて実装
  正しさ: 4 — selftest 全 PASS。手動での画面反映確認は未実施。Tex2D GUID→SRV 解決 (white フォールバック) の実走確認もヘッドレス
  コード品質: 4 — 既存規約準拠。DrawFullscreen の userTexSRVs は default 引数で後方互換
  テスト: 4 — Tex2D round-trip テストを追加 (テスト 7)。schema 駆動 Inspector の描画パスはヘッドレスからテスト不可

不安・質問:
  - Tex2D 値として GUID hex 以外のパス文字列が来た場合に白にフォールバックするが、エラーログも出さない。planner 確認推奨
  - black/gray/bump ビルトインテクスチャが現状 white にフォールバック (専用テクスチャ未整備)。sub-04 以降での整備推奨

触ったファイル:
  - src/Engine/Renderer/ProjectShaderProperties.h
  - src/Engine/Renderer/FxStackAsset.cpp
  - src/Engine/Renderer/FxStackSelfTest.cpp
  - src/Engine/Renderer/ProjectEffectRunner.h
  - src/Engine/Renderer/ProjectEffectRunner.cpp
  - src/Engine/Engine/RenderSystem.cpp
  - src/Editor/Windows/InspectorWindow.h
  - src/Editor/Windows/InspectorWindow.cpp

申し送り:
  - FxStackKind::Compute の実行本体 (sub-04)
  - black/gray/bump ビルトインテクスチャの専用 SRV 提供 (sub-04 または独立タスク)
  - Tex2D GUID hex → テクスチャ非同期ロード後の自動 SRV 更新 (現状: ロードキックのみ、初フレームは white)
  - ShaderManager ソース取得 API 追加 (EnsureCached の外部化推奨)

手動検証手順 (受け入れ 1–2):
  1. `assets/shaders/MyTint.post.hlsl` を作成:
     ```hlsl
     /*@MyEngineProperties
     [Range(0.0, 1.0)] _Intensity ("Intensity", Float) = 1.0
     _Tint ("Tint", Color) = (1, 1, 1, 1)
     @*/
     Texture2D gSceneColor : register(t0);
     SamplerState gLinear : register(s0);
     cbuffer MyEnginePerEffect : register(b1) { float _Intensity; }
     float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
         float4 c = gSceneColor.Sample(gLinear, uv);
         return c * _Intensity;
     }
     ```
  2. `assets/MyScene.fxstack.json` を作成:
     ```json
     { "version": 1, "passes": [{ "kind": "post", "shader": "MyTint.post",
       "enabled": true, "insertion": "BeforeTonemap", "priority": 100,
       "properties": { "_Intensity": 0.5, "_Tint": [1, 0.9, 0.8, 1] } }] }
     ```
  3. シーンのメインカメラに `CameraPostFxComponent` を追加し、`fxStack` フィールドへ上記 `.fxstack.json` を割り当て
  4. Play → 画面が暗くなる (_Intensity=0.5) ことを確認
  5. Asset Browser で `.fxstack.json` を選択 → Inspector の "MyTint.post" パスの Properties に Intensity スライダが表示されることを確認
  6. スライダを動かして Save → 画面の明るさが変化することを確認 (save-on-apply)

---

実装:
  新規ファイル:
    - src/Engine/Renderer/FxStackAsset.h     — FxStackAsset / FxStackEntry / PropValue 型、LoadFxStack / SaveFxStack / InsertionToString / InsertionFromString
    - src/Engine/Renderer/FxStackAsset.cpp   — nlohmann::json による JSON ロード・保存実装
    - src/Engine/Renderer/FxStackSelfTest.h  — RunFxStackSelfTest 宣言
    - src/Engine/Renderer/FxStackSelfTest.cpp — 6 ケース (空JSON / Post round-trip / Compute / Insertion変換 / 壊れたJSON / 混在パス)

  既存ファイル変更:
    - src/Engine/Renderer/ProjectEffectRunner.h/cpp — SetPasses() 追加 (シェーダキャッシュ保持更新)
    - src/Engine/Engine/AssetDatabase.h/cpp          — AssetType::FxStack 追加、*.fxstack.json 分類
    - src/Engine/Core/Components.h/cpp               — CameraPostFxComponent に AssetID fxStack フィールド追加
    - src/Engine/Engine/RenderSystem.h/cpp           — projectEffectRunner_ / lastFxStackId_ メンバ追加、ResolvePost でロード→SetPasses→PostProcess::Resolve に渡す
    - src/Engine/Core/LocalizationTable.inl          — Insp_FxStack* StrId 追加
    - src/Editor/Windows/InspectorWindow.h/cpp       — fxstack Inspector UI (LoadFxStackEdit / DrawFxStackInspector / DrawAssetRef FxStack 対応)
    - src/Editor/EditorMain.cpp                      — RunFxStackSelfTest 登録
    - build/Engine.vcxproj / .filters                — 新規 .cpp/.h 手動追加

検証結果:
  - Engine ビルド: 成功 (exit 0)
  - Editor ビルド: 成功 (exit 0, LNK4204 警告のみ・従来からあるもの)
  - Editor.exe --selftest: exit 0
      FxStack SelfTest (M78c): 全ケース PASS (37アサーション)
      ProjectEffectRunner SelfTest: ALL PASS (既存テスト)
  - tools\check_rules.ps1: 0 errors, 0 warnings

仕様との差分:
  [追加] FxStackSelfTest.cpp の MYE_LOG_INFO 文字列は英語表記にした (PowerShell/MSBuild 経由でのマルチバイト文字列リテラル化け回避のため)。ログ内容は同等。
  [追加] Inspector の Preview (保存前リアルタイム反映) は RenderSystem::ResolvePost が毎フレーム CameraPostFxComponent の fxStack を参照する形で実現。明示的な「ダーティフラグ」は未実装で常に AssetID 変化を検知する形。
  [スコープ外確認] コンピュート Runner 本体は sub-04 として申し送り。FxStackKind::Compute のエントリは JSON ロード・保存済み。

不安・質問:
  - Inspector の Properties ウィジェットは DragFloat / ColorEdit4 のみ実装。sub-01 スキーマの "テクスチャ選択" ウィジェット (PropValue にテクスチャ AssetID を持つ形) は sub-01 で未実装のため申し送り。
  - Preview (保存前反映) の仕様が「Play 中は毎フレームまたはダーティ時」と曖昧のため、毎フレーム AssetID 比較で対応した。セーブ前プレビューは「保存後に Runner が更新」される形なので実質 save-on-apply となっている。planner 確認推奨。
  - FxStackAsset は heap に保持せず stack ローカルでロードして ProjectEffectRunner.SetPasses に渡す設計。大量パス時のコストは問題ないと判断したが、sub-04 でコンピュート対応時に再検討余地あり。

申し送り (sub-04 以降):
  - FxStackKind::Compute の実行本体 (ProjectEffectRunner または専用 ComputeRunner)
  - PropValue へのテクスチャ AssetID variant 追加と Inspector テクスチャ選択ウィジェット
  - ShaderManager ソース取得 API (EnsureCached での動的プロパティ反映に必要)

---

## フィードバック履歴
- round 1: VERDICT REWORK — (1) Properties UI は JSON キー列挙ではなく sub-01 スキーマ駆動必須。(2) Tex2D の値・Inspector・Runner バインドが未達で受け入れ 2 / やること未充足。(3) save-on-apply プレビューは仕様「厳密でなくてよい」により受理。手動 Tint 手順をメモに残す (should)。
- round 2: VERDICT OK — スキーマ駆動 UI・Tex2D 往復／バインド・SelfTest・手動手順を確認。should: 未知 Tex2D 文字列の WARN、black/bump 専用 SRV は後続で可。
- review-1 #1/#3/#5: 差し戻し — (1) CameraOverride 時は両 Runner を ClearPasses するか Resolve/BeforePost に渡さない (SelfTest または再現手順)。(3) black/bump/gray 未実装は white フォールバック＋WARN＋コメント/docs 同期で可。(5) 手動用サンプル `*.post.hlsl` + `*.fxstack.json` を assets に 1 セット追加
- round 3: VERDICT OK — Override ClearPasses+nullptr 注入・policy SelfTest・Tex2D WARN+docs・MyTint サンプルを確認。Compute ClearPasses も同コミットで対応済み (sub-04 #1 と重複分は sub-04 で確認)
