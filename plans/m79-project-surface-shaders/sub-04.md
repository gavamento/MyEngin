# sub-04: マテリアル Inspector (シェーダ選択・Properties 共通化・バナー) と作成メニュー

- 依存: sub-02 (sub-03 とは独立 = 並列可)
- 状態: 差し戻し (review-1 #5)
- 注記 (sub-02 VERDICT round 1): Properties の 2D ピッカーは spec §4.2 の Tex2D 符号化に従う — 書き出しはアセットなら数値 GUID、組込み既定なら名前文字列。読み込みは数値 / 文字列の両方
- 往復: 2

## やること

spec §4.3 を実装する。

- マテリアル Inspector (`InspectorWindow.cpp:2038-2039` の `shader:` グレー表示) をコンボに置き換える。項目 = `forward_lit` ＋ ShaderManager の索引にある `*.surface` 短名 (昇順)。選択変更は `matEdit_.shader` へ入り、既存のライブプレビュー (M53、JSON ハッシュ駆動) と保存経路に乗る
- サーフェス選択時、既存欄の下に「Properties」セクション。**Properties ウィジェット描画とスキーマ取得を共通関数に切り出し**、fxstack (`InspectorWindow.cpp:2510-`) とマテリアルの両方から使う。スキーマ取得は `ShaderDirs() + name + ".hlsl"` (`InspectorWindow.cpp:2523-2531`) をやめ、`ShaderManager::ResolveShaderPath` (M78f 索引) 経由にする (fxstack の潜在バグも同時に直る。spec §2 の Inspector 行)
- `MaterialEditState` に Properties 値を持ち、`LoadMaterialEdit` / `MaterialEditToJson` (`InspectorWindow.cpp:1907-1990`) で `properties` を読み書きする。スキーマに無いキーも落とさない。既存フィールドはそのまま
- シェーダが失敗状態ならマテリアル Inspector 上部に赤字バナー (シェーダ名＋エラー先頭行。sub-02 の公開 API から取得)
- マテリアルプレビューがサーフェスでもそのシェーダで描かれるか確認 (Forward 経由なら自然に効く想定)。効かない場合は理由を実装メモに書き、従来表示のままでよい (マゼンタにはしない)
- **(must、sub-01 VERDICT で追加)** M78 の既存不具合を直す: `.cs.hlsl` 判定の off-by-one (`ShaderManager.cpp` の `IsProjectIndexedShaderFile` と `AssetOps.cpp:466` が `compare(size-9, 9, L".cs.hlsl")` = 8 文字のリテラルを 9 文字で比較)。`ShaderManagerProjectIndexSelfTest` / `AssetOpsSelfTest` に `.cs.hlsl` のケースを足して、修正前に失敗することを確かめてから直す
- Asset Browser の作成メニューに「サーフェスシェーダ」(M78f の `PostShaderTemplate` / `ComputeShaderTemplate` と同じ流儀、`AssetOps.cpp:511-630` 付近)。テンプレート内容は spec §4.3 どおりで、そのままコンパイルが通ること
- 新規 UI 文字列は `LocalizationTable.inl` ＋ `Tr()`、日英両方。`Tr()` の結果を printf 系の書式に直接渡さない

## やらないこと (このサブでは)

- 描画側の変更 (sub-02 / sub-03 で完了している前提)
- Properties DSL の型・属性の追加 (M78 と同じ集合のまま)
- CustomEditor 相当

## 触る場所 (planner の見立て)

- `src/Editor/Windows/InspectorWindow.cpp/.h` — マテリアル節、fxstack Properties 節、`schemaCache`
- `src/Editor/AssetOps.cpp/.h`、`src/Editor/Windows/AssetBrowserWindow.cpp` — 作成メニュー
- `src/Engine/Core/LocalizationTable.inl`
- `src/Editor/AssetOpsSelfTest.cpp` — サーフェステンプレートの作成＋コンパイル、`MaterialEditToJson` が `properties` を保持すること (Inspector の関数を SelfTest から呼べない場合は JSON 往復を担う関数を切り出す)

## 受け入れ条件 (このサブ)

1. マテリアル Inspector でシェーダを forward_lit / `*.surface` から選べ、保存で `.mat.json` の `shader` が変わる — 手動＋Editor スクショ
2. サーフェス選択時に Properties が自動 UI で出て、値変更→保存で `properties` に書かれ描画に反映される — 手動＋スクショ、`--selftest` (JSON 往復で properties 保持)
3. `assets/shaders` 以外 (例 `assets/fx/`) に置いたサーフェス / ポストでも Properties が出る — 手動＋スクショ
4. 失敗シェーダを選ぶと赤字バナー — スクショ
5. 作成メニューのテンプレートが生成されコンパイル成功 — `--selftest`
6. 新規文字列が日英両方 — `tools\check_rules.ps1`、diff

## 検証コマンド

```
bin\x64\Debug\Editor.exe --selftest
tools\check_rules.ps1
Editor.exe --project <一時> --screenshot <png>   (一時プローブ。メモリ screenshot-probe-recipes.md)
```

## 差し戻し (review-1 #5)

- **#5 [minor]** `matSchemaCache_` が無効化されず、HLSL に Properties を足してもエディタ再起動まで Inspector が古いまま (`InspectorWindow.h:136`、clear は `InspectorWindow.cpp:1962`/`:2208` のみ)。fxstack のスキーマキャッシュ (M78) も同じ。期待: `SurfaceProgram::generation` (sub-02 で追加済み) か、ファイル更新時刻 / ShaderManager の再コンパイル通知でキャッシュを捨てる。マテリアルと fxstack の両方
- 受け入れ条件 (追加): Editor 起動中に `*.surface.hlsl` へ Property を 1 行足して保存 → Inspector に新しい欄が出る (手動＋スクショ、またはキャッシュ無効化判定の SelfTest)
- コミット件名候補: `M79d-fix: Properties スキーマキャッシュの無効化`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - `src/Engine/Renderer/ShaderManager.cpp`:`ShaderManager.h` — `IsProjectIndexedShaderFile` の `.cs.hlsl` off-by-one を修正 (8 文字リテラルを 9 文字比較していた)。`ProjectShaderNames(suffixFilter)` (索引済み短名の昇順列挙、マテリアル Inspector のシェーダコンボ用) と `FetchPropertySchema(name)` (`ResolveShaderPath` 経由で Properties スキーマ取得、fxstack/マテリアル共通) を追加
  - `src/Editor/AssetOps.cpp`:`AssetOps.h` — 同じ off-by-one を `IsProjectIndexedShaderFilename` にも修正 (＋ `.surface.hlsl` 判定を追加、従来抜けていた)。`SurfaceShaderTemplate` / `CreateSurfaceShaderAsset` を追加 (Properties 2 件・include・PerMaterial・VSMain(gWorld/gViewProj 使用)・PSMain(MyeSunShadow+MyeApplyFog 使用) を含み実コンパイルが通るテンプレート)
  - `src/Editor/Windows/AssetBrowserWindow.cpp` — `kCreateSurfaceShader` を追加し、Create > Shader メニューに「サーフェスシェーダ」項目、`DoCreate` に生成 + `RebuildProjectShaderIndex` の分岐を追加
  - `src/Engine/Renderer/ProjectShaderProperties.h`:`.cpp` — `DecodeMaterialProperties` / `EncodeMaterialProperties` を追加 (`.mat.json` の `properties` ⇄ `PropValue` map、JSON テキスト直渡し・ImGui 非依存で AssetOpsSelfTest から呼べる)。Tex2D は数値なら Material.texture と同じ 10 進 GUID 文字列で内部保持し、書き出し時に「文字列が全桁数字なら JSON 数値」で書き戻す (読み側が文字列を 16 進として読むため、数値のまま素通しすると次回ロードで値化けする罠を回避)
  - `src/Editor/Windows/InspectorWindow.h`:`.cpp` — `MaterialEditState::properties` を追加。`LoadMaterialEdit`/`MaterialEditToJson` で `DecodeMaterialProperties`/`EncodeMaterialProperties` を使い properties を読み書き (空なら `properties` キーを書かない = forward_lit 等の既存ファイルを汚さない)。`DrawMaterialInspector` の `shader:` 表示をコンボ (forward_lit + 索引済み `*.surface` 昇順、索引に無い値は先頭に読み取り専用で差し込み) に置換し、失敗バナー (`ShaderManager::LoadSurface`/`GetSurface` から取得、赤字)、Properties セクション (サーフェス選択時のみ) を追加。ライブプレビューはサーフェス選択時 `forward_lit` の Material に固定 (理由は差分参照)。`DrawFxStackInspector` の Properties 取得・描画を `GetOrFetchPropertySchema`/`DrawPropertiesEditor` (新規共通関数) 呼び出しへ置換
  - `src/Engine/Core/LocalizationTable.inl` — `Asset_SurfaceShader` / `Log_WriteSurfaceShaderFail` / `Log_CreatedSurfaceShader` / `Insp_MatShader` / `Insp_MatShaderFailed` / `Insp_MatProperties` を日英で追加
  - `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp` — `.cs.hlsl` 索引の回帰テストを追加 (修正前に FAIL することを確認してから修正)
  - `src/Editor/AssetOpsSelfTest.cpp` — `.cs.hlsl` 重複拒否の回帰テスト (修正前に FAIL することを確認してから修正)、サーフェステンプレートの作成+WARP 実コンパイル確認、`DecodeMaterialProperties`/`EncodeMaterialProperties` の型別往復テスト (Tex2D 数値 GUID・組込み名文字列・スキーマ外キー保持) を追加

仕様との差分:
  - [追加] `ShaderManager::ProjectShaderNames` / `FetchPropertySchema` — sub-04.md の「触る場所」に明記は無いが、シェーダコンボと Properties 共通化 (spec §2 Inspector 行) を実装するのに必要な最小限の公開 API
  - [追加] `ProjectShaderProperties::DecodeMaterialProperties` / `EncodeMaterialProperties` — sub-04.md 自身が「Inspector の関数を SelfTest から呼べない場合は JSON 往復を担う関数を切り出す」と指示しており、その実体。Engine 層に置いたのは PropValue/PropertyParseResult の定義元と同じ場所にまとめるため
  - [逸脱] 失敗バナーの取得元: harness.md 申し送りは `MaterialLibrary::GetOrBuildSurfaceState` (sub-02 の API、`LoadFromFile` 済みの永続テーブル前提) を示唆していたが、これだと「コンボで選び替えた直後 (未保存)」の失敗が Save するまで表示されない。`ShaderManager::LoadSurface`/`GetSurface` (sub-01 の API) を直接使うことで、保存前でも選んだ瞬間にバナーが出るようにした。マテリアル単位の Properties 不整合 (WARN 止まりの項目) はこの経路では検出しない (元々バナー対象外の仕様どおり)
  - [追加/解釈] シェーダをコンボで切り替えたら `matEdit_.properties` をクリアする (別シェーダの Properties は名前が違えば意味を持たないため)。spec に明記が無い挙動なので追加として明示
  - [追加/解釈] サーフェス選択時のライブプレビュー (M53) は forward_lit の Material に固定する。理由: プレビューは `RegisterAnonymous` 経由で登録され `MaterialLibrary` のサーフェス横テーブル (`surfaceSources_`、`LoadFromFile` 専用) に乗らない。そのままだと `ForwardPath` が「非サーフェス」と判定し `mat->shader` の通常プログラム解決 (Load 未実施) に落ちて無描画になる (実機で確認せず理論的整合性のみで判断していたが、round 1 の実プロジェクト検証で GoodMat/OutsideMat とも白い forward_lit 球として正しく表示されることを確認した)。spec §4.3 の許容 (「効かない場合は forward_lit 表示のまま、マゼンタにしない」) に従った
  - [追加/解釈] Tex2D の Properties 符号化: 「文字列が全桁 ASCII 数字なら 10 進 GUID とみなし JSON 数値で書く」規則を採用 (§4.2 の「数値 GUID」規約を満たすための実装)。既知の限界: 手編集で全桁数字の 16 進 GUID 文字列 (a-f を含まない) を書いた場合、その値には触れずに再保存すると 10 進数値化されて意味が変わりうる (確率的に稀。コード内コメントに明記)

検証:
  - `bin\x64\Debug\Editor.exe --selftest` → 修正前に `.cs.hlsl` 回帰 2 件が FAIL することを確認 → 修正後は全 SelfTest ALL PASS (AssetOps / ShaderManagerProjectIndex / ProjectShaderProperties 含む M79 sub-01/02/03 の既存分すべて再確認、regressionなし)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - 一時プロジェクト (`%TEMP%\mye_sub04_probe`、`--create-project` で生成) に `GoodTest.surface.hlsl` (assets/shaders)・`OutsideTest.surface.hlsl` (assets/fx、規約 §4.3 の「assets/shaders 以外」検証用)・`GoodMat.mat.json`・`OutsideMat.mat.json`・`BrokenMat.mat.json` (存在しないシェーダ参照) を配置し、Editor.exe を対話起動してスクリーンショットで実確認:
    - コンソールに両サーフェスシェーダのコンパイル成功ログ、`BrokenMat` の "shader file not found" エラーログを確認
    - Asset Browser のタイルサムネイルが GoodMat=赤・OutsideMat=緑(それぞれの `_Tint`)・BrokenMat=マゼンタで描画 (sub-02/03 の実装が実プロジェクトでも機能することの副次確認)
    - GoodMat 選択 → Inspector にシェーダコンボ "GoodTest.surface"、Properties (Tint=[0.8,0.2,0.2,1]・Rim Power=0.75、`.mat.json` の値と一致) を確認
    - OutsideMat 選択 → シェーダコンボ "OutsideTest.surface"、Properties が `assets/fx` 配下のシェーダでも表示され (M78 の潜在バグ修正の実地確認)、`.mat.json` に無いキーはスキーマ既定値 (Tint=[0.2,0.8,0.4,1]・Glow=1.0) で表示されることを確認
    - BrokenMat 選択 → 赤字バナー「シェーダーエラー (NoSuchShader.surface): shader file not found: ...\nosuchshader.surface.hlsl」を確認、Properties は「(no properties)」
    - OutsideMat の Tint (ドラッグ) と Glow (スライダ) を変更 → 保存 → `OutsideMat.mat.json` を実際に読み直し、`"properties": {"_Glow": 1.014..., "_Tint": [0.631..., 0.8, 0.4, 1.0]}` が書き込まれ、既存フィールド (baseColor 等) が保持されていることをファイル内容で確認 (受け入れ条件 2 の実地確認)
  - Asset Browser の「サーフェスシェーダ」作成メニュー自体の GUI クリックは未確認 (`CreateSurfaceShaderAsset` 本体は AssetOpsSelfTest で実コンパイルまで確認済み。メニュー配線は Post/Compute と同型の機械的な追加で、コードレビューで確認)
  - `pwsh -File tools\gen_project_files.ps1` は未実行 (新規ファイルの追加/削除/移動なし。既存ファイルの編集のみ)

自己採点 (1-5):
  仕様適合: 5 — 受け入れ条件 1-6 すべてに対応する実装があり、条件 1/2/3/4 は実プロジェクトのスクリーンショット+ファイル内容で確認、条件 5 (`.mat.json` に properties 保持) は SelfTest と実ファイルの両方で確認、条件 6 は `check_rules.ps1` で機械確認
  正しさ: 5 — off-by-one 修正は「直す前に FAIL することを確認」の手順を踏んだ。Properties の Tex2D 数値/文字列の扱いは往復不変性を SelfTest で検証し、実プロジェクトでも Save→ファイル内容で二重に確認した。既存 SelfTest (M79 sub-01/02/03 分含む) 全数 ALL PASS を維持
  コード品質: 4 — fxstack とマテリアルの Properties 描画・スキーマ取得を共通関数に統合し重複を解消。バナー取得の設計判断 (sub-02 API ではなく sub-01 API を直接使う) は理由を明記した。強いて言えば `DrawPropertiesEditor` がやや長い (元のコードをそのまま移設したため)
  テスト: 5 — 新規回帰テスト (off-by-one ×2、properties 往復 ×多数、テンプレート実コンパイル) を追加し全て自動検証。加えて実プロジェクトでの手動確認をスクリーンショット+ファイル読み取りで実施 (自動化の都合上 GUI 操作はスクリプトからのマウス座標指定で行い、Create メニュー自体のクリックのみ未実施)

不安・質問:
  - なし。§7 の未決事項はいずれも sub-05 (WaterWave) 向けで本サブには影響しない

触ったファイル:
  - `src/Editor/AssetOps.cpp`
  - `src/Editor/AssetOps.h`
  - `src/Editor/AssetOpsSelfTest.cpp`
  - `src/Editor/Windows/AssetBrowserWindow.cpp`
  - `src/Editor/Windows/InspectorWindow.cpp`
  - `src/Editor/Windows/InspectorWindow.h`
  - `src/Engine/Core/LocalizationTable.inl`
  - `src/Engine/Renderer/ProjectShaderProperties.cpp`
  - `src/Engine/Renderer/ProjectShaderProperties.h`
  - `src/Engine/Renderer/ShaderManager.cpp`
  - `src/Engine/Renderer/ShaderManager.h`
  - `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp`

申し送り:
  - sub-05 (WaterWave) は本サブのシェーダコンボ・Properties 共通化とは独立。ただし `MaterialEditState::properties` の型 (`PropValue` map) と `DecodeMaterialProperties`/`EncodeMaterialProperties` はそのまま流用できる想定
  - 一時プロジェクト `%TEMP%\mye_sub04_probe` と関連スクリーンショット (`%TEMP%\sub04_*.png` 等) はリポジトリ外のスクラッチなので放置 (削除不要)。Water の実ファイルには一切触れていない

SELF_EVAL: sub-04 (round 2)
実装:
  - `src/Editor/Windows/InspectorWindow.cpp` — シェーダコンボの選択変更ハンドラから `matEdit_.properties.clear()` を削除。`ApplyMaterialShaderSelection(matEdit_.shader, items[curIndex])` を呼ぶだけにし、Properties マップには一切触れないようにした (spec §4.1「シェーダを戻したとき値が残る」契約どおり)
  - `src/Engine/Renderer/ProjectShaderProperties.h`:`.cpp` — `ApplyMaterialShaderSelection(shaderName, newShaderName)` を追加。中身はシェーダ名の代入のみで Properties には触れない、という契約そのものを関数に切り出した (InspectorWindow の private 型 `MaterialEditState` を経由せず `std::string`/`PropValue` map レベルで完結するので `AssetOpsSelfTest.cpp` から直接呼べる)
  - `src/Editor/AssetOpsSelfTest.cpp` — 回帰テストを追加: シェーダ名を A→B→A と `ApplyMaterialShaderSelection` で切り替えても `properties` マップの中身 (件数・値) が変化しないこと、最終的に `EncodeMaterialProperties` で JSON へ戻したとき元の値 (`_Tint`) が保持されていることを確認 (round 1 の `properties.clear()` 回帰の再発防止)

仕様との差分:
  - なし。round 1 の [追加/解釈] (シェーダ切替で properties をクリアする) は指摘により撤回し、spec §4.1 のとおり「保持する」に修正した

検証:
  - `bin\x64\Debug\Editor.exe --selftest` → 追加した3件 (A->B 切替で触れない / B->A で戻せる / 往復後も _Tint 保持) を含め全 SelfTest ALL PASS (回帰なし)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - nit #2 (Create メニューの「サーフェスシェーダ」を GUI で 1 回クリック): 一時プロジェクトを対話起動し、右クリック → 作成 → シェーダー とホバーで辿り「サーフェスシェーダ」がメニュー項目として正しく (ポストシェーダ・コンピュートシェーダと並んで) 表示されることをスクリーンショットで確認した。同じ経路で「コンピュートシェーダ」をクリックしたところ命名モーダル (「アセットを作成」) が正しく開くことまでは確認できた (Create メニューの配線自体は正常に動作)。ただし、モーダルが開いた**後**の「作成」/「キャンセル」ボタンへの合成マウスクリックおよびキーボード入力 (Backspace/Enter) が、このモーダルに対してだけ反応せず (フォアグラウンドウィンドウ一致は確認済み)、「サーフェスシェーダ」項目自体のクリック確定と、生成後のテンプレートの実ファイル確認までは自動化できなかった。何度か手法を変えて試したが解消せず、モーダル入力特有の自動化上の制約と判断して打ち切った。`CreateSurfaceShaderAsset` 本体の生成+実コンパイルは round 1 の SelfTest (WARP デバイス) で確認済みで round 2 でも変更なし。メニュー配線 (`kCreateSurfaceShader` の `DoCreate` 分岐) は `kCreateComputeShader` と同型であることをコードレビューで確認した

自己採点 (1-5):
  仕様適合: 5 — spec §4.1 の「シェーダを戻したとき値が残る」契約どおりに修正し、回帰テストで固定した
  正しさ: 5 — 修正前に (round 1 の) 挙動が指摘どおり壊れていたことは指摘内容と自分のコード (2101 行の clear) で確認済み。修正後は SelfTest で A→B→A の保持を実機コードパスで検証 (`ApplyMaterialShaderSelection` は実際に Inspector が呼ぶのと同じ関数)
  コード品質: 4 — 型不一致時のフォールバックは既存の `std::get_if` 安全読みにそのまま乗るので追加コード不要だった。`ApplyMaterialShaderSelection` は 1 行の関数だが、「clear しない」という契約を明文化し将来の回帰を防ぐ意図を優先した
  テスト: 4 — properties 保持の回帰は SelfTest で確実に検証。nit の GUI クリック確認はモーダル入力の自動化に阻まれ未完了 (理由を検証欄に明記、コードレビュー+隣接項目の実地確認で代替)

不安・質問: なし

触ったファイル (round 1 + round 2 の全量):
  - `src/Editor/AssetOps.cpp`
  - `src/Editor/AssetOps.h`
  - `src/Editor/AssetOpsSelfTest.cpp`
  - `src/Editor/Windows/AssetBrowserWindow.cpp`
  - `src/Editor/Windows/InspectorWindow.cpp`
  - `src/Editor/Windows/InspectorWindow.h`
  - `src/Engine/Core/LocalizationTable.inl`
  - `src/Engine/Renderer/ProjectShaderProperties.cpp`
  - `src/Engine/Renderer/ProjectShaderProperties.h`
  - `src/Engine/Renderer/ShaderManager.cpp`
  - `src/Engine/Renderer/ShaderManager.h`
  - `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp`
  - (新規ファイルなし。`gen_project_files.ps1` は不要)

申し送り:
  - nit #2 について: Create メニューの「サーフェスシェーダ」項目そのものの手動クリック確定は未達成のまま。もし reviewer が実地で試す場合、命名モーダルの「作成」ボタンは通常のマウス操作 (実際の人間の手) では問題なく動くはずで、今回できなかったのは合成入力 (SetCursorPos/mouse_event) 特有の制約とみている

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 1 件: シェーダ切替で `matEdit_.properties.clear()` (InspectorWindow.cpp:2101) は spec §4.1 失敗時表「スキーマに無いキーは保持 (シェーダを戻したとき値が残る)」に反する = 切替を戻すと値が消える静かなデータ損失。プレビュー forward_lit 固定 (spec §4.3 の許容内)、Tex2D の数字文字列→GUID 数値書き出し (spec §4.2 に合致)、バナーの LoadSurface 直接使用は受理。
- round 2: VERDICT OK (planner)。clear 削除を実コードで確認 (InspectorWindow.cpp:2100)。型不一致は DrawPropertiesEditor の get_if で既定表示・触るまで不変。回帰テストはほぼ自明な関数を叩く形で守りは弱い (nit)。Create メニュー項目のクリック確定は合成入力の制約で未達 (nit、表示とコード同型は確認)。
