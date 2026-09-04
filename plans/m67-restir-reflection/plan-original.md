# ReSTIR 反射 + ReflectionClass（反射に映る側の品質制御）

## Context

MyEngine の RT 反射レーン（M46h）は **GGX VNDF で 1spp 撃って SVGF に投げる**だけの構成
（`assets/shaders/rt_refl.cs.hlsl`）。1spp なので roughness 0.3〜0.6 帯の分散が大きく、
`kRtReflAtrousIterations = 2` の A-Trous で均している。結果として**反射像のディテールが
ブラーで溶ける**。

目的は **プレイヤー・敵・車両など重要な動的オブジェクトが反射に映るとき、その部分を
高品質にする**こと。ReSTIR（時空間サンプル再利用）でレイ数を増やさずに実効サンプル数を上げ、
**ReflectionClass = 反射に映る物体側のクラス**で再利用の厳しさをオブジェクト種別ごとに変える。

### ★ 設計修正（初版からの訂正）

初版は ReflectionClass を **G-Buffer RT3.a** に置いていた。これは誤り —
反射パスがシェーディングしているピクセルは **反射する側（床・水面）** であり、そこから読める
クラスは「床のクラス」でしかない。目的は **反射に映る側（プレイヤー）** の制御なので、
クラスは**レイのヒット点で引く**必要がある。

置き場所は **`RtInstance.pad0`**（実物確認済み — `RtTypes.h` に `pad0`/`pad1` の 8 バイトが空、
`static_assert(sizeof(RtInstance) == 80)` は不変）。`rt_common.hlsli:282` の `RtHitMaterial` が
既に `gRtMaterials[gRtInstances[hit.inst].materialIndex]` を引いているので、**同じ構造体への
アクセスが 1 回増えるだけ = 追加コスト実質ゼロ**。

この訂正で **G-Buffer を一切触らなくなる** → `MaterialCB` の 16B→32B 拡張、`ForwardPath` への
波及、`deferred_gbuffer.hlsl` の変更が全部不要になり、計画が小さくなる。RT3.a は空いたまま残す。

### 元の提案からの意図的な変更点

| 元の提案 | 本計画 | 理由 |
|---|---|---|
| ReSTIR でレイを 1/3 に減らす | **レイ数は据え置き（品質に振る）** | ReSTIR はレイ削減手法ではない。再利用するのは「色」ではなく**サンプル（ヒット点）**で、再利用先で target function を評価し直して初めて成立する。色を混ぜるのは SVGF が既にやっている |
| 補間 ON / OFF | **再利用の半径・タップ数・履歴長** | 正しい ReSTIR では再利用は劣化ではなく改善。「プレイヤーは補間 OFF」＝「プレイヤーに当たったサンプルは半径を狭く・履歴を短く（＝シャープでゴーストしない）」が正しい対応 |
| ObjectID ごとに設定 | **ReflectionClass（5 段）を `RtInstance` 単位で** | `RtInstance` は元々オブジェクト単位なので「ObjectID ごと」の要件を満たす。値の出所は Material |
| 静的は Cubemap / 動的は RT | **現状維持（roughness で分岐）** | 既に `kRtReflMaxRoughness = 0.6` で RT ↔ プリフィルタ IBL を分岐済み |

**レイ数を減らす sparse tracing は本計画のスコープ外**（独立した最適化。後続タスクに切る）。

---

## ★ 前提条件：スキンメッシュは BVH に入っていない

`src/Engine/Engine/RenderSystem.cpp:823`:

```cpp
if (collectRt && world.GetComponent<SkinnedMeshComponent>(c.e) == nullptr) {
```

**`SkinnedMeshComponent` を持つオブジェクトは RT 反射に一切映らない**
（CPU 頂点がバインドポーズのままで姿勢を反映できない、という M46b からの v1 制限）。

現在の音響デモのプレイヤー・敵は **ただの箱**（`DemoContent.cpp:3075`
「プレイヤーはただの箱として立っているだけになる」）なので、**今の内容では検証も動作も成立する**。
ただし実際のスキンキャラクタを入れた瞬間、プレイヤーは反射から消え、ReSTIR をどれだけ
積んでも目的は達成されない。

→ **本計画は先に ReSTIR を箱アクタで成立させ、スキンメッシュの BVH 投入は後続タスクに切る。**

---

## 完了条件と検証手段

- **完了条件**: RT 反射 on + ReSTIR on のとき、同一レイ数で反射像のノイズが目視で減り、
  かつ **ReSTIR off では既存の絵とビット一致**する。加えて、反射に映る箱アクタのクラスを
  変えると再利用の挙動が実際に変わる。
- **検証手段**:
  1. `tools\shot_verify.bat` の golden 19 枚が **無変更**（後方互換の機械的証明）
  2. `Editor.exe --selftest` に ReSTIR 数学の CPU ミラーテストを追加して通す
  3. `Editor.exe --acoustic-demo` で ReSTIR on/off をトグルし、デバッグモードで
     reservoir の M が伸び、クラス色分けが反射像側に出ることを確認
- **品質の良し悪しはユーザー判定**（SOP フェーズ1）。エージェントの責務は「変更が意図どおり
  反映されて動く」まで。パラメータ調整は短いフィードバックループで回す。

---

## 設計

### 1. Reservoir の中身（reconnection 方式）

方向だけを保存すると別ピクセルで再利用できない。ReSTIR GI と同じく **ヒット点そのもの**を持つ。

| 項目 | 内容 |
|---|---|
| `xs` | 反射レイの first hit のワールド座標 |
| `ns` | ヒット点の法線（Jacobian に必要）。**ゼロベクトル = スカイヒットのセンチネル** |
| `Ls` | ヒット点から出た放射輝度（既存 `RtTraceRadianceLod` の戻り値） |
| `cls` | **ヒットしたインスタンスの ReflectionClass** ← 本計画の要 |
| `wSum` | RIS の重み和 |
| `M` | 統合したサンプル数 |

- **Target function**: `p̂ = luminance(Ls) · D_vis(L | V, N, α)`、`L = normalize(xs − P)`
  - `D_vis` = GGX VNDF。ソース pdf も `D_vis` なので初期重みは `w = p̂/p = luminance(Ls)`
- **最終出力**: `Ls · wSum / (M · luminance(Ls))`
  （`D_vis` が約分で消えるので resolve では評価不要。resampling の比較時のみ必要）
  → 出力の次元は現行と同じ「入射放射輝度」なので、**合成側
  （`common.hlsli::ApplyLightingHybrid`）は 1 行も変えなくてよい**
- **再利用時の Jacobian**: `J = (cosθs_new / cosθs_old) · (d_old² / d_new²)`
- **スカイヒット**: `xs` が無限遠なので reconnection せず方向をそのまま再利用し `J = 1`。
  反射に映る空は面積が大きいのでこの分岐は必須

### 2. バッファ配置

既存 `RtPasses::RtHistory`（`RtPasses.h:116-125`）の viewKey 別 ping-pong の流儀を踏襲。
反射用に独立した `RtReservoir` を持つ（GI 用は作らない）。

| テクスチャ | フォーマット | 内容 |
|---|---|---|
| `resPos[2]` | `R32G32B32A32_FLOAT` | xyz = `xs`、w = `wSum`（ワールド座標は精度が要る） |
| `resRad[2]` | `R16G16B16A16_FLOAT` | xyz = `Ls`、w = `M` |
| `resNrm[2]` | `R16G16B16A16_FLOAT` | xyz = `ns`（0 = スカイ）、w = `cls` |

ヒット距離は `xs` と `P` から復元できるので保存しない。
内部解像度（`rtResolutionScale` 既定 0.5）で 32 B/px。1600x900 なら約 23 MB/viewKey。
`RtHistory` と同じく**使った viewKey スロットだけ遅延確保**するので、froxel の 14 MB/viewKey と
同オーダー。

DX11 / `cs_5_0` は制約にならない（per-pixel reservoir なので atomic 不要、spatial reuse も
近傍テクセルの `Load` で足り、wave intrinsics は不要）。UAV は現在 `u0` のみ使用で余裕がある。

### 3. パス構成

```
rt_refl.cs.hlsl              (改) 1spp トレース → reservoir 初期化（cls 込み）を書き出す
rt_refl_restir_temporal.cs   (新) velocity で再投影 → 前フレーム reservoir を統合
rt_refl_restir_spatial.cs    (新) k タップの近傍統合 → resolve して reflRt_ へ
rt_temporal / rt_variance / rt_atrous  (無変更) 既存 SVGF 鎖
```

temporal と spatial は**別 Dispatch に割る**（同一 Dispatch 内では前段の書き込みを同期できない）。

ReSTIR と SVGF の temporal が二重に効くので `kRtReflMaxHistory = 8` は要再調整
（reservoir の M が実効サンプル数を稼ぐぶん SVGF 側は短くできるはず — **仮定、S5 で実測**）。

### 4. ReflectionClass（反射に映る側）

**置き場所**: `RtInstance.pad0`（8 バイト空き。`static_assert(sizeof == 80)` 不変）

**引き方**: `RtHit.inst` → `gRtInstances[hit.inst].pad0`。`RtHitMaterial` が既に同じ構造体を
読んでいるのでキャッシュに乗っている。

**値の出所**: `Material::reflectionClass`（末尾 append）→ `RtScene` が TLAS 構築時に
`RtInstance.pad0` へ写す。**ECS もワールドハッシュも `.rep` も触らない**。将来オブジェクト単位で
上書きしたくなっても `RtInstance` は元々 per-instance なのでレイアウト変更なしで足せる。

**クラスが制御するもの** — クラスはレイが当たった後にしか分からないので、用途を分けて考える:

| 制御対象 | 予測が要るか | v1 |
|---|---|---|
| **再利用の厳しさ**（spatial 半径 / タップ数 / temporal M 上限 / 棄却しきい値） | **不要** — reservoir がヒット情報を持っているので、再利用しようとしているサンプルのクラスは既知 | **やる** |
| レイ本数 | 要 — 前フレームの再投影 reservoir のクラスで予測できる（reservoir が既に持っているので無料）| スコープ外 |

v1 は上段のみ。これが元提案の「プレイヤー：補間 OFF / 小物：補間 ON」の、品質を壊さない正しい形。

| Class | 用途 | spatial 半径 | タップ数 | temporal M 上限 | 狙い |
|---|---|---|---|---|---|
| 0 Hero | プレイヤー | 2 px | 2 | 8 | 動く主役。にじませず、ゴーストも出さない |
| 1 Character | 敵 | 4 px | 4 | 16 | |
| 2 Vehicle | 車両 | 6 px | 6 | 24 | |
| 3 Prop | 小物 | 12 px | 8 | 32 | 積極的に再利用してノイズ最小化 |
| 4 Default | 静的背景・未指定 | 8 px | 4 | 16 | |

**初版の表とは向きが逆**である点に注意 — Hero は「半径を狭く・履歴を短く」。正しい ReSTIR では
再利用は改善なので、「重要 = 再利用を多く」ではなく「重要 = 再利用を保守的に（ディテールと
応答性を優先）」が対応関係になる。

表の値は CB 経由で渡す（**C++ が唯一の出所** — 既存の `kRtRefl*` と同じ流儀）。
**既定クラス（4）の値は現行挙動と一致させる**こと。

### 5. Spatial reuse の可視性（バイアスの扱い）

近傍の `xs` がこのピクセルから本当に見えるかは、厳密には visibility ray が要る。
**v1 では撃たない**（ローブ棄却 + Jacobian のみ）:

- 撃つと 2 rays/px になり現行より確実に遅くなる
- 鏡面ローブは狭いので再利用の大半は妥当。ゲーム用途では一般的な妥協

CB の uniform フラグで**後から有効化できる形**にしておき、ライトリークが目に見えるようなら
切り替える。バイアスがあることは ADR に明記する。

---

## 変更するファイル

### 新規

- `assets/shaders/rt_restir_common.hlsli` — Reservoir 構造 / `RestirUpdate` / `RestirMerge` /
  `RestirJacobian` / `RestirTargetFn`（`RtMath.h` の CPU ミラーと対にする）
- `assets/shaders/rt_refl_restir_temporal.cs.hlsl`
- `assets/shaders/rt_refl_restir_spatial.cs.hlsl`
- `docs/adr/ADR-016-restir-reflection.md` — 「なぜ reconnection 方式か」「なぜクラスは
  受け側 G-Buffer ではなくヒット側 `RtInstance` なのか」「なぜ v1 は visibility ray を撃たないか」

### 変更

| ファイル | 内容 |
|---|---|
| `assets/shaders/rt_common.hlsli` | `RtTraceRadianceLod` の **first-hit 情報（位置・法線・インスタンス index・スカイ判定）を取り出す派生**を追加。既存関数はそれを呼ぶ薄いラッパにして式を複製しない（`:517` の実装を分解すれば足りることを確認済み） |
| `assets/shaders/rt_refl.cs.hlsl` | reservoir 初期化の書き出し（`u1`〜`u3`）。`gRfRestirOn == 0` なら**現行と完全に同じ経路**（uniform 分岐） |
| `src/Engine/Renderer/RayTracing/RtTypes.h` | Reservoir レイアウト + `kReflClass*` テーブル定数 + `RtInstance.pad0` の意味付け（**コメントのみ。`static_assert` は不変**） |
| `src/Engine/Renderer/RayTracing/RtMath.h` | ReSTIR 数学の CPU ミラー（`RtGgxVndf` / `RtReflWeight` と同じ流儀） |
| `src/Engine/Renderer/RayTracing/RtPasses.h/.cpp` | `RtReservoir` 構造体 + viewKey 別スロット + 2 Dispatch + `GpuTimer` 2 本 + `RestirGpuMs()` |
| `src/Engine/Engine/RayTracing/RtScene.cpp` / `RtSceneBuild.cpp` | Material の `reflectionClass` を `RtInstance.pad0` へ写す |
| `src/Engine/Renderer/RenderTypes.h` | `RenderView` **末尾 append**: `rtReflRestir` / `rtReflRestirSpatial` / `rtReflRestirVisRay`。デバッグモードを 12（reservoir M）/ 13（ヒット側 ReflectionClass 色分け）へ拡張 |
| `src/Engine/Renderer/GpuResources.h/.cpp` | `Material` **末尾に `reflectionClass` を append**（`emissiveIntensity` が M46i でそうやって足された前例あり）+ `ParseMaterialJson`（`GpuResources.cpp:1001-1052`）に 1 キー。**欠損キーは既定値に落ちるので既存 `.mat.json` は挙動不変** |
| `src/Engine/Engine/RayTracing/RtSelfTest.cpp` | ReSTIR 数学のテスト（reservoir 更新の重み保存 / Jacobian の対称性 / M=1 で現行推定と一致） |
| `src/Editor/EditorApp.cpp` | RT サブメニュー（`:1062-1240` 付近）に ReSTIR トグル + デバッグモード 12/13 |
| `src/Engine/Core/LocalizationTable.inl` | UI 文字列（en/ja 両方、`###` の右辺は両言語一致かつ一意） |
| `tools/check_rules.ps1` | `$constGroups` に新規の C++⇄HLSL 共有定数を登録（**忘れると定数バッファ不一致で静かに壊れる**） |

### 触らないもの（初版から削除）

`deferred_gbuffer.hlsl` / `DeferredPath.cpp` の `MaterialCB` / `ForwardPath.cpp` —
**G-Buffer には一切手を入れない**。RT3.a は空いたまま残す。

---

## 実装ステップ

各ステップは「途中で止めても壊れていない」単位。

### S0. ベースライン計測（調査）

`Editor.exe --acoustic-demo` で ProfilerWindow の **現行 `ReflGpuMs` / `ReflDenoiseGpuMs` を記録**。
`--screenshot` で比較用の絵を 1 枚取る。
- 検証: 数値が取れたこと。以降の増分はすべてこれとの差で語る

### S1. ReflectionClass の配管（ヒット側）

`Material::reflectionClass` → `ParseMaterialJson` → `RtScene` → `RtInstance.pad0` →
`rt_common.hlsli` でヒット時に引く → デバッグモード 13 で反射像側を色分け表示。
- 検証: ビルド + `tools\shot_verify.bat` **19 枚無変更**（G-Buffer を触らないので当然だが機械で固定）
  + デバッグモード 13 で、床に映った箱の色がクラス別に変わる

### S2. Reservoir の初期化のみ（再利用なし）

`rt_common.hlsli` に first-hit 派生を足し、`rt_refl.cs.hlsl` から reservoir を書き出す。
resolve は「M=1 の RIS」= 現行の推定と数学的に等価。
- 検証: ビルド + `restir on` で**現行とほぼ同じ絵**（M=1 なので原理的に一致するはず）。
  デバッグモード 12 で M が全画素 1

### S3. Temporal reuse

velocity 再投影（既存 `RtHistoryUv` / `RtReprojectValid` を流用）+ Jacobian + クラス別 M クランプ。
- 検証: ビルド + デバッグモード 12 で **M が時間とともに伸びる**。静止画でノイズが減る

### S4. Spatial reuse（ReflectionClass 駆動）

k タップ、**サンプルのヒット側クラス**で半径・タップ数・棄却しきい値を切り替える。
- 検証: ビルド + 箱アクタのマテリアルのクラスを変えると反射像の見え方が変わること +
  クラス境界のアーティファクト目視

### S5. パラメータ調整（ユーザー判定ループ）

SVGF の `kRtReflMaxHistory` / `kRtReflAtrousIterations` 再調整、クラス別パラメータの追い込み、
visibility ray の要否判断。
- 検証: **ユーザーが実機で試してフィードバック**。ここはエージェントが良し悪しを判定しない

### S6. 仕上げ

ADR-016、`RtSelfTest.cpp`、`check_rules.ps1` の `$constGroups`、`engine_spec.md` §6 更新、
必要なら `tools\shot_verify.bat --update`。

---

## 検証

```
# 1. ビルド（vswhere で MSBuild を解決 — CLAUDE.md の手順）
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo

# 2. ヘッドレス回帰（ReSTIR 数学の CPU ミラーを含む）
bin\x64\Debug\Editor.exe --selftest

# 3. 静的規則検査（規則 9 = C++⇄HLSL 定数の一致）
pwsh -File tools\check_rules.ps1

# 4. ピクセル回帰 — ReSTIR 既定 off なので 19 枚とも無変更が期待値（先に Release ビルド）
tools\shot_verify.bat

# 5. リプレイ決定論（描画はハッシュ非対象だが念のため）
tools\replay_verify.bat

# 6. 実機目視 — GUI なので cmd /c を挟む
cmd /c bin\x64\Release\Editor.exe --acoustic-demo
#    メニュー → RT 反射 on → ReSTIR on/off をトグル
#    RT Debug → 12（reservoir M）/ 13（ヒット側 ReflectionClass）
#    ProfilerWindow で ReflGpuMs / RestirGpuMs を S0 と比較
```

**受け入れ基準（リポジトリの既存ルール）**: ReSTIR off のとき既存の絵とビット一致。
`shot_verify` の 19 枚が赤くなったら後方互換が壊れた証拠として扱う。

---

## スコープ外（別タスク）

- **★ スキンメッシュを BVH に入れる** — `RenderSystem.cpp:823` の除外を外すには、スキニング後の
  頂点を GPU/CPU に持って BLAS を毎フレーム refit する必要がある。**実キャラクタで本計画の
  目的を達成するには必須の前提**だが、独立した大きめのタスクなので後続に切る
- **レイ間引き（sparse tracing + 時空間再構成）** — 元提案の「Ray 1/3」。反射は既に内部解像度 1/2
  （ピクセル数 1/4）で撃っているので上積みは 0.数 ms 見込み。S0 の実測値を見てから判断
- **オブジェクト単位のクラス上書き** — 現状は Material 単位。`RtInstance` は per-instance なので
  レイアウト変更なしで後から足せる
- **BVH に静的背景を入れず cubemap で代替** — ミス時に cubemap へ落とす設計。BVH が小さくなるが
  反射に映る静的物体が消えるトレードオフ
- **GI レーンへの ReSTIR 適用** — 同じ機構が使えるが、まず反射で成立させてから

## 未確定・実装時に確認すること

- `RtScene` / `RtSceneBuild` のどちらで `RtInstance.pad0` を埋めるのが自然か（**未読**）
- ReSTIR temporal と SVGF temporal の二重適用による specular ラグの度合い（S5 で実測）
- クラス境界（Hero の反射像と Prop の反射像が隣接する画素）で再利用パラメータが急変することに
  よるアーティファクトの有無 — 出るならクラス間で線形補間する
