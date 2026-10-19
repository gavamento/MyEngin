# M94: 2D レーンの拡充 — 事前調査 (司会 → planner)

司会 (メインセッション) が 2026-10-19〜20 に調べた現状と方針案。**planner はこれを仮説として扱い、実コードで裏取りする。**
計画時の番号 (ABI 版・TypeId・ADR 番号・SimSnapshot 版) は着手時の末尾から取り直す。

## 0. ユーザー判断 (確定、変えるのはユーザーだけ)

| # | 論点 | 判断 |
|---|---|---|
| U1 | 2D スケルタルアニメ (ボーン・ウェイト・スプライトスワップ) | **やらない**。スプライト列アニメ (スプライトシート) で足りる |
| U2 | 見下ろし (Y ソート、XZ 平面) | **やらない**。横スクロール = XY 平面、Z は奥行き (ソートには使わない) |
| U3 | 2D 物理の方式 | 既存 3D ソルバ (`src\Engine\Engine\Physics\Rigid`) への**平面拘束**。専用 2D ソルバは作らない |
| U4 | タイルのセルの保存先 | **専用アセット** (`.tilemap.json` 等)。シーン JSON の中には置かない |
| U5 | ピクセルパーフェクトの余白 | **黒帯を作らない**。整数倍ズームで縦 (または短辺) を合わせ、余りはワールドを広く見せる (Unity Crop Frame = None 相当)。M70b の教訓 (UI の黒帯案が差し戻された) と同じ方針 |
| U6 | 2D ライトと影 | **範囲に含める** |
| U7 | 規模 | 下の段階 1〜5・7・8。段階 6 (骨) は U1 で除外 |
| U8 | planner の質問 | **推奨案で裁定して進める**。`[ユーザーに聞ける]` の印で返し、司会が Notion に記録する。ユーザーは後から変えられる |
| U9 | AGENTS.md | planner / coder は最初に **AGENTS.md を全文読む** (第 4 章の技術制約、第 5 章の規約、第 6・7 章の進め方と検証) |

## 1. 現状 (司会が確認した事実)

| 領域 | 現状 | 根拠 |
|---|---|---|
| スプライト | `SpriteRendererComponent` は VFX 用のワールド板 (texture / color / size / billboardMode)。テクスチャ全体のみ、flip・ソート層・スライス・pivot・9-slice なし。描画専用 = `kComponentNoHash`。`VfxRenderer` が透明メッシュの後・粒子の前に描く | `src\Engine\Core\Ecs\Components.h:856-866`、`src\Engine\Engine\Vfx\VfxRenderer.cpp` |
| カメラ | `CameraComponent` は `fovYDeg / nearZ / farZ / isPrimary` のみ。正射影なし | `Components.h:51-57` |
| 剛体 | 3D のみ。`freezeRotation` (bool) はあるが軸単位の位置・回転拘束は無い。フィールドは末尾 append でシーン互換を保つ慣例 | `Components.h:381-421` |
| コライダー | `ColliderComponent` 1 種に shape 列挙 (箱・球・カプセル・OBB・メッシュ・凸・複合、M59/M60) | `Components.h:213` 付近、`src\Engine\Engine\Physics\Collider` |
| テクスチャ設定 | `.meta` の `TextureImportSettings` は srgb / mips / cook のみ。**フィルタ (Point) と PPU が無い。wrap はサンプラがパス単位のため v1 で見送り** と明記 | `src\Engine\Core\Asset\ImportMetaResolver.h:9-12` |
| タイルマップ・アトラス・スプライトアニメ・2D ライト・ピクセルパーフェクト | **存在しない** (grep 0 件)。粒子の flipbook (M42) だけある | |
| ソート順の前例 | UI Canvas の `sortOrder` → `UIElement.order` → `entity.index` の 3 段キー、デカールの `sortOrder` + タイブレーク | `Components.h:517-531`、`src\Engine\Renderer\Pipeline\RenderTypes.h:491-530` |
| 画面 UI | M75 の RectTransform / Canvas / Layout / ウィジェットは**画面空間**。ワールド 2D ではないが、矩形解決の純関数 `uilayout::Resolve` と「描画とヒットテストが同じ関数」の構造は参考になる | `plans\m75-ugui.md`、`src\Engine\Engine\UI\UILayout.h` |
| アニメ基盤 | Animator Controller (`.controller.json` v2) は状態・遷移・型付きパラメータ 16 スロット・1D/2D ブレンド・イベント (script/sound/effect/noise)・描画補間を持つ。スケルタルは pose program (ADR-027) | `engine_spec.md` §12.2 Animation 行、`src\Engine\Engine\Animation` |
| サブアセット | `.meta` GUID をキーにしたサブアセット ID (M74、ADR-019)。クローン・改名に強い | `docs\adr\ADR-019-guid-subasset-keys.md` |
| プロジェクトシェーダ | M78 の `.fxstack.json` (ポスト・コンピュート) と M79 のサーフェスシェーダ (Forward / Deferred 両対応、Properties DSL) | `plans\m78-project-shaders`、`plans\m79-project-surface-shaders` |
| 番号の末尾 (2026-10-20) | ABI **v30** = `MYE_API_VERSION 30u` (M75h が v31 を予約、未着手)、TypeId **80** (`TwoBoneIK`)、SimSnapshot **v50**、ADR **032**、Scene `kDocVersion` は要確認 | `src\Shared\EngineAPI.h:56`、`src\Engine\Core\Ecs\Components.cpp` |
| 検証基盤 | `Editor.exe --selftest` (45+ スイート)、`tools\replay_verify.bat` (Debug/Release/WARP のハッシュ一致)、`tools\shot_verify.bat` (golden、Release 要)、`tools\check_rules.ps1`、`/p:MyeWarnAsError=true` | `AGENTS.md` 第 7 章 |

## 2. 参照したエンジン (Unity 2D)

- **Sprite**: テクスチャのサブアセット (矩形・pivot・PPU・9-slice の border・カスタム物理形状)。SpriteRenderer は flipX/Y、Draw Mode (Simple / Sliced / Tiled)、Sorting Layer + Order in Layer、Mask Interaction。Sorting Group で子をまとめてソート。
- **Pixel Perfect Camera**: Assets PPU と基準解像度を揃え、低解像度 RT に描いて拡大 (Upscale Render Texture)、スプライトを画素格子へ吸着 (Pixel Snapping)。テクスチャは Filter = Point、Compression = None。
  - https://docs.unity3d.com/6000.3/Documentation/Manual/urp/2d-pixelperfect-ref.html
  - https://docs.unity3d.com/6000.0/Documentation/Manual/urp/2d-pixelperfect-configure.html
- **Tilemap**: Grid → Tilemap → TilemapRenderer。Tile アセットの Collider Type (None / Sprite / Grid) から TilemapCollider2D が形状を作り、CompositeCollider2D が隣接を結合 (Unity 6 は Merge / Intersect / Difference / Flip)。Rule Tile (2D Extras) は隣接 8 方向の条件でタイルを選ぶ。
  - https://docs.unity3d.com/6000.0/Documentation/Manual/tilemaps/work-with-tilemaps/tilemap-collider-2d.html
  - https://docs.unity3d.com/Manual/class-CompositeCollider2D.html
- **2D 物理**: Box2D ベースの Rigidbody2D (Dynamic / Kinematic / Static、Constraints = Freeze X / Y / Rotation Z)、Collider2D (Box / Circle / Capsule / Polygon / Edge / Tilemap / Composite)、Physics Material 2D、Effector2D (Platform / Area / Point / Surface / Buoyancy)、Joint2D (Hinge / Distance / Spring / Slider / Wheel / Fixed / Relative / Target / Friction)。
  - https://docs.unity3d.com/Manual/2d-physics/2d-physics.html
- **URP 2D Renderer**: Light2D (Global / Freeform / Sprite / Point)、Sorting Layer ごとのライトブレンドスタイル、Shadow Caster 2D、法線マップ対応。
- **Cinemachine 2D**: Framing Transposer (デッドゾーン・ソフトゾーン・ルックアヘッド・減衰)、Confiner 2D (ポリゴン境界)。

## 3. 方針案 (planner が仕様へ落とす前提。却下した案も spec の「疑った点と結論」へ)

### 3.1 座標系とカメラ
- XY 平面、+Z が奥。カメラは `CameraComponent` に `projection` (0 透視 / 1 正射影) と `orthoSize` (半高、ワールド単位) を**末尾 append** (既存シーンのビット同一を保つ。既定は透視)。
- Scene View の 2D モード: 正射影・XY 固定・ズームとパン・グリッド・ソート層の可視化・2D コライダーのギズモ。
- 正射影の投影行列は Renderer 内で閉じ、D3D 型は外へ出さない (AGENTS.md 4.1)。

### 3.2 スプライト資産とテクスチャ設定
- `.meta` の `TextureImportSettings` に `filter` (0 auto / 1 linear / 2 point)、`pixelsPerUnit`、`sprites[]` (name / rect / pivot / border / physicsShape 任意) を追加。各スライスは **M74 のサブアセット GUID キー**で参照する。
- サンプラはパス単位なので、Sprite2D パスが Point / Linear の 2 本を持ち、テクスチャの `filter` で選ぶ。
- スプライトエディタ窓: グリッド分割・自動分割 (透明画素の連結成分)・pivot・border。Undo 対応。
- ソート層は `project_settings.json` の名前付き配列 (順序 = 描画順)。参照は名前ではなく**安定 ID** (改名に耐える)。

### 3.3 Sprite2D 描画
- 新コンポーネント `Sprite2D` (TypeId は末尾から): sprite (サブアセット)、color、flipX/Y、drawMode (Simple / Sliced / Tiled)、size (Sliced / Tiled 用)、sortingLayer、orderInLayer、maskInteraction。描画専用 = NoHash (既存の SpriteRenderer と同じ扱い。**ただしスプライトアニメの「現在のコマ」は sim 状態なので別コンポーネントかアニメ側に置く**)。
- 専用パス (`Sprite2DPass`): 透明キューに相当する位置。ソートキーは **(ソート層 → 層内順序 → Sorting Group → entity.index)** の整数キーだけ。**浮動小数点の距離ソートは使わない** (決定的で golden が安定)。テクスチャ単位でバッチ、動的 VB。
- Sorting Group: 子孫のスプライトを親の 1 キーにまとめる。Sprite Mask: ステンシル。
- プロジェクトのスプライトシェーダ: M79 のサーフェスシェーダの枝として `Sprite2D` 対応 (フラッシュ・アウトライン・ディゾルブ・パレット差し替え)。Deferred には載せない (スプライトは Forward 的に描く)。
- 2D 用ポスト (パレット量子化・CRT・画面揺れ) は M78 の fxstack に乗せる。

### 3.4 スプライト列アニメ
- Animator Controller に **`"sprite"` クリップ**を 1 種追加: コマ列 (スプライト参照 + 継続 tick)。状態・遷移・型付きパラメータ・イベント (sound / effect / noise / script) を**そのまま流用**。
- 現在コマの決定は tick 基準 (`statePhase` 等の既存の Q16/uint32 位相の流儀)。sim 状態なので SimSnapshot・ワールドハッシュ・replay に入る。描画補間は不要 (コマは離散)。
- Animator が付いたエンティティの Sprite2D へ現在コマを書く経路を 1 本に固定し、スクリプトから `sprite` を直接書くのと競合しないルールを決める (Unity は Animator が勝つ)。

### 3.5 タイルマップ
- アセット `.tilemap.json` (U4): グリッド設定 (セルサイズ、原点)、レイヤー配列 (名前・ソート層・順序・コライダー有無)、チャンク (16x16) ごとのセル配列 (タイル ID + flip/回転ビット)。タイルパレット `.tilepalette.json`: タイル定義 (スプライト参照・Collider Type (None / Grid / Sprite)・一方通行フラグ・HP・ルールタイル定義・アニメーションタイル定義)。
- コンポーネント `Tilemap` がアセットを参照し、描画はチャンク単位のメッシュを**変更差分で再構築** (tick 側で dirty チャンクを記録、描画側で焼く)。
- 衝突: Collider Type から**行単位に結合した箱**と辺コライダーを生成 (Composite 相当)、一方通行床は上面だけの辺として生成。`ColliderComponent` に新 shape を足すか、タイルマップ専用の静的ブロードフェーズを持つかは planner 判断 (計測で決める。AGENTS.md 3.5)。
- ルールタイル: 隣接 8 方向のマスクでスプライトを選ぶ。アニメーションタイル: tick 基準でコマ送り (決定的)。
- スクリプト API (ABI): セル読み書き、ワールド ⇄ セル変換、範囲塗り。セルの書き換えは sim 状態なので**セル配列をハッシュ対象**にし、SimSnapshot にも入れる (アセットは初期値、差分は snapshot へ)。
- 破壊可能タイル: セル HP と破壊時の差し替え・エフェクト生成。M80 の破壊とは経路を分ける (タイルは軽量)。
- タイルパレット窓: ブラシ・消去・塗り潰し・矩形・選択、Undo 対応。シーン上で直接塗る。

### 3.6 2D 物理 (U3: 平面拘束)
- `RigidbodyComponent` に `plane2D` (bool、末尾 append、既定 off = 既存シーンのビット同一) を足す。ON のとき**各サブステップの積分直後**に z 位置・z 速度・X/Y 角速度を**厳密に 0.0f** へ書く (スリープの流儀と同じ、残留ビットを消す)。接触解決は 3D のまま使う。
- 2D 向けコライダー shape: 円 (Z 方向に薄い円柱ではなく**球を使う**と z 拘束で同じ結果か、planner が確認)、カプセル (既存)、多角形 (凸分解)、辺 (チェーン)。
- 2D クエリ ABI: `Raycast2D` / `BoxCast2D` / `OverlapCircle2D` (レイヤーマスク付き)。3D の `RaycastWorld` を z=0 で呼ぶだけで済むものはそうする。
- エフェクター: 一方通行床 (接触法線で無効化)、風・ベルトコンベア (接触面速度)、浮力 (M59b2 の浮力を XY で流用)、引力エリア。
- 2D ジョイント: ヒンジ・距離・スプリング・スライダー・ホイールを M60 のジョイントの平面版として (新規に書かず、拘束軸の指定で表現できるか planner が確認)。
- replay_verify に「平面拘束 ON の剛体の z が 3000 tick 後も厳密に 0」を固定するシーンを足す。

### 3.7 CharacterController2D
- 接地・坂 (slopeLimit)・段差 (stepOffset)・一方通行床・移動床 (乗っている床の速度を継承)・登攀面 (はしご)・壁接触の報告。既存 `CharacterControllerComponent` (`Components.h:840`) の 2D 版。
- **Rigidbody との双方向** (dogfooding 17 の教訓): CC2D が剛体を押し、剛体が CC2D を押す。
- コヨーテタイム・ジャンプバッファはゲーム側 (スクリプト) の責務とし、エンジンは「接地を離れてからの tick 数」などの素材だけ出す。

### 3.8 2D ライトと影 (U6)
- `Light2D` (Global / Point / Spot / Freeform)、ソート層ごとの適用、`ShadowCaster2D` (スプライトの輪郭 or 任意ポリゴン)、法線マップ対応スプライト。
- 専用のライトバッファ (画面サイズ、RGB) に光を加算し、Sprite2D パスで乗算する。Deferred の G-Buffer とは独立。
- 影は 1D シャドウマップ (角度 → 距離) か、ポリゴン影の押し出しか。planner が計測で選ぶ。

### 3.9 形状: Line2D・Sprite Shape
- `Line2D` (点列・幅・色・テクスチャ) と塗り潰しポリゴン。
- `SpriteShape`: スプライン (制御点・接線) に沿って縁スプライトを敷き、内側を塗りスプライトで埋める。コライダーも同じスプラインから辺チェーンを生成。スプライン編集は Scene View のハンドル。

### 3.10 カメラ追従・AI・視差
- `Camera2DFollow`: 対象・デッドゾーン・ルックアヘッド・減衰・境界矩形 (タイルマップの矩形から自動)。**tick 側で決定的に動かし**、描画補間は M89f と同じ `interpAlpha`。
- グリッド A*: タイルマップから歩行可能判定 (ジャンプリンク任意)。BT の MoveTo が 2D でも使えるよう `NavMeshAgent` と並ぶ `GridAgent2D` を足すか、Nav の抽象へ寄せるか planner 判断。Recast は使わない。
- AIPerception の視野扇形を 2D で判定・描画。
- `Parallax2D`: カメラ移動に対する倍率と繰り返し。描画側だけで完結 (NoHash)。

### 3.11 ピクセルパーフェクトカメラ (U5)
- `PixelPerfectCamera2D`: 基準解像度・Assets PPU・整数ズーム・スナップ。低解像度 RT に描いて Point で整数倍拡大。余りは黒帯にせずワールドを広く見せる。
- 画面 UI (M75 Canvas) はピクセルパーフェクトの RT の**外**で通常解像度に描く (UI がぼけない)。

### 3.12 ABI・デモ・文書
- ABI bump は **1 回に寄せる** (段階 8)。Sprite2D の sprite 設定、スプライトアニメの再生、タイルのセル読み書き・座標変換、2D クエリ、CC2D の入力・状態、Camera2DFollow の対象。C# ミラー (`Interop.cs` は位置ミラー + 版検証なし → 順序を機械照合、`abi-bump-verification` の手順) と MyeScript の糖衣。
- `--2d-demo`: 横スクロールの 1 面 (タイルマップ地形・一方通行床・移動床・敵のグリッド A*・2D ライト・視差・ピクセルパーフェクト)。shot_verify の golden と replay_verify のペアに登録。
- engine_spec の節 (§6 に 2D 描画、§10 にタイルマップ・スプライト資産、§11.3 に 2D の検証)、ADR 1 本 (2D レーンの方式: 平面拘束・整数ソートキー・アセット保存)、`docs\test_checklists.md` の M94 節、README。

## 4. 段階とサブ分割の目安 (段階 6 の骨は U1 で除外)

| 段階 | 内容 | 目安 |
|---|---|---|
| 1 基盤 | 正射影カメラ + Scene View 2D モード、`.meta` のフィルタ / PPU / スライス + スプライトエディタ窓、ソート層、Sprite2D パス (flip / Sliced / Tiled / Sorting Group / Mask) | 4 サブ |
| 2 動き | スプライト列クリップ、平面拘束 + 2D コライダー + 2D クエリ、CharacterController2D + 一方通行床 + 移動床 | 3〜4 サブ |
| 3 タイル | タイルマップ資産・チャンク描画・衝突、タイルパレット窓、ルールタイル + アニメーションタイル + レイヤー、破壊可能タイル | 4 サブ |
| 4 見た目 | 2D ライト + 影 + 法線マップ、スプライトシェーダ (M79 の枝)、視差、2D ポスト (M78 の枝) | 3〜4 サブ |
| 5 形状 | Line2D、Sprite Shape (描画 + 衝突 + スプライン編集) | 2 サブ |
| 7 周辺 | 2D ジョイント + エフェクター、グリッド A* + 2D 知覚、Camera2DFollow、ピクセルパーフェクト | 4 サブ |
| 8 締め | ABI bump + C# ミラー + MyeScript、`--2d-demo` + golden + replay、engine_spec / ADR / test_checklists / README | 2 サブ |

合計 22〜24 サブ。段階 1〜3 で横スクロールゲームが作れる。段階 4・5・7 は互いに独立で順序を入れ替えられる。
サブは依存順に 1 本ずつ (司会が 1 サブ = coder 1 インスタンスで回す)。

## 5. エンジンの契約に触れる点 (planner が spec 4.4 と受け入れ条件に必ず書く)

- **決定性**: スプライトアニメのコマ、タイルセル、平面拘束の剛体、CC2D、Camera2DFollow、グリッド A* は sim 状態 → ハッシュ・SimSnapshot・`.rep` に入る。描画側だけのもの (Sprite2D の見た目、視差、ライト、ピクセルパーフェクト) は NoHash。境界を spec で表にする。
- **既存シーンのビット同一**: 新フィールドは末尾 append + 既定 off。既存 golden (`ui_probe*` / `flow_title` / `render_bench*` 等) は maxDiff=0 を手で当てる。動いてよいのは新設の `2d_*` だけ。
- **版 bump の回数を宣言**: Scene `kDocVersion`、SimSnapshot、`.rep`、ABI、`kCookVersion` のどれを何回上げるかを spec に先に書く。計画外の bump は設計ミスとして扱う (M75 の不変条件と同じ)。
- **検証**: Debug / Release を `/p:MyeWarnAsError=true` で 0 警告 → `Editor.exe --selftest` → `pwsh tools\check_rules.ps1` → `tools\shot_verify.bat` (Release) → `tools\replay_verify.bat`。ソース追加時は `pwsh tools\gen_project_files.ps1`。exe は PowerShell ツールから起動 (Bash の `cmd /c` は実行されない)。
- **ローカライズ**: エディタ UI の文字列は `LocalizationTable.inl` + `Tr()`、両言語を埋める。
- **コメント**: 日本語で理由を書く (`japanese-comment-style`)。新規 .h/.cpp はヘッダー付き。
- **浮動小数点**: `/fp:precise`、sim レーンは scalar (SIMD 禁止)、`rand()` 禁止。

## 6. 司会が planner に特に裏取りを求める仮定

1. 既存の `SpriteRendererComponent` を拡張するのではなく **別コンポーネント `Sprite2D`** にする (VFX のビルボードと責務が違う)。妥当か。
2. **平面拘束で z が厳密に 0 のまま**か (接触解決が z 成分を生むなら、拘束の位置をサブステップ内のどこに置くか)。CCD・スリープ・島との相互作用。
3. タイルマップの**セル配列を SimSnapshot に入れる**コスト (1 面 = 数千〜数万セル、v50 の snapshot サイズと分岐デバッグ M72 の lane コピーへの影響)。差分保存 (アセット初期値 + 変更セルのリスト) で足りるか。
4. Animator Controller に **sprite クリップを足す**のと、別の軽い `SpriteAnimator` を新設するのと、どちらが M89 の構造を壊さないか。
5. ソート層の **安定 ID** の持ち方 (project_settings の配列順を ID にしない)。
6. **ピクセルパーフェクトの RT** と M75 Canvas・ImGui・ポストエフェクトの描画順。TAA (M54) は 2D では off にする前提でよいか。
7. 2D ライトバッファと Forward / Deferred の両経路の関係 (2D は Forward 固定でよいか。`--deferred` でも同じ絵になる必要があるか)。
