# M94 2D レーンの拡充 — 仕様書

- slug: m94-2d
- 状態: 確定 (2026-10-20。U1〜U9 はユーザー判断、それ以外の論点は planner が裁定し 2. に `[ユーザーに聞ける]` の印を付けた。司会が Notion「M94 2D: ハーネスの保留質問」へ写す)
- 依頼原文: 2D レーンと2D分野をさらに拡充させたい
- 事前調査: `C:\HAL\MyEngin\plans\m94-2d\design-draft.md` (司会)。§0 の U1〜U9 は確定、§1〜§6 は本書で裏取りした
- 基点コミット: `25fb355`
- **coder / reviewer は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (第 4 章 = 必須制約、第 5 章 = 規約、第 6・7 章 = 進め方と検証)。本書の受け入れ条件はそれを前提に書いてある

## 1. 目的 (なぜ作るか)

MyEngine で**横スクロール 2D ゲーム** (XY 平面、+Z が奥) を、3D と同じ契約 (Debug / Release / WARP / Server.exe のシミュレーションがビット一致、リプレイ、What-if 分岐、ロールバック) のまま作れる状態にする。
現状は 2D 専用の部品がゼロ (正射影カメラ無し、スプライトは VFX 用ビルボードで UV・ソート層・flip 無し、タイルマップ・アトラス・スプライト列アニメ・2D ライト・ピクセルパーフェクト無し。`design-draft.md` §1 を実コードで確認済み)。
到達点: 段階 1〜3 (基盤・動き・タイル) が終わった時点で、**タイルマップの地形 + 一方通行床 + 移動床 + スプライト列アニメの主人公 + 2D 物理の箱**という横スクロールの 1 面が、エディタで組めて GameLogic (C++ / C#) から操作でき、`replay_verify` を通る。段階 4・5・7 は見た目と周辺、段階 8 はデモ・文書。

## 2. 疑った点と結論

U = ユーザー判断 (design-draft §0)。`[ユーザーに聞ける]` = planner の裁定で進めるが、司会が Notion に記録してユーザーが後から変えられるもの。

| # | 疑い | 根拠 (コード / 事実) | 判断 | 結論 |
|---|---|---|---|---|
| 1 | 既存 `SpriteRendererComponent` を拡張すれば足りるか (司会の仮定 1) | `Components.h:859-865` は texture / color / size / billboardMode のみ。`VfxRenderer.cpp:244-318` は Sprite / Trail / TextMesh をまとめて **viewZ 降順 (浮動小数点)** に並べ、UV は 0..1 固定 (:352-356)、常に LINEAR (:364)。ビルボード基底は `vfx::BillboardBasis`。2D に要るのは整数ソートキー・UV 矩形・flip・Sliced/Tiled・Point サンプラで、責務が別物 | — | **別コンポーネント `Sprite2D` と専用パス** (4.1.3)。SpriteRenderer は 1 バイトも触らない = 既存 golden (`fog` / `particle_*` / `demo_*`) の不変が自明になる |
| 2 | スプライト列アニメを Animator Controller のクリップ種にするか、軽い `SpriteAnimator` を新設するか (仮定 4) | コントローラは `"skel"` (骨、名前参照) と `"clip"` (プロパティクリップ `.anim.json`) の 2 口を持つ (`AnimatorController.cpp:1095-1140`)。プロパティクリップは **float 系フィールドしか駆動できず** (`Animation.cpp:73-84` `FieldFloatCount`)、遷移中は Step でも線形ブレンドする (:207-209)。イベントと描画補間は骨専用。長さは `ControllerStateLengthTicks` (:1542) で決まり、BT の PlayAnimation もこれを共有 | — | **コントローラのステートに第 3 の口 `"sprite"` を足す** (4.1.4)。遷移・型付きパラメータ・イベント (sound / effect / noise / script)・`AnimatorPlay` ABI・BT PlayAnimation をそのまま流用できる。**現在コマは `stateTimeTicks` (既に hash 対象) から導出する値**なので新しい sim 状態は増えない (M89 の原則「時刻は sim 状態、ポーズは導出値」と同じ)。却下: `SpriteAnimator` 新設 = 遷移・パラメータ・イベント・ABI を二重に持つ。却下: プロパティクリップで `Sprite2D.sprite` を駆動 = AssetRef を float トラックで書けない |
| 3 | 平面拘束で z が厳密に 0 のまま保てるか。拘束をサブステップ内のどこに置くか (仮定 2) | `PhysicsSystem::Update` (`PhysicsSystem.cpp:1525-5063`) のサブステップは 速度積分 → 外力 → ブロードフェーズ → 関節 → 接触生成 → 速度ソルバ (8 回) → **位置補正パス (8 回、`translateBody` が法線の z で押す :4453)** → XPBD → CCD → 位置・姿勢積分 (:4826-4868)。`freezeRotation` は `invI` を零行列にするだけで角速度は 0 にしない (:2635-2654、:5056)。球-球の法線は `dz/d` (`Shapes.cpp:99-108`)、摩擦の接線基底は法線 (0,1,0) のとき Z 方向 (:3978-3990)。入眠時は `v = w = 0.0f` を厳密に書く (:5013-5016) | U3 (平面拘束、専用ソルバは作らない) | **3 点で拘束する** (4.1.5): (A) 収集時に `plane2D` なら `invI` を Z 軸成分だけ残す (接触・関節が X/Y 回転を注入できない)、(B) 速度ソルバの後・位置補正パスの前に `vz = wx = wy = 0.0f` を代入、(D) 位置・姿勢積分の後に `pz = z0` (tick 開始時の z)・`qx = qy = 0.0f` を代入し `vz = wx = wy = 0.0f` を再度書く。CCD の移動量 (:4840) も D で掴む。OFF なら 1 演算も増やさない分岐ゲート (M59f1 の流儀)。**成立の証明は sub-01 の selftest** (Debug / Release で 3000 tick 後の z / vz / wx / wy / qx / qy がビット一致で初期値 / 0.0f)。成立しなければ ABORTED ではなく、拘束点を増やす (毎反復射影 = 候補 C) |
| 4 | z を 0 に強制するか、初期値を保つか | 3D の衝突判定は z を見る。Unity 2D は z を無視するが、ここは 3D ソルバ流用なので「z が違う物は当たらない」 | `[ユーザーに聞ける]` | **初期値を保つ** (z0 = tick 開始時の z)。見た目の奥行きで z をずらした物を物理に入れても落ちない。**異なる z の plane2D 物同士は当たらない** (Unity と違う) と Inspector の注記と engine_spec に書く。却下: z=0 強制 = シーン配置で z をずらした瞬間に飛ぶ |
| 5 | CharacterController2D を新コンポーネントにするか (draft 3.7) | 既存 CC (`Components.h:841-853`、`SolveCharacters` `PhysicsSystem.cpp:1375-1465`) は Y が上、`moveInput` の x/z が水平。2D は Y 上・X 水平で **z を固定するだけ**。段差 (`stepOffset`) と坂 (`slopeLimitDeg`) は既にある。無いのは一方通行・移動床の速度継承・剛体を押す応答 (:1373-1374 は一方向と明記) | — | **既存 CC に `plane2D` を末尾 append** し、2D 専用の追加 (一方通行の通過・移動床継承・剛体押し・壁接触の報告) はこのフラグの中だけで有効にする (4.1.6)。既定 off = 既存 CC はビット同一。却下: 新コンポーネント = TypeId・Inspector・Nav/BT/ルートモーションの CC 連携 (`AnimatorControllerComponent.applyRootMotion` が CC.moveInput へ書く) を全部二重化する |
| 6 | CC の形状はカプセルのままでよいか (ピクセル系プラットフォーマーは箱 CC を好む) | `SolveCharacters` はカプセル専用 | `[ユーザーに聞ける]` | **カプセルのまま** (既存の押し出し・段差・坂のコードを流用。箔は 2D でも坂で引っかからない)。逆を選ぶと sub-09 に箱 CC の押し出しが 1 本増える |
| 7 | タイルマップのセル配列を SimSnapshot に入れるコスト (仮定 3) | World 節はカラムの生バイト、外部状態は節 (`SimSnapshot.cpp:593-624`、Compute の例 :562-589)。予算は TimeTravel 側 (64 MB、1 枚 148 KB 既定デモ)。1 面 = 100×50 セル × 4 B = 20 KB でも毎 tick 30 間隔 120 枚で 2.4 MB | U4 (専用アセット) | **アセットが初期値、sim 状態は「書き換えたセルの差分」だけ** (4.2.3)。`TilemapSystem` の override 表 (ソート済み (entity, layer, cellKey) → 値) を内容ゲートで hash・snapshot する。Tilemap が無いシーンは hash も snapshot も 1 バイトも変わらない (Nav / Compute と同じ presence gate)。復元後は衝突キャッシュとチャンクメッシュを全部 dirty にする (導出値、`AcousticField::Invalidate` の流儀) |
| 8 | タイル衝突を `ColliderComponent` の新 shape にするか、専用ブロードフェーズを持つか (draft 3.5) | 静的コライダーの収集は `PhysicsSystem.cpp:1701` の 1 箇所。ブロードフェーズは sort & sweep (:3550-3606)。kMesh は三角形スープで静的専用 | — | **行方向に結合した箱 (Composite 相当) を静的収集に 1 つの供給元として足す** (4.1.7)。箱の数は 100×50 の床で数百以下 (計測を受け入れ条件に)。専用ブロードフェーズは「計測で足りなければ」(AGENTS.md 3.5) — sub-11 で 1 tick の物理時間を測り、既存の `--physics-demo` の 2 倍を超えたらチャンク AABB の前カリングを足す |
| 9 | 一方通行床の判定方式 | Unity Platform Effector 2D は Surface Arc (ローカル上を中心とする弧) の外からの接触を無効化する (https://docs.unity3d.com/Manual/2d-physics/effectors/platform-effector-2d-reference.html 、既定値は未記載) | `[ユーザーに聞ける]` | **`ColliderComponent.oneWay` (bool、末尾 append)**。接触を採るのは「接触法線 (床→物体) がローカル +Y 側 (n·up > 0) **かつ** 物体の tick 開始時の最下点が床の天面 − slop 以上」のとき。CC も同じ規則 + `ignoreOneWay` (下入力で通り抜け)。弧の角度は固定 180° (Unity の Surface Arc は作らない。逆を選ぶと float 1 本増えるだけ)。タイルの一方通行もこのフラグで同じ経路を通る |
| 10 | ソート層の安定 ID の持ち方 (仮定 5) | `project_settings.json` の前例: `physicsLayers` / `tags` は添字 = ID、`partTags` は名前ハッシュ (改名で切れる)、`navAgentTypes` は要素に安定 `int id` (最大 + 1、`NavAgentTypes`) | `[ユーザーに聞ける]` | **`sortingLayers: [{id, name}]`、配列順 = 描画順、`id` は 0..31 の未使用最小値を割り当て、削除で空く**。コンポーネントは `sortingLayerId` を持つ。**上限 32** にするのは 2D ライトの `targetLayerMask` (uint32、bit = id) のため。id 0 "Default" は削除不可。参照先の無い id は Default として描き Inspector に警告。逆 (上限なし) を選ぶとライトの対象指定が名前リストになり、Light2D のフィールドが POD に収まらない |
| 11 | スプライト (スライス) をサブアセットとしてどう参照するか | M74: `AssetID = HashStr("guid://<16hex>#mesh3#part0")` (`AssetKeyResolver.h:29`、`DemoContent.cpp:4977`)。名前をキーにすると改名で ID が変わる | — | **`.meta` の `tex.sprites[]` が各スライスに安定 `id` (テクスチャ内で最大 + 1、改名・並べ替えで不変) を持ち、`AssetID = HashStr(SubAssetKeyPrefix(texPath) + "#sprite" + id)`**。テクスチャ自身の AssetID も「全面 1 枚のスプライト (pivot 中央、PPU は meta)」として受理する = スライスしていない絵をそのまま貼れる (4.2.1) |
| 12 | テクスチャ単位の Point / Linear をどう実現するか (`ImportMetaResolver.h:10` は wrap を見送った) | サンプラはパス単位。`VfxRenderer` / `UIRenderer` は**バッチのフラグで s0 を point / linear に切り替える** (`VfxRenderer.cpp:483-486`、`UIRenderer.cpp:407-446`)。`VfxRenderer.cpp:480` の「サンプラを増やさない」は VfxRenderer のフォグ統合契約で、他パスの新設を禁じるものではない (UIRenderer / DeferredPath は自前で持つ) | — | **Sprite2D パスが point / linear の 2 サンプラを持ち、`SpriteDef.filter` でバッチを分ける**。`.meta` の `tex.filter` (0 auto = linear / 1 linear / 2 point)。wrap は引き続き見送り (Tiled は頂点を並べて実現するので wrap サンプラ不要) |
| 13 | ピクセルパーフェクトの RT と UI / ImGui / ポストの順序。TAA は off でよいか (仮定 6) | 全段が `target.width/height` 等倍 (`RenderSystem.cpp:645`、:2352)。UI Canvas と ImGui はポストの**後**に同じ rtv へ描く (`EngineLoop.cpp:3251, 3274`)。TAA は Deferred + taaOn のときだけ (:768-770)。render scale / upscale 機能は無い。Unity の式 (一次情報 = 実装): `zoom = max(1, min(H / refY, W / refX))` (整数除算)、crop 無し + upscaleRT で `RT = (W / zoom / 2 * 2, H / zoom / 2 * 2)`、`orthoSize = RT.h * 0.5 / assetsPPU` (https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.universal/Runtime/2D/PixelPerfectCameraInternal.cs) | U5 (黒帯なし、余りはワールドを広く) | **`RenderSystem::Render` の中で閉じる**: 主カメラに `PixelPerfectCamera2D` があり zoom > 1 なら、シーン (3D + Sprite2D + ポスト) を低解像度 RT に描き、Point で `target.rtv` へ整数倍 blit する。UI Canvas / ImGui は従来どおり後段でフル解像度に描く (ぼけない)。**TAA のジッタと FXAA はピクセルパーフェクト中は無効** (ドットが滲む)。ポストは低解像度で掛かる (レトロ表現として意図どおり、engine_spec に明記)。式は Unity と同じ (4.1.12) |
| 14 | 2D ライトバッファと Forward / Deferred の関係 (仮定 7) | Sprite2D は無灯なので Forward / Deferred で同じ絵。Unity URP 2D は「影テクスチャ → ライトテクスチャ → それを使って描く」(https://docs.unity3d.com/6000.0/Documentation/Manual/urp/Lights-2D-intro.html) | U6 (含める) | **画面サイズのライトバッファ (RGBA16F) を Sprite2D パスの直前に作り、Sprite2D の PS で乗算する**。G-Buffer とは無関係 = 両経路で同じ絵。**presence gate: シーンに `Light2D` が 1 つも無ければライトバッファを作らず従来色** (golden 不変)。ソート層ごとの適用は `targetLayerMask` で、描画順に層を走り「効くライトの集合が変わるたびに」バッファを作り直す (4.1.13) |
| 15 | 2D の影を 1D シャドウマップにするか、ポリゴン押し出しにするか (draft 3.8) | Unity は Shadow Caster 2D の形状をライトから押し出して影テクスチャに描く方式 (https://docs.unity3d.com/6000.0/Documentation/Manual/urp/ShadowCaster2D.html)。ステンシルは未使用で D24S8 は揃っている (`SwapChain.cpp:86`、`RenderTexture.cpp:59`) | — | **ポリゴン押し出し**。辺ごとにライトから遠方へ押し出した四辺形を R8 の影 RT に描き、ライト形状の PS で `1 − shadow × strength` を掛ける。1D シャドウマップは角度サンプリングのエイリアスと、ライトごとの RT が要る分だけ不利 |
| 16 | 法線マップ対応スプライトを v1 に入れるか | Unity は Secondary Textures とライトの Normal Map Quality。ライトバッファが色だけでは N·L が出せず、法線 RT の 1 パスが要る | `[ユーザーに聞ける]` | **入れる (sub-16、影と同じサブ)**。`Sprite2D.normalMap` (同じ矩形のテクスチャ)。法線を持つスプライトだけ法線 RT へ描き、ライト形状の PS が N·L を掛ける。逆を選ぶと sub-16 が影だけになり受け入れ条件 33 が落ちる |
| 17 | グリッド A* を `NavMeshAgent` と並ぶ別コンポーネントにするか、Nav の抽象へ寄せるか (draft 3.10) | 自前 A* は無い (経路は Detour に任せる)。`AcousticNav` は 26 近傍 Dial 法の流れ場で A* ではない。BT の MoveTo は `NavMeshAgentComponent` へ直接書き `status` を読む (`BehaviorTreeSystem.cpp:507-566`)。抽象インターフェースは無い | `[ユーザーに聞ける]` | **`GridAgent2D` を新設し、`destination / hasDestination / status` を NavMeshAgent と同じ名前・同じ status 値で持つ。BT の MoveTo は NavMeshAgent が無ければ GridAgent2D を見る** (2 行の分岐)。Recast は使わない (U1 と draft)。却下: Nav の抽象化 = M82 の構造を今回の目的の外で動かす |
| 18 | ABI bump を段階 8 に寄せると、段階 3 の時点でスクリプトからタイルを触れない (司会の補足) | 汎用 `Get/SetComponentField` (EngineAPI.h:512-521) が AssetRef / EntityRef / bool / float を書ける = Sprite2D の sprite、CC の moveInput、Camera2DFollow の target、GridAgent2D の destination は**関数なしで届く**。`AnimatorPlay` は既存。届かないのはタイルのセル読み書き・座標変換・ダメージと、マスク付き OverlapBox・BoxCast。次の版 v31 は M75h が予約 (`engine_spec.md:3475`) | `[ユーザーに聞ける]` | **bump は 1 回、位置は段階 3 の末尾 (sub-14)**。版は着手時の末尾 (M75h より先なら **v31**、M75h が先に入れば v32。スロット数も着手時の `EngineAPI.h` から)。中身は 4.1.10 の 10 本。逆 (段階 8) を選ぶと段階 1〜3 の「横スクロールが作れる」がスクリプトからのタイル操作抜きになる |
| 19 | 2D 向け形状 (多角形・辺) をどう持つか | `collidershape::` は Sphere / Box / Capsule / Mesh (静的) / Terrain (静的) / Convex (凸、動的可) (`Components.h:203-211`)。POD コンポーネントに可変長の点列は入らない。kMesh / kTerrain は `meshAsset` を共有している | — | **`.shape2d.json` (点列) を `meshAsset` で参照する shape を 2 つ append: `kPolygon2D = 6`、`kEdge2D = 7`**。静的な多角形は z 方向に押し出したプリズムの三角形スープとして kMesh の経路へ (凹も可)、動的な多角形はプリズムの凸包として kConvex の経路へ (凹は埋まる、Inspector に注記)、辺は 1 線分 = 薄い箱 (半厚 `edgeRadius`) の列として静的収集へ (タイルの箱と同じ供給口)。Unity の Polygon / Edge Collider 2D 相当 |
| 20 | 2D ジョイントを新設するか | `jointtype::` Ball / Hinge / Fixed / Slider / Cone、軸は `axis` 1 本 (`Components.h:1367-1397`)。plane2D で X/Y 回転と Z 並進が消えれば、Hinge(axis Z) = 2D 回転関節、Slider(axis 任意 XY) = 2D スライダー、Fixed = 2D 固定、SpringJoint = 2D 距離ばね | — | **新設しない**。sub-20 の selftest で「plane2D + Hinge(axis=(0,0,1)) の 2 体が 1000 tick 後も不変量を満たし、Debug / Release で一致」を固定し、Inspector の JointComponent に「2D (Z 軸)」ボタンを 1 つ足す。ホイールは Hinge + 既存モータ |
| 21 | エフェクター (風・ベルトコンベア・浮力・引力) をどう置くか | `BuoyancyComponent` は env の水平な水面 (`PhysicsSystem.cpp:3024-`)。`ConstantForce` は自分の Rigidbody にしか効かない。トリガー Collider は `isTrigger` で既にある | — | **`Effector2D` 1 種 (type = Area / Surface / Buoyancy / Point)**。同じエンティティのトリガー Collider を領域にし、領域に重なる plane2D 剛体へ毎サブステップ力 (Area: 一定力、Point: 中心への力、Surface: 接触面速度 = コンベア、Buoyancy: 領域天面を水面として浸水率 × 密度)。sim 入力なので hash 対象 |
| 22 | タイルパレット窓の Undo をシーンの UndoStack に載せるか | `UndoStack` はシーン (fileId) 単位 (`UndoStack.h`)。アセット編集窓の手本は BT 窓の独自 Undo (`BehaviorTreeEditModel.h:179-238`、128 段) | `[ユーザーに聞ける]` | **BT と同じ独自 Undo (アセットのスナップショット deque)**。Play 中はパレット窓で塗れない (sim 状態への編集はスクリプト経由だけ、と窓に表示)。スプライトエディタ窓も同じ流儀。逆を選ぶと Play 中の塗りを sim の override として記録する経路が 1 本増える |
| 23 | Scene View の 2D モードをどこまで作るか | SceneView は Ortho トグルがあるが 1px = 0.02 固定でズーム無し (`SceneViewWindow.cpp:228-231`)、グリッドは XZ 固定 (`EditorLinePass.cpp:239-250`)、円の API は無い (扇形は `AddLine` の折れ線) | — | **「2D」トグル**: 正射影 + XY 正面固定、ホイールで orthoSize ズーム、中/右ドラッグでパン、XY グリッド、ImGuizmo を XY 並進 + Z 回転に制限、Sprite2D の矩形・pivot・2D コライダー (箱 / 球 = 円 / 多角形 / 辺) のギズモ。ビューキューブは作らない |
| 24 | スプライト同士の Z ソートを完全に捨ててよいか | U2 で「Z は奥行き、ソートには使わない」。Unity の既定も Sorting Layer → Order in Layer → (同値のときだけ) 距離 | U2 | **ソートキーは (層の配列順 → orderInLayer → SortingGroup → entity.index) の整数だけ** (4.1.3)。ZWrite off / ZTest LEqual なので 3D メッシュには隠れうる (Unity と同じ)。浮動小数点ソートを持たないので golden が安定 |
| 25 | スプライト用のプロジェクトシェーダ (M79 の枝) の接続点 | サーフェス経路は `RenderItem` (メッシュ + Material) 前提 (`ForwardPath.cpp:647-665, 796-859`)。`MyEngineSurfaceEntries.hlsli` に 5 エントリを連結してコンパイル (`ShaderManager.cpp:1049-1074`) | — | **`MyEngineSurfaceEntries.hlsli` にスプライト用エントリ (`MyeVSSprite2D` / `MyePSSprite2D`、Sprite2D の頂点レイアウト) を 1 系統足し、`Sprite2D.material` (空 = 内蔵) の `.mat.json` から `*.surface.hlsl` を引く**。Properties DSL はそのまま。Deferred には載せない (スプライトは常に Forward 的) |
| 26 | 版 bump の回数 | `Components.cpp:55`「生バイトが伸びるので kSimSnapshotVersion は上げる」= 既存型へのフィールド append も bump。新型・外部節も bump。Scene `kDocVersion` はフィールド名で読むので不要。テクスチャは CookedCache の対象外 (`CookedCache.h` の利用箇所はモデル / 地形 / PCM / 凸包 / modal) | — | 4.4 に宣言: **SimSnapshot は「sim に効く型 / フィールド / 外部節を足したサブ」ごとに +1 (見込み 15 回)、ABI 1 回 (sub-14)、Scene kDocVersion 0 回、kCookVersion 0 回、`.rep` は snapshot に従う (使い捨て)**。これ以外の bump は設計ミスとして VERDICT で止める |
| 27 | 既存シーンのビット同一をどう守るか | 新フィールドは末尾 append + 既定 off。opt-in 型は hash 不変 (`Components.cpp:52-56`)。hash 対象フィールドの append は**既存シーンの WorldHash 値を変える**が挙動は変えない (M59h の `sleepTicks` と同じ。replay は毎回録り直すので問題無い) | — | **既存 golden 30 枚は maxDiff = 0**、動いてよいのは新設の `2d_*` だけ (受け入れ条件 1)。replay_verify の既存 14 ジョブは PASS のまま。WorldHash の値が変わるのは許容し、`--hash-diff` で差分が append したフィールドだけであることを sub-01 / sub-02 / sub-09 で 1 回ずつ確かめる |
| 28 | ルールタイル・アニメーションタイルの「現在の見た目」は状態か | 隣接 8 方向からの選択は純関数。アニメのコマは sim tick から `(tick / ticksPerFrame) % frames` で出る | — | **どちらも導出値**。描画側でチャンクを焼くときに評価し、sim 状態は持たない。衝突はルールの変種に依存しない |
| 29 | 破壊可能タイルを M80 と分けるか | M80 は `FracturePiece` に割れる重い経路 (draft 3.5) | — | **セルの damage バイト (8 bit) と palette の `hp`**。`TilemapDamageCell` で加算、`hp` 到達で palette の `destroyedTile` (0 = 空) に置き換え、`destroyEffect` (プレハブ) を tick 末尾のエフェクト生成に積む (M89i の effect イベントと同じ経路) |
| 30 | AIPerception を 2D でどう使うか | 前方は +Z 行、目の高さは +Y (`PerceptionSystem.cpp:160-164`)。LOS は 3D コライダーへの Raycast | — | **`forwardAxis` (int32、0 = +Z、1 = +X) を末尾 append**。2D では +X (Z 回転で向きが変わる)。eyeHeight は Y のままで正しい。LOS は z = 0 の 3D コライダーにそのまま撃てる。ギズモは XY に描く |
| 31 | Camera2DFollow をどの tick 位相に置くか | カメラは LocalTransform だけで動く。`Nav::PostPhysics` が物理の後・Transform (4) の前に位置を上書きする前例 (`TickRunner.cpp:512`)。M89l は LocalTransform の連鎖で目標を解く | — | **物理の後・Transform の前の PostPhysics 位相**。対象の位置は LocalTransform の連鎖で合成する (前 tick の WorldMatrix を読まない = ラグ無し)。描画補間は既存 M36b の WorldMatrix lerp がカメラにも掛かる (`RenderSystem.cpp:707-710`) ので新設不要 |

## 3. スコープ

- やる (段階 1〜5・7・8。番号は 6. のサブ):
  - 平面拘束 `Rigidbody.plane2D`、一方通行 `Collider.oneWay`、2D 不変量の selftest (01)
  - 正射影カメラ、Scene View 2D モード (02)
  - `.meta` の filter / PPU / `sprites[]`、`SpriteLibrary`、ソート層 (03)
  - `Sprite2D` + `Sprite2DPass` (Simple / flip / 色 / 整数ソート / Point・Linear) (04)、スプライトエディタ窓 (05)、Sliced / Tiled / `SortingGroup2D` / `SpriteMask2D` (06)
  - スプライト列クリップ `.spriteanim.json` + コントローラの `"sprite"` ステート + イベント + 窓 (07)
  - 2D 形状 (`kPolygon2D` / `kEdge2D`) と C++ クエリ (`OverlapBoxMasked` / `BoxCast`) (08)
  - CC の `plane2D` (一方通行・移動床・剛体押し・壁 / 接地報告) (09)
  - `.tilepalette.json` / `.tilemap.json`、`Tilemap`、チャンク描画 (10)、タイル衝突 + セル書き換えの sim 状態 + 破壊 (11)、タイルパレット窓 (12)、ルールタイル・アニメーションタイル・複数レイヤー (13)
  - ABI 1 回 (10 本) + C# ミラー + MyeScript (14)
  - `Light2D` + ライトバッファ (15)、`ShadowCaster2D` + 法線マップ (16)、スプライトのサーフェスシェーダ枝 + `Parallax2D` + 2D ポスト例 (17)
  - `Line2D` (18)、`SpriteShape2D` (19)
  - 2D ジョイント検証 + `Effector2D` (20)、`GridAgent2D` + BT MoveTo の fallback + Perception `forwardAxis` (21)、`Camera2DFollow` + `PixelPerfectCamera2D` (22)
  - `--2d-demo` + golden `2d` / `2d_lights` + replay_verify `2d` (23)、engine_spec / ADR / test_checklists / README (24)
- やらない (明示的に外したもの):
  - 2D スケルタルアニメ (ボーン・ウェイト・スプライトスワップ) — U1
  - 見下ろし (Y ソート、XZ 平面)、Transparency Sort Mode / Custom Axis — U2
  - 専用 2D ソルバ (Box2D 相当) — U3。Joint2D / Rigidbody2D / Collider2D という別コンポーネント群
  - 黒帯 (Pillarbox / Letterbox / Windowbox / Stretch Fill) — U5
  - Unity の Platform Effector の Surface Arc 可変 (2. #9)、Collider Type = Sprite (スプライトの物理形状からタイル衝突)、Composite の Intersect / Difference / Flip
  - テクスチャの wrap サンプラ、Texture Atlas の自動パッキング (スライスは手動 / グリッド / 自動分割のみ)
  - World Space Canvas (M75 の判断を維持)、Rect Tool (M75i)
  - ネット対戦 (M81 / M93) との統合テスト。2D は sim なので理屈上は通るが、M94 では replay_verify までを範囲とする
- 後回し:
  - Light2D の Volumetric・Blend Style 複数 (v1 は乗算 1 種)、Sprite2D のソフトマスク (v1 はステンシル = 二値)、SpriteMask2D の Unity 互換 (描画順に関係なく range 内の全スプライトに効く / マスク無しの VisibleInside は不可視) とステンシル bit の分割 (range の違う複数マスクの重なり)
  - タイルの Collider Type = Sprite、Isometric / Hex グリッド
  - GridAgent2D の群衆回避

## 4. 仕様

### 4.1 振る舞い

座標系: **XY 平面、+Y が上、+Z が奥 (カメラから離れる)**。カメラは +Z を見る (既存の LH 系のまま)。1 ワールド単位 = `pixelsPerUnit` 画素。

#### 4.1.1 正射影カメラ (sub-02)

- `CameraComponent` に末尾 append: `bool orthographic = false`、`float orthoSize = 5.0f` (半高、ワールド単位)。既定は透視 = 既存シーンのビット同一。
- `RenderSystem::ResolveCamera` (`RenderSystem.cpp:661-726`) は `orthographic` なら `XMMatrixOrthographicLH(2·orthoSize·aspect, 2·orthoSize, nearZ, farZ)`。`UILayout.cpp:529-544` (ワールド追従 UI の射影) も同じ分岐を持つ。射影行列は Renderer 内で閉じる (D3D 型を外へ出さない)。
- Scene View の「2D」トグル (2. #23)。トグルの状態はエディタ設定 (レイアウト保存、キー "Scene 2D"、無ければ OFF) に残し、シーンには保存しない。sub-02 で確定: ON で 3D 姿勢を退避し「3D 視点の光軸が z=0 と交わる点」を画面中心に、yaw/pitch 0、camPos.z = −50。ホイール ×1.1 (0.1..1000)、中/右ドラッグでパン、F で選択へ。XY グリッドは 10 の冪の間隔で画面範囲だけ。ImGuizmo は並進 XY / 回転 Z / 拡縮 XY。OFF で 3D 姿勢を復元。

#### 4.1.2 スプライト資産とソート層 (sub-03 / sub-05)

- `.meta` の `tex` に append: `filter` (int32: 0 auto = linear / 1 linear / 2 point、既定 0)、`pixelsPerUnit` (int32、既定 100)、`sprites` (配列。無いときは書かない = 既存 .meta とバイト同一)。各要素: `{ "id": 1, "name": "idle_0", "rect": [x, y, w, h] (px、左上原点), "pivot": [0.5, 0.5] (0..1、左下原点), "border": [l, b, r, t] (px、既定 0) }`。`id` は 1 以上、テクスチャ内で一意、最大 + 1 で割り当て、改名・削除・並べ替えで他の id は変わらない。
- `SpriteLibrary` (Engine 層、`src\Engine\Engine\Rendering\Sprite2D\` 配下): `AssetID → SpriteDef { texture AssetID, rectPx, pivot01, borderPx, ppu, filter }`。起動走査 (`RegisterAssetLibraries`) と、スプライトエディタの Apply、ReloadHub の texture 再読込で再登録。スライス無しのテクスチャ AssetID は「全面・pivot 中央・meta の ppu」として解決する (sub-03 で確定: 全面は `rect` の w/h = 0 (`IsFullTexture()`) で表し、画素寸法は描画側が `TextureLibrary` の Texture から引く。`.meta` は画像寸法を持たず、SpriteLibrary がヘッダを読む案は I/O の二重化なので却下)。未解決の AssetID は `Get` が nullptr を返し、描画側が内蔵の 16×16 マゼンタ (既存の `project_post_magenta` と同じ考え) に落とし、Inspector に警告。`.meta` の `filter` / `pixelsPerUnit` も既定値なら書かない (スプライト設定の無い既存 .meta はバイト同一)。`.meta` 単体の変更は ReloadHub の監視対象外 (`InspectorWindow.cpp:3136` と同じ) なので、スプライトエディタの Apply と Inspector の Import Settings の Apply が `SpriteLibrary::Register` を直接呼ぶ。`SortingLayers` の起動時 `Load` は `TagNames` と同じ場所 (EngineLoop の初期化。HeadlessSim / Server は描画しないので不要) で、sub-04 が接続する。
- ソート層: `project_settings.json` の `"sortingLayers": [{"id": 0, "name": "Default"}, ...]` (2. #10)。Engine 層の `SortingLayers` シングルトン (`TagNames` の流儀: `Load / Save / Get / DiffersFromDisk`)。Project Settings 窓に追加・削除・改名・並べ替え。id 0 は削除不可、上限 32。
- スプライトエディタ窓 (sub-05): テクスチャを開き、グリッド分割 (セルサイズ / セル数、オフセット、パディング)、自動分割 (アルファ > 0 の連結成分、最小サイズ)、矩形の手動追加・削除・ドラッグ、pivot プリセット 9 種 + カスタム、border の 4 辺、名前。Slice の方式は Unity の Delete Existing / Smart / Safe のうち **Delete Existing と Safe (既存と重なる新規は捨てる) の 2 つ**。独自 Undo (128 段)、Ctrl+S / Apply で `.meta` を書き `SpriteLibrary` と `TextureLibrary::ReplaceFromFile` を呼ぶ。sub-05 で確定: **id は Delete Existing でも既存の最大 + 1 から継続する** (再スライスのたびに増えるが、消した id の使い回しで古い `Sprite2D.sprite` が別の絵を指す事故を防ぐ。古い id は未解決 = マゼンタで見える)。Project 窓でテクスチャをダブルクリックするとスプライトエディタが開く (OS 既定アプリではなくなる。stb が読めない形式 (.dds 等) は OS 既定アプリへフォールバック — sub-06 の追補)。キャンバスの Point 表示 (拡大でにじまない) は Renderer/ImGui に Point サンプリングの draw callback を足して実現する (Editor 層から D3D を触らない。sub-06 の追補)。

#### 4.1.3 Sprite2D と描画パス (sub-04 / sub-06)

- `Sprite2DComponent` (TypeId は着手時の末尾、見込み 81。**kComponentNoHash**): `sprite` AssetID / `color` Float4 (1,1,1,1) / `flipX` `flipY` bool / `drawMode` int32 (0 Simple / 1 Sliced / 2 Tiled) / `size` Float2 (Sliced / Tiled のワールド寸法、既定 1,1) / `sortingLayerId` int32 (0) / `orderInLayer` int32 (0) / `maskInteraction` int32 (0 None / 1 VisibleInside / 2 VisibleOutside) / `material` AssetID (空 = 内蔵、sub-17) / `normalMap` AssetID (sub-16) / `lit` bool (true、sub-15)。後から足すフィールドは末尾 append。
- 寸法: Simple は `rect.wh / ppu` × エンティティのワールドスケール (x, y)。pivot は矩形内の原点。回転はワールド行列をそのまま使う (Z 回転が普通だが 3D 回転も壊れない)。
- **ソートキー**: (ソート層の配列順, orderInLayer, SortingGroup の有無と親キー, entity.index) の辞書式。安定ソート。viewZ を一切見ない (2. #24)。
- `Sprite2DPass` (Renderer 層 `src\Engine\Renderer\Passes\Sprite2DPass.{h,cpp}` に描画、Engine 層 `Sprite2DRenderer` に収集): `DrawParticlesAndDebug` (`RenderSystem.cpp:2047`) の `vfx->Render` の**直前**に、HDR シーン RT へ描く。動的 VB、テクスチャ (SRV) と filter が変わるとバッチを切る。ブレンド SRC_ALPHA / INV_SRC_ALPHA (premultiplied ではない)、ZTest LEqual、ZWrite off、CULL_NONE。シェーダ `assets\shaders\sprite2d.hlsl`。SceneView / GameView / Runtime / スクリーンショットの全部で同じ関数を通る。
- **色の契約 (sub-04 round 1 で確定)**: 2D テクスチャは `.meta` の `srgb` = auto (既存の非同期経路どおり UNORM でロード = 画素値をそのまま scene RT へ書く) を前提にし、2D のカメラは `CameraPostFxComponent.tonemapMode = 0` (Passthrough。`postfx_tonemap.hlsl:82` は HDR 値を加工せず返す) を使う。この組み合わせで**元画素 (r,g,b) が画面で ±1 で出る** (パレット忠実)。ACES (既定 1) では白が 231 になる (sub-04 のスクショ)。Light2D (sub-15) の乗算はこのガンマ符号化された値に掛かる (Unity の Gamma 色空間 + 2D ライトと同じ割り切り。engine_spec に書く)。`srgb` = on のテクスチャを Sprite2D に使うと Passthrough で暗く出る (Inspector の警告対象、sub-06)。`--2d-demo` のカメラは tonemapMode 0。
- Sliced: border で 9 分割、角は原寸、辺と中央は伸縮。border が全部 0 なら Simple と同じ (Unity の記載なしの箇所は「伸縮」とする)。Tiled: 中央と辺を原寸で繰り返し、端は切る (Unity の Tile Mode = Continuous 相当。Adaptive は作らない)。
- `SortingGroup2DComponent` (NoHash、見込み 82): `sortingLayerId` / `orderInLayer`。子孫の Sprite2D は自分のキーの代わりに「グループのキー + 子孫内の自キー」で並ぶ (Unity の Sorting Group、入れ子は最も近い祖先が勝つ)。
- `SpriteMask2DComponent` (NoHash、見込み 83): `sprite` / `alphaCutoff` (0.5) / `sortingLayerId` / `orderInLayer`、`rangeBack` / `rangeFront` (orderInLayer 範囲、既定は全域)。実装は**ステンシル** (D24S8 の 8 bit、マスク = ref 1 を書く、VisibleInside = Equal 1、VisibleOutside = Equal 0)。ステンシル初使用 = パス開始時にステンシルを 0 でクリアする (深度は触らない。sub-06 で確定: クリアと DSV 差し替えは「描画リストにマスクがあるとき」だけ、Pass は元の RT / DSV を控えて戻す)。**効き方 (sub-06 で確定、v1)**: マスクは描画順 (層 → order) で並び、**自分より後に描かれるスプライトのうち、`maskInteraction` を持ち、かつそのスプライトの orderInLayer が「ここまでに出たどれかのマスクの range」に入るもの**にだけ効く。該当マスクが無い `maskInteraction` 付きスプライトはテスト無しで描く (Unity は「マスク無しの VisibleInside は見えない」— 違いは engine_spec に書く。マスクしたいスプライトよりマスクの order を小さくするのが使い方)。マスクはグループに入れず自分の層 / order で並ぶ。range の違う複数マスクが同じ画素に重なると区別できない (ref 1 の 1 bit。後回し: bit 分割)。Tiled の原点は左・下 (端のタイルは右・上が切れる)。
- Inspector: `sprite` の AssetRef ピッカー (`DrawAssetRef` のフィールド名 "sprite" 分岐 → SpriteLibrary の Enumerate)、テクスチャの D&D は「全面スプライト」として受理。EditorComponentCatalog に 2D カテゴリを足す。Scene View のギズモ: 選択中の Sprite2D の矩形 (白) と pivot (黄の十字)。

#### 4.1.4 スプライト列アニメ (sub-07)

- `.spriteanim.json`: `{ "engine":"MyEngine", "spriteanim":1, "name":"Walk", "frames":[{"sprite": <uint64 AssetID>, "ticks": 6}, ...] }`。`ticks` ≥ 1。長さ = Σticks。`SpriteAnimLibrary` (GUID キー、`PhysMatLibrary` の流儀、ReloadHub 対応、Project 窓の Create → Sprite Animation、ダブルクリックで Inspector 編集 (コマの追加・削除・順序・ticks・プレビュー))。
- コントローラのステートに `"sprite": <uint64 AssetID>` を足す (`"skel"` と排他、`"clip"` とは併用可)。`ControllerStateLengthTicks` が長さを返す = `hasExitTime` / `loop` / `speed` が効く。現在コマ = `stateTimeTicks` を frames の累積 ticks で二分探索 (整数)。ブレンドツリー (`blend1d` / `blend2d`) はスプライトでは作らない (JSON にあれば警告してスプライトを優先)。
- **書き込み先**: コントローラのエンティティ自身の `Sprite2D.sprite` と、部分木の Sprite2D のうち `Sprite2D` を持つ**最初の子孫** (DFS 順) のどちらか: 自身にあれば自身、無ければ最初の子孫。毎 tick 上書き (Unity と同じく Animator が勝つ)。スクリプトが `sprite` を書いても次 tick で戻る、と engine_spec に書く。
- 遷移中 (duration > 0) は `transitionTick · 2 >= transitionDuration` なら遷移先のコマ、それ以外は元のコマ (整数比較、ブレンド無し)。推奨 duration は 0。
- イベント: `"clipEvents"` のキーに `.spriteanim.json` の `name` を使う。発火規則 (`(old, new]`、ループ折り返し、`stateEntered`) は骨と同じ関数を通す。kind 4 種そのまま。イベントの位置は `"joint"` を無視してコントローラのエンティティ位置。
- 描画補間: コマは離散。`WriteSkeletalProgram` 相当は書かないので対象外 (`stepQ` の分岐も触らない)。
- Animator Controller 窓: ステート編集に Sprite Animation のピッカー、ノード 2 行目に "sprite: Walk"、`DrawPoseLayers` はスプライトのとき現在コマ番号を表示。プレビュー窓 (M89n) はスプライトのときコマを表示 (ステップ再生)。

#### 4.1.5 平面拘束 (sub-01)

- `RigidbodyComponent` に末尾 append: `bool plane2D = false`。hash 対象 (sim 入力)。
- ON のボディは 2. #3 の (A)(B)(D) で拘束される。不変量: **tick 末尾で `position.z == z0`、`velocity.z == 0.0f`、`angularVelocity.x == angularVelocity.y == 0.0f`、`rotation.x == rotation.y == 0.0f` がビット単位で成立** (z0 = そのボディの tick 開始時の `LocalTransform.position.z`)。スリープ・CCD・複合コライダー・ジョイント・ラグドール (held) と組み合わせても成立する。
- `ColliderComponent` に末尾 append: `bool oneWay = false` (2. #9)。床 = 静的または kinematic の `oneWay` 形状 (動的な oneWay 剛体は通常の接触)。接触を**採る**のは「法線 (床→物体) が上向き (ny > 0)」かつ「物体の tick 開始時の最下点 (形状 AABB の y 最小) ≥ 床の天面 (oneWay 形状のワールド AABB の y 最大) − `kOneWaySlop` (0.05)」**または**「相対 vy < −`kOneWayLandingSpeed` (0.5 m/s) で降下中」のとき (sub-01 round 1 で確定: 着地 tick の貫通は 1 tick の降下量 (5.7 m/s で 0.067) まで深くなり slop を超えるため、速度条件が無いと速い落下が床を素通りする)。判定関数は 1 つ (`RejectOneWayContact`) で、接触生成・位置補正パス・睡眠ペアの報告・CCD 掃引の 4 箇所から呼ぶ (位置補正と CCD にも入れないと下から抜ける物を押し戻す)。`isTrigger` との併用は無視 (トリガーは接触を作らない)。複合コライダー (M60e) の子形状の `oneWay` は v1 未対応 (ボディ自身のコライダーだけ見る。7. R9)。
- ABI の `AddForce` / `AddImpulse` / `AddTorque` は plane2D のとき z 成分 / X・Y トルクを捨てる (`ApplyTorqueWorld` が freezeRotation で 0 を返す前例 :5318)。
- PhysicsSelfTest に `Plane2D` 節: 箔 10 個 + 球 5 個 + Hinge 1 組を plane2D で 3000 tick 落として積む。(a) 不変量 (b) 2 ワールド並走の `HashWorld` 一致 (c) 最終 `HashWorld` を定数に焼き、Debug / Release で同値 (d) `oneWay` 床を下から上に抜け、上から乗る (e) `--hash-diff` で既存デモの差分が append したフィールドだけ。

#### 4.1.6 CharacterController の 2D モード (sub-09)

- `CharacterControllerComponent` に末尾 append (全部 hash 対象。read-only は `kFieldReadOnly`):
  - `bool plane2D = false` — ON: `moveInput.z` を無視、z を z0 に固定、以下の機能が有効
  - `bool ignoreOneWay = false` — true の間、`oneWay` 床との接触を無視 (下入力で降りる)
  - `float pushPower = 0.0f` — > 0 なら接触した非 kinematic の plane2D 剛体へ `pushPower × 水平移動量 / dt` のインパルス (質量で割らない、Unity の OnControllerColliderHit の定番と同じ単純則)
  - `int32_t wallContact = 0` (read-only) — −1 左 / 0 無し / 1 右 / 2 両側
  - `int32_t ticksSinceGrounded = 0` (read-only) — 接地で 0、離地で +1、32767 で飽和 (ハッシュが動き続けないため)
  - `EntityID groundEntity = {}` (read-only、FieldType::EntityRef) — 接地している床のエンティティ (無ければ無効 ID)
  - `DirectX::XMFLOAT3 groundVelocity = {0,0,0}` (read-only) — 床が Rigidbody (kinematic / dynamic) ならその `velocity`、それ以外は 0
- 移動床: `plane2D` かつ接地中、CC の変位に `groundVelocity × dt` を足す (Transform を直接動かした床には乗れない。**床は kinematic Rigidbody の velocity で動かす**、と Inspector 注記と engine_spec に書く)。
- 一方通行: `SolveCharacters` の押し出しで `oneWay` 形状は「CC の tick 開始時の最下点が天面 − slop 以上、かつ `velocity.y <= 0`、かつ `!ignoreOneWay`」のときだけ押す。ジャンプで下から抜ける。
- 剛体 → CC は従来どおり (CC は剛体から押される)。CC → 剛体は `pushPower` (上)。
- PhysicsSelfTest に `CC2D` 節: 坂 (30°) を登る / 45° 超は滑る / 0.3 の段差を登る / 一方通行を下から抜けて上から乗り `ignoreOneWay` で降りる / kinematic 床 (velocity.x = 1) に乗って 60 tick で 1.0 進む / 箔を押して動く / 壁で `wallContact` が立つ / z 不変量 / Debug・Release ハッシュ一致。

#### 4.1.7 タイルマップ (sub-10 〜 sub-13)

- `.tilepalette.json`: `{ "engine":"MyEngine", "tilepalette":1, "tiles":[ { "id":1, "name":"grass", "sprite":<uint64>, "collider":1 (0 None / 1 Grid), "oneWay":false, "hp":0 (0 = 壊れない、1..255), "destroyedTile":0, "destroyEffect":<uint64 prefab or 0>, "rule": { "variants":[ { "neighbors":"xxx.x..." (8 文字、x = 同種が必要 / o = 空が必要 / . = 任意、順は NW N NE W E SW S SE), "sprite":<uint64> } ] } (任意), "anim": { "ticksPerFrame":8, "frames":[<uint64>...] } (任意) } ] }`。`id` は 1..65535、最大 + 1 で割り当て。
- `.tilemap.json`: `{ "engine":"MyEngine", "tilemap":1, "palette":<uint64>, "cellSize":[1,1], "layers":[ { "name":"Ground", "sortingLayerId":0, "orderInLayer":0, "collision":true, "chunks":[ { "cx":0, "cy":0, "cells":[ [16 個] × 16 行 ] } ] } ] }`。チャンクは 16×16、空のチャンクは書かない。セルの値 = uint32: bit0-15 `tileId` (0 = 空)、bit16 flipX、bit17 flipY、bit18-19 回転 (0/90/180/270)、bit24-31 damage。レイヤー数の上限 8。
- `TilemapComponent` (hash 対象、見込み 84): `tilemap` AssetID / `layer` int32 (0..31 衝突レイヤー) / `mask` uint32 / `friction` float (0.5) / `physMaterial` AssetID / `showCollision` bool (kFieldNoHash)。原点はエンティティのワールド位置 (セル (0,0) の左下)、ワールドスケールは無視 (Inspector 注記)。
- 描画: チャンクごとに頂点列を焼いて Sprite2D パスに流す (同じソートキー規則、レイヤーの層と順序)。dirty チャンクだけ焼き直す。ルールタイルの変種とアニメーションタイルのコマは焼くときに評価 (2. #28)。アニメーションタイルを含むチャンクは毎フレームではなく **コマが変わる tick** にだけ焼き直す。
- 衝突 (sub-11): `collision` レイヤーの `collider != 0` セルを行方向に結合して箱 (`oneWay` は別の箔列として結合) にし、PhysicsSystem の静的収集 (`:1701`) に `TilemapSystem` が供給する。箱は chunk 単位でキャッシュし dirty で作り直す。ワールド AABB は Tilemap エンティティのワールド位置から。
- sim 状態 (sub-11): `TilemapSystem::OverrideStore` = `(entity.index, entity.generation, layer, cellKey = cy·65536 + cx) → uint32` のソート済み配列。`SetCell` は値がアセットの初期値と等しければ項を消す (差分が最小に保たれる)。hash は内容ゲートで `WorldHasher` の SimSources 末尾に追加、snapshot は 'TM2D' 節 (Compute 節の次、World の前)。復元後は全チャンク dirty + 衝突キャッシュ無効化。Tilemap エンティティが消えたら項を捨てる (tick 末尾の構造変更後)。
- 破壊 (sub-11): C++ API `DamageCell(e, layer, cx, cy, amount)`: `hp == 0` なら何もしない。damage += amount、`>= hp` で `destroyedTile` に置き換え (damage 0)、`destroyEffect` を tick 末尾のエフェクト生成キューへ (セル中心)。
- タイルパレット窓 (sub-12): 左にパレットのタイル一覧 (スプライトのサムネイル)、上にツール (選択 / ブラシ / 消去 / 塗り潰し / 矩形 / ピック)、対象 Tilemap とレイヤーのコンボ、flip / 回転ボタン。Scene View (2D モード) 上でクリック・ドラッグで塗る。独自 Undo (2. #22)、Ctrl+S で `.tilemap.json` を書き、ReloadHub が読み直す。Play 中は無効 (窓に理由を表示)。
- ルールタイル・アニメーションタイル・レイヤー (sub-13): 上の `rule` / `anim` と `layers[]` の UI (Inspector でのレイヤー追加・削除・順序、パレットでの rule / anim 編集)。

#### 4.1.8 2D 形状とクエリ (sub-08)

- `collidershape::kPolygon2D = 6`、`kEdge2D = 7` (append)。点列は `.shape2d.json` (`{ "shape2d":1, "points":[[x,y],...], "closed":true, "edgeRadius":0.02, "depth":1.0 }`) を `meshAsset` で参照。解決は 2. #19。`ShapeLibrary2D` (GUID キー、ReloadHub、Create メニュー)。
- Scene View のギズモで点をドラッグ編集 (`HandlePatrolRoute` の流儀)、点の追加 (辺をクリック) と削除 (Delete)。
- C++ クエリ: `OverlapBoxWorld` にマスク付きの口、`BoxCastWorld` (AABB の掃引、`SphereCastWorld` と同じ 32 回の保守的前進)。既存 `RaycastWorld` / `OverlapSphereWorld` は z = 任意の 3D 関数としてそのまま 2D に使う。
- PhysicsSelfTest に `Shapes2D` 節: 凹 L 字の静的多角形の窪みで球が止まる / 辺チェーンの両面で止まる / 動的多角形 (凸包) が床に乗る / `BoxCast` が最初の床を返す / `OverlapBoxMasked` がマスクで除外する。

#### 4.1.9 2D ライト・影・法線 (sub-15 / sub-16)

- `Light2DComponent` (NoHash、見込み 85): `type` int32 (0 Global / 1 Spot / 2 Freeform / 3 Sprite) / `color` Float4 / `intensity` float (1) / `targetLayerMask` uint32 (全 bit) / `innerRadius` `outerRadius` float (0 / 3) / `innerAngleDeg` `outerAngleDeg` float (360 / 360 = 点光源) / `falloff` float (1) / `pointCount` int32 + `points[16]` Float2 (Freeform、ローカル) / `falloffWidth` float (Freeform) / `sprite` AssetID (Sprite) / `shadows` bool (false) / `shadowStrength` float (1) / `normalMapDistance` float (1)。向きはエンティティの Z 回転。
- ライトバッファ: RGBA16F、シーン RT と同サイズ (`RenderTexture::Resize` パターン)。層を描画順に走り、`targetLayerMask` にその層の bit を含むライトの集合が前の層と変わったら、バッファを 0 で clear → Global を加算 → 形状ライトを加算 (SRC_ONE / DEST_ONE)。Sprite2D の PS (`lit` のもの) は `color × tex × lightBuffer[screenUV]`。**presence gate** (2. #14)。SceneView の非 Play でも効く (ライトの配置が見える)。
- `ShadowCaster2DComponent` (NoHash、見込み 86): `source` int32 (0 = 同エンティティの Collider の形状 (Box / Sphere=円 16 角 / Polygon2D) / 1 = `points[16]` + `pointCount`) / `selfShadow` bool (false) / `castsShadows` bool (true)。`shadows` のライトごとに R8 の影 RT を clear(0) → 全キャスターの辺をライトから遠方 (RT 対角の 2 倍) へ押し出した四辺形を 1 で描く → `selfShadow == false` のキャスターは自分の多角形を 0 で描き戻す → ライト形状の PS が `1 − shadow × shadowStrength` を掛ける。
- 法線マップ: `Sprite2D.normalMap` を持つスプライトを、ライトバッファ作成前に法線 RT (RGBA8、clear = (0.5, 0.5, 1, 1)) へ描く (同じ頂点列、flip で X/Y を反転)。形状ライトの PS は `N·L` (L = (lightPos − pixelWorld, normalMapDistance) を正規化) を掛ける。Global は掛けない。
- 参照: https://docs.unity3d.com/Manual/urp/2DLightProperties.html (Spot の Inner/Outer Radius・Angle・Falloff、Light Order、Overlap Operation = 加算既定)、ShadowCaster2D (2. #15)。Blend Style は乗算 1 種、Light Order は orderInLayer 的な並びにしない (加算なので順序に依存しない)。

#### 4.1.10 ABI (sub-14、1 回)

版 = 着手時の末尾 (2. #18)。スロット (10 本、順序固定):

1. `OverlapBoxMasked(center, halfExtents, mask, outEntities, maxCount) → count`
2. `BoxCast(origin, halfExtents, dir, maxDist, mask, outHit) → hit?` (`MyeRaycastHit` 流用)
3. `SpriteByName(texturePath, spriteName) → uint64` (0 = 無し)
4. `TilemapGetCell(e, layer, cx, cy) → uint32`
5. `TilemapSetCell(e, layer, cx, cy, value) → 1/0`
6. `TilemapFillRect(e, layer, x0, y0, x1, y1, value) → count`
7. `TilemapDamageCell(e, layer, cx, cy, amount) → 0 無傷 / 1 損傷 / 2 破壊 / -1 対象外`
8. `TilemapWorldToCell(e, worldPos, outCx, outCy) → 1/0`
9. `TilemapCellToWorld(e, cx, cy) → Vec3 (セル中心)`
10. `TilemapGetBounds(e, layer, outMinX, outMinY, outMaxX, outMaxY) → 1/0`

その他 (Sprite2D.sprite、CC の plane2D 系、GridAgent2D.destination、Camera2DFollow.target、Light2D) は `Get/SetComponentField`。スプライト列の再生は既存 `AnimatorPlay`。C# `Interop.cs` は位置ミラー (`abi-bump-verification` の手順: 順序を check_rules 11-a で機械照合)、`MyeScript.cs` に `Tilemap.GetCell(...)` 等の糖衣、`ScriptAPI.h` に C++ 糖衣。`PartSelfTest.cpp:178` の版固定と `check_rules.ps1:743` の表を更新。

#### 4.1.11 形状・視差・シェーダ (sub-17 / sub-18 / sub-19)

- `Parallax2DComponent` (NoHash、見込み 87): `factor` Float2 (1,1。0 = カメラに固定、1 = 通常) / `repeatX` bool / `repeatY` bool / `repeatSize` Float2 (0 = スプライト寸法)。描画位置 = `pos + (cam.xy − pos.xy) × (1 − factor)`、repeat は表示範囲を覆う枚数だけ複製 (上限 64 枚)。
- スプライトのサーフェスシェーダ (2. #25): `Sprite2D.material`。`MyEngineSurfaceEntries.hlsli` に `MyeVSSprite2D` / `MyePSSprite2D`、`SurfaceProgram` にスプライト用エントリ。作者は `PSMain` で `MyeSpriteInput { float4 color; float2 uv; float3 world; }` を受け取り `float4` を返す。例 `assets\shaders\SpriteFx.surface.hlsl` (Properties: flash 色・量、outline 色・幅、dissolve 量)。2D ポスト例 `assets\shaders\CrtScanline.post.hlsl` (fxstack 経由、エンジン変更なし)。
- `Line2DComponent` (NoHash、見込み 88): `pointCount` + `points[32]` Float2 (ローカル) / `width` float (0.1) / `color` Float4 / `texture` AssetID / `loop` bool / `fill` bool (閉じた多角形を耳刈りで塗る) / `sortingLayerId` / `orderInLayer`。Sprite2D パスに頂点列を流す。
- `SpriteShape2DComponent` (hash 対象、見込み 89): `profile` AssetID (`.spriteshape.json`: `{ "spriteshape":1, "fill":<uint64>, "edges":[ { "angleFrom":-45, "angleTo":45, "sprite":<uint64> } ], "height":1.0 }`) / `pointCount` + `points[24]` Float2 / `closed` bool / `smooth` bool (Catmull-Rom、1 区間 8 分割) / `collider` bool / `oneWay` bool / `sortingLayerId` / `orderInLayer`。描画: 辺に沿って角度範囲に合う edge スプライトを原寸で敷く、閉じていれば fill を耳刈りで塗る (UV = ワールド座標 / ppu)。衝突: `collider` のとき辺チェーン (sub-08 の薄い箔列) を静的収集へ供給 (タイルと同じ供給口)。Scene View で点のドラッグ・追加・削除。

#### 4.1.12 カメラ追従とピクセルパーフェクト (sub-22)

- `Camera2DFollowComponent` (hash 対象、見込み 92): `target` EntityRef / `offset` Float2 / `deadZone` Float2 (半幅半高、0 = 無し) / `damping` Float2 (0..1、1 tick で詰める割合、既定 0.15) / `lookAheadX` float (対象の velocity.x の符号 × この距離、Rigidbody か CC の速度を読む) / `useBounds` bool / `boundsMin` `boundsMax` Float2 / `boundsFromTilemap` EntityRef (指定時はその Tilemap の衝突レイヤーの AABB を境界にする)。PostPhysics 位相 (2. #31) で `camPos += (desired − camPos) × damping` を float で計算し LocalTransform.position.xy に書く (z は触らない)。境界は正射影の半幅半高 (orthoSize × aspect; aspect は `PixelPerfect` の有効値か `GameView` の寸法ではなく **16:9 固定の仮定ではない → sim 入力に画面寸法を持ち込まない**ため、境界クランプは `orthoSize` と `boundsAspect` (float、既定 16/9、hash 対象) で行う)。
- `PixelPerfectCamera2DComponent` (NoHash、見込み 93): `assetsPPU` int32 (100) / `refResolutionX` `refResolutionY` int32 (320 / 180) / `upscaleRT` bool (true) / `pixelSnapping` bool (false)。式は 2. #13。`RenderSystem::Render` は zoom > 1 かつ upscaleRT なら低解像度 RT (`RenderTexture` メンバ、サイズが変わったら Resize) をシーンのターゲットにし、終わりに `sprite2d_blit` (Point) で `target.rtv` へ整数倍拡大。拡大先は中央寄せで余りは出ない (RT の寸法が W/zoom/2*2 なので最大 2 px 未満の端数は黒 1 行になりうる → **奇数の端数は RT 側を 1 px 大きく取り、blit で端を切る**。黒帯は作らない)。`orthoSize` の有効値を上書き (`CameraComponent.orthoSize` の値は無視、Inspector に有効値を表示)。`pixelSnapping` (upscaleRT = false のとき) は Sprite2D の頂点位置を `1 / assetsPPU` 格子へ丸める (描画側のみ)。TAA ジッタ / FXAA を無効化 (2. #13)。純関数 `pixelperfect2d::Compute(W, H, settings) → { zoom, rtW, rtH, orthoSize }` を Renderer 層に置き、`UILayout` の射影もこれを使う。

#### 4.1.13 グリッド A* と知覚 (sub-21)

- `GridAgent2DComponent` (hash 対象、見込み 91): `tilemap` EntityRef (経路を引く Tilemap) / `destination` Float3 / `hasDestination` bool / `speed` float (3) / `maxJumpHeight` int32 (セル、2) / `maxJumpLength` int32 (セル、3) / `jumpSpeed` float (CC.jumpSpeed に書く値) / `replanInterval` int32 (tick、30) / `status` int32 (read-only、`navagentstatus` の値を流用: Idle / Moving / Arrived / NoPath / Stuck / Inactive) / `nextCellX` `nextCellY` int32 (read-only)。
- `GridNavSystem` (Nav の 3.4b と同じ位相、NavSystem の直後)。歩行可能セル = 衝突レイヤーで空かつ直下が固体 (oneWay 含む)。辺: 左右 (コスト 10)、落下 (段差 ≤ 任意、コスト 10 + 高さ)、ジャンプ (高さ ≤ maxJumpHeight、距離 ≤ maxJumpLength、経路上のセルが空、コスト 20 + 距離)。A* は二分ヒープ、キーは (f, g, cellKey) の全順序 (決定的タイブレーク)。1 tick の展開上限 4096 ノード、超えたら次 tick に続ける (status は Moving)。出力は CC の `moveInput.x = ±speed` と、ジャンプ辺に入る tick に `jumpSpeed`。CC が無ければ Inactive。
- BT の MoveTo: `NavMeshAgentComponent` が無く `GridAgent2DComponent` があればそちらへ `destination / hasDestination` を書き `status` を読む。到着判定の `HorizontalDistance` は GridAgent のとき XY で測る。
- `AIPerceptionComponent` に `int32_t forwardAxis = 0` を末尾 append (2. #30)。

#### 4.1.14 デモ・検証 (sub-23)

- `--2d-demo` (`kShowcases` に追加、`BuildTwoDShowcaseScene`): タイルマップ地形 (3 レイヤー: 背景 / 地面 / 前景、一方通行床、壊れるブロック) + 主人公 (CC plane2D、`.spriteanim.json` ×2 (Idle / Walk) のコントローラ、`--synth-input` で左右ジャンプ) + 箔 (plane2D 剛体 ×3) + kinematic 移動床 + 敵 (GridAgent2D、BT の MoveTo で追う) + Camera2DFollow + PixelPerfectCamera2D (320×180、PPU 16) + `CameraPostFx.tonemapMode = 0` (色の契約) + 視差背景 ×2 + Light2D (Global + Spot ×2、`--2d-night` で ON) + ShadowCaster2D + Line2D のロープ + SpriteShape2D の丘。アセットは `assets\2d\` に手描きの小さな PNG (16 px タイル、32 px キャラ。coder が生成スクリプト `tools\gen_2d_assets.py` で作る。外部素材は使わない)。
- golden: `2d` (`--2d-demo`、frame 3)、`2d_lights` (`--2d-demo --2d-night`、frame 120)。tol は既定 3。
- replay_verify: ジョブ `2d` (`:chain cache\golden_2d.rep "--2d-demo" "--2d-demo"`、記録側 `--synth-input`)。`tools\mcp\src\verify.rs` のジョブ表 (規則 15-e) も更新。
- 不変量: `--2d-demo` の 3000 tick で全 plane2D 剛体の z / vz / wx / wy / qx / qy が初期値 / 0.0f (PhysicsSelfTest の `Plane2D` 節とは別に、デモシーンで `--selftest` から 1 回確認する項目を足す)。

### 4.2 データ・保存形式・互換性

#### 4.2.1 新アセット

| 拡張子 | 内容 | ライブラリ | 作成 | リロード |
|---|---|---|---|---|
| `.meta` (`tex.sprites[]` 等) | スライス・pivot・border・PPU・filter | `SpriteLibrary` | スプライトエディタ窓 | Apply 時に明示 (監視対象外、`InspectorWindow.cpp:3136` と同じ) |
| `.spriteanim.json` | コマ列 | `SpriteAnimLibrary` | Project 窓 Create | ReloadHub (rank は controller の前) |
| `.tilepalette.json` | タイル定義 | `TilePaletteLibrary` | Project 窓 Create | ReloadHub |
| `.tilemap.json` | グリッド・レイヤー・セル | `TilemapLibrary` | Project 窓 Create / パレット窓の保存 | ReloadHub (全チャンク dirty、override は保持) |
| `.shape2d.json` | 点列 | `ShapeLibrary2D` | Project 窓 Create | ReloadHub (静的収集のキャッシュ無効化) |
| `.spriteshape.json` | 縁・塗りのプロファイル | `SpriteShapeLibrary` | Project 窓 Create | ReloadHub |

全部 `AssetType` enum 末尾 append、`ClassifyPath` は最長サフィックス優先、`TypeName` / `ParseTypeName`、`RegisterAssetLibraries`、`kTypeFilters` / `TileLabel` / `FileTypeIcon`、`SplitAssetName::kCompound`、Inspector の分岐。`.meta` は `WriteMeta` が `sprites` 無しのテクスチャで従来とバイト同一を保つ (配列が空なら書かない)。

#### 4.2.2 既存型への append (全部末尾、既定は従来挙動)

| 型 | フィールド | hash | サブ |
|---|---|---|---|
| `RigidbodyComponent` | `plane2D` | 対象 | 01 |
| `ColliderComponent` | `oneWay` | 対象 | 01 |
| `CameraComponent` | `orthographic`, `orthoSize` | 対象 (既存型の流儀に従う) | 02 |
| `CharacterControllerComponent` | `plane2D`, `ignoreOneWay`, `pushPower`, `wallContact`, `ticksSinceGrounded`, `groundEntity`, `groundVelocity` | 対象 | 09 |
| `AIPerceptionComponent` | `forwardAxis` | 対象 | 21 |
| `collidershape::` | `kPolygon2D = 6`, `kEdge2D = 7` | — | 08 |

旧シーン (フィールド無し) は既定値で読まれ、挙動はビット同一。

#### 4.2.3 sim 状態の境界 (decidability table)

| 状態 | 置き場所 | hash | SimSnapshot | 備考 |
|---|---|---|---|---|
| plane2D 剛体の位置・速度 | Rigidbody / LocalTransform (既存) | 対象 | World 節 | 新しい状態は無い |
| CC の 2D 追加フィールド | CharacterController | 対象 | World 節 | |
| スプライト列の現在コマ | 導出 (`stateTimeTicks`) | — | — | Sprite2D.sprite は NoHash の出力 |
| タイルセルの書き換え・damage | `TilemapSystem::OverrideStore` | 内容ゲート | 'TM2D' 節 | アセットは初期値 |
| ルール変種・アニメコマ | 導出 | — | — | |
| タイル衝突の箱 | 導出 (キャッシュ) | — | — | 復元で dirty |
| Camera2DFollow の位置 | LocalTransform (既存) | 対象 | World 節 | |
| GridAgent2D の経路 | 導出 (再計画可能)。`status` 等はコンポーネント | コンポーネントのみ | World 節 | 復元後は再計画 (経路は決定的に同じ) |
| Sprite2D / SortingGroup2D / SpriteMask2D / Light2D / ShadowCaster2D / Parallax2D / Line2D / PixelPerfectCamera2D | コンポーネント | **NoHash** | World 節 (生バイト) | 描画専用 |
| SpriteShape2D / Effector2D / Tilemap / Camera2DFollow / GridAgent2D | コンポーネント | 対象 | World 節 | sim 入力 |

### 4.3 UI / ビジュアル

- Scene View 2D モード: ツールバーに「2D」トグル (Ortho トグルの隣)。グリッドは XY (1 単位、原点を太線)。ギズモ: Sprite2D 矩形 (白)、pivot (黄)、2D コライダー (緑: 箱 = 矩形、球 = 円 32 分割、Polygon2D / Edge2D = 点列、Tilemap の衝突箱 = `showCollision` で緑)、Light2D (type ごとの輪郭、黄)、ShadowCaster2D (紫)、Camera2DFollow の deadZone と bounds (青)、GridAgent2D の経路 (橙)。
- スプライトエディタ窓・タイルパレット窓・Project Settings の Sorting Layers: 文字列は全部 `LocalizationTable.inl` + `Tr()`、両言語。
- スクショで検証できる形: golden `2d` (昼: タイル・主人公・箔・視差・ピクセルパーフェクトの整数倍)、`2d_lights` (夜: スポット 2 灯の影と法線)。reviewer は `tests\actual\2d*.png` を見る。

### 4.4 非機能

- **決定性**: 4.2.3 の表が境界。sim 側は scalar float (`/fp:precise`)、SIMD 禁止、`rand()` 禁止、ハッシュコンテナの走査順を使わない (override 表はソート済み配列)。描画側 (Sprite2D 収集・チャンク焼き・ライト) は sim に 1 バイトも書かない。
- **既存シーンのビット同一**: 受け入れ条件 1〜3。
- **版 bump の宣言** (2. #26): SimSnapshot +1 × 見込み 15 (sub-01, 02, 04, 06, 09, 10, 11, 13 (レイヤー欄が増えれば), 15, 16, 17, 18, 19, 20, 21, 22 のうち sim 形式が変わるもの)、ABI +1 (sub-14)、Scene kDocVersion 0、kCookVersion 0、`.rep` は snapshot 追随 (`kReplayFileVersion` は hash の意味を変えない限り据え置き)。各サブの SELF_EVAL に「版を触ったか」を必ず書く。
- **性能** (計測して受け入れ条件に): `--2d-demo` で Sprite2D 2000 枚 + タイル 100×50×3 層の収集 + 描画が WARP 960×540 で 1 フレーム 16 ms 以内 (`--frames 300` の平均、`PerfDrawBenchmark` の流儀でログ)。タイル衝突の箱数と物理 1 tick の時間を sub-11 でログ。
- **ローカライズ**: 全エディタ文字列。`check_rules.ps1` 規則 10。
- **コメント**: 日本語で理由を書く。新規 .h/.cpp はヘッダー付き (AGENTS.md 5)。
- **レイヤー**: Sprite2D の D3D 型は `src\Engine\Renderer\` の中に閉じる。Engine 層は収集した頂点列・ライト列を渡すだけ。SpriteLibrary / TilemapSystem は Engine 層。エディタ窓は Editor 層。
- **検証の運用 (ユーザー判断 9、2026-10-20: 重い検証はサブ 5 本に 1 回)**:
  - **毎サブ必須**: Debug / Release を `/p:MyeWarnAsError=true` で 0 警告、`pwsh tools\check_rules.ps1` が 0、そのサブが新設・変更した selftest 節だけの実行 (`MYE_SELFTEST_ONLY=m94` — sub-01 で入口を足し、以降のサブは自分の suite をその分岐に追加する。節単位で切れない suite (PhysicsSelfTest 等) は suite 単位でよい)、サブ固有の軽い検証 (hash-diff、スクショ 1 枚など)。ソース追加時は `pwsh tools\gen_project_files.ps1`。exe は PowerShell ツールから起動する (Bash の `cmd /c` は実行されない)。
  - **チェックポイント (sub-05 / sub-10 / sub-15 / sub-20 / sub-23 / sub-24 の完了時) だけ**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat` 全ジョブ、`tools\shot_verify.bat` (Release) + 既存 golden 30 枚の `--tol 0` 照合。割れたら、その 5 本の範囲で原因サブを特定して差し戻す。
  - VERDICT は「全体 selftest / replay_verify / shot_verify 未実行」を REWORK の理由にしない (チェックポイント以外)。

## 5. 受け入れ条件

共通 (全サブ、毎回):
1. Debug / Release が `/p:MyeWarnAsError=true` で 0 警告、`check_rules.ps1` が 0、そのサブの selftest 節 (`MYE_SELFTEST_ONLY=m94`) が PASS。
2. 版 bump が 4.4 の宣言どおり (触った版と理由を SELF_EVAL に書く。宣言外の bump は REWORK)。
3. 新規・変更のエディタ文字列が `LocalizationTable.inl` 経由で両言語。

チェックポイント (sub-05 / sub-10 / sub-15 / sub-20 / sub-23 / sub-24 の完了時にだけ求める。ユーザー判断 9):
4. 既存 golden 30 枚 (`tests\golden\*.png`) が **maxDiff = 0** (`tools\shot_verify.bat` の結果と、`Editor.exe --img-diff ... --tol 0` を既存 30 枚に手で当てたログ)。動いてよいのは `2d_*` だけ。例外 (チェックポイント 1 で確定): `demo_terrain_deferred` は基点 `25fb355` の Runtime でも golden と maxDiff=5 / 2 画素ずれる (WARP の版差、`shot_verify.bat` が tol=12 を取っている 1 枚)。この 1 枚は「基点の絵と maxDiff=0」を条件にする (基点の絵は coder がチェックポイントごとに `git archive` で作るか、チェックポイント 1 の `tests\actual\demo_terrain_deferred.png` を `cache\m94\baseline_terrain.png` に残して比べる)。
5. `tools\replay_verify.bat` の既存 14 ジョブ + `rules` (sub-23 以降は `2d` も) が PASS。`Editor.exe --selftest` 全体 (Debug / Release) に新しい FAIL が無い。

段階 1 基盤:
6. (01) PhysicsSelfTest `Plane2D` 節: 3000 tick 後の plane2D 剛体 16 体 + Hinge 1 組の z / vz / wx / wy / qx / qy がビット単位で初期値 / 0.0f。2 ワールド並走の HashWorld 一致。焼いた最終 HashWorld が Debug / Release で同値。
7. (01) `oneWay` 床: 球が下から上へ抜け、上から落ちて乗る。横から当たっても止まらない。selftest。
8. (01) 既存デモの `--hash-diff` で差分が `plane2D` / `oneWay` の append だけ (ログを SELF_EVAL に)。
9. (02) `orthographic` の正射影で `--screenshot` を撮り、1 単位の箱が `orthoSize` から計算した画素数 (±1 px) で写る。透視カメラのシーンは golden 不変。
10. (02) Scene View の 2D トグル: XY グリッド、ホイールズーム、パン、ImGuizmo が XY 並進 + Z 回転だけ。`--screenshot` 相当のエディタスクショ (`plans\m94-2d\screenshots\sceneview_2d.png`)。
11. (03) `.meta` に `sprites[]` を書いて読み戻すと同一 JSON。`sprites` 無しのテクスチャの .meta は従来とバイト同一。`SpriteLibrary` が `#sprite<id>` キーで解決し、改名後も同じ AssetID。selftest。
12. (03) Project Settings の Sorting Layers: 追加 / 削除 / 改名 / 並べ替えで id が安定 (selftest: 3 層作って中を消して足し直すと空いた id を再利用)。上限 32。
13. (04) `Sprite2D` 4 枚 (層 2 種 × order 2 種) を `--screenshot` で撮り、描画順が (層順 → order → index) で、flip と color が効く。Point filter のテクスチャがにじまない (隣接画素の値をチェック)。golden 不変。
14. (05) スプライトエディタ窓: グリッド分割 (4×4) と自動分割 (連結成分 3 個の PNG) が期待の矩形を出す (純関数の selftest)。Undo 3 段戻る。Apply で .meta が書かれ Sprite2D の表示が変わる。エディタスクショ 1 枚。
15. (06) Sliced (border 4 px) を 3×1 単位に伸ばしたとき角が原寸、Tiled が原寸で繰り返す (スクショの画素幅を数える)。SortingGroup2D で子 2 枚が別グループの間に割り込まない。SpriteMask2D の VisibleInside / Outside がステンシルで効く (スクショ)。

段階 2 動き:
16. (07) `.spriteanim.json` 3 コマ (6/6/12 tick) のステートで、tick 0〜5 がコマ 0、6〜11 が 1、12〜23 が 2、24 でループ。`hasExitTime` の遷移が長さ 24 で発火。`clipEvents` が指定 tick で 1 回だけ発火 (selftest)。Animator 窓にピッカーとコマ番号 (スクショ)。
17. (07) スプライトアニメだけのシーンの WorldHash が Sprite2D.sprite を手で書き換えても変わらない (NoHash の確認)。
18. (08) `Shapes2D` 節 (4.1.8) が Debug / Release で PASS。`.shape2d.json` の点編集ギズモ (エディタスクショ)。
19. (09) `CC2D` 節 (4.1.6) が PASS。既存 CC のシーン (`--nav-demo` 等) は replay_verify PASS、hash-diff が append のみ。

段階 3 タイル:
20. (10) `.tilemap.json` (2 レイヤー、3 チャンク) を読み、`--screenshot` でタイルが層順で写る。flip / 回転ビットが効く。空チャンクは書かれない。読み書きの往復で同一 JSON。
21. (11) 100×50 の床で生成される箱の数と物理 1 tick の時間をログ (`--physics-demo` の 2 倍以内)。plane2D 箔がタイル床に乗り、一方通行タイルを下から抜ける。`SetCell` で床を消すと箔が落ちる (selftest)。
22. (11) override 表の hash / snapshot: セルを 3 個書き換えて capture → 新ワールドへ restore → 100 tick 連続実行と一致 (selftest)。Tilemap の無いシーンの hash と snapshot バイト列が着手前と同一 (presence gate)。
23. (11) `DamageCell` で hp 3 のタイルが 3 回で `destroyedTile` になり、`destroyEffect` のプレハブが 1 個生成される (selftest)。
24. (12) タイルパレット窓: ブラシ / 消去 / 塗り潰し / 矩形 / ピックの各ツールの純関数 selftest (塗り潰しは 4 連結)、Undo、保存で `.tilemap.json` が変わり ReloadHub が読み直す。Play 中は無効の表示。エディタスクショ。
25. (13) ルールタイル (地面の縁 4 種) が隣接で変種を選ぶ、アニメーションタイル (ticksPerFrame 8) が tick 8 でコマが変わる (スクショ 2 枚の差分)。レイヤー 3 枚の順序 UI。
26. (14) ABI: 版が着手時の次番号、スロット数が `check_rules.ps1` の表と一致、`Interop.cs` / `MyeScript.cs` / `ScriptAPI.h` がミラー、`PartSelfTest` の版固定を更新。C++ スクリプトと C# スクリプトの両方から `TilemapSetCell` → 次 tick で `TilemapGetCell` が返り、箔が落ちる (selftest または `--2d-demo` 前の temp プローブ、ログを SELF_EVAL に)。

段階 4 見た目:
27. (15) Light2D Global 1 + Spot 1 のシーンで、スポット内のスプライトが外より明るく、`targetLayerMask` で外した層が暗くならない (スクショの画素値)。Light2D の無いシーンは golden 不変 (条件 1 と、`--2d-demo` 昼のスクショが sub-15 前後で同一)。
28. (15) Forward と `--deferred` で 2D ライトのシーンのスクショが maxDiff ≤ 3。
29. (16) ShadowCaster2D の箔がスポットの影を落とし、`selfShadow=false` で自分は暗くならない (スクショ)。法線マップ付きスプライトでライト側が明るい (左右のライト位置を入れ替えて明暗が反転する 2 枚)。
30. (17) `SpriteFx.surface.hlsl` の flash で白く、dissolve で穴が開く (スクショ 2 枚)。Parallax2D factor 0.5 でカメラを 2 動かすと背景が 1 動く (スクショの位置差)。`CrtScanline.post.hlsl` を fxstack に載せて走る。
31. (17) サーフェスシェーダのコンパイルエラー時は内蔵シェーダへフォールバックし `surface_error` 相当の表示 (既存の流儀)。

段階 5 形状:
32. (18) Line2D 5 点・幅 0.2・`fill` の多角形がスクショに写る。点列の端が切れない。
33. (19) SpriteShape2D の丘 (8 点、smooth) に縁スプライトが角度範囲で選ばれ、fill が塗られる (スクショ)。`collider` で球が丘に沿って転がり落ちる (selftest)。点のドラッグ (エディタスクショ)。

段階 7 周辺:
34. (20) `Plane2D` 節に Hinge(Z) / Slider(X) / SpringJoint の 3 組を追加し不変量と Debug / Release 一致。Effector2D の 4 種 (Area: 箔が横へ流れる / Point: 中心へ寄る / Surface: 乗った箔が流れる / Buoyancy: 密度の低い箔が浮く) の selftest。
35. (21) 10×10 のタイル面で GridAgent2D が段差 2 をジャンプで越えて目的地に着く (status Arrived)、到達不能で NoPath、2 ワールド並走で HashWorld 一致。BT の MoveTo が GridAgent2D で動く (`--bt-demo` は不変)。Perception `forwardAxis=1` で +X 側の対象だけ見える (selftest)。
36. (22) Camera2DFollow: deadZone 内では動かず、外れると damping で追う、bounds で止まる (selftest、位置の数値)。
37. (22) PixelPerfectCamera2D: 960×540 で ref 320×180 → zoom 3、RT 320×180、1 タイル (16 px) が画面で 48 px ちょうど (スクショの画素数)。1280×720 で zoom 4。UI Canvas の文字はフル解像度のまま (スクショで UI のエッジが 1 px)。TAA / FXAA が無効化される (ログ)。

段階 8 締め:
38. (23) `--2d-demo` が golden `2d` / `2d_lights` に登録され、replay_verify の `2d` ジョブ (Debug record → Debug verify → Release verify → Server verify) が PASS。`verify.rs` のジョブ表が一致 (規則 15-e)。
39. (23) デモの 3000 tick 不変量 (4.1.14) と、4.4 の性能 (16 ms) のログ。
40. (24) engine_spec.md に §6.15 (2D 描画: Sprite2D / ソート層 / ライト / ピクセルパーフェクト)、§10.13 (2D 資産: スプライト / スプライトアニメ / タイルマップ / 形状)、§11.3 の 2D ジョブ、§12.2 の M94 行。ADR-033「2D レーンの方式」(平面拘束・整数ソートキー・アセット保存 + 差分 sim 状態・ライトバッファ・却下案)。`docs\test_checklists.md` の M94 節。README の機能表。`plans\m94-2d\harness.md` の申し送り。

## 6. サブ分割

依存は一方向。「なし」同士は並列可だが司会は 1 本ずつ回す。コミット件名は `M94x:` 接頭辞 + 日本語。TypeId / 版は**着手時の末尾**から取り、コミットメッセージに実際の番号を書く。

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| 01 | 平面拘束 `Rigidbody.plane2D` と一方通行 `Collider.oneWay`、2D 不変量の selftest | なし | 1-3, 6-8 | `M94a: 剛体の平面拘束 (plane2D) と一方通行コライダー、2D 不変量の selftest (snapshot vNN)` |
| 02 | 正射影カメラと Scene View の 2D モード | なし | 1-3, 9, 10 | `M94b: 正射影カメラ (orthographic / orthoSize) と Scene View の 2D モード` |
| 03 | スプライト資産 (`.meta` の sprites / filter / PPU、SpriteLibrary) とソート層 | なし | 1-3, 11, 12 | `M94c: スプライトのスライスを .meta のサブアセットにし、ソート層を project_settings に足す` |
| 04 | `Sprite2D` と `Sprite2DPass` (Simple / flip / 整数ソート / Point・Linear) | 02, 03 | 1-3, 13 | `M94d: Sprite2D コンポーネントと整数ソートの Sprite2D パス (TypeId NN)` |
| 05 | スプライトエディタ窓 (**チェックポイント**: sub-01〜05 の分の 4, 5) | 03 | 1-5, 14 | `M94e: スプライトエディタ窓 (グリッド / 自動分割、pivot、border、Undo)` |
| 06 | Sliced / Tiled、SortingGroup2D、SpriteMask2D (ステンシル) | 04 | 1-3, 15 | `M94f: Sprite2D の Sliced / Tiled、SortingGroup2D、SpriteMask2D` |
| 07 | スプライト列クリップ (`.spriteanim.json`、コントローラの `"sprite"`、イベント、窓) | 04 | 1-3, 16, 17 | `M94g: スプライト列アニメを Animator Controller の sprite ステートとして追加する` |
| 08 | 2D 形状 (`kPolygon2D` / `kEdge2D`、`.shape2d.json`) と C++ クエリ | 01 | 1-3, 18 | `M94h: 2D の多角形 / 辺コライダーと OverlapBoxMasked / BoxCast` |
| 09 | CharacterController の `plane2D` (一方通行・移動床・剛体押し・報告) | 01 | 1-3, 19 | `M94i: CharacterController の 2D モード (一方通行、移動床、剛体を押す、壁と接地の報告)` |
| 10 | タイルパレット / タイルマップ資産、`Tilemap`、チャンク描画 (**チェックポイント**: sub-06〜10) | 04 | 1-5, 20 | `M94j: タイルマップ資産 (.tilepalette / .tilemap) と Tilemap のチャンク描画` |
| 11 | タイル衝突 + セル書き換えの sim 状態 + 破壊 | 08, 10 | 1-3, 21, 22, 23 | `M94k: タイルの衝突 (行結合箱) とセル書き換えの sim 状態 (TM2D 節)、破壊可能タイル` |
| 12 | タイルパレット窓 | 10, 02 | 1-3, 24 | `M94l: タイルパレット窓 (ブラシ / 消去 / 塗り潰し / 矩形 / ピック、Undo)` |
| 13 | ルールタイル、アニメーションタイル、レイヤー UI | 11, 12 | 1-3, 25 | `M94m: ルールタイルとアニメーションタイル、タイルレイヤーの編集` |
| 14 | ABI (10 本) + C# ミラー + MyeScript | 08, 11 | 1-3, 26 | `M94n: ABI vNN — タイルのセル操作・座標変換、OverlapBoxMasked / BoxCast、SpriteByName (NN スロット)` |
| 15 | Light2D とライトバッファ (**チェックポイント**: sub-11〜15) | 06 | 1-5, 27, 28 | `M94o: 2D ライト (Global / Spot / Freeform / Sprite) とソート層ごとのライトバッファ` |
| 16 | ShadowCaster2D と法線マップ | 15 | 1-3, 29 | `M94p: 2D の影 (ポリゴン押し出し) と法線マップ付きスプライト` |
| 17 | スプライトのサーフェスシェーダ枝、Parallax2D、2D ポスト例 | 06 | 1-3, 30, 31 | `M94q: スプライトのプロジェクトシェーダ、視差 (Parallax2D)、CRT ポストの例` |
| 18 | Line2D | 06 | 1-3, 32 | `M94r: Line2D (点列・幅・塗り)` |
| 19 | SpriteShape2D (描画・衝突・ハンドル) | 08, 18 | 1-3, 33 | `M94s: SpriteShape2D (スプラインに沿う縁と塗り、辺衝突、ハンドル編集)` |
| 20 | 2D ジョイント検証と Effector2D (**チェックポイント**: sub-16〜20) | 09 | 1-5, 34 | `M94t: 2D ジョイントの検証と Effector2D (風 / コンベア / 浮力 / 引力)` |
| 21 | GridAgent2D、BT MoveTo の fallback、Perception forwardAxis | 11, 09 | 1-3, 35 | `M94u: グリッド A* の GridAgent2D と BT MoveTo の 2D 対応、知覚の前方軸` |
| 22 | Camera2DFollow と PixelPerfectCamera2D | 02, 11 | 1-3, 36, 37 | `M94v: 2D カメラ追従とピクセルパーフェクトカメラ (低解像度 RT の整数倍拡大)` |
| 23 | `--2d-demo`、golden `2d` / `2d_lights`、replay_verify `2d` (**チェックポイント**: sub-21〜23、golden 新設) | 全部 (01-22) | 1-5, 38, 39 | `M94w: 2D デモ (--2d-demo) と golden 2d / 2d_lights、replay_verify の 2d ジョブ` |
| 24 | 文書 (engine_spec / ADR-033 / test_checklists / README) (**チェックポイント**: 最終) | 23 | 1-5, 40 | `M94x: 2D レーンの ADR-033 と engine_spec / 検証表 / README を更新する` |

最初のサブ (01) が最も荷重のかかる未知 (U3 の平面拘束が 3D ソルバで厳密に成立するか) を潰す。02 / 03 は 01 と独立。

## 7. 未決事項・リスク

- (R1) 平面拘束の (A)(B)(D) で不変量が割れたら: 速度ソルバの毎反復で射影 (候補 C) を足す。それでも割れる = 位置補正パスの `translateBody` を plane2D ボディでは z 成分を捨てて呼ぶ。sub-01 の SELF_EVAL で報告。
- (R2) タイル衝突の箱数が多すぎて sort & sweep が遅い場合: チャンク AABB の前カリング (sub-11 で計測して判断)。
- (R3) ステンシルの初使用で SceneView / GameView の RT の DSV がステンシル付きであることを確認する (`RenderTexture.cpp:55-59` は D24S8 だが、読み取り専用 DSV の差し替え (`RenderSystem.cpp:2097`) との順序に注意)。
- (R4) ピクセルパーフェクトの低解像度 RT を `PostProcess::Acquire` の LRU (上限 4) に入れると他サイズを追い出す可能性 → 専用 `RenderTexture` メンバで持つ (4.1.12 どおり)。
- (R5) M75h が先に ABI v31 を使った場合は sub-14 は v32。司会は着手時に `EngineAPI.h` を見る。
- (R6) `.meta` の `sprites[]` を手で編集して重複 id を作った場合: 後勝ちで警告 (壊さない)。
- (R7) GridAgent2D の 1 tick 4096 展開上限で広い面は数 tick かかる。status Moving のまま待つ (Stuck にはしない)。
- (R9) sub-01 の既知の限界: (a) 複合コライダーの子形状の `oneWay` は見ない (b) 親付き (親フレームが identity でない) plane2D 剛体の `LocalTransform.z` 保証は未検証 — z0 はワールド z で、書き戻しは親の逆変換を通る。Rigidbody は「ルート = ワールド位置前提」(`Components.h` の Rigidbody コメント) なので v1 はルート限定とし、engine_spec に書く (sub-24) (c) `FractureSystem.cpp:188` は破片へ `freezeRotation` をコピーするが `plane2D` はまだコピーしない → sub-08 で 1 行足す。
- (R10) 一方通行の速度条件 (kOneWayLandingSpeed) は「上面が天面より上で、半分以上沈んで降下中の物」も押し上げる。プラットフォーマーの着地としては望ましい挙動だが、天面より下から高速で上昇中の物は法線条件で通す (期待どおり)。問題が出たら深さの上限を足す。
- (R11) 正射影カメラ + froxel (`--froxel`) の描画は未確認 (`engine_spec.md:406` は正射影でグリッドをスキップすると書く)。sub-15 (ライト) か sub-23 (デモ) で `--2d-demo --froxel` を 1 回撮って壊れないことを見る。
- (R8) 2D ライトのバッファ再構築回数が層数に比例する。層 32 × 別々のライト集合という極端な配置では 32 回 clear + 加算。v1 はそのまま、engine_spec に書く。

## 8. 変更履歴

(確定後の変更のみ。出所と理由を書く)

- 2026-10-20 ユーザー判断 9 (「selftest や検証 bat はサブ 5 回につき 1 回でいい、回すのが長すぎる」): 4.4 の検証運用を「毎サブ = 0 警告ビルド + check_rules + 自分の selftest 節 (`MYE_SELFTEST_ONLY=m94`) + 軽い固有検証」「チェックポイント (sub-05 / 10 / 15 / 20 / 23 / 24) = 全体 selftest + replay_verify + shot_verify (golden 30 枚 maxDiff=0)」に変更。5. の共通条件を 1-3 (毎回) と 4-5 (チェックポイント) に分け、6. の表と sub-01〜24 の「検証コマンド」を追随させた。sub-01 の Plane2D 節 (a)〜(d) と hash-diff は核心なので必須のまま。
- 2026-10-20 coder SELF_EVAL sub-01 round 1 (planner 採用): 4.1.5 の `oneWay` 判定に速度条件 (`kOneWayLandingSpeed` 0.5 m/s) と `kOneWaySlop` 0.05 を追記し、判定箇所を 4 箇所 (接触生成・位置補正・睡眠ペア・CCD) に広げた。理由: 条件式だけでは y=2 から静止落下した球が床を素通りした (再現済み)。7. に R9 (複合子の oneWay 未対応、親付き plane2D 未検証、Fracture の plane2D 未コピー) と R10 を追加。平面拘束は A+B+D の 3 点で不変量が成立 (R1 の追加拘束は不要だった)。snapshot は v51。
