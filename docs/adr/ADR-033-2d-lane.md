# ADR-033: 2D レーンは 3D ソルバへの平面拘束・整数ソートキー・アセット保存 + 差分 sim 状態で作る

- 状態: 採用 (2026-10-22、M94)
- 出所: 仕様 `plans\m94-2d\spec.md` (疑った点 31 件の裁定が正本)、台帳 `plans\m94-2d\harness.md`。
  仕様の写しは [engine_spec.md](../../engine_spec.md) §6.15 / §10.5 末尾 / §10.9 / §10.13、検証の手順は [docs\test_checklists.md](../test_checklists.md) の M94 節

## 背景

横スクロール 2D ゲーム (XY 平面、+Z が奥) を、3D と同じ契約 (Debug / Release / WARP / `Server.exe` でシミュレーションがビット一致、
リプレイ、What-if 分岐、ロールバック) のまま作れる状態にする。着手前の 2D 専用部品はゼロだった (正射影カメラ無し、スプライトは VFX 用ビルボードで
UV・ソート層・flip 無し、タイルマップ・スプライト列アニメ・2D ライト・ピクセルパーフェクト無し)。
ユーザー判断: 2D スケルタルアニメ (ボーン・ウェイト・スワップ) と見下ろし (Y ソート、XZ) はやらない。

## 決定

1. **2D 物理は 3D ソルバへの平面拘束 (`Rigidbody.plane2D`) にする。** 専用ソルバ (Box2D 相当) も `Rigidbody2D` / `Collider2D` / `Joint2D` も作らない。
   拘束は 3 点: (A) 収集時に慣性の逆行列を Z 軸成分だけ残す、(B) 速度ソルバの直後に `vz = wx = wy = 0.0f`、(D) 位置・姿勢積分 (CCD 含む) の後に
   `pz = z0`・`qx = qy = 0.0f` と速度の再ゼロ。z は 0 に強制せず**初期値 z0 を保つ** (異なる z の plane2D 同士は当たらない = Unity と違う)。
   OFF なら 1 演算も増やさない (M59f1 の分岐ゲートの流儀)。CC は既存 CC に `plane2D` を足して一方通行・移動床・剛体押し・壁と接地の報告を 2D 専用に有効化する。
   2D ジョイントは新設せず Hinge(Z) / Slider / Fixed / SpringJoint をそのまま使う。
2. **一方通行は `Collider.oneWay` (bool) の固定 180° 判定。** 判定関数は `RejectOneWayContact` 1 つで、接触生成・位置補正・睡眠ペア・CCD 掃引の 4 箇所から呼ぶ。
   タイルもこのフラグで同じ経路を通る。
3. **スプライトは別コンポーネント `Sprite2D` と専用パス。** 既存の `SpriteRendererComponent` (VFX のビルボード) には 1 バイトも触らない。
   並びは**整数ソートキー** (ソート層の配列順, orderInLayer, SortingGroup, entity.index) の辞書式で、viewZ を見ない。
   ソート層は `project_settings.json` の安定 id (0..31、上限 32 = Light2D の `targetLayerMask` が uint32)。
4. **スプライトのスライスは `.meta` のサブアセット。** 各スライスに安定 `id` を持たせ AssetID は `HashStr(SubAssetKeyPrefix(texPath) + "#sprite" + id)`
   (名前キーは改名で ID が変わるため不採用)。filter (Point / Linear) はパスが 2 サンプラを持ちバッチで分ける。
5. **スプライト列アニメは Animator Controller のステートに第 3 の口 `"sprite"` を足す。** 遷移・型付きパラメータ・イベント・`AnimatorPlay`・BT の PlayAnimation を
   そのまま使え、現在コマは `stateTimeTicks` から導出する値なので新しい sim 状態は増えない。
6. **タイルマップは「アセットが初期値、sim 状態は書き換えたセルの差分だけ」。** `.tilemap.json` を読み、`TilemapSystem::OverrideStore`
   (ソート済み配列 `(entity, layer, cellKey) → 値`) を内容ゲートで hash・snapshot ('TM2D' 節) に入れる。Tilemap が無いシーンは hash も snapshot も 1 バイトも変わらない。
   ルール変種・アニメのコマ・衝突の箱は導出値。衝突は行方向に結合した箱を静的収集の供給口 (`AddStaticShapeSource`) から渡し、動く物の届く範囲だけに絞り込む。
7. **2D ライトは画面サイズのライトバッファへの加算 + スプライト PS での乗算、影は辺の押し出し。** G-Buffer と無関係なので Forward / Deferred で同じ絵になる。
   ライトが 1 つも無ければバッファを作らない (presence gate、既存 golden 不変)。ソート層ごとに「効くライトの集合」が変わるたびに作り直す。
   影は R8 の影 RT にポリゴンの辺を押し出して描く。法線マップは法線 RT に描いて N·L を掛ける。
8. **ピクセルパーフェクトは `RenderSystem::Render` の中で低解像度 RT に描いて Point で整数倍 blit。** 黒帯は作らず、RT は画面を覆う最小の偶数。
   TAA のジッタと FXAA は無効、ポストは低解像度で掛かり、UI / ImGui は後段でフル解像度。式は Unity の `PixelPerfectCameraInternal` と同じ
   (`zoom = max(1, min(H / refY, W / refX))`)。シミュレーションに画面寸法を入れないため、Camera2DFollow の境界は `CameraComponent.orthoSize` と `boundsAspect` で計算する。
9. **ABI は 1 回、段階 3 の末尾 (v31 = 188 スロット、10 本)。** タイルの読み書き・座標変換・ダメージ、マスク付き OverlapBox、BoxCast、`SpriteByName`。
   それ以外のフィールドは汎用 `Get/SetComponentField` で届く。SimSnapshot はサブごとに上げた (最終 v65)。Scene `kDocVersion` と `kCookVersion` は据え置き。
10. **グリッド A* は `GridAgent2D` を新設し、BT の MoveTo は NavMeshAgent が無ければ GridAgent2D を見る** (同じ status 値の 2 行の分岐)。Nav の抽象化はしない。

## 却下した案

| 案 | 却下の理由 |
|---|---|
| 既存 `SpriteRenderer` を拡張する | VFX のビルボードは viewZ 降順の浮動小数点ソート・UV 固定・常に LINEAR で責務が別物。触ると既存 golden の不変が自明でなくなる |
| 専用 2D ソルバ / Rigidbody2D 系コンポーネント群 | TypeId・Inspector・Nav/BT/ルートモーションの CC 連携を全部二重化する。平面拘束 3 点でビット単位の不変量が成立した (下記) ので不要 |
| z を 0 に強制 | シーン配置で z をずらした瞬間に物理物が飛ぶ |
| 新コンポーネントの CC2D / 箱 CC | 既存の押し出し・段差・坂のコードを流用できなくなる (箱 CC の押し出しが 1 本増える) |
| 一方通行の Surface Arc 可変 | Collider に float が 1 本増え CC とタイルの判定にも角度が入る。固定 180° で足りる |
| ソート層の上限なし | Light2D の対象指定が名前リストになり POD に収まらない |
| `SpriteAnimator` の新設 | 遷移・パラメータ・イベント・ABI を二重に持つ |
| プロパティクリップで `Sprite2D.sprite` を駆動 | プロパティクリップは float 系しか駆動できない (AssetRef は書けない) |
| セル配列全体を SimSnapshot に入れる | 100×50×3 層でもTimeTravel の snapshot 予算 (64 MB) を食う。差分だけで足りる |
| タイル衝突の専用ブロードフェーズ | 計測で足りた (供給口 + 動く物の範囲の前カリングで物理 1 tick が `--physics-demo` 比 0.82) |
| 1D シャドウマップ | 角度サンプリングのエイリアスとライトごとの RT が要る分だけ不利 |
| ピクセルパーフェクトの黒帯 (Pillarbox 等) | ユーザー判断 (黒帯なし)。RT を「画面を覆う最小の偶数」にして余りはワールドを広く見せる |
| ピクセルパーフェクト RT を `PostProcess::Acquire` の LRU に入れる | 他サイズを追い出す。専用 `RenderTexture` メンバで持つ |
| Nav の抽象化 (NavMeshAgent と GridAgent の共通インターフェース) | M82 の構造を今回の目的の外で動かす |
| ABI を最終段階に寄せる | 段階 3 で横スクロールが作れる時点でスクリプトからタイルを触れなくなる |
| 展開上限超えで「最初からやり直す」A* | 同じ入力で同じ結果になり永遠に進まない。最良ノードへの部分経路で動く |
| タイル衝突のチャンク境界をまたぐ結合 | v1 は 16 セル境界で箱が分かれる。縁で引っかかる報告が出たら足す |

## 結果

- **平面拘束 (sub-01)**: 3 点拘束で 3000 tick 後の plane2D 剛体 16 体 + Hinge 1 組の `z == z0`・`vz == 0.0f`・`wx == wy == 0.0f`・`qx == qy == 0.0f` がビット単位で成立し、
  2 ワールド並走の HashWorld が一致、焼いた値 `639107476EA401A3` が Debug == Release。毎反復射影 (候補 C) は不要だった。
  一方通行は「法線条件 + tick 開始時の最下点 + 降下速度条件」で、速度条件が無いと静止落下の球が床を素通りした (再現済み)。
- **CC 2D (sub-09)**: 焼き値 `C04B7FFA3E31E5D0`。kinematic かつ plane2D の剛体は velocity で積分する (round 1 の must)。
- **タイル衝突 (sub-11)**: 100×50 の床は 350 箱。前カリング前は物理 1 tick が `--physics-demo` 比 4.6 倍、後は 0.82 倍。タイル並走シナリオの焼き値 `326BA3CD14F4BBB8`。
  描画の焼き直し (sub-13 の計測): 100×50×3 層の全 84 チャンクで最悪 2.8 ms / フレーム (チャンク単位の dirty は不要と判断)。
- **GridAgent2D (sub-21) / Camera2DFollow (sub-22) / Effector (sub-20)**: 焼き値 A `05C77D287CAAA51B` / `D5147EA06255DFC2` / joint2D `A82F779A97738D34`・effector2D `9504211B9041ECC2`。
- **デモ (sub-23)**: `--2d-demo` の 3000 tick 不変量と WorldHash `55DEACAD78012785` (Debug == Release)。replay_verify の `2d` ジョブは Debug record / Debug stress / Release / `Server.exe` の 4 経路すべて PASS。
  golden `2d` / `2d_lights` を新設し、既存 golden は動かなかった (29 枚 maxDiff 0、`demo_terrain_deferred` は基点の絵と一致)。
- **性能 (Release、WARP 960×540)**: デモ 0.88〜1.39 ms/フレーム、`--2d-stress` (Sprite2D 2000 枚 + 100×50×3 層) 4.43〜4.60 ms/フレーム (16 ms 目標内)、
  sim 0.21 ms/tick (Debug 2.6 ms)、供給口つきクエリ Raycast 12.7 µs / BoxCast 67.5 µs (86 箱)。1 tick の sim が 2 ms を超えないので供給口の tick 単位キャッシュは入れていない。
- **契約**: 版は SimSnapshot v65・ABI v31 = 188・TypeId 81〜93。`check_rules.ps1` は M94 の外にある既存 3 error のまま (M94 で増やしていない)。
- **v1 の限界**は [engine_spec.md](../../engine_spec.md) §10.13 末尾に集約した (親付き plane2D、複合子の oneWay、回転床、BoxCast の近似、タイル絞り込みの余裕、
  マスクの効き方、影・法線・Freeform、SpriteShape の height、GridAgent の動く足場、Effector の領域内は眠らない、froxel は 2D に効かない 等)。

## 参照

- Unity: Platform Effector 2D — https://docs.unity3d.com/Manual/2d-physics/effectors/platform-effector-2d-reference.html
- Unity: URP 2D Lights — https://docs.unity3d.com/6000.0/Documentation/Manual/urp/Lights-2D-intro.html
- Unity: 2D Light Properties — https://docs.unity3d.com/Manual/urp/2DLightProperties.html
- Unity: Shadow Caster 2D — https://docs.unity3d.com/6000.0/Documentation/Manual/urp/ShadowCaster2D.html
- Unity: PixelPerfectCameraInternal.cs — https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.universal/Runtime/2D/PixelPerfectCameraInternal.cs
- 関連 ADR: [ADR-024 AI 知覚](ADR-024-ai-perception.md)、[ADR-025 ビヘイビアツリー](ADR-025-behavior-tree.md)、[ADR-027 骨ポーズプログラム](ADR-027-skeletal-pose-program.md)
