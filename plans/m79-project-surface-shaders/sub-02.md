# sub-02: Forward 描画とマテリアル (遅延 Load・横テーブル・properties・マゼンタ)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること

spec §4.1「パスへの組み込み」の Forward 行、「失敗時」、§4.2 `.mat.json` を実装し、**Forward パスで作者の式のメッシュが見える**縦切りを通す。

- `MaterialLibrary`: `.mat.json` の `shader` が `*.surface` のとき、シェーダを**遅延 Load** (sub-01 のサーフェス用ロード) し、`Material` POD の外の**横テーブル** (マテリアル AssetID → シェーダ名・Properties 値・パック済み PerMaterial バイト列・Tex2D の AssetID) に持つ。`properties` の JSON 符号化は fxstack と同じ (`FxStackAsset.cpp:19`)。スキーマに無いキーは保持して CB には書かない
- PerMaterial のパックは **リフレクションのオフセット**で行う (spec §2 の PerMaterial 行)。Properties にあって cbuffer に無い名前は WARN、サイズ不一致は WARN＋書かない。Tex2D は作者 `Texture2D` の名前スロットへ (未割当 / 未解決は M78 と同じ組込み既定、不明名は white＋WARN)
- `.mat.json` のホットリロード / シェーダのホットリロードで横テーブルとパックを作り直す (既存の再読込経路に乗せる)
- ForwardPath: 不透明ループと透明ループで、サーフェスマテリアルのアイテムはサーフェスプログラムの**色エントリ**で描き、予約 CB (PerFrame / SurfaceFrame / PerObject、`MyEngineWater` は 0 埋め) と予約テクスチャ・サンプラ・PerMaterial・作者テクスチャを**名前スロットへ**張る。描いた後に既存 forward_lit のバインド前提 (b0-b2, t0-t9, s0-s2) を壊さないこと (次のアイテムが forward_lit のとき)
- 失敗 (名前なし / コンパイル失敗 / 規約違反 / Properties パース失敗) は `surface_error` でマゼンタ描画＋ Console ERROR (同じマテリアルで毎フレーム出さない)。ホットリロード失敗は旧プログラム維持
- スキン＋サーフェスは従来のスキン経路＋WARN 1 回。インスタンス run はもともと forward_lit 限定なので変更不要であることを確認
- **(sub-01 から移管・must)** `SurfaceProgram` をホットリロード (`RequestRecompileForFile` / `PollAsyncCompiles` の include 依存グラフ。作者ファイル・`MyEngineSurface.hlsli`・`MyEngineSurfaceEntries.hlsli` の変更で再コンパイル、失敗は旧プログラム維持＋WARN) とバイトコードキャッシュ (生成エントリ込みのソース全体をキー、spec §4.4) に乗せる。同じ名前の `LoadSurface` を繰り返しても再コンパイルしないこと
- **(should)** `MyeApplyFog` にフロクセル合成を足す (既存 forward_lit と同じ判定。CB フィールドは sub-01 で確保済み、位置を変えない)
- Editor が使う公開 API: 「マテリアルのシェーダ状態 (OK / 失敗とエラー文)」を取れる口 (sub-04 のバナーが使う)。形は coder 判断
- サンプル: エンジン `assets/shaders/` に平塗り (トゥーン) の `*.surface.hlsl` を 1 本 (M78 の `MyTint.post.hlsl` と同じ位置付け。既定シーンからは参照しない)

## やらないこと (このサブでは)

- Deferred パス・速度・影 (sub-03)。**Deferred では従来どおり GBuffer に描かれてよい** (このサブの時点では)
- Inspector・作成メニュー (sub-04)
- WaterWave (sub-05)

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/GpuResources.h/.cpp` — `MaterialLibrary`、`ParseMaterialJson` (`GpuResources.cpp:1046-`、「フィールドを足すときはここだけ」の唯一の本体)。遅延 Load には ShaderManager への参照が要る — 読み込み時に渡すか、描画時に名前から解決するかは coder 判断 (層の向きに注意: Renderer 内で閉じる)
- `src/Engine/Renderer/ForwardPath.cpp` — `DrawItems` (`ForwardPath.cpp:392-476`)
- `src/Engine/Renderer/ProjectShaderProperties.*` — パース再利用。リフレクションオフセット版のパックは新関数でよい (M78 の `PackProperties` の意味は変えない)
- マテリアルのホットリロード経路 (`src/Engine/Engine/HotReload/` 周辺)
- SelfTest: `.mat.json` の `properties` 読み込み→リフレクションオフセットでのパック、失敗時にエラーシェーダへ落ちる判定、既存 `CookedCacheSelfTest` が無変更で通る (`sizeof(Material)` 不変)

## 受け入れ条件 (このサブ)

1. `shader: "X.surface"` のメッシュが Forward パスで作者の式で描かれる (不透明・透明とも) — 一時シーン＋`Runtime.exe --screenshot`
2. `properties` の Float/Range/Color/Vector/2D が描画に効く — 値違い 2 枚のスクショ＋`--selftest` (パック)
3. 失敗 4 種でマゼンタ＋Console エラー、クラッシュなし。ホットリロード失敗で旧絵維持 — スクショ＋ログ抜粋
4. サーフェスの無いシーンは Forward の絵が不変、`sizeof(Material)` 不変 — `--selftest` (既存 golden / Cooked 系)
5. サーフェスとforward_lit のアイテムが混在しても forward_lit 側の見た目が壊れない (バインドの張り直し) — 混在一時シーンのスクショ

## 検証コマンド

```
tools\gen_project_files.ps1   (ファイル追加時)
bin\x64\Debug\Editor.exe --selftest
tools\check_rules.ps1
Runtime.exe --project <一時プロジェクト or Water のコピー> --scene <一時シーン> --screenshot <png>
```

一時シーン・一時アセットはコミットに残さない (Water プロジェクトの本物のファイルは書き換えない。必要ならコピーで検証)。スクショの取り方はメモリ `screenshot-probe-recipes.md` (シーン撮りは Runtime.exe)。

## 実装メモ (coder が追記)

## フィードバック履歴
