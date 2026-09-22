# sub-01: Properties DSL パース＋値パック基盤

- 依存: なし
- 状態: 判定待ち (round 1 VERDICT OK — コミット待ち)
- 往復: 1

## やること

HLSL 先頭の `/*@MyEngineProperties ... @*/` をパースし、プロパティスキーマ (名前・表示名・型・既定・属性) を得る純関数／モジュールを追加する。スキーマに従い「名前→値」辞書から定数バッファ用バイト列をパックする処理と、テクスチャ／スカラーの型対応表を用意する。`Editor.exe --selftest` から呼べる SelfTest で文法・パック・エラーを固定する。

このサブではまだ PostProcess への挿入や Inspector UI は行わない。後続が依存する **契約 (DSL 文法・型・パック結果)** を閉じる。

## やらないこと (このサブでは)

- Resolve フック、マゼンタ描画、fxstack JSON、コンピュート Dispatch、ABI
- サーフェス／Material への接続
- ImGui Inspector 本体 (スキーマを返す API まででよい)

## 触る場所 (planner の見立て)

- 新規: `src/Engine/Renderer/ProjectShaderProperties.h` / `.cpp` (または `Engine/Asset/` 配下 — 層として Renderer がパースしてよいか確認。文字列パースのみなら Core 寄りでも可)
- 新規: `*SelfTest.cpp` を隣に配置
- 必要なら `tools/gen_project_files.ps1` で vcxproj 更新
- **触らない**: `PostProcess.cpp`、`EngineAPI.h`、`Material` / `GpuResources` の Material POD

## 受け入れ条件 (このサブ)

1. 仕様 4.2 の DSL 例 (Float/Range/Color/Vector/2D＋Header/HDR/HideInInspector) をパースできる  
   — SelfTest
2. 不正ブロック (閉じ漏れ・未知型) で失敗を返し、例外で落とさない  
   — SelfTest
3. 既定値どおりに CB バイト列をパックし、順序／サイズがテストで期待と一致  
   — SelfTest
4. `Editor.exe --selftest` と `tools\check_rules.ps1` がパス  
   — コマンド

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

### round 1 SELF_EVAL

**実装:**
- `src/Engine/Renderer/ProjectShaderProperties.h` : `PropType` / `PropAttr` / `PropertySchema` / `PropertyParseResult` / `ParseProperties` / `PackProperties` の型と公開 API 宣言
- `src/Engine/Renderer/ProjectShaderProperties.cpp` : DSL ブロック抽出・属性パース・プロパティ行パース・CB オフセット割り当て・パック実装
- `src/Engine/Renderer/ProjectShaderPropertiesSelfTest.h` / `.cpp` : 12 ケースの回帰テスト
- `src/Editor/EditorMain.cpp` : include 追加・selftest 連鎖末尾に `RunProjectShaderPropertiesSelfTest()` を追加
- `build/Engine.vcxproj` / `Engine.vcxproj.filters` : gen_project_files.ps1 で更新 (新 4 ファイル)

**仕様との差分:**
- [追加] `PropType::Header` の代わりに `name.empty()` で Header-only 行を判別している。仕様に明記なしのため追加扱い。後続で Inspector 側が使う分岐は同じ条件で問題ない。
- [追加] `AssignCbOffsets` はパース順パックルール採用 (仕様「パース順または Reflection オフセット」の前者)。後続 sub-02/03 で Reflection オフセット方式へ変更が必要になれば差分として挙げる。

**検証:**
- `Editor.exe --selftest` → exit 0、`[selftest] ProjectShaderProperties: ALL PASS`
- `tools\check_rules.ps1` → exit 0、`0 error(s), 0 warning(s)`

**自己採点:**
- 仕様適合: 5 — 受け入れ条件 1/2/3/4 をすべて検証済み (SelfTest 12 ケース、check_rules PASS)
- 正しさ: 5 — SelfTest ALL PASS。Header-only 行の cbOffset バグを修正後に再実行確認済み
- コード品質: 4 — ヘッダコメント・日本語コメント・命名規則に準拠。ParseFloat が errno をリセットしない軽微な nit あり
- テスト: 5 — 正常系・異常系・境界値 (16B パック) を網羅。ビルド含め全コマンドをパス

**不安・質問:**
- なし

**申し送り:**
- `ParseProperties` が返す空スキーマ (`cbSizeBytes == 0`) のとき `PackProperties` は `cbData.clear()` して `true` を返す。後続 sub-02 で PostProcess 挿入時に cbSize==0 の場合の CB バインド省略を考慮すること。
- Header-only PropertySchema は `name.empty() && cbOffset == -1` で識別可能。Inspector 側 (sub-03) はこの条件で区切りラベルを描画する。

## フィードバック履歴
- round 1: VERDICT OK — 受け入れ 1–4 を SelfTest 12 ケースと check_rules で満たす。Header は `name.empty()`、CB はパース順を契約として確定 (spec 変更履歴へ)。nit: ParseFloat の errno は任意。
