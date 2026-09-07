# sub-01: review-1 minor 5 件の回収

- 依存: なし
- 状態: 未着手
- 往復: 0

## やること

M67 review-1 の minor 1〜5 を全件片付ける。**golden は 1 枚も動かさない**。
仕様は `spec.md` の §4.1 (JSON) / §4.2 (UI) / §4.3 (CB・コメント) / §4.5 / §4.7。

1. **minor 1 — 実効 ReSTIR 状態の規則を 1 本に。**
   `RenderSystem` に読み取り専用の判定を 1 本置き (`RtRestirGpuMs()` の並び)、
   `RenderSystem.cpp:1148-1149` の `view.rtReflRestir` と `EditorApp.cpp:1267` の `BeginDisabled` が
   **両方それを呼ぶ**。デバッグ 12 / 14 を表示している間もサブメニューが触れるようになる。
   コメントに「実効状態はここ 1 本 (public メンバを直接読む第 3 の経路を作らない)」を書く。
2. **minor 2 — サブメニューを 1400x900 窓に収める。**
   `EditorApp.cpp:1305-1321` のクラス表 5 行 × 3 スライダ (計 20 項目) を **1 つの子メニューへ畳む**
   (:1294 の `for` は**クラス上書きのラジオ**なので混同しないこと)。
   親は 10 項目程度になり `Restir_Reset` (:1333) と `Restir_Gpu` (:1339) が見える。
   新規文字列 1 本を `LocalizationTable.inl` に en/ja (`###` 右辺は両言語一致・一意)。
   スライダの範囲・Reset の意味 (`rp = RtReflRestirParams{}`) は変えない。
3. **minor 3 — 読み手のいない CB 項目を落とす。**
   `RtPasses.cpp:164` の `uint32_t frameIndex` と `:786` の代入、`rt_restir_cb.hlsli:34` の
   `uint gRsFrameIndex` を明示パディングへ。`static_assert(sizeof == 240)` (`RtPasses.cpp:172`) と
   `static_assert(offsetof(classTable) == 160)` (:173) は**そのまま通ること**。
   パディングのコメントに **外した理由** (M67f でタップ回転のフレーム項を外した = 回すと乗り換えが
   そのままフリッカーになり実測 2 倍。`kRtRestirTapSeed` のコメントが正本) を残す。
4. **minor 4 — 実態と食い違うコメント。**
   `RtPasses.h:60`「reservoir の組 A の rad と nrm」→ 実体は `slot.set[slot.write]` (rt_refl が今フレーム
   書いた面)。`RtPasses.h:197`「B (t11-t15) を読み A (u1-u5) へ書き戻しつつ」→ 実体は書き戻さない
   (`RenderRestirSpatial` の UAV は u0 のみ)。
   ついでに `rt_restir_cb.hlsli` の `float4 gRsClass[...]` 直前へ「C++ の `offsetof` 160 と一致 —
   前に float を足すときは両方」の 1 行 (M67 sub-04 の nit)。
5. **minor 5 — `.mat.json` の `reflectionClass` を値で判定する。** spec §4.1 の表がそのまま仕様。
   - 受理規則を**共有関数 1 本**にして `ParseMaterialJson` (`GpuResources.cpp:1022`) と
     `LoadMaterialEdit` (`InspectorWindow.cpp:1481`) の両方から呼ぶ。
   - キーがあって落としたときだけ `MYE_LOG_WARN` を 1 行 (欠損は無言)。
   - `AssetOpsSelfTest.cpp:328-352` の表に **`3.0` → 3** と `4.0` → 4 を足す
     (`1.5` / 文字列 / 範囲外 / 欠損 / 両端 0・4 の既存ケースは残す)。真偽値 `true` のケースも足す。
   - 書き出し (`InspectorWindow.cpp:1527`) は整数のまま。
   - `engine_spec.md:478` の "a missing, non-integer or out-of-range JSON value falls back to 4" を
     新しい規則に合わせて書き直す (「小数部を持つ値」であって「浮動小数点型」ではない、と読める文に)。
6. **確認して報告するだけ** (コードは触らない):
   - `.mat.json` の parse 結果が cooked blob に入るか (`ModelCook.cpp` を読む)。入るなら
     `kCookVersion` の扱いを planner へ上げる — **勝手に bump しない**。
   - `Material` の中身がワールドハッシュに載らないこと (載るのは `MeshRendererComponent::material` の
     `AssetID`)。1 行で根拠を示す。

## やらないこと (このサブでは)

- 既定値 (`kRtReflClassTable` / `spatial` / `visRay` / `svgfHistory` / `atrousIterations` /
  `kRtRestirRadiusAlphaRef` / `kRtRestirWMax`) に**一切触らない**。それは sub-02。
- golden の更新。このサブは「24 枚が 1 枚も動かない」を主張する側。
- `.mat.json` 以外の JSON 整数フィールド (spec §2 S7)。
- `--img-diff` の矩形オプション、`shot_verify.bat` の frame 枠の SHOTBASE 化。
- 保存形式の変更 (書き出しは整数のまま)。

## 触る場所 (planner の見立て。鵜呑みにせず実物で確認すること)

| ファイル | 位置 | 何を |
|---|---|---|
| `src/Engine/Engine/RenderSystem.h` | `:244` (`RtRestirGpuMs`) の並び | 実効 ReSTIR 状態の判定を 1 本追加 |
| `src/Engine/Engine/RenderSystem.cpp` | `:1148-1149` | 上の判定を呼ぶ形へ |
| `src/Editor/EditorApp.cpp` | `:1267` / `:1305-1321` | `BeginDisabled` の判定 / クラス表を子メニューへ |
| `src/Engine/Core/LocalizationTable.inl` | ReSTIR の文字列群 | 子メニュー名 1 本 (en/ja) |
| `src/Engine/Renderer/RayTracing/RtPasses.cpp` | `:164` / `:786` | `frameIndex` をパディングへ |
| `assets/shaders/rt_restir_cb.hlsli` | `:34` / `gRsClass` 直前 | `gRsFrameIndex` をパディングへ / offsetof の注意 1 行 |
| `src/Engine/Renderer/RayTracing/RtPasses.h` | `:60` / `:197` | コメントを実態へ |
| `src/Engine/Renderer/GpuResources.cpp` | `:1017-1027` | 共有関数を呼ぶ形へ |
| `src/Editor/Windows/InspectorWindow.cpp` | `:1480-1486` | 同上 |
| 共有関数の置き場所 | — | `GpuResources.h` は **nlohmann を include していない** (重いヘッダを増やさない方が良い)。`Engine/Core` には**置けない** (`kRtReflClass*` は Renderer 層の `RtTypes.h`、Core は Renderer を知らない)。**Renderer 層に小さなヘッダを 1 枚足す**のが planner の見立て。新規ファイルを足したら `pwsh -File tools\gen_project_files.ps1` |
| `src/Editor/AssetOpsSelfTest.cpp` | `:328-352` | `3.0` / `4.0` / `true` のケース追加 |
| `engine_spec.md` | `:478` | 受理規則の文言 |

## 受け入れ条件 (このサブ)

1. **A2 / A3 / A4**: `3.0` → 3、`1.5` / `"Hero"` / `true` / `-1` / `9` → 4 (例外なし)、欠損 → 4 かつ無言。
   規則は 1 本 (呼び出し 2 か所を file:line で示す)。落としたときの警告 1 行を selftest ログから貼る。
2. **A5 / A6**: 実効 ReSTIR 状態の判定が 1 本。1400x900 窓で `Reset to defaults` と GPU 時間の行が見える
   (reviewer が実機で判定するので、coder は**構造上そうなっている根拠**を実装メモに書く: 親メニューの項目数)。
3. **A7 / A8**: `gRsFrameIndex` / `RtRestirCB::frameIndex` の**宣言と読み書き**が 0 件
   (旧名を挙げた「なぜ外したか」のコメントは残す — spec §5 A7 の 2026-09-08 訂正)。
   `static_assert` 240 / offsetof 160 が通る。コメント 3 か所が実態と一致。
4. **A1**: `shot_verify.bat` が **24 枚全緑・maxDiff=0**。`git status` に `tests\golden\` の変更が無い。
5. **A15**: Debug / Release とも `/p:MyeWarnAsError=true` で警告 0、`check_rules.ps1` 0 error / 0 warning。
6. spec §4.5 の確認 2 件 (cook 経路 / ワールドハッシュ非接触) に根拠付きで答えている。

## 検証コマンド

```
rem ビルド (vswhere で MSBUILD を解決してから)
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo /p:MyeWarnAsError=true
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo /p:MyeWarnAsError=true

bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
git status
```

- `Editor.exe` / `Runtime.exe` は GUI サブシステムなので **PowerShell からは `cmd /c` を挟む**。
- `replay_verify.bat` はこのサブでは回さない (sim 非接触。最終的な担保は sub-02)。
  ただし「回さない理由」を SELF_EVAL の検証欄に明記すること (「未実行」と書いて理由を添える)。
- ファイルを足したら `pwsh -File tools\gen_project_files.ps1`。

## 実装メモ (coder が追記)

### SELF_EVAL: sub-01 (round 1)

実装:
- `src/Engine/Renderer/ReflectionClassJson.h` / `.cpp` (新規) — `.mat.json` の
  `reflectionClass` 受理規則を 1 本に。「非整数」は **JSON の型ではなく値**で判定
  (`node.is_number()` → `get<double>()` → `isfinite && v == floor(v)` → `[0, kRtReflClassCount)`)。
  欠損は無言、キーがあって落としたときだけ `MYE_LOG_WARN` 1 行。
  警告の値表示は `dump(-1, ' ', false, error_handler_t::replace)` — 既定の `dump()` は
  不正 UTF-8 で `type_error` を投げるため (「例外を外へ出さない」が存在理由の関数で
  警告側が投げたら本末転倒)。
- `src/Engine/Renderer/GpuResources.cpp:1022` (`ParseMaterialJson`) — 共有関数を呼ぶ形へ。
- `src/Editor/Windows/InspectorWindow.cpp:1481` (`LoadMaterialEdit`) — 同上。
- `src/Engine/Engine/RenderSystem.h:245-255` — `RtRestirEffective()` (実効 ReSTIR 状態の唯一の判定)。
- `src/Engine/Engine/RenderSystem.cpp:1149` / `src/Editor/EditorApp.cpp:1268` — 両方がそれを呼ぶ。
- `src/Editor/EditorApp.cpp:1309-1327` — クラス表 5 行 × 3 スライダ (20 行) を子メニュー
  `Restir_ClassTable` へ畳む。親メニューは 9 行 + セパレータ 2 本。
- `src/Engine/Core/LocalizationTable.inl:180` — `Restir_ClassTable` (en/ja、`###` 無し)。
- `src/Engine/Renderer/RayTracing/RtPasses.cpp:164-170` — `frameIndex` → `pad1` (外した理由付き)、
  `:792` の代入を削除。
- `assets/shaders/rt_restir_cb.hlsli:34` — `gRsFrameIndex` → `gRsPad1`、`gRsClass` 直前に
  offsetof 160 の注意 1 行。
- `src/Engine/Renderer/RayTracing/RtPasses.h:60` / `:199` — コメントを実態へ。
- `src/Editor/AssetOpsSelfTest.cpp:324-383` — spec §4.1 の表を全行固定: `3.0` / `4.0` / `-0.0` /
  `-0` (受ける側) と `true` / `null` / `[3]` / `{}` / `3.0e10` (落とす側) を追加。
- `engine_spec.md:478` — 受理規則の文言を「値で判定」「欠損は無言 / 落としたら警告 1 行」へ。

確認だけ (コードは触らない):
- **`.mat.json` は cooked blob に入らない** → `kCookVersion` の bump 不要。根拠:
  `ModelCookData::AddMaterial` の呼び出しは `FbxLoader.cpp:400,523` と `ModelLoader.cpp:174`
  の 3 か所だけ = モデル取り込み経路。`.mat.json` を読む `MaterialLibrary::LoadFromFile` /
  `MaterialFromJsonText` は `ModelCookData` に一切触れない。
- **`Material` はワールドハッシュに載らない** → replay 非接触。根拠:
  `MeshRendererComponent` (`src/Engine/Core/Components.h:44-48`) のフィールドは
  `AssetID mesh` / `AssetID material` の 2 本だけ。`WorldHasher.cpp` / `.h` に
  `Material` / `MaterialLibrary` / `GpuResources` の語は 1 つも無い (grep 0 件)。

検証 (全て実行済み):
- Debug / Release とも `/p:MyeWarnAsError=true` で警告 0 / エラー 0。
- `Editor.exe --selftest` → exit 0 (45 スイート全 PASS)。反射クラスの 3 検査 PASS、
  落とした 9 入力それぞれに警告 1 行、実 run (shot_verify の 24 本) では 0 行 = 欠損は無言。
- `check_rules.ps1` → 0 error / 0 warning。
- `shot_verify.bat` → **24 枚 PASS、全て maxDiff=0**。`git status` に `tests/golden/` の変更なし。
- 追加プローブ: `--rt-debug 12` 単独 と `--rt-debug 12 --rt-restir` の frame 40 が
  `--tol 0` で maxDiff=0 (= `RtRestirEffective()` の 2 分岐が同じ絵を出す)。
  M ヒートマップの反射帯 (330,255)-(470,400) の平均 G = 131 (赤 = M 1 ではない = 再利用が伸びている)。

## フィードバック履歴

- round 1: **VERDICT OK** (planner、2026-09-08)。指摘は should 1 / nit 2 で must は 0。
  - [逸脱] (A7 の grep) は **planner の仕様の書き間違い**と判定し、spec §5 A7 を「宣言と読み書きが 0 件。
    理由コメントは残してよい」へ訂正した (spec §8)。実装は §4.3 の意図どおりで、直すのは仕様の側。
  - 不安 3 (`ProfilerWindow.cpp:147` / `DeferredPath.cpp:1108` を寄せない) は実物を読んで**承認**。
    どちらも 10 / 11 と `rtReflOn` を含む別の述語で、`RtRestirEffective()` に寄せると絵が変わる。
  - 不安 2 (クラス表が 4 階層目になった) は **reviewer の実機確認へ回す** (下の should 1)。
    ImGui は ActiveId のある間ポップアップを閉じないのでドラッグは成立する見込みだが、
    M67 で実証済みなのは 3 階層目まで。**駄目だったときの倒し先は 2 列化** (spec §4.2 が許容する 3 案の 1 つ) と
    先に決めておく — round 2 でここを再び議論しない。
  - planner が実物で再検証した項目: 新規 2 ファイルの規則 1 本化と呼び出し 2 か所 / `RtRestirEffective()` の
    呼び出し 2 か所 / CB の pad 化とコメント / `RtPasses.h` の 2 か所 / selftest の 15 入力 /
    `engine_spec.md:478` / `git status` に `tests\golden\` の変更が無いこと / `WorldHasher` に material が 0 件。
