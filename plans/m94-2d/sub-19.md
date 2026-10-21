# sub-19: SpriteShape2D (描画・衝突・ハンドル)

- 依存: sub-08, sub-18
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.11、4.2.3

## やること
1. `.spriteshape.json` / `SpriteShapeLibrary` (プロファイル: `fill`、`edges[] {angleFrom, angleTo, sprite}`、`height`。GUID キー、ReloadHub、Create メニュー、Inspector 編集)。
2. `SpriteShape2DComponent` (**hash 対象**、TypeId 末尾): `profile / pointCount / points[24] / closed / smooth / collider / oneWay / sortingLayerId / orderInLayer`。Catalog、Create → 2D → Sprite Shape。`kSimSnapshotVersion` +1。
3. 純関数: `EvaluateSpline(points, count, closed, smooth) → 折れ線` (smooth = Catmull-Rom、1 区間 8 分割、端は端点を複製)、`BuildEdgeQuads(polyline, profile) → 頂点列` (各区間の角度で `edges[]` を選び、`height` の帯を区間に沿って敷く。UV の u は区間長 / スプライト幅で繰り返し)、fill は sub-18 の `Triangulate` を折れ線に掛ける (UV = ワールド座標 / ppu)。
4. 衝突: `collider` のとき、折れ線を `StaticShapeSource` (sub-08) の辺チェーン (薄い箔列、`oneWay` 継承、layer / mask は 0 / 全部 — 必要なら Collider と同じフィールドを足して差分に書く) として供給。sim 入力なので点が変われば箱も変わる (キャッシュは点列のハッシュで無効化)。
5b. (sub-18 の申し送り、コード品質) `HandleShape2DEdit` (sub-08) と `HandleLine2DEdit` (sub-18) が持つ重複した 2 本の lambda (toScreen / mouseLocal) と点ハンドルの描画・ヒット判定を、SceneViewWindow の小さな共通関数 (例 `PointHandleEdit`) に寄せ、SpriteShape の点編集もそれを使う。挙動は変えない (2 つの既存編集のスクショか、コードの読み合わせで同値を示す)。
5. Scene View ハンドル: 点のドラッグ・追加・削除 (`UndoStack::Record`)。ギズモ: 折れ線 (白)、`collider` なら緑。
6. selftest: スプラインの点数、角度範囲の選択、球が 8 点の丘に沿って転がり落ちる (plane2D、Debug / Release ハッシュ焼き込み)、2 ワールド並走の一致。
7. 確認スクショ: 丘 (8 点 smooth、縁 3 種 + fill)。

## やらないこと (このサブでは)
- 角スプライト (corner sprites)、ベジェ接線のハンドル、開いた形状の fill

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Rendering\Sprite2D\SpriteShape.{h,cpp}` (ライブラリ + 純関数)、`SpriteShapeSelfTest.{h,cpp}`
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `Sprite2DRenderer.{h,cpp}`、`src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (供給元の登録、sub-11 と同じ口)、`SceneViewWindow.cpp`
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub`、`AssetBrowserWindow.cpp`、`AssetOps`、`InspectorWindow.cpp`
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 33) スクショ `plans\m94-2d\screenshots\spriteshape.png`、ハンドルのエディタスクショ、selftest PASS (Debug / Release ハッシュ同値)。
2. SpriteShape2D の無いシーンの hash-diff が無差分 (`--physics-demo` 1 本、opt-in 型)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `SpriteShapeSelfTest` を足して両構成 PASS、snapshot +1 のみ。重い 3 本は sub-20。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --physics-demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-20)。

## 実装メモ (coder が追記)
SELF_EVAL: sub-19 (round 1)
実装:
  - src\Engine\Core\Ecs\Components.h / Components.cpp: SpriteShape2DComponent (TypeId 89、**hash 対象**。profile / pointCount 2 / points[24] を point0..23 で個別登録 / closed / smooth / collider / oneWay / sortingLayerId / orderInLayer。既定は (-2,0)-(2,0) の 2 点)
  - src\Engine\Engine\Rendering\Sprite2D\SpriteShape.{h,cpp} (新規): SpriteShapeProfile / SpriteShapeLibrary (.spriteshape.json、GUID キー、Enumerate は名前昇順、ToJson / FromJson / Sanitize)、spriteshape::Install / Resolve、純関数 EvaluateSpline (Catmull-Rom、1 区間 8 分割、端は端点複製、閉は先頭を重ねない)、SelectEdge、OutwardAngleDeg、BuildEdgeQuads、BuildFill (耳刈り + 格子で切って頂点で繰り返す)
  - src\Engine\Engine\Physics\Collider\SpriteShapeCollider.{h,cpp} (新規): StaticShapeSource 実装 SpriteShapeColliders。collider が立った形状の折れ線を線分ごとの薄い箱 (半厚 0.1、z 厚 1) にして供給。点列の内容ハッシュでキャッシュ、持ち主 index 昇順、CollectWithin で範囲に届かない形状を除外 (3 軸)、AppendEntityShapes
  - src\Engine\Engine\Physics\Rigid\Shapes.{h,cpp}: MakeSegmentBox2D を ExpandEdge2D から抜き出して公開 (ExpandEdge2D は同じ式で呼ぶだけ = 挙動不変)。PhysicsSystem.{h,cpp}: ComposeEntityWorldScale (親フレーム合成のワールドスケール、符号つき)
  - EngineLoop.cpp / HeadlessSim.cpp / SimInit.{h,cpp}: SpriteShapeLibrary と SpriteShapeColliders を所有し、AddStaticShapeSource (タイルの隣)、SimLibraries に spriteShapes を末尾 append (spriteshape::Install / 解除)
  - AssetDatabase.{h,cpp} (AssetType::SpriteShape を末尾 append、ClassifyPath / TypeName / ParseTypeName)、DemoContent.cpp (起動走査)、ReloadHub.{h,cpp} (rank 6、ReloadSpriteShape)、AssetOps (CreateSpriteShapeAsset、既定は上面 + 左右の壁の 3 縁、複合サフィックス)、AssetBrowserWindow (Create / フィルタ / ラベル)、EditorWidgets (アイコン)、LocalizationTable.inl (日英)
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}: CollectShapes / EmitShape。折れ線をワールド XY に移してから塗り (profile.fill、格子 = スプライト 1 枚の寸法) → 縁 (profile.edges の添字順、同じ縁は 1 バッチ) を同じ Sprite2D パスへ。ソートキーの末尾に isShape。ライトのあるシーンでは Line2D / Tilemap と同じく常に lit (法線 RT は平らな法線)
  - src\Editor\Windows\Scene\SceneViewWindow.{h,cpp}: **やること 5b**: 点列ハンドルの共通部 (PointHandleFrame / PointHandleScan、PointToScreen / MouseToLocal / ScanPointHandles / PickSegmentUnderMouse) を作り、HandleShape2DEdit の 2 本の lambda・ハンドル描画・辺ピックをそれへ置き換え。さらに「コンポーネントへ直接書く点リスト (Undo 付き)」の状態機械を EditPointList にまとめ、HandleLine2DEdit と新設の HandleSpriteShape2DEdit が使う (Line2D の状態 3 つは PointListEditState に)。DrawSpriteShape2DGizmos (輪郭は白、collider が立っていれば緑)
  - src\Editor\Windows\Scene\SpriteShapeEditor.{h,cpp} (新規) と InspectorWindow.{h,cpp}: .spriteshape.json の Inspector (塗りのスプライト、縁の厚み、縁の一覧 = 角度範囲 + スプライトのピッカー + 並べ替え / 削除 / 追加、保存 / 戻す)、profile の AssetRef ピッカーと .spriteshape.json の D&D、sortingLayerId の名前コンボ、プロファイル未設定の警告
  - CreateMenu.cpp (Create → 2D → Sprite Shape = 6 点の閉じた滑らかな小山)、EditorComponentCatalog.cpp (SpriteShape2D)
  - src\Engine\Engine\Replay\SimSnapshot.h: v61 → v62。AcousticAudioSelfTest.cpp の焼いた版番号を 62 に追随
  - src\Engine\Engine\Rendering\Sprite2D\SpriteShapeSelfTest.{h,cpp} (新規、100 項目) と EditorMain.cpp (m94 suite と通常の全体 suite の両方)。AssetDatabase / ReloadHub / AssetOps の selftest に .spriteshape.json の行を追加
仕様との差分:
  - [追加] 縁の角度の定義: spec は「各区間の角度」とだけ。区間の外向き法線の角度 (+Y が 0、反時計回りが正 = 左の壁が +90、右の壁が -90) とした。外側 = 進行方向の左。閉じた輪郭は内部で時計回りに揃える (反時計回りで与えても同じ)。開いた線は「左から右へ描くと上が表」(右から左へ描くと下向き = 180 度でどの縁にも当たらず何も出ない。Unity の Flip 相当は作っていない)
  - [追加] 縁の帯: 輪郭の線を中心に厚み profile.height (ワールド単位。エンティティのスケールには掛からない = ワールド空間で組むため)、スプライト 1 枚の幅 = rect.w / ppu で u を繰り返し (端数は UV を切る)、同じ縁が続く区間は位相を引き継ぎ継ぎ目はマイター (長さの上限は半厚の 2 倍)。縁が変わる / 縁なしの区間で位相は 0 に戻る。1 形状のクアッド上限 682 (超えたら警告 1 回)
  - [追加] 塗りの「UV = ワールド座標 / ppu」: wrap サンプラが無い (spec 2. #12) ので、三角形を座標系の原点基準の格子 (1 枚 = スプライトの寸法) で切って頂点で繰り返した。profile.fill は SpriteLibrary のスプライト ID (テクスチャ全面かスライス)。1 三角形あたり 4096 セルを超える細かさは描かず警告
  - [追加] 衝突: layer / mask は既定 (0 / 全部) のままで ColliderComponent 系のフィールドは足していない (spec の「必要なら」を使わなかった)。箱の半厚 0.1 / z 厚 1 は SpriteShape.h の定数。collider は profile に依らず点列だけで有効。点が変わっても眠っている剛体は起こさない (通常の静的 Collider を動かしたときと同じ。起こすと snapshot 復元で眠りの状態が変わる)。供給元は PhysicsSystem に埋め込まず StaticShapeSource の新実装 (sub-08 / sub-11 と同じ口)。空間クエリ (physicsquery) も同じ口で形状を見る
  - [追加] 3D の姿勢: 描画の頂点 z はエンティティ原点の z で一定 (xy 平面の形状が前提)。衝突はワールド姿勢と符号つきスケール (親を含む) を毎 tick 合成して箱を作る
  - [追加] lit の規則: spec に無かったので Line2D / Tilemap と同じ (ライトのあるシーンでは常に lit、Light2D が 0 個なら従来経路)。SpriteShape2D に lit の欄は足していない (足すと hash 対象のフィールド append = 焼き値の再採取が要る)
  - [追加] 5b を spec より広くした: lambda の共通化 (spec どおり) に加え、Line2D と SpriteShape が同じ点リスト編集の状態機械を持つ重複を避けるため EditPointList へまとめた (HandleLine2DEdit の挙動は読み合わせで同値: 状態 3 つ・入力条件・Undo 名・Delete の下限 2・Esc を移しただけ)
  - [追加] 既定: Create → 2D → Sprite Shape は 6 点の閉じた滑らかな小山 (コンポーネント単体の既定は 2 点の線)。新規 .spriteshape.json の既定は上面 (-45..45) / 左の壁 (45..135) / 右の壁 (-135..-45) の 3 縁でスプライトは空 (空の縁は描かれない)
  - [未実装] 角スプライト、ベジェ接線ハンドル、開いた形状の塗り (spec のやらないこと)。Scene View のクリック選択 (ピッキング) には載せていない (Line2D と同じ)
検証:
  - Debug / Release /p:MyeWarnAsError=true → 0 警告 (どちらも SelfTestUnused.kernel.hlsl の C4100 既知の癖で 1 回目だけ落ち、2 回目で exit 0)。pwsh tools\gen_project_files.ps1 → build\Editor / Engine の vcxproj / filters に新規ファイル分の差分
  - MYE_SELFTEST_ONLY=m94 Debug / Release → どちらも exit 0、PASS 2529 / FAIL 0 (sub-18 の 2429 + SpriteShape 100)。PhysicsSelfTest 既存節 (plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 / CC2D C04B7FFA3E31E5D0) と Tilemap の焼き値 326BA3CD14F4BBB8 は PASS = 不動
  - SpriteShape2D: スプライン (点数 57 / 64 / 9、制御点を厳密に通る、共線、2 回評価で同ビット)、縁の選択 (境界の先勝ち、180 度またぎ、NaN)、BuildEdgeQuads (中心配置、端数の UV 切り、位相の引き継ぎ 6 枚、マイター共有、閉じた四角形の向き正規化、上限、右から左は縁なし)、BuildFill (面積保存、UV、打ち切り)、JSON / ライブラリ、TypeId 89・hash 対象・point0..23 のオフセット、点を動かすと WorldHash が変わる、Collect (バッチ / 頂点数 / ソートキー / lit / 範囲外の pointCount)、辺の箱 (56 / 3 / 2 箱、index 昇順、oneWay の継承、点変更でキャッシュ無効、位置・スケール・Z 回転 90 度・負スケール、CollectWithin の 3 軸)
  - 丘シナリオ (8 点 smooth、球が (-3.5, 4.5) から転がり落ちる、600 tick): 終点 (2.054, 0.523)、表面からの最小クリアランス +0.349 m (沈まない)、plane2D の z / vz が厳密に 0、2 ワールド並走で毎 tick の HashWorld 一致、絞り込みあり / なしの最終ハッシュ一致、焼いた最終ハッシュ AE6DC1DD4BAEC0E6 が Debug == Release。一方通行 (下から投げた球が抜けて上に乗る / 上から落とした球が乗る = どちらも y 0.350)
  - 既存の焼き値: 全体 selftest (Release、チェックポイント外の参考) でも FAIL 行なし。NavAgent の庭 8581BEE4A676337C は不動。新型の追加なので既存型への hash 対象フィールド append は無く、再採取は不要
  - 受け入れ 1: plans\m94-2d\screenshots\spriteshape.png (Release Runtime、960x540、一時シーン: 8 点の閉じた smooth の丘、縁 3 種 + 塗り、tonemapMode 0)。上面は草、左の壁は明るい茶、右の壁は暗い茶と灰の縁で、外側が上 / 外を向く。塗りは 1 枚 1 単位の格子で繰り返し。下向きの辺は縁なし。plans\m94-2d\screenshots\spriteshape_handles.png (Release Editor、--select Hill: 白の点ハンドル 0..7 と番号、Inspector の SpriteShape2D = profile 解決・point0..)。点のドラッグそのものの実クリックは未確認
  - 受け入れ 2: SpriteShape2D の無いシーンが基点 = 現 HEAD 26aa0fa (git archive を Release ビルドした cache\m94\s15\base_head に更新) の Runtime と --img-diff --tol 0 → temp 2D シーン 2 本 (素のスプライト 4 体 / Light2D Global + Spot + lit スプライト 4 体) を Forward / --deferred とも 4 組 maxDiff=0 diffPixels=0。Runtime.exe --physics-demo --replay-record ... --hash-dump を基点と新で採り --hash-diff: 差は UIInteraction.prevSurfX / prevSurfY / dragging (実マウス座標のミラー = 入力) と #total だけ。同じ基点を 2 回採った base vs base でも同じ行だけ割れる (world の行は 776 行全て一致)
  - 受け入れ 3: pwsh tools\check_rules.ps1 → 3 error / 50 warning (基準どおり、M94 由来の増減なし)。snapshot 61 → 62 のみ、ABI 変更なし
  - 未実行: replay_verify / shot_verify / build_managed (sub-20)。点のドラッグ / 辺クリック追加 / Delete / Esc の実クリックと Create → 2D → Sprite Shape の実クリックは未確認 (自動操作の手段が無い。コードの読み合わせとビルドまで)。透視カメラでの見た目、回転 / スケール付きの親の見た目は未確認
版: snapshot 61 -> 62 のみ (SpriteShape2D は World 節のカラム生バイト)。ABI / Scene kDocVersion / kCookVersion は不変。hash 対象フィールドの append は既存型に無し
自己採点 (1-5):
  仕様適合: 4 — やること 1〜7 (5b 含む) と受け入れ 1〜3 を満たした。縁の角度の定義 / 塗りの繰り返し / lit / 眠りの扱いは spec が決めていない所を最小解釈で埋めて差分に書いた。点ドラッグの実操作は未確認
  正しさ: 4 — 純関数は境界・向き・位相・面積まで selftest、衝突は 600 tick の決定論と Debug == Release のハッシュ、描画は目視 + Collect の頂点数 (画素値の解析照合はしていない)。Scene View の編集操作は実クリック未確認
  コード品質: 4 — 5b で 3 つの点編集の重複を除いた (HandleShape2DEdit は共通部品、Line2D / SpriteShape は EditPointList)。EmitShape が長め、SpriteShape.h が json.hpp を Sprite2DRenderer.h 経由で配る (Shape2DLibrary.h と同じ)
  テスト: 4 — 100 項目 + 焼き値 + 基点との tol 0。GUI の編集は未テスト
不安・質問:
  - 点が変わった tick に眠っている剛体を起こさない (通常の Collider と同じ) でよいか。Tilemap の SetCell は起こしているが、それは明示の API 経由の書き換えで、SpriteShape の点は普通のフィールド書き込みなので、起こすと snapshot 復元の決定性が崩れる (キャッシュの新旧で起きる / 起きないが変わる)
  - 開いた線は「進行方向の左が表」で、右から左へ描くと何も出ない。Unity の SpriteShape の Flip に当たる欄 (bool) を足すなら hash 対象の append になる (焼き値の再採取が要る)。v1 は点の並びを逆にする運用でよいか
  - 縁の帯の厚み (profile.height) がエンティティのスケールに追従しない (ワールド空間で組むため)。スケールを掛ける方がよければ EmitShape の 1 箇所
触ったファイル:
  - src\Editor\App\EditorMain.cpp
  - src\Editor\Asset\AssetOps.cpp / AssetOps.h / AssetOpsSelfTest.cpp
  - src\Editor\Widgets\CreateMenu.cpp / EditorComponentCatalog.cpp / EditorWidgets.cpp
  - src\Editor\Windows\Asset\AssetBrowserWindow.cpp
  - src\Editor\Windows\Scene\InspectorWindow.cpp / InspectorWindow.h / SceneViewWindow.cpp / SceneViewWindow.h
  - src\Editor\Windows\Scene\SpriteShapeEditor.h / SpriteShapeEditor.cpp (新規)
  - src\Engine\Core\Ecs\Components.h / Components.cpp、src\Engine\Core\Localization\LocalizationTable.inl
  - src\Engine\Engine\Asset\AssetDatabase.h / AssetDatabase.cpp / AssetDatabaseSelfTest.cpp
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp、src\Engine\Engine\Replay\SimSnapshot.h
  - src\Engine\Engine\Demo\DemoContent.cpp
  - src\Engine\Engine\HotReload\ReloadHub.h / ReloadHub.cpp / ReloadHubSelfTest.cpp
  - src\Engine\Engine\Loop\EngineLoop.cpp / HeadlessSim.cpp / SimInit.h / SimInit.cpp
  - src\Engine\Engine\Physics\Rigid\PhysicsSystem.h / PhysicsSystem.cpp / Shapes.h / Shapes.cpp
  - src\Engine\Engine\Physics\Collider\SpriteShapeCollider.h / SpriteShapeCollider.cpp (新規)
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h / Sprite2DRenderer.cpp
  - src\Engine\Engine\Rendering\Sprite2D\SpriteShape.h / SpriteShape.cpp / SpriteShapeSelfTest.h / SpriteShapeSelfTest.cpp (新規)
  - plans\m94-2d\screenshots\spriteshape.png / spriteshape_handles.png (新規)、plans\m94-2d\sub-19.md (この節)
  - 新規 .meta / .spriteshape.json は無し (確認用の temp 資産は assets\_tmp_m94s に作って撮影後に削除)。build\Editor.vcxproj / Engine.vcxproj と各 filters は gen_project_files の生成物 (一覧外)
申し送り:
  - sub-20 (チェックポイント 4): sub-19 は PhysicsSystem (ComposeEntityWorldScale の追加) と Shapes (MakeSegmentBox2D の抜き出し = ExpandEdge2D の挙動不変) を触った。replay_verify は全ジョブの個別 PASS 行を確認。新しい供給元 SpriteShapeColliders は SpriteShape2D の無いシーンでは Collect が空配列を返すだけ (アーキタイプの走査 1 回)
  - sub-21: Perception の LOS が同じ口を見る予定なら physicsquery::CollectStaticShapes で SpriteShape の箱も返る (供給口の Collect は SpriteShapeColliders::Collect)
  - sub-23: ロープは Line2D、丘は SpriteShape2D (閉じた smooth、profile = 縁 3 + fill)。確認用の生成スクリプト (PIL で 32x16 の縁 3 枚と 32x32 の塗り) は cache\m94\s19\make_hill_scene.py / make_gate_scenes.py (git 管理外)。デモ資産の .meta は filter = 2 (point)、ppu = スプライトの 1 単位の画素数にする (縁 32x16 / ppu 32 = 幅 1 単位、厚み height 0.5)
  - sub-24 限界欄: 縁の角度は外向き法線 (+Y = 0、反時計回りが正) / 開いた線は左から右が表 / height はワールド単位でスケールに追従しない / 頂点 z はエンティティ原点の z / 角スプライト無し / 開いた形状の塗り無し / 衝突の箱は半厚 0.1 (細すぎる形状の貫通は未検証) / 点が変わっても眠っている剛体は起きない / クアッド上限 682・塗りの頂点上限 4096 / 塗りの格子は座標系の原点基準 (エンティティを動かすと絵が流れる)
  - cache\m94\s15\base_head は現 HEAD 26aa0fa の Release ビルドに更新済み (git 管理外、日本語名の .md は展開に失敗しているがビルドには無関係)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。スクショ `spriteshape.png` (上面の草、左右の壁の縁、格子で繰り返す塗り) と `spriteshape_handles.png` を確認。selftest 100 項目、丘シナリオ (沈まない、z 厳密 0、並走一致、絞り込みあり / なし同一、焼き値 AE6DC1DD4BAEC0E6 両構成同値、一方通行)、既存の焼き値不動、基点との tol 0 (4 組)、physics-demo の hash-diff は入力ミラーだけ。5b は Line2D と SpriteShape の状態機械まで共通化 (広げた方を採用)。不安 1 (眠りを起こさない) は採用、2 (Flip 無し) は採用、3 (height がスケールに追従しない) は `[ユーザーに聞ける]`。spec 4.1.11 に反映。
