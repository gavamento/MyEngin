# sub-08: 2D 形状 (`kPolygon2D` / `kEdge2D`、`.shape2d.json`) と C++ クエリ

- 依存: sub-01
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #19、4.1.8

## やること
1. `collidershape::kPolygon2D = 6`、`kEdge2D = 7`、`kCount = 8`。`.shape2d.json` と `ShapeLibrary2D` (`src\Engine\Engine\Physics\Collider\Shape2DLibrary.{h,cpp}`、GUID キー、ReloadHub、Create メニュー、Inspector 編集は点の一覧)。`ColliderComponent.meshAsset` で参照 (kMesh / kTerrain と同じ流儀。Inspector の表示名を shape で切り替える)。
2. 解決 (`PhysicsSystem` の静的収集 :1701 と動的ボディの形状確定):
   - 静的 `kPolygon2D`: 点列を耳刈りで三角形化し、z ∈ [−depth/2, +depth/2] のプリズム (前後面 + 側面) の三角形スープを作って kMesh の経路に流す (`ConvexColliderLibrary` / `CookedCache` のような焼き込みは不要。メモリ上のキャッシュを `ShapeLibrary2D` が持つ)。凹も可。
   - 動的 `kPolygon2D`: プリズムの頂点の凸包を kConvex の経路へ (`ConvexCollision.cpp` の既存)。凹は埋まる (Inspector 注記)。質量・慣性は kConvex の既存計算。
   - `kEdge2D` (静的のみ。動的に付いていたら WARN して無視): 線分ごとに薄い箱 (半厚 `edgeRadius`、長さ = 線分、z 半厚 = depth/2、Z 回転 = 線分の向き) を**静的収集に複数の ShapePose として供給する口**を作る (sub-11 のタイル箱、sub-19 の SpriteShape も同じ口を使う。`StaticShapeSource` のような小さなインターフェースで、PhysicsSystem が tick ごとに `Collect(std::vector<StaticShape>&)` を呼ぶ。所有は呼び出し側)。`oneWay` は Collider の値を全線分に継承。
3. クエリ: `OverlapBoxWorld` にマスク付きの口 (`PhysicsQueries.cpp:80` に `mask` 引数の overload)、`BoxCastWorld(center, halfExtents, dir, maxDist, mask, skip, outHit)` (`SphereCastWorld` :92 の 32 回保守的前進と同じ構造。AABB vs 形状の最近接距離は `shapes::` の既存 Overlap を距離化できないなら、箱の 8 頂点 + 中心からの `Raycast` で近似してよい — 採った方式を差分に書く)。
4. Scene View ギズモ: `kPolygon2D` / `kEdge2D` の点列 (緑)、選択中は点をドラッグ (`HandlePatrolRoute` の流儀)、辺クリックで追加、Delete で削除。編集はアセット (`.shape2d.json`) に書く (独自 Undo は sub-05 の `SpriteEditModel` と同型の小さいもの、またはドラッグ 1 回 = 保存 1 回で Undo 無し。採った方を差分に書く)。
5. PhysicsSelfTest に `Shapes2D` 節 (spec 4.1.8 の 5 項目) + Debug / Release のハッシュ焼き込み。
6. (sub-01 round 1 の申し送り、spec 7. R9 (c)) `FractureSystem.cpp:188` 付近で破片の Rigidbody へ `plane2D` もコピーする (`freezeRotation` の隣に 1 行)。plane2D の破壊物の破片が XY 平面に留まることを `Shapes2D` 節か FractureSelfTest に 1 項目で確認。

## やらないこと (このサブでは)
- ABI 露出 (sub-14)、タイルの箱 (sub-11)、SpriteShape (sub-19)

## 触る場所 (planner の見立て)
- `Components.h:203-211` (shape 列挙)、`Components.cpp` (和名の説明)
- 新規 `src\Engine\Engine\Physics\Collider\Shape2DLibrary.{h,cpp}`、`StaticShapeSource.h` (名前は任意)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.{h,cpp}` (:1701 静的収集、形状確定、:1843-2054 凸包の質量)、`PhysicsQueries.cpp:80-92`、`Shapes.{h,cpp}` (必要なら)
- `src\Engine\Engine\Physics\Collider\ConvexCollision.cpp` (読むだけで済むはず)
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub`、`AssetBrowserWindow.cpp`、`AssetOps`、`InspectorWindow.cpp` (Collider 節の表示)
- `SceneViewWindow.cpp` (ギズモ)、`PhysicsSelfTest.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 18) `Shapes2D` 節 5 項目が Debug / Release で PASS、焼いたハッシュ同値。
2. (spec 18) 点編集のエディタスクショ `plans\m94-2d\screenshots\shape2d_edit.png`。
3. 既存形状 (0..5) のシーンの hash-diff が無差分 (`--physics-demo`、`--joint-demo`、`--fracture-demo`)。
4. (spec 1-3) 0 警告、check_rules 0、Physics suite が `MYE_SELFTEST_ONLY=m94` で PASS、版 bump なし (shape は int32 の値追加、形は変わらない)。重い 3 本は sub-10。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --physics-demo / --joint-demo / --fracture-demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-10)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-08 (round 1)
実装:
  - Shape2DLibrary.{h,cpp} (新規): `Shape2D` / `ShapeLibrary2D` (SpriteAnimLibrary 範型、GUID キー、`shape2d::Install/Library/Resolve/ResolveSoup/ResolveHull`)、`TriangulatePolygon2D` (耳刈り。CW 入力は反転、共線は三角形を出さず落とす、耳なしでも先頭を強制で切って必ず終わる)、登録時に導出データ (プリズムの三角形スープ = `MeshColliderData`、プリズム頂点の凸包 = `ConvexHullData`) を作る。JSON は `{engine, shape2d:1, points, closed, edgeRadius, depth}`、Sanitize (非有限の点を落とす、点 512 上限、edgeRadius / depth を範囲へ)
  - Components.h: `collidershape::kPolygon2D = 6` / `kEdge2D = 7` / `kCount = 8` (int32 の値追加、形は不変 = snapshot bump なし)
  - Shapes.cpp: `ApplyScaledExtents` が kPolygon2D を `pose.shape = kMesh` + soup、kEdge2D を `kMesh` + null (展開しない経路は「衝突なし」)。`UseHullForDynamicPolygon2D` (動的 → `kConvex` + hull)、`ExpandEdge2D` (線分ごとの箱。半厚 `edgeRadius * max(sx,sy)`、長さは線分 + 両端 edgeRadius、z 半厚 = depth/2*sz、向きは方向ベクトルから (c, s) を直接作る = 三角関数なし)
  - PhysicsSystem.cpp: `SetBodyShape` が rb 付き非 kinematic の多角形を凸包へ差し替え / 動的 (kinematic 含む) の Edge2D は WARN (1 回) して col を無効化 / 静的収集の本体を `makeStaticBody` に抽出し、kEdge2D を線分ごとの Body へ展開 / `AddStaticShapeSource` / `RemoveStaticShapeSource` と、静的収集のあとに登録順で `Collect` を呼ぶ口 / bodies の整列を `std::stable_sort` に (同じ持ち主の静的形状が並ぶため。持ち主が一意なら std::sort と同じ並び) / `ShapeVolumeWorld` / `MeanProjectedAreaWorld` / 凸包質量のゲートに kPolygon2D
  - StaticShapeSource.h (新規): `StaticShape { owner, pose, col }` と `StaticShapeSource::Collect(World&, std::vector<StaticShape>&)`。所有は呼び出し側 (sub-11 / sub-19 が同じ口を使う)
  - PhysicsQueries.cpp / PhysicsSystem.cpp (RaycastWorld): Edge2D を線分の箱へ展開 (整列は stable_sort)、`OverlapWorld` は同じ持ち主の複数の箱を 1 回だけ数える、`BoxCastWorld` (新規)
  - FractureSystem.cpp:`CopyOtherRigidbodyFields` に `dst.plane2D = src.plane2D` (やること 6)。FractureSelfTest (16a) で root に plane2D を立て、分離した破片の Rigidbody が plane2D を持つことを 1 項目で確認
  - アセット周り: AssetDatabase (`AssetType::Shape2D` 末尾、ClassifyPath / TypeName / ParseTypeName)、ReloadHub (`.shape2d.json` rank 6、`ReloadShape2D`)、SimInit (`SimLibraries.shapes2d`、Install / Uninstall)、EngineLoop / HeadlessSim (所有)、DemoContent の起動走査、AssetOps `CreateShape2DAsset` (既定 = 1x1 の四角形) + SplitAssetName の複合サフィックス、AssetBrowser (Create メニュー / 種別フィルタ / タイルラベル)、FileTypeIcon
  - Editor: Shape2DEditor.{h,cpp} (新規。`SaveShape2D` / Inspector の点の一覧 = 座標・削除・追加・closed / edgeRadius / depth・Save / Revert)、InspectorWindow (形状コンボに Polygon2D / Edge2D (英日)、アセット選択時に Shape2D の Inspector、Collider.meshAsset のピッカーが shape 6/7 のとき 2D 形状を列挙 + `.shape2d.json` のドラッグ、Collider のメモ = 未解決 / 動的な多角形は凸包で凹が埋まる / 動的な辺は無視、Edit Collider を 2D 形状でも有効に)、SceneViewWindow (`AddShape2DWire` = 点列の緑の輪郭 (前後面 + 支柱) を DrawColliderGizmos から / `HandleShape2DEdit` = Edit Collider が入った 2D 形状の点ハンドル (緑、選択は黄、番号つき)・点のドラッグ・辺クリックで追加・Delete で削除・Esc で終了、Ctrl でスナップ)
  - LocalizationTable.inl: Asset_Shape2D ほか 14 件 (英日) + Insp_EditColliderUnsupported の文言更新
  - テスト: PhysicsSelfTest に `Shapes2D` 節 (耳刈り 5 / 資産 5 / シミュレーション 13 / クエリ 11 の計 34 項目 + ハッシュ焼き込み `0x2ECF7CFD5CC45314`)、AssetDatabase / ReloadHub / AssetOps の行、`MYE_SELFTEST_ONLY=m94` に `RunFractureSelfTest` を追加
仕様との差分:
  - [追加] `OverlapBoxWorld` のマスク付きの口は**既に存在した** (`mask` 引数、既定 = 全レイヤー) → 新しい overload は作らず、`Shapes2D` 節で既存の口を検証した
  - [逸脱] `BoxCastWorld` の引数順は spec の「mask, skip, outHit」ではなく既存の `RaycastWorld` / `SphereCastWorld` に揃えた `(world, center, halfExtents, dir, maxDist, outHit, mask = 全, skip = null, skipUser = null)` — 同じ系列のクエリと呼び方を揃えるため。sub-14 の ABI は別に決まる
  - [追加] `BoxCastWorld` の方式: 箱の 27 点 (頂点 8 + 辺の中点 12 + 面の中心 6 + 中心) から進行方向へ `Raycast` を撃った最短距離 (sub-08.md が許した近似)。始点で `Overlap` していれば距離 0・法線 -dir (Unity と同じ)。形状の突端が撃つ点の隙間に入る場合は取りこぼす。32 回の保守的前進は採らなかった (AABB vs 形状の距離化が要る)
  - [追加] 動的 `kPolygon2D` は `b.rb != null && !b.kinematic` のとき凸包、kinematic は静的と同じ三角形スープ (kMesh が kinematic で使えるのと同じ)。複合コライダーの子に付いた多角形は v1 では三角形スープのまま (動的剛体の子としては未対応 = Inspector / 台帳の申し送り)
  - [追加] 点編集の Undo: 採ったのは「ドラッグ 1 回 = 保存 1 回、Undo 無し」(sub-08.md が許した方)。ドラッグ中はライブラリの項目だけ差し替えて見た目を追従させ、離したときに `SaveShape2D` (Sanitize → ファイル書き込み → LoadFromFile) する。辺クリックの追加と Delete も即保存
  - [追加] `Edit Collider` のトグルを 2D 形状でも使う (点ハンドルの入口)。`HandleShape2DEdit` は `rt_.IsValid()` を見ない (最初のフレームで DrawGizmo がフラグを落とすのを避けるため)
  - [追加] `kEdge2D` の箱は折れ点の外側に隙間を作らないよう両端を edgeRadius ぶん伸ばす (長さ = 線分 + 2*edgeRadius)。非一様スケール × 回転のシアーは無視 (既存の箱と同じ割り切り)。`closed` は 3 点以上でだけ閉じる
  - [追加] 親付き (frame が identity でない) Edge2D / Polygon2D は `MakePose` を通るので親のスケールは効くが未検証 (sub-01 の plane2D と同じ扱い)
検証:
  - Debug / Release `/p:MyeWarnAsError=true` ビルド → 0 警告・0 エラー (Release の初回だけ既知の `SelfTestUnused.kernel.hlsl` C4100 で落ち、同じコマンドの再実行で通った = sub-01 の申し送りどおり)
  - `pwsh tools\gen_project_files.ps1` → build\*.vcxproj / filters を更新 (新規 5 ファイル)
  - `pwsh tools\check_rules.ps1` → 着手前 3 error / 50 warning、終了後も 3 error / 50 warning (既存の EditorMain `#ifdef _DEBUG`・Editor.bat / Setup.bat の LF のまま。新しい error / warning なし)
  - `MYE_SELFTEST_ONLY=m94` (Editor.exe --selftest、Physics + Fracture + UI + Sprite2DAsset + SpriteEditor + AnimatorController(+Edit) + AssetDatabase + ReloadHub + AssetOps) → Debug / Release とも exit 0、FAIL 行 0、PASS 1676。`Shapes2D` 節 34 項目 PASS、シーンハッシュ @600 = 2ECF7CFD5CC45314 (Debug == Release、焼き込み後の照合も PASS)、plane2D の焼いたハッシュ 639107476EA401A3 も不変で PASS
  - 既存形状のシーン: 今日 08:2x にチェックポイント 1 で録った `cache\golden_physics.rep` / `golden_joints.rep` / `golden_fracture.rep` を新ビルドで `--physics-demo / --joint-demo / --fracture-demo --replay-verify` → Debug / Release の 6 本とも `VERIFY PASS: 600 ticks hash-identical` (= 全 tick のワールドハッシュが無差分。`--hash-diff` より強い照合。新規録画 .rep はバイト一致しないが、これは .rep 内の入力ミラー等の差で、全 tick の照合が通っている)
  - スクショ: `plans\m94-2d\screenshots\shape2d_edit.png` (Release Editor.exe、一時プローブで 2D モード + Edit Collider + L 字 6 点の Polygon2D を選択 → 緑の輪郭、番号つきの点ハンドル、下部のバナー、Inspector の Edit collider チェック済みを確認)。プローブと一時アセット (`assets_tmp_m94h\ProbeL.shape2d.json`) は撮影後に除去、imgui.ini は撮影前の内容へ復元 (ハッシュ一致を確認)
  - 未実行: 全体 `--selftest`、`replay_verify.bat`、`shot_verify.bat` (ユーザー判断 9 により sub-10)。点のドラッグ / 辺クリック追加 / Delete 削除の**実クリック**は入力を注入できないので未確認 (ロジックは写像と保存経路をコードで追っただけ。ユーザー目視 / sub-24 の checklists へ)
自己採点 (1-5):
  仕様適合: 4 — やること 1〜6 と受け入れ 1〜4 を満たす。BoxCast の引数順と近似方式 (許可済み)・既存 mask 口は差分に記載。複合コライダーの子の多角形は未対応
  正しさ: 4 — 凹 L 字の窪み / 辺の両面 / 動的凸包 / 供給口 / BoxCast / マスクを実シミュレーションとクエリで検証、Debug == Release のハッシュ、既存 3 デモは 600 tick 無差分。GUI の実クリックと Edge2D の親付きスケールは未検証
  コード品質: 4 — SpriteAnimLibrary 範型で既存の流儀に揃え、静的収集の Body 組み立てを 1 箇所 (makeStaticBody) に集約。SceneView の HandleShape2DEdit が長い (点の描画・判定・ドラッグ・追加・削除を 1 関数) のは nit
  テスト: 4 — Shapes2D 34 項目 + 資産 / Reload / AssetOps / Fracture の行。点編集 GUI の自動テストは無い
不安・質問:
  - Q1 (planner 判断): `BoxCastWorld` の引数順を spec の「mask, skip, outHit」から `RaycastWorld` 流 (outHit の後ろに mask / skip) へ変えた。sub-14 の ABI で再度決まるので C++ 側はこのままでよいか
  - Q2: 複合コライダー (`compoundColliders`) の子として付いた kPolygon2D は動的剛体の一部として凸包で解けない (v1 は三角形スープのまま = 動的剛体には不適)。spec R9 (a) の oneWay と同じ「v1 未対応」として engine_spec に書く (sub-24) でよいか
  - Q3: `MYE_SELFTEST_ONLY=m94` に `RunFractureSelfTest` を足した (plane2D の破片コピーの 1 項目を m94 で走らせるため)。重ければ外して sub-10 の全体 selftest に任せてもよい
触ったファイル:
  src\Editor\App\EditorMain.cpp、src\Editor\Asset\AssetOps.cpp / .h / AssetOpsSelfTest.cpp、src\Editor\Widgets\EditorWidgets.cpp、src\Editor\Windows\Asset\AssetBrowserWindow.cpp、src\Editor\Windows\Scene\InspectorWindow.cpp / .h、src\Editor\Windows\Scene\SceneViewWindow.cpp / .h、src\Editor\Windows\Scene\Shape2DEditor.cpp / .h (新規)、
  src\Engine\Core\Ecs\Components.cpp / .h、src\Engine\Core\Localization\LocalizationTable.inl、src\Engine\Engine\Asset\AssetDatabase.cpp / .h / AssetDatabaseSelfTest.cpp、src\Engine\Engine\Demo\DemoContent.cpp、src\Engine\Engine\HotReload\ReloadHub.cpp / .h / ReloadHubSelfTest.cpp、src\Engine\Engine\Loop\EngineLoop.cpp / HeadlessSim.cpp / SimInit.cpp / SimInit.h、
  src\Engine\Engine\Physics\Collider\Shape2DLibrary.cpp / .h (新規)、src\Engine\Engine\Physics\Fracture\FractureSelfTest.cpp / FractureSystem.cpp、src\Engine\Engine\Physics\Rigid\PhysicsQueries.cpp / PhysicsSelfTest.cpp / PhysicsSystem.cpp / PhysicsSystem.h / Shapes.cpp / Shapes.h / StaticShapeSource.h (新規)、
  plans\m94-2d\screenshots\shape2d_edit.png (新規)、plans\m94-2d\sub-08.md (実装メモ)。build\*.vcxproj / filters は gen_project_files の生成物 (一覧外)
申し送り:
  - sub-11 (タイル): 静的形状の供給口は `PhysicsSystem::AddStaticShapeSource` + `StaticShapeSource::Collect` (StaticShape = owner / pose / col)。同じ持ち主の形状は並べて返すこと (`stable_sort` が収集順を保つ)。形状の姿勢は呼び出し側がワールドで確定して渡す。`col` は tick の間変わらない ColliderComponent を指すこと (oneWay / 材料 / レイヤーの出所)
  - sub-14 (ABI): `OverlapBoxWorld` は既にマスク付き。`BoxCastWorld` を足す (C++ の引数順は `(world, center, half, dir, maxDist, outHit, mask, skip, skipUser)`)
  - sub-19 (SpriteShape): `shapes::ExpandEdge2D` は `Shape2D` 資産 (点列) を読む。SpriteShape の辺は自前の点列から同じ式で箱を作るか、`StaticShapeSource` で箱を返す
  - 2D 形状の資産キー = `.shape2d.json` の GUID。ライブラリの項目は Register で同じ map 要素へ書き換わる (ポインタは有効なまま中身が変わる) ので、tick の途中で Register しないこと (エディタの点ドラッグは UI フェーズで呼ぶ)
  - 既知の限界: 複合コライダーの子の多角形 / 親付きの多角形・辺 / 点編集 GUI の実クリックは未検証

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。スクショ `shape2d_edit.png` で L 字 6 点の緑の輪郭・番号つきハンドル・バナー・Inspector の Polygon2D / Edit collider を確認。`Shapes2D` 節 34 項目が両構成 PASS、焼いたハッシュ 2ECF7CFD5CC45314 が Debug == Release、plane2D の焼いたハッシュも不変。既存形状の不変は「チェックポイント 1 の .rep を新ビルドで `--replay-verify`」の 6 本 (physics / joints / fracture × Debug / Release) 全 tick 無差分 = hash-diff より強い。Q1 (BoxCast の引数順) は RaycastWorld 流を採用、Q2 (複合子の多角形) は R9 (d) として v1 未対応、Q3 (Fracture を m94 分岐に) はそのまま。[追加] (27 点 Raycast の BoxCast、kinematic はスープ、辺の両端延長、Undo 無しの点編集) は spec 4.1.8 に反映。
