# sub-06: サーフェスの境界余白と両面 (boundsPadding / doubleSided)

- 依存: sub-03 (review-1 #1 の差し戻し分が OK になってから)、sub-04 (review-1 #5 の差し戻し分が OK になってから)
- 状態: OK (コミット待ち)
- 往復: 1
- 出所: review-1 #3 (major、planner 宛て) / #7 (minor、planner 宛て)

## やること

spec §4.1「カリングと境界」「両面」、§4.2 `.mat.json` の `boundsPadding` / `doubleSided`、§4.3 の Inspector 追記を実装する。

- `MaterialLibrary` の横テーブルに `boundsPadding` (float [m]、欠損 0、負値は 0＋WARN) と `doubleSided` (bool、欠損 false) を読む。`Material` POD は変えない。`.mat.json` の再読込で反映
- `RenderSystem` の視錐台カリング (`RenderSystem.cpp` のステージ 2、`RenderableInFrustum` 呼び出し付近) と CSM のキャスター AABB 集約 (sceneMin / sceneMax) で、サーフェスマテリアルのアイテムはメッシュ AABB をワールド空間で各軸 `boundsPadding` 広げた箱を使う。0 なら従来と完全に同じ判定 (既定シーンのビット一致)。カリングはジョブ並列の純関数なので、余白の値は並列ステージの前に解決しておく (横テーブル参照を並列内でしない)
- `doubleSided: true` のサーフェスは、Forward の不透明・透明、Deferred のサーフェス段・透明段、ShadowPass の影エントリの全部で Cull None のラスタライザ状態を使う。描いた後は各パスの既定ラスタライザへ戻す (spec §4.1「固定スロットの復元」と同じ扱い。Wireframe 表示中の状態を壊さない)
- マテリアル Inspector: サーフェス選択時に `boundsPadding` (DragFloat、0 以上) と `doubleSided` (Checkbox) を出し、保存・JSON 往復で保持。ツールチップ (日英) で「頂点変位で形がメッシュの外へ出るなら余白を付ける (付けないとカメラ外判定で消える)」
- **(sub-05 round 3 VERDICT から移管)** `src/Editor/AssetOps.cpp` の `SurfaceShaderTemplate` (574-622 行付近) の `lit = _Tint.rgb * (shadow + rim)` は、影の中で rim が小さいと真っ黒になる。環境光 (`gAmbient`) の項を足して、生成直後のテンプレートが影の中でも黒く潰れないようにする (review-1 #6 の「テンプレートも同じ観点」の回収。sub-04 と同じファイルを触るため、sub-04 の差し戻しが OK になってからここで行う)。AssetOpsSelfTest の「テンプレートがコンパイルできる」は維持
- docs (`docs/surface-shaders-deferred-limits.md` かサーフェスの作者向け docs) に、カリングの余白と両面、裏面判定 (`SV_IsFrontFace` は渡らない。法線と視線の内積で作者が判定) を追記。テンプレートのコメントにも 1 行

## やらないこと (このサブでは)

- HLSL 側でのレンダーステート宣言
- `SV_IsFrontFace` を作者へ渡すこと (シグネチャ規約を変える)
- 自動の境界計算 (変位量をシェーダから推定する等)
- スキンメッシュのカリング変更

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/GpuResources.h/.cpp` — 横テーブルと `ParseMaterialJson`
- `src/Engine/Engine/RenderSystem.cpp` — カリング候補の構築 (`cullCands`)、ステージ 2 (`:1095-1115` 付近)、キャスター AABB
- `src/Engine/Renderer/ForwardPath.cpp`、`DeferredPath.cpp`、`ShadowPass.cpp` — Cull None のラスタライザ状態と戻し
- `src/Editor/Windows/InspectorWindow.cpp/.h`、`src/Engine/Core/LocalizationTable.inl`

## 受け入れ条件 (このサブ)

1. `boundsPadding` を付けたサーフェスは、変位で元の AABB の外へ出てもカメラに写り、影も落とす。0 は従来どおり — `--selftest` (余白付き AABB のカリング判定の単体)、reviewer rv1 の liftA 相当 (余白あり/なし) の Runtime.exe スクショ
2. `doubleSided: true` のサーフェスは裏面も描かれ、影も両面で落ちる。false は従来どおり — `--selftest` (薄板を裏から read-back) またはスクショ
3. Inspector で 2 つの欄が出て、保存で `.mat.json` に書かれ、JSON 往復で保持 — `--selftest` (往復)＋Editor スクショ
4. 既定シーンの絵・決定論が不変 — Debug/Release `--selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
5. 新規文字列が日英両方 — `tools\check_rules.ps1`

## 検証コマンド

```
tools\gen_project_files.ps1   (ファイル追加時。pwsh で実行)
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <一時> --scene <一時> [--deferred] --frames <N> --screenshot <png>
```

一時シーン作成の罠 (台帳申し送り): `.meta` の GUID は自動生成を読む、メッシュは `builtin://cube`、`--frames` は撮影フレーム (既定 60) 以上、Editor の `--project` は `project.mye.json` 必須。

## 実装メモ (coder が追記)

SELF_EVAL: sub-06 (round 1)
実装:
  - `src/Engine/Renderer/FrustumCull.h`:`AabbInFrustum`/`RenderableInFrustum`/`WorldAabb` — `worldPaddingM` 引数 (既定 0) を追加。ワールド空間の半径 (we) に一様加算するだけなので、0 のときは既存呼び出し (`RenderSystem.cpp` の他用途、`TerrainSystem.cpp`、`AssetPreviewCache.cpp` 等) と 1 ビットも変わらない
  - `src/Engine/Renderer/GpuResources.h/.cpp`:`MaterialLibrary::SurfaceMaterialSource` に `boundsPadding`/`doubleSided` を追加。`ParseMaterialJson` に読み取りを追加 (`boundsPadding` 負値は 0 にクランプ+WARN、`doubleSided` 欠損は false)。新規 `GetSurfaceBoundsPadding`/`GetSurfaceDoubleSided` (サーフェスでない = 横テーブル未登録なら 0/false)
  - `src/Engine/Engine/RenderSystem.cpp`:`CullCand` に `boundsPadding` を追加。ステージ 1 (直列収集) で `MaterialLibrary::GetSurfaceBoundsPadding` を解決してキャッシュし、ステージ 2 (並列カリング) の `RenderableInFrustum` と、不透明キュー確定時の CSM キャスター AABB 集約 (`WorldAabb`) の両方へ渡す。並列ステージの中でテーブルを引かない (spec の指示どおり)
  - `src/Engine/Renderer/ForwardPath.h/.cpp`:`rasterizerCullNone_` を追加。`DrawSurfaceItem` (色エントリ、opaque/transparent 共用) で `doubleSided` なら描画直前に Cull None を張り、`DrawItems::restoreForwardLitBindings` が既定ラスタライザ (`rasterizer_`/`rasterizerWire_`、Wireframe 込み) へ必ず戻す
  - `src/Engine/Renderer/DeferredPath.h/.cpp`:同様の `rasterizerCullNone_`。`RenderSurfaceForward` (不透明サーフェス段・速度エントリ) はアイテムごとに Cull None→描画→既定へ復元。`RenderTransparent`→`DrawSurfaceTransparentItem` (透明段・色エントリ) も同じ形
  - `src/Engine/Renderer/ShadowPass.h/.cpp`:同様の `rasterizerCullNone_` (深度バイアスは `rasterizer_` と同値)。影エントリで `doubleSided` なら描画直前に Cull None、`restoreFixedShadowSlots`(既存の b0/t0 復元ラムダ) に RS 復元を追加
  - `src/Editor/Windows/InspectorWindow.h/.cpp`:`MaterialEditState` に `boundsPadding`/`doubleSided` を追加。`LoadMaterialEdit` は負値を `(std::max)(0.0f, ...)` で丸めて読み、`MaterialEditToJson` は常に書き出す (metallic 等の既存スカラフィールドと同じ「常に書く」規約)。`DrawMaterialInspector` にサーフェス選択時だけ `DragFloat`(0 以上)+ツールチップ、`Checkbox` を追加
  - `src/Engine/Core/LocalizationTable.inl`:`Insp_MatBoundsPadding`/`Insp_MatDoubleSided`/`Insp_TipBoundsPadding` を日英で追加
  - `src/Editor/AssetOps.cpp`:`SurfaceShaderTemplate` の `lit` 式に `gAmbient` を足し、影の中 (shadow=0) かつ rim が小さいときの黒潰れを解消 (sub-05 round 3 VERDICTからの移管)。テンプレート先頭に boundsPadding/doubleSided/裏面判定の案内コメントを 1 行追加
  - `docs/surface-shaders-deferred-limits.md`:カリング余白・両面描画・`SV_IsFrontFace` 非対応の節を追加
  - 回帰テスト追加 (AGENTS.md §7):`src/Engine/Renderer/RenderSelfTest.cpp`(`AabbInFrustum`/`WorldAabb` の `worldPaddingM` 単体)、`src/Engine/Renderer/SurfaceMaterialSelfTest.cpp`(`MaterialLibrary` の boundsPadding/doubleSided 読み込み・負値クランプ・非サーフェス無効化、および `ForwardPath` の doubleSided 実描画+ラスタライザ復元を read-back で検証する新規テスト)、`src/Engine/Renderer/SurfaceDeferredSelfTest.cpp`(`ShadowPass` の doubleSided 裏面影を read-back で検証する新規テスト)

仕様との差分:
  - [追加] Inspector の `boundsPadding`/`doubleSided` は forward_lit 選択時も含めて常に `.mat.json` へ書き出す (spec §4.2 は「読み込むが効かない」としか書いておらず、保存時に常に書くかは明示していない)。既存の `metallic`/`roughness`/`emissive`/`reflectionClass` と同じ「常に書く」規約に揃えた解釈

検証:
  - `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 警告 0・エラー 0
  - `msbuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 警告 0・エラー 0 (無関係な `ProjectComputeRunnerSelfTest.cpp` の既存警告のみ)
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0、`FAIL:` 0 件 (新規テスト 3 種を含む全件 PASS)
  - `bin\x64\Release\Editor.exe --selftest` → exit 0、`FAIL:` 0 件
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → `[PASS]` (最終コードで 1 回、直前のテスト追加版で 1 回の計 2 回実行、いずれも Debug/Release 8 シーン + snapshot 往復 + time travel + rule check の 13 ジョブ全 PASS)
  - 実プロジェクト経路 (reviewer の rv1 を一時スクラッチへコピーして検証。原本は書き換えず、コピー先でのみ `.mat.json` を編集):
    - `liftA.scene.json`(変位で AABB の外へ出て消えていたシーン) の `Lift.mat.json` に `boundsPadding` を追加せずに撮影 → Forward で reviewer の `liftA.png` と同じく板が消えて空のみ (再現確認)。`"boundsPadding": 9.0` を追加して再撮影 → Forward/Deferred とも板が写るようになった (スクショで実測)。`liftB`/`liftC`(回帰チェック) は無変更で従来どおり描画された
    - 新規シーンで巨大な立方体 (`builtin://cube`、scale 20) の内側にカメラを置き `doubleSided` を検証: `false`(既定) では Cull Back で内壁が全部消え背景の空のみ、`true` にすると内壁 (固定色) が Forward/Deferred 両方で描画された
  - 未実行:Inspector の GUI 実地確認 (`boundsPadding`/`doubleSided` 欄が実際に描画されるところを Editor スクショで見る、受け入れ条件 3 の検証手段の一部)。理由は下記「不安・質問」

自己採点 (1-5):
  仕様適合: 5 — spec §4.1 (カリング/CSM 余白、両面、固定スロット復元)・§4.2 (`.mat.json` フィールド)・§4.3 (Inspector UI・テンプレート) の全項目を実装し、受け入れ条件 1・2・4・5・12・14・15 を検証手段どおり確認した (条件 3 は一部未検証、後述)
  正しさ: 4 — 描画/カリング系は SelfTest (単体+read-back) と実プロジェクトの Runtime.exe スクショの両方で実測確認した。Inspector の GUI 表示 (DragFloat/Checkbox が実際に出る) は実行時の確認をしておらず、対称的な既存フィールドと同型であることのコードレビューに留まる
  コード品質: 4 — Forward/Deferred/ShadowPass の 3 パスで「サーフェス描画直後に固定バインドへ戻す」既存パターン (review-1 #1/#2 fix) をラスタライザ状態にも一貫して適用した。3 パスそれぞれが独立して `rasterizerCullNone_` を持つ小さな重複があるが、既存コードも各パスが独立してラスタライザ一式を持つ設計なのでそれに合わせた
  テスト: 5 — 単体テスト (`FrustumCull.h` の `worldPaddingM`)、統合 read-back テスト (`ForwardPath`/`ShadowPass` の doubleSided)、`MaterialLibrary` の JSON 解析 (負値クランプ・非サーフェス無効化) を新規に追加し、Debug/Release とも全件 PASS を確認した

不安・質問:
  - Inspector の GUI 実地確認 (受け入れ条件 3 の一部) を実施していない。理由は sub-04 round 2 と同じ懸念 (同一マシンで別セッションが動いている可能性があり、`SetForegroundWindow` 系のフォーカス奪取操作を避けた) — 台帳の申し送りにある「難しければ SelfTest で代替し理由を書く」に従い、`MaterialLibrary` 側の同等ロジック (負値クランプ・非サーフェス無効化) を SelfTest で検証することで代替した。`InspectorWindow::LoadMaterialEdit`/`MaterialEditToJson` 自体の専用テストは無い (sub-04 の `metallic`/`roughness` 等の既存フィールドにも専用テストが無く、`InspectorWindow` を直接インスタンス化する SelfTest の前例も無いため、既存の検証密度に合わせた)。reviewer が実地確認する場合は `%TEMP%\mye_sub04_probe`(sub-04.md 実装メモ) と同じ手順で、任意のサーフェスマテリアルを選択し `boundsPadding`/`doubleSided` 欄が Properties の上に出ることを確認できる

触ったファイル:
  - `src/Engine/Renderer/FrustumCull.h`
  - `src/Engine/Renderer/GpuResources.h`
  - `src/Engine/Renderer/GpuResources.cpp`
  - `src/Engine/Engine/RenderSystem.cpp`
  - `src/Engine/Renderer/ForwardPath.h`
  - `src/Engine/Renderer/ForwardPath.cpp`
  - `src/Engine/Renderer/DeferredPath.h`
  - `src/Engine/Renderer/DeferredPath.cpp`
  - `src/Engine/Renderer/ShadowPass.h`
  - `src/Engine/Renderer/ShadowPass.cpp`
  - `src/Editor/Windows/InspectorWindow.h`
  - `src/Editor/Windows/InspectorWindow.cpp`
  - `src/Engine/Core/LocalizationTable.inl`
  - `src/Editor/AssetOps.cpp`
  - `docs/surface-shaders-deferred-limits.md`
  - `src/Engine/Renderer/RenderSelfTest.cpp`
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.cpp`
  - `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp`

申し送り:
  - 一時検証物は `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\sub06verify\`(reviewer の rv1 を複製し `Lift.mat.json` に `boundsPadding` を追記したもの、`doublesided.scene.json` 等の新規フィクスチャ)。reviewer の rv1 原本 (`...\scratchpad\rv1\`) は書き換えていない
  - 新規ファイルは追加していないので `tools\gen_project_files.ps1` は実行していない (build/Engine.vcxproj 等は変更なし)

## フィードバック履歴
- round 1: VERDICT OK (planner)。liftA_padded_fwd.png (余白ありで板が写る) と ds_double_fwd.png (内壁が描かれる) を目視。カリング余白は既定 0 でビット一致 (replay_verify 13/13)。boundsPadding/doubleSided を forward_lit でも常に書き出す解釈は受理 (読み込み側で効かない)。Inspector の実 GUI 確認は未実施 = reviewer round 2 へ (nit)。
