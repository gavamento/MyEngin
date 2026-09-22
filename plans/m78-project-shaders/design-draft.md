# M78 プロジェクト側シェーダ追加 — 事前調査 (design-draft)

planner 向けの事前調査書。ユーザーとの合意事項と、着手前にコードを読んで確認した現状を残す。
**この文書は仕様書ではない**。spec.md は planner が書く。

## 1. ユーザーとの合意事項 (2026-09-22、AskUserQuestion で確認済み)

| 論点 | 決定 |
| --- | --- |
| 対象範囲 | **3 系統すべて**: (a) マテリアル用サーフェスシェーダ / (b) ポストエフェクト / (c) コンピュートシェーダ |
| 独自パラメータ | **持たせる (Unity の Properties 相当)**。シェーダ内の宣言を解析し、`.mat.json` 等に任意プロパティを保存、Inspector に自動で UI を出す |
| 対応パス | **Forward + Deferred GBuffer** の両方。skinned / instanced のバリアント差し替えは範囲外 (今回は据え置き) |

補足: パーティクル/VFX 用シェーダの差し替えは対象外と判断された。

## 2. すでにある土台 (作り直さないこと)

### 2.1 複数シェーダルート = プロジェクト上書きは実装済み
`src/Engine/Renderer/ShaderManager.h:36` のコメントのとおり、シェーダルートは
**優先度順の複数持ち** `[<project>\assets\shaders, <engineRepo>\assets\shaders]`。
プロジェクト側に同名 `.hlsl` を置けばエンジン組込みを上書きでき、置かなければエンジン側が使われる。

配線箇所:
- `src/Engine/Engine/EngineLoop.cpp:255-261`
- `src/Editor/EditorMain.cpp:388-395`
- `src/Engine/Renderer/VolumeTexture.cpp:552` (`options.shaderDirs`)

`ShaderManager::ResolvePath` が各ルートを順に探し、`ReportShadowedBuiltins()` が
上位ルートが組込みを隠している箇所を警告する。**「ファイルを置く導線」はもう存在する**。

### 2.2 実行時コンパイル / ホットリロード / バイトコードキャッシュ
- `Load(name)` → `<name>.hlsl` (エントリ `VSMain` / `PSMain`)、`LoadCompute(name)` → `<name>.cs.hlsl` (`CSMain`)。
- `RequestRecompileForFile` + `PollAsyncCompiles` で `.hlsli` の依存グラフを辿った非同期ホットリロード。
- `SetCacheDir` でバイトコードキャッシュ (RT の CS 9 本で起動が 6.6 秒止まっていた対策)。
- 失敗しても AssetID は返り、`ShaderProgram::valid == false` になる。

### 2.3 マテリアルのシェーダ指定
`.mat.json` に `"shader": "forward_lit"` があり、`GpuResources.cpp:1048` で
`AssetID{ HashStr(root.value("shader", ...)) }` になる。

## 3. 現状の欠落 (= M78 の本体)

### 3.1 新規シェーダは誰もコンパイルしない ★最大の穴
`MaterialLibrary::LoadFromFile` は名前を **ハッシュするだけ**で `shaders.Load(name)` を呼ばない
(`src/Engine/Renderer/GpuResources.cpp:1048`)。起動時に `Load` されるのは
エンジンが名前を直書きしている分のみ (`src/Editor/EditorApp.cpp:89`、各 Pass の `Init`)。

結果: `.mat.json` に独自名を書くと `shaders.Get(id)` が null → `ForwardPath.cpp:443` の
`if (!prog || !prog->valid) continue;` で **描画がまるごとスキップ = 物体が消える**。
エラー表示すら出ない (Unity のマゼンタに相当するものが無い)。

→ 必要なもの: マテリアル読み込み時の遅延 `Load`、および **コンパイル失敗時のフォールバック**
(エラーシェーダで描く / 組込みへ落とす のどちらか。planner が決めること)。

### 3.2 Deferred の不透明パスは `mat->shader` を見ていない
`src/Engine/Renderer/DeferredPath.cpp:941` — 常に `gbufferShader_` / `gbufferSkinnedShader_` /
`gbufferInstancedShader_` 固定。`mat->shader` が効くのは:
- `ForwardPath.cpp:441` (非スキン・非インスタンスのみ。skinned は `skinnedShader_` に、
  instanced は `litInstancedShader_` に強制差し替え)
- `DeferredPath.cpp:1397` (**透明後段だけ** — ここは forward_lit をそのまま使う経路)

→ 合意は「Forward + Deferred GBuffer」なので、GBuffer 出力規約
(法線 / 材質 / velocity / `rtReceiver`) を独自シェーダに守らせる仕組みが要る。
`MeshBind.h:29` 近辺と `DeferredPath.cpp:21`(レイアウト注記), `:905`, `:941` を読むこと。

### 3.3 シェーダ固有パラメータが無い
`MaterialCB` は **16 バイト固定** (`src/Engine/Renderer/MeshBind.h:29`):
`metallic / roughness / hasNormal(int) / emissive`。
テクスチャは `BindMaterialTextures` が t0 (albedo) と `normalSlot` の 2 枚だけを張る。
スロット規約が **パスごとに違う** ことに注意: forward_lit は法線が t2 (t1 は影)、
GBuffer は t1 (`MeshBind.h` の `kForwardNormalSlot` / `kGBufferNormalSlot`)。

→ ユーザープロパティ用の cbuffer スロットと **テクスチャスロットの予約範囲**を
新たに決める必要がある (エンジン予約 t0..tN / ユーザー tN.. の線引き)。

`Material` 構造体 (`GpuResources.h:204`) には **触るときの重い制約**がある:
> cooked キャッシュ (ModelCook) に memcpy で丸ごと書かれ、CookedCacheSelfTest が
> `sizeof(Material)` 全体を memcmp する。暗黙パディングを作ると
> 「同じ入力から作った cooked ファイルのバイト列が run ごとに違う」が生まれる (`pad0` の由来)。

→ **可変長プロパティを `Material` に直接生やすのは危険**。別テーブル + ハンドル参照を推奨するが、
判断は planner。`src/Engine/Engine/Asset/ModelCook.cpp:282` も合わせて読むこと。

### 3.4 Inspector にシェーダを選ぶ UI が無い
`src/Editor/Windows/InspectorWindow.cpp:2028` は `ImGui::TextDisabled("shader: %s", ...)` の
**読み取り専用表示**。編集状態は `InspectorWindow.h:116` の `std::string shader = "forward_lit"`。
保存は `InspectorWindow.cpp:1958`、読み込みは `:1911`。

ライブプレビュー機構 (M53) が既にある: 保存する JSON をそのまま `Material` に組み直して描くので
「保存したらこう見える」が保証されている (`MaterialEditToJson` → `MaterialFromJsonText` →
`AssetPreviewCache::GetOrRequestMaterial`)。**独自プロパティもこの経路に乗せれば
プレビューの一貫性は自動で付いてくる**。

### 3.5 ポストエフェクトは固定チェーン
`src/Engine/Renderer/PostProcess.h` の `Settings` は
tonemap / bloom / fxaa / 色収差 / ビネット / ゴッドレイ / LUT / 自動露出 / DoF / モーションブラー / TAA を
**構造体のフィールドとして直書き**した固定チェーン。ユーザーパスを差し込む口は無い。
`CameraPostFxComponent` (`src/Engine/Core/Components.h:769`) がシーン側の露出口。

→ 「挿入位置」「順序」「パラメータ露出」を新規に設計する必要がある。
Unity URP の ScriptableRendererPass + Volume Component が参考になるはず (別途調査)。

### 3.6 コンピュートシェーダをプロジェクトから叩く口が無い
`src/Shared/EngineAPI.h` は現在 **`MYE_API_VERSION 20`** (関数ポインタ約 118 スロット)。
メモリにある「ABI v16 = 110 スロット」は M70c 時点の値で**古い** — v17..v20 で
`GetSceneName` / `IsDevelopmentRun` / ウィンドウモード 2 本 / 汎用タグ 4 本が増えている。
描画系の口は `SetMeshRenderer` 程度で、バッファ確保 / dispatch / 結果読み出しの API は存在しない。

→ コンピュート対応は **C ABI 境界の新規設計 (v21)** を含む。`src/Shared/` は C ABI + POD のみ
(STL / vtable / 例外を渡さない) という AGENTS.md の制約が効く。制約が厳しい:
- 既存スロットの並びとシグネチャは変えない (`Interop.cs` が**位置ベース**でミラー)
- `tools\check_rules.ps1` 規則 11 が「順序・件数・名前・引数個数 + version⇄スロット数の同時性」を機械検査
- メモリ確保・解放は常にエンジン側。文字列は呼び出しの間だけ有効

ABI bump の検証手順はメモリ `abi-bump-verification.md` に従うこと
(C# レーンは replay 被覆外なので temp プローブで実走確認が要る)。

## 4. 横断的な制約 (見落とすと終盤で崩れる)

1. **決定論**: Debug/Release/CI(WARP) のシミュレーションがビット一致すること。
   描画は sim 非対象 (`PostProcess.h` 冒頭「ワールドハッシュには一切関与しない」) だが、
   **golden 画像**は動く。独自シェーダはプロジェクト側資産なので golden 被覆外だが、
   エンジン組込みシェーダの規約を変えると golden が動く。`golden-diff-triage.md` の 4 点計測を先に。
2. **層構造**: 上位層は下位層にしか依存できない。生の D3D 型は Renderer から出さない。
   ユーザープロパティの型定義をどの層に置くかが効く。
3. **レイヤ別のテスト**: `*SelfTest.cpp` を機能の隣に置く。`Editor.exe --selftest` /
   `tools\check_rules.ps1` (C++/HLSL 定数の一致チェックあり) / `tools\replay_verify.bat`。
   **`check_rules.ps1` が C++/HLSL 定数一致を見る**ので、スロット番号を増やすときは両側を揃える。
4. **作業ツリーが汚れている**: 現ブランチ `GY-PS_Kadai` に 34 ファイルの未コミット変更と
   `.agents/` / `scratch_vox/` / `_agent_diff*.patch` の未追跡物がある。
   着手前に状態を確定させること (コミットするか、別ブランチを切るか)。

## 5. planner への申し送り

- **既存エンジンの実装を先に見ること** (メモリ `reference-existing-engines.md`)。
  Unity の ShaderLab Properties / URP の GBuffer パス規約 / Volume Component、
  UE のマテリアルドメインと Post Process Material は、方式を決める前に公式ドキュメントを当たる。
  → 本 M78 では並行して調査済み。結果は同ディレクトリの `reference-unity-ue.md` を参照。
- 3 系統を 1 サブにまとめないこと。少なくとも
  (a) サーフェス基盤 + 失敗フォールバック / (b) プロパティ宣言と Inspector /
  (c) GBuffer 対応 / (d) ポストエフェクト / (e) コンピュート + ABI
  の粒度で切れるはず。ABI bump を含むサブは最後に寄せると手戻りが減る。
- 「プロジェクト側で」の**プロジェクト**が何を指すかを最初に確定させること。
  外部プロジェクト (HAL Collector / 三校) が実在し、配布されたエンジンを使っている。
  `エンジン配布用/` は現在空。
