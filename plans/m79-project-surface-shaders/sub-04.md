# sub-04: マテリアル Inspector (シェーダ選択・Properties 共通化・バナー) と作成メニュー

- 依存: sub-02 (sub-03 とは独立 = 並列可)
- 状態: 未着手
- 注記 (sub-02 VERDICT round 1): Properties の 2D ピッカーは spec §4.2 の Tex2D 符号化に従う — 書き出しはアセットなら数値 GUID、組込み既定なら名前文字列。読み込みは数値 / 文字列の両方
- 往復: 0

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

## 実装メモ (coder が追記)

## フィードバック履歴
