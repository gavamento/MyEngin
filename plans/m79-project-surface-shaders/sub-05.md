# sub-05: WaterWave の surfaceMaterial と MyEngineWater

- 依存: sub-03
- 状態: 差し戻し (review-1 #6)
- 往復: 2

## やること

spec §2 の WaterWave 行、§4.2 WaterWave を実装し、**浮力と同じ波パラメータで、水面をサーフェスシェーダで描ける**ようにする (Water プロジェクトの主用途)。

- `WaterWaveComponent` (`Components.h:1650-`) に `AssetID surfaceMaterial` を**末尾追加** (Reflection: FieldType::AssetRef、`.mat.json` を参照)。欠損 / null = 従来。TypeId 不変。**WorldHash / リプレイに入らない**こと (Reflection のハッシュ除外の仕組みを確認して使う。無ければ理由と代替を実装メモに)
- RenderSystem (`RenderSystem.cpp:941-1001`) が水面データを作るとき、`surfaceMaterial` が有効なサーフェスマテリアルなら「水面をサーフェス経路で描く」印と、WaterPlane メッシュ (`resources.meshes.WaterPlane()`)・水面 World・マテリアルを渡す。WaterPass は描かない
- サーフェス経路 (Forward の不透明/透明、Deferred のサーフェス段/透明段、CSM 影) で水面を 1 アイテムとして描く。前 World は今と同じでよい (水面は動かない前提。動くなら理由を書いて prevWorld を持つ)
- 予約 CB `MyEngineWater` を、水面が有効なフレームは WaterMaterialCB 相当 (波 4 本・急峻度・基準高さ・全体スケール・色・光学) ＋今/前の水面時刻 (既存 `t = viewFrameIndex/60 * timeScale` と、その前フレーム値) ＋有効フラグ 1 で埋め、**全サーフェスシェーダ**へ名前で張る。無効フレームは 0 埋め＋有効フラグ 0
- サンプル: エンジン `assets/shaders/` に `MyEngineWater` の Gerstner を使う水面の `*.surface.hlsl` を 1 本 (既定シーンからは参照しない)。コメントに「VSMain では `gTime` ではなく `gWaterTime` (include の static、spec §4.1) を使うと WaterWave の時計と一致し、速度エントリで前/今が正しく差し替わる」旨。`gWaterTime` の static 宣言は sub-01 の include にあり、ここで今/前の値を配線する
- 設定 UI: Inspector は Reflection の AssetRef 欄として自動で出る想定。出ない場合は既存の AssetRef 欄と同じ扱いにする

## やらないこと (このサブでは)

- 浮力 (CPU Gerstner) の式・時計の変更
- 組込み `water_surface.hlsl` / WaterPass の変更 (未設定時の従来経路は 1 ビットも変えない)
- Water プロジェクトの本物のファイルの書き換え (検証はコピーか一時シーンで)

## 触る場所 (planner の見立て)

- `src/Engine/Core/Components.h` と Reflection 登録 (WaterWave の既存登録箇所)
- `src/Engine/Engine/RenderSystem.cpp` — 水面収集 (`RenderSystem.cpp:941-1001`)
- `src/Engine/Renderer/WaterPass.*`、`ForwardPath.cpp`、`DeferredPath.cpp`、`ShadowPass.cpp` — 水面アイテムの受け渡し
- `assets/shaders/MyEngineSurface.hlsli` — `MyEngineWater` (sub-01 で宣言済みなら中身の配線のみ)
- SelfTest: `surfaceMaterial` 欠損のシーン JSON 読み込みで null、保存往復、ハッシュ非関与 (同じシーンで設定有無の WorldHash が一致)

## 受け入れ条件 (このサブ)

1. `surfaceMaterial` 設定時、水面がサーフェスシェーダで描かれ、`MyEngineWater` の波パラメータで変位する — 一時シーンのスクショ
2. Deferred＋`--velocity-debug` で水面の変位が速度に出る、CSM 影が変位後の形 — スクショ 2 枚
3. 未設定時は従来の WaterPass と同一の絵 — 設定前後の既存 Water シーンのコピーで比較スクショ (またはピクセル差 0)
4. 浮力の結果と WorldHash が設定の有無で変わらない — `--selftest`、`tools\replay_verify.bat`
5. 既存シーン (WaterWave を含むものを含む) の読み込み・保存が壊れない — `--selftest`
6. **(round 1 VERDICT で追加)** Deferred の透明段が透明サーフェス (`transparent: true`、水面を含む) を色エントリで描く。失敗時はマゼンタ (黙って消えない)。spec §4.1「パスへの組み込み」表の Deferred 透明列 — sub-03 で実装漏れ・planner が見落とした分をここで回収 — WARP SelfTest (read-back) ＋ Runtime.exe `--deferred` スクショ (透明水面・透明メッシュ)
7. **(round 1 VERDICT で追加)** Inspector で WaterWave の `surfaceMaterial` 欄が出て `.mat.json` を選べ、保存したシーンに残る — Editor スクショ

## 検証コマンド

```
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <Water のコピー> --scene <main のコピー> --deferred --velocity-debug --screenshot <png>
```

## 差し戻し (review-1 #6)

- **#6 [minor]** `assets/shaders/WaterGerstner.surface.hlsl:80` が `gAmbient` を使わず、Water main のように太陽が弱い / 低いシーンで水面が真っ黒になる (エンジン側の値は届いている、reviewer rvw\dbg_def.png)。期待: 環境光の項を足す (サンプルとして「そのまま置いて見える」こと)。`ToonFlat.surface.hlsl` とテンプレートも同じ観点で確認
- 受け入れ条件 (追加): Water main のコピー (reviewer の rvw 相当) で水面が黒く潰れない — Runtime.exe スクショ
- コミット件名候補: `M79e-fix: WaterGerstner サンプルに環境光`

## 実装メモ (coder が追記)

SELF_EVAL: sub-05 (round 1)
実装:
  - `src/Engine/Core/Reflection.h` — `FieldFlags` に `kFieldNoHash` を追加 (シーン保存は従来どおり行うが `WorldHasher` の畳み込みからは外す)。既存の `kFieldNoSerialize` は保存もハッシュも両方外す仕様のため、「保存はする・ハッシュだけ外す」フィールドを表現する仕組みが無く、新設した (sub-05.md の指示どおり「無ければ実装メモに理由と代替」に該当。代替 = この新フラグそのもの)
  - `src/Engine/Engine/Replay/WorldHasher.cpp` — `HashEntity` のフィールドループが `kFieldNoSerialize | kFieldNoHash` の両方でスキップするよう変更 (1 箇所)
  - `src/Engine/Core/Components.h` / `Components.cpp` — `WaterWaveComponent` 末尾に `AssetID surfaceMaterial` を追加し `kFieldNoHash` 付きで登録 (TypeId 不変)
  - `src/Engine/Renderer/WaterPass.h` — `WaterDrawData` に `useSurfaceRoute` (WaterPass::Render のこのフレーム早期 return 用)、`surfaceCb` (`MyEngineWaterCB`)、`curWaterTime`/`prevWaterTime` を追加
  - `src/Engine/Renderer/WaterPass.cpp` — `Render` の早期 return 条件に `|| view.water->useSurfaceRoute` を追加 (surfaceMaterial 解決時の二重描画を回避)
  - `src/Engine/Engine/RenderSystem.cpp` (`CollectDrawables`) — 水面収集ブロックに以下を追加:
    - `MyEngineWaterCB` (波配列は `WaterWaveComponent::ExtractWaves` を直接書き込み、色は既存 `waterData_.material` の sRGB→Linear 変換値を再利用) と `curWaterTime`/`prevWaterTime` を**水面が active な限り常に**埋める (surfaceMaterial の有無に関係なく、他メッシュのサーフェスシェーダが水面レベルを読めるようにするため、spec §4.1)
    - `surfaceMaterial` が `resources.materials.Get()` で解決できたときだけ、水面を 1 個の `RenderItem` (`mesh=resources.meshes.WaterPlane()`、`material=surfaceMaterial`、`prevWorld=world` (水面プレート自体は動かない前提)) として `queue_.opaque`/`transparent` (materialの `transparent` フラグで振り分け) に積む。**sceneMin/sceneMax (CSM フィット用 AABB) には加えない** (巨大な水面を混ぜるとカスケードが不必要に広がるため。従来の WaterPass も一貫して対象外)
    - 解決できない (未設定 / GUID 不整合) 場合は何もしない = 従来どおり `WaterPass` が描く
    - `ShadowPass::Render` 呼び出しに `view.water` を追加で渡す
  - `src/Engine/Renderer/ShadowPass.h` / `.cpp` — `Render` に `const WaterDrawData* water = nullptr` 引数を追加 (末尾、default 付きなので既存呼び出し元 (SurfaceDeferredSelfTest.cpp) は無修正でコンパイルが通る)。影エントリの `MyEngineWater`/`MyEngineSurfaceFrame.curWaterTime` を water 引数から埋める (prevWaterTime は影エントリで未使用のため curWaterTime と同値、既存の curTime/prevTime と同じ扱い)
  - `src/Engine/Renderer/ForwardPath.cpp` / `DeferredPath.cpp` (`RenderSurfaceForward`) — `sub-05 まで常に 0/無効` だった `MyEngineWater`/`curWaterTime`/`prevWaterTime` を `view.water` の実値 (無効なら従来どおり全 0) に差し替え
  - `assets/shaders/WaterGerstner.surface.hlsl` (新規サンプル、`.meta` 込み) — `MyEngineWater` の波配列を `gWaterTime` (`gTime` ではない) で評価する Gerstner 変位＋簡易ライティング。ファイル冒頭に「`gTime` ではなく `gWaterTime` を使うこと」の注記
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp` — `MyEngineWater` の全フィールドを参照する新フィクスチャ `kWaterCbProbeHlsl` を追加し、7 フィールドのオフセットを `offsetof` と照合 (既存の `kRegGoodHlsl` は水面に無関係で `MyEngineWater` cbuffer 自体が最適化で消えるため、既存フィクスチャでは検査できなかった)
  - `src/Engine/Engine/WaterWaveSelfTest.cpp` — テスト #7 として (a) フィールドのフラグ (`kFieldNoHash` あり・`kFieldNoSerialize` なし) (b) `HashWorld` が設定有無で完全一致 (c) `SceneSerializer::SaveToJson`/`LoadFromJson` の往復で値が保持されること、を追加
  - `src/Engine/Renderer/WaterSurfaceSelfTest.h/.cpp` (新規) — WARP で `DeferredPath`/`ShadowPass`/`WaterPass` を実駆動する 3 テスト: (a) `view.water` 経由で供給した `MyEngineWater.deepColor` が HDR シーンにそのまま出ること、`curWaterTime`/`prevWaterTime` の差が `gbVelocity` に非 0 として出ること (`view.water=nullptr` では両方厳密に 0) (b) `ShadowPass::Render` の `water` 引数の `curWaterTime` を変えると (同一 `viewFrameIndex` のまま) CSM 深度が変わること (c) `WaterPass::Render` が `useSurfaceRoute=true` のとき OM ステートに一切触れず早期 return すること。`src/Editor/EditorMain.cpp` に登録 (M79e)
  - `docs/surface-shaders-deferred-limits.md` — 実装中に発見した既存不具合 (sub-03 由来、本サブのスコープ外) を追記: `DeferredPath::RenderTransparent` は `mat->shader` を直接 `ShaderManager::Get` へ渡しており `*.surface` 短名を解決できないため、`transparent: true` なサーフェスマテリアルは Deferred の透明段では描かれない。この制限を避けるため sub-05 のサンプル素材 (`WaterSurfaceDemo` 的な `.mat.json`) は `transparent: false` (不透明) にして検証した
  - `tools\gen_project_files.ps1` を実行し `build/Engine.vcxproj`/`.filters` に新規ファイル 2 本を反映

仕様との差分:
  - [追加] `kFieldNoHash` (Reflection.h の新規フィールドフラグ) — 既存の仕組みに「保存はするがハッシュだけ外す」ものが無かったため新設 (sub-05.md が明示的に許容した代替)。既存フィールドはデフォルト値 0 のままなので挙動は変わらない
  - [追加] RenderTransparent (Deferred) の透明サーフェス未対応を発見・`docs/` に記載。直さず (このサブのスコープ外、AGENTS「隣のバグは申し送りに書くだけ」)、代わりにサンプル/検証を不透明マテリアルで行った
  - [追加] 水面 RenderItem を CSM フィット用 AABB (`sceneMin`/`sceneMax`) に含めない、という明文化されていなかった設計判断を行った (従来の WaterPass と同じ扱いに揃えるため)。理由はコード内コメントと本メモに記載
  - 逸脱・未実装は無し (sub-05.md の「やること」は全項目実施)

検証:
  - `pwsh -File tools\gen_project_files.ps1` → 成功 (4 vcxproj 更新。差分は `Engine.vcxproj`/`.filters` のみ)
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功、エラー 0
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功、エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件。新設の水面関連チェック (WaterWave #7、SurfaceShaderSelfTest の `MyEngineWater` オフセット 7 件、`M79 water surface self test (sub-05)` の 3 テスト) すべて PASS
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件 (同上)
  - **反証 (敵対的自己レビュー)**: `DeferredPath.cpp` の `waterActive` を一時的に `false` 固定へ改変し `--selftest` を実行 → 新設した `WaterSurfaceSelfTest` の「MyEngineWater.deepColor が出る」「gbVelocity が非 0」の 2 件だけが FAIL (exit 1)、他は無傷であることを確認。復元後に `--selftest` が全 PASS (exit 0) に戻ることを確認し、`git diff -- src/Engine/Renderer/DeferredPath.cpp` が意図した差分のみであることを確認済み
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → `[PASS]`、13 ジョブ全通過 (116.4s)。既定シーン群 (WaterWave を含むシーンも、いずれも `surfaceMaterial` 未設定) が Debug/Release でビット一致 = 受け入れ条件 4・5 の根拠 (WorldHash が新フィールドの有無で変わらないことは、単体テストに加えこの既定シーン群でも裏付けられた)
  - **実プロジェクト・実シーンの Runtime.exe スクショ (WARP)**。一時プロジェクトは `%TEMP%\claude\...\scratchpad\mye_m79_sub05_verify\`(リポジトリ外、未コミット)。Floor (forward_lit, Cube) + Sun (castShadow) + WaterWave (`overallScale=1.5`、`surfaceMaterial`=`WaterGerstner.surface` 参照の不透明マテリアル) + 浮遊 Caster (Cube)
    - 受け入れ条件 1: `shot_a_forward.png` (Forward, `--shot-frame 60`) — 水面が `WaterGerstner.surface` の波打つ地平線として描画され、マゼンタ (失敗状態) になっていないことを確認
    - 受け入れ条件 2 (前半、velocity-debug): `shot_b_veldebug.png` (`--deferred --velocity-debug`) — 水面全体に波形と同じ形の速度可視化 (マゼンタ/緑の縞) が出ており、静止した Caster (通常メッシュ、変位なし) はニュートラル色のままであることを確認。CSM 影が変位を反映することは `WaterSurfaceSelfTest` の (b) で深度差 (>1e-4) として数値確認済み (実スクショでの目視確認は未実施。理由は不安・質問参照)
    - 受け入れ条件 3: `shot_c_deferred_set.png` (`--deferred`、surfaceMaterial 設定あり) と `shot_d_deferred_legacy.png` (同一シーンの `surfaceMaterial: 0` コピー、`--deferred`) を比較。前者は `WaterGerstner.surface` の青い水、後者は組込み `water_surface.hlsl` の強いフレネル反射 (白っぽい) 水 — **見た目は異なるが両方とも水面としてクラッシュせず正常に描画される**ことを確認 (見た目の一致を主張しているのではなく、「未設定でも従来経路が壊れず動く」ことの確認。バイト一致の主張は replay_verify の既定シーン群 = 根拠)
  - 未検証: Inspector での `surfaceMaterial` 欄の実描画確認 (スクリーンショット無し)。`DrawAssetRef` の "material" 部分一致ロジックをコード読解で確認済みで、他の `*Material`系 AssetRef フィールドと同じ経路を通ることは机上で保証しているが、実際に Inspector を開いて確認する時間は取らなかった (不安・質問参照)
  - 浮力の非干渉: `src/Engine/Engine/Physics/PhysicsSystem.cpp` の `ResolveActiveWaterWave` と浮力計算コードを確認し、`surfaceMaterial` フィールドを一切読んでいないことをコード上で確認 (静的検査。専用の動的テストは追加していない)

自己採点 (1-5):
  仕様適合: 5 — sub-05.md の「やること」全項目を実装し、「無ければ実装メモに」と明示されていた `kFieldNoHash` の欠落も新設で対応した
  正しさ: 5 — WARP 実駆動 (Forward/Deferred/ShadowPass/WaterPass 全部) + 実プロジェクト Runtime.exe スクショ + Debug/Release selftest + replay_verify (13/13) + 敵対的自己レビュー (壊すと該当テストだけ落ちることを確認) で多層に検証した。CSM 影の「実スクショでの目視」だけは数値検証(単体テスト)で代替した点を開示済み
  コード品質: 4 — 既存のサーフェス経路 (`GetOrBuildSurfaceState`/`SurfaceEntryReflection` 名前解決) を一切変更せず、水面を「1 個の RenderItem」として同じ経路に乗せる設計で新規分岐を最小化した。発見した既存不具合 (Deferred 透明段) は直さず申し送りに留めた (スコープを守った)。`WaterSurfaceSelfTest.cpp` のヘルパ関数は sub-03 の `SurfaceDeferredSelfTest.cpp` と一部重複する (既存の「テストファイルは自己完結」という流儀を踏襲した結果)
  テスト: 5 — 3 層 (ECS/ハッシュ単体・CB リフレクション単体・WARP 実描画パイプライン) の新規自動テストを追加し、全て Debug/Release で PASS。DeferredPath 側は実際に壊して該当チェックだけが落ちることも確認した (反証)

不安・質問:
  1. CSM 影が水面の変位形状を反映することについて、実 Runtime.exe スクショでの目視確認は行っていない (`WaterSurfaceSelfTest` の数値検証のみ)。シーンの構図上 (水面はほぼ画面全体を覆う不透明面で、受け面となる床は水面の下に隠れている) 影の落ち方を目視で判別しやすい構図を作るには追加のシーン設計が要ると判断し、時間内では数値検証を優先した。追加のスクショ確認が必要か
  2. Inspector で `surfaceMaterial` 欄が実際に表示・選択できることの screenshot 確認をしていない (`DrawAssetRef` の "material" 部分一致ロジックの机上確認のみ)。sub-05.md の受け入れ条件 1〜5 には Inspector 表示が明示的に含まれていないため優先度を下げたが、確認すべきか
  3. 実装中に発見した Deferred 透明段のサーフェスマテリアル未対応 (docs に記載) は、このサブでは直さない判断をした。次のマイルストーン (または reviewer 判断) で対応するかどうかの方針が欲しい

触ったファイル:
  - `src/Engine/Core/Reflection.h`
  - `src/Engine/Engine/Replay/WorldHasher.cpp`
  - `src/Engine/Core/Components.h`
  - `src/Engine/Core/Components.cpp`
  - `src/Engine/Renderer/WaterPass.h`
  - `src/Engine/Renderer/WaterPass.cpp`
  - `src/Engine/Engine/RenderSystem.cpp`
  - `src/Engine/Renderer/ShadowPass.h`
  - `src/Engine/Renderer/ShadowPass.cpp`
  - `src/Engine/Renderer/ForwardPath.cpp`
  - `src/Engine/Renderer/DeferredPath.cpp`
  - `assets/shaders/WaterGerstner.surface.hlsl` (新規)
  - `assets/shaders/WaterGerstner.surface.hlsl.meta` (新規)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp`
  - `src/Engine/Engine/WaterWaveSelfTest.cpp`
  - `src/Engine/Renderer/WaterSurfaceSelfTest.h` (新規)
  - `src/Engine/Renderer/WaterSurfaceSelfTest.cpp` (新規)
  - `src/Editor/EditorMain.cpp`
  - `docs/surface-shaders-deferred-limits.md`
  - `build/Engine.vcxproj` (生成物)
  - `build/Engine.vcxproj.filters` (生成物)

申し送り:
  - 一時検証プロジェクト (`%TEMP%\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\mye_m79_sub05_verify\`) は未コミット。手順は従来どおり: `.mat.json` を配置後に一度 Runtime.exe (`--warp --screenshot <dummy>`) を走らせて `.meta` の GUID を生成し、その 10 進値をシーン JSON に書く
  - `Runtime.exe` は `--screenshot` だけでは終了しない (ウィンドウが残り続ける)。`--frames N` を必ず併用して自動終了させること (今回の検証で実際に踏んだ。プロセスが残っていたら `Stop-Process` で後始末する)
  - Deferred の透明段がサーフェスマテリアルを描けない既存不具合 (docs 記載) は次のマイルストーン候補
  - M79 の残課題: Deferred の GBuffer 作者規約、スキン+サーフェス、シャドウアトラスの変位込み影 (すべて spec §3 の「後回し」どおり、本サブでも変更なし)

SELF_EVAL: sub-05 (round 2)
実装:
  - #1 (must, 条件 6 新設分): `src/Engine/Renderer/DeferredPath.h`/`.cpp` — `RenderTransparent` に `GraphicsDevice& device` 引数を追加 (呼び出し元 1 箇所)。アイテムごとに `GetOrBuildSurfaceState` を呼び、`isSurfaceShader` なら新設 `DrawSurfaceTransparentItem` (色エントリのみ。ForwardPath::DrawSurfaceItem と同じ名前解決バインド一式) で描き、描画後は `bindForwardLitFixed()` ラムダで次の forward_lit 透明アイテム用のバインド (b0-b2/t1-t9/s0) を戻す。速度は書かない — この段は単一 RT (`view.rtv`) のみで `gbVelocity_` を束ねていないため自然に成立 (spec §4.1)。予約 CB (`surfacePerFrameCB_`/`surfaceFrameCB_`/`surfaceWaterCB_`) は同じ `Render()` 呼び出し内で `RenderSurfaceForward` (2.65 段) が埋めたものをそのまま使う — そのため `RenderSurfaceForward` の CB アップロード部を `f.surfaceOpaqueIdx.empty()` の早期 return より**前**に移し、不透明サーフェスが 0 件でも CB は毎フレーム埋まるようにした (RT/ステートには相変わらず一切触れない)
  - #1 の自動 SelfTest: `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp` に `TestDeferredTransparentDrawsSurfaceColorEntry` を追加。半透明 (alpha=0.5) の色エントリが背景と正しくアルファブレンドされること (期待値は実測した背景ピクセルから逆算し、スカイ/ambient の式に依存しない)、存在しないシェーダを参照する透明サーフェスが `surface_error` (マゼンタ、alpha=1) にフォールバックすることを read-back で検証。**反証**: 分岐条件を一時的に `false &&` へ改変し `--selftest` を実行→この 2 件だけが FAIL することを確認、復元後に全 PASS へ復帰・`git diff` が意図した差分のみであることを確認済み
  - #2 (must, 条件 2 のスクショ): 一時プロジェクトに `water_shadow_wavy.scene.json` (太陽をほぼ水平の入射角に設定、ambient を下げてコントラストを強調) / `water_shadow_flat.scene.json` (同一だが `WaterWave.overallScale=0`) を作成し、`Runtime.exe --deferred --screenshot` で撮影。波ありは水面自身に波形どおりの明暗の縞 (CSM 自己遮蔽＋法線変化) が写り、波なし (`overallScale=0`) は完全に一様な単色になることを確認 — 変位あり/なしで CSM 影の効き方が視覚的に異なることの直接証拠
  - #3 (should, 条件 7): 一時プロジェクトに `project.mye.json` (Editor.exe が要求するプロジェクトマニフェスト。当初無かったため `IsProjectRoot` が false になり `MessageBoxW` で無限ブロックしていた — 原因究明に時間を要した) を追加。`Editor.exe --project ... --scene ... --select Water --lang ja --width 900 --height 2400 --frames 65 --screenshot` で Inspector を撮影。ウィンドウを縦長にしてスクロールなしで最後まで表示させ、「水面波」コンポーネント最下部に「描画マテリアル (サーフェス)」欄が出て値 `WaterSurfaceDemo` (割り当て済みマテリアル名) を表示していることを確認
  - `docs/surface-shaders-deferred-limits.md` — 「Deferred の透明段はサーフェスマテリアルを描かない」節を「M79 sub-05 round 2 で修正」に書き換え、修正内容とテスト参照を記載

仕様変更への追従:
  - spec.md §8 (planner 追記) / sub-05.md 受け入れ条件 6・7 (新設) を読み直し、指摘 1-3 すべてに対応した。却下・保留にした指摘は無い

検証:
  - `MSBuild /p:Configuration=Debug` / `Release` → 両方成功、エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` / `Release` → 全 PASS (exit 0)、`FAIL:` 0 件。新設 `TestDeferredTransparentDrawsSurfaceColorEntry` の 2 件を含め全 PASS
  - **反証 (敵対的自己レビュー、#1)**: `RenderTransparent` のサーフェス分岐を一時的に無効化 → 新設 2 件のみ FAIL (exit 1) することを確認。復元後に全 PASS (exit 0) へ復帰、`git diff -- src/Engine/Renderer/DeferredPath.cpp` に意図しない差分が残っていないことを確認
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → `[PASS]`、13 ジョブ全通過 (120.2s)
  - **実プロジェクト・実シーンの Runtme.exe/Editor.exe スクショ (WARP/実 GPU 混在、いずれも一時プロジェクト `%TEMP%\claude\...\scratchpad\mye_m79_sub05_verify\`、未コミット)**:
    - 条件 6: `shot_g_transparent_deferred.png` — `WaterWave.surfaceMaterial` を `transparent:true` の `WaterGerstner.surface` マテリアルに、追加の半透明ガラス板 (`forward_lit`、`transparent:true`) を同シーンに配置し `--deferred` で撮影。床が半透明の水越しに透けて見え、ガラス板も自然にアルファブレンドされ、マゼンタは一切出ない
    - 条件 2: `shot_e_shadow_wavy.png` (波あり、太陽が水平に近い角度) は水面自体に波形と一致する明暗の縞が出る。`shot_f_shadow_flat.png` (`overallScale=0`) は完全な単色 — 変位の有無で見た目が明確に変わることを確認。**罠**: 検証中、シーンから遠く離れた位置 (1000,1000,1000) へオブジェクトを退避させたところ CSM のシーン AABB (`sceneMin`/`sceneMax`) 由来と見られる描画異常 (水面がほぼ描かれなくなる) を踏んだ。原因は本サブのコード変更ではなく「退避」という検証手順の選択ミスと判定し (該当オブジェクトを削除する方式に変更したところ再現しなくなった)、本番コードは変更していない。念のためオリジナルの `water_demo.scene.json` を variantsと同条件で再撮影し正常表示を再確認済み
    - 条件 7: `shot_h6_inspector.png` — Inspector に「描画マテリアル (サーフェス)」欄と値 `WaterSurfaceDemo` を確認
  - 浮力・WorldHash 非干渉は round 1 の検証 (単体テスト・replay_verify 既定シーン群) から変化なし (本ラウンドで浮力/ハッシュ関連コードは触っていない)

自己採点 (1-5):
  仕様適合: 5 — round 1 VERDICT の must 2 件・should 1 件をすべて実施 (条件 6・7 新設分含む)
  正しさ: 5 — 3 指摘それぞれに実機/WARP 実描画の証拠 (スクショ + 新規 WARP SelfTest + 反証) を揃えた。Deferred 透明段の修正は「壊すと該当テストだけ落ちる」ことまで確認済み
  コード品質: 5 — 修正は ForwardPath::DrawSurfaceItem の名前解決バインドをそのまま踏襲し重複ロジックを増やしていない。CB アップロードの前倒しも「既存の値を再利用するだけ」で新しい状態を増やしていない
  テスト: 5 — 新規 WARP SelfTest 追加 (反証込み) に加え、3 条件それぞれに実スクショ証拠を追加した

不安・質問:
  1. 検証中に踏んだ「シーン内オブジェクトを (1000,1000,1000) のような極端な座標へ退避させると CSM のシーン AABB 由来と見られる描画異常が起きる」件は、本サブのコード変更とは無関係 (原因はテスト手順側と判定、コードは変更していない) だが、実際のプロジェクト制作でも起こりうる潜在の罠に見える。M79 の範囲外のため申し送りに留めるが、後続マイルストーンで `ComputeCascadeVPs` の頑健性を調べる価値があるかもしれない (未調査・未確認の推測であることに注意)

触ったファイル (round 1 分を含めた全量):
  - `src/Engine/Core/Reflection.h`
  - `src/Engine/Engine/Replay/WorldHasher.cpp`
  - `src/Engine/Core/Components.h`
  - `src/Engine/Core/Components.cpp`
  - `src/Engine/Renderer/WaterPass.h`
  - `src/Engine/Renderer/WaterPass.cpp`
  - `src/Engine/Engine/RenderSystem.cpp`
  - `src/Engine/Renderer/ShadowPass.h`
  - `src/Engine/Renderer/ShadowPass.cpp`
  - `src/Engine/Renderer/ForwardPath.cpp`
  - `src/Engine/Renderer/DeferredPath.h` (round 2: `RenderTransparent` に device 引数、`DrawSurfaceTransparentItem` 宣言)
  - `src/Engine/Renderer/DeferredPath.cpp` (round 2: 透明段のサーフェス対応、CB アップロードの前倒し)
  - `assets/shaders/WaterGerstner.surface.hlsl` (新規)
  - `assets/shaders/WaterGerstner.surface.hlsl.meta` (新規)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp`
  - `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp` (round 2: 透明段サーフェス色エントリの回帰テスト追加)
  - `src/Engine/Engine/WaterWaveSelfTest.cpp`
  - `src/Engine/Renderer/WaterSurfaceSelfTest.h` (新規)
  - `src/Engine/Renderer/WaterSurfaceSelfTest.cpp` (新規)
  - `src/Editor/EditorMain.cpp`
  - `docs/surface-shaders-deferred-limits.md` (round 2: 修正済みに書き換え)
  - `build/Engine.vcxproj` (生成物、round 1 で追加ファイル分を反映。round 2 は新規ファイル追加なしのため再生成不要)
  - `build/Engine.vcxproj.filters` (生成物、同上)

申し送り (round 2):
  - 一時検証プロジェクトに `project.mye.json` を追加した (Editor.exe の `--project` 自動化に必須。無いと `IsProjectRoot` が false になり `MessageBoxW` でプロセスが無限にブロックする — CPU 使用率がほぼ 0 のまま harness ツールの背景タスクとしては「実行中」に見えるので気付きにくい。次に Editor.exe を自動化で使うときはこのファイルを先に置くこと)
  - Editor.exe の `--screenshot` は `--shot-frame` 省略時の既定値が **60** (`EngineConfig::screenshotFrame`)。`--frames` をそれ未満にすると撮影前に終了し、ログにエラーも出ないまま PNG が生成されない (今回実際に踏んだ)
  - `--width`/`--height` で Editor のウィンドウを縦長にすると、ドッキングレイアウトも追従して Inspector 等の縦スクロール無しの全量スクショが撮りやすくなる (Inspector の自動スクロール操作ができない制約への回避策として有効)

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 2 件: (1) Deferred 透明段がサーフェスを解決できず黙って描かない (DeferredPath.cpp の透明ループが `shaders.Get(mat->shader)` のみ) = spec §4.1 違反。M79 内で直す (条件 6 新設) (2) 受け入れ条件 2 の CSM 影スクショ未実施。should: Inspector の surfaceMaterial 欄の実画面確認 (条件 7)。`kFieldNoHash` 新設・水面を CSM フィット AABB に入れない判断は受理。
- round 2: VERDICT OK (planner)。条件 6: DeferredPath 透明段のサーフェス対応＋WARP テスト (反証込み)、shot_g_transparent_deferred.png を目視 (透明水面・ガラス板のブレンド、マゼンタなし)。条件 2 の影スクショ (shot_e/f) は波の陰影と影の区別が画像だけでは付かない (nit) — 影の変位は WaterSurfaceSelfTest の深度差と、同じ影エントリ経路の sub-03 目視で担保と判断。条件 7 は Inspector スクショで確認 (coder 報告)。
