# sub-18: Line2D

- 依存: sub-06
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.11

## やること
1. `Line2DComponent` (NoHash、TypeId 末尾): `pointCount / points[32] (Float2 ×32、個別登録) / width (0.1) / color / texture (AssetID、空 = 白) / loop / fill / sortingLayerId / orderInLayer`。Catalog、Create → 2D → Line。`kSimSnapshotVersion` +1。
2. 純関数 (Engine 層、selftest 対象): `BuildStroke(points, count, width, loop) → 頂点列` (各頂点で左右に width/2、継ぎ目はマイター (角度が鋭すぎる場合はベベル)、端は平ら)、`Triangulate(points, count) → index 列` (耳刈り、CW/CCW どちらでも、自己交差は未定義 = 入力側の責務)。`fill` は閉じた多角形を color で塗り、`texture` があれば UV = ローカル座標 / 1 単位。
3. `Sprite2DRenderer` の収集に Line2D を足す (同じソートキー規則、同じ VB)。Scene View ギズモ: 点列 (白) と、選択中の点ドラッグ・追加・削除 (sub-08 の流儀。コンポーネントのフィールドに直接書くので `UndoStack::Record` が使える)。
4. selftest `Line2DSelfTest`: 5 点の頂点数、loop の継ぎ目、凸 / 凹多角形の三角形数 (n − 2)、`pointCount` 0 / 1 / 2 の境界 (何も描かない / 描かない / 1 本)。
5. 確認スクショ: 5 点・幅 0.2 の折れ線 + 塗りの凹五角形。

## やらないこと (このサブでは)
- 丸いキャップ / ジョイン、破線、テクスチャの繰り返しモード、グラデーション

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- 新規 `src\Engine\Engine\Rendering\Sprite2D\Line2DGeometry.{h,cpp}`、`Line2DSelfTest.{h,cpp}`
- `Sprite2DRenderer.{h,cpp}`、`SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 32) スクショ `plans\m94-2d\screenshots\line2d.png`、端が切れていない (端点の画素値)。selftest PASS。
2. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `Line2DSelfTest` を足して PASS、snapshot +1 のみ。重い 3 本は sub-20。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot`
チェックポイントではない (重い 3 本は sub-20)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-18 (round 1)
実装:
  - src\Engine\Core\Ecs\Components.h / Components.cpp: Line2DComponent (TypeId 88、NoHash。pointCount 2 / points[32] を point0..31 で個別登録 / width 0.1 / color / texture / loop / fill / sortingLayerId / orderInLayer、既定は (-1,0)-(1,0) の 2 点)
  - src\Engine\Engine\Rendering\Sprite2D\Line2DGeometry.{h,cpp} (新規): line2d::BuildStroke (マイター、半幅 x 2 を超える鋭角はベベル、端は平ら、重複点は 1 つに、loop は 3 点以上、u = 長さの割合 0..1 / v = 左 0 右 1)、line2d::Triangulate (既存 TriangulatePolygon2D を呼ぶだけ = 重複実装なし)
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}: CollectLines / EmitLine。Item に isLine / line、ソートキーの末尾に isLine を足した (同じエンティティの Sprite2D との順を決定的に)。塗り (UV = ローカル / 1 単位) → 帯の順に同じバッチへ。texture が空 = TextureLibrary::White()、未ロードは読み込みを頼んで 1 フレーム描かない、パスが引けないものはマゼンタ
  - src\Editor\Windows\Scene\SceneViewWindow.{h,cpp}: DrawLine2DGizmos (全 Line2D の点列を白で)、HandleLine2DEdit (選択中の Line2D の点ハンドルと番号、クリックで選択 + ドラッグ移動 (BeginRecord / CaptureBefore / CaptureAfter / EndRecord で 1 ドラッグ 1 Undo)、点を選んでいる間の辺クリックで追加 (undo.Record)、Delete で削除 (undo.Record、2 点までしか減らさない)、Esc で解除、Ctrl で snap。点を選んでいる間だけ true を返して変形ギズモを隠す)
  - src\Editor\Widgets\CreateMenu.cpp (Create → 2D → Line)、EditorComponentCatalog.cpp (Line2D、アイコン BEZIER_CURVE)、InspectorWindow.cpp (Line2D の sortingLayerId も Sprite2D と同じ名前コンボ)
  - src\Engine\Core\Localization\LocalizationTable.inl: Create_Line2D / SceneView_EditingLine2D (日英)
  - src\Engine\Engine\Replay\SimSnapshot.h: v60 → v61。AcousticAudioSelfTest.cpp の焼いた版番号を 61 に追随
  - src\Engine\Engine\Rendering\Sprite2D\Line2DSelfTest.{h,cpp} (新規) と EditorMain.cpp (m94 suite と通常の全体 suite の両方に RunLine2DSelfTest)
仕様との差分:
  - [追加] Line2D を lit にする規則: spec に無かったので Tilemap と同じ「ライトのあるシーンでは常に lit (Light2D が 0 個なら従来経路 = unlit)」にした。法線 RT が有効なシーンでは平らな法線で載せる (Tilemap と同じ)。Line2D 側に lit の欄は足していない
  - [追加] 帯の UV: spec は fill の UV (ローカル座標 / 1 単位) だけを決めていたので、帯は u = 折れ線全体の長さに対する 0..1 (繰り返しモードは対象外でサンプラは clamp のため) / v = 左 0 .. 右 1 にした。fill の UV は spec どおり (x, y) だが v だけ符号を反転した (-y。テクスチャの上 = +y = Sprite2D と同じ向き)。逆にしたいなら EmitLine の 1 行
  - [追加] 既定値: pointCount 2 / points (-1,0) (1,0) (Add Component / Create で何も見えない状態を避ける)。spec の「pointCount 0 / 1 / 2」の境界は描画側 (BuildStroke / Collect) で検査した
  - [追加] 鋭角の判定 kMiterLimit = 2 (マイター長が半幅の 2 倍まで = 曲がり角 120 度まで)。超えるとベベルで、外側の隙間に三角形 1 枚 (内側は前後の四角形が重なるので半透明だと二重に乗る = 既知の限界)
  - [追加] 点の編集の入り方: 点ハンドルをクリックして選ぶまでは従来の変形ギズモ (エンティティの移動) のまま。辺クリックの追加は「点を選んでいる間」だけ (選んでいないときに辺を押したら他のエンティティの選択に譲るため)。削除は 3 点以上のときだけ。HandleShape2DEdit と重なる 2 本の lambda (toScreen / mouseLocal) は触らずコピーした (既存の検証済みコードを今回は動かさない)
  - [追加] 帯の頂点は loop で最後と最初が同位置でも 1 つにまとめ、2 点以下の loop は開いた線として扱う
  - [未実装] Scene View のクリック選択 (ピッキング) に Line2D は載せていない (Sprite2D も 2D モードのギズモのみ)。Hierarchy から選ぶか点ハンドルで選ぶ
検証:
  - Debug / Release `/p:MyeWarnAsError=true` → 0 警告 (Release は SelfTestUnused.kernel.hlsl の C4100 既知の癖で初回落ち、非 WX を挟んでから WX 2 回目で exit 0)
  - `MYE_SELFTEST_ONLY=m94` Debug / Release → どちらも exit 0、PASS 2429 / FAIL 0。Line2D: 帯 (5 点 = 24 頂点 / 端が平ら / 一直線 / 直角のマイター (1.1,-0.1) と (0.9,0.1) / 鋭角 = ベベル 15 頂点 / 折り返し / loop の継ぎ目が同じ点 / loop の u が 0..1 / 全三角形が反時計回り / 0・1・2 点 / 幅 0 / 重複点)、Triangulate (凸・凹 5 角形 = 3 枚で面積一致、CW も、3 点、2 点以下)、登録 (TypeId 88、NoHash、point0..31 のオフセット)、Collect (既定 6 頂点 / 白 / unlit、0・1 点は空、塗り 9 頂点、塗り + 帯が 1 バッチ、ワールド行列、Sprite2D との並び、Light2D で lit)
  - 受け入れ 1: `plans\m94-2d\screenshots\line2d.png` (Release Runtime、5 点・幅 0.2 の折れ線 + 塗りの凹五角形 + 白の外周)。端点の画素値は `line2d_log.txt`: 始点・終点の 0.05 内側 = (255,128,25) / 0.05 外側 = 背景 (2,2,3) = 端は切れておらず、1 点目より外にも出ない。切れ込み (4,2.5) は塗られず、塗りの内側 (4,1.0) は緑
  - ライトのあるシーン: Global (0.4,0.4,0.5) + Spot の下の線 = (102,51,13) = 橙 x Global (lit)
  - Line2D の無いシーンが従来経路とビット一致: 基点 = HEAD f694013 を Release ビルドした `cache\m94\s15\base_head` の Runtime と、今の Release Runtime で temp 2D シーン 2 本 (素のスプライト 4 体 / Light2D Global + Spot + lit スプライト 4 体) を Forward / --deferred とも撮り、Editor.exe --img-diff --tol 0 → 4 組とも maxDiff=0 diffPixels=0
  - `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり、M94 由来の増減なし)
  - 未実行: 全体 selftest / replay_verify / shot_verify / build_managed (sub-20)。Scene View の点ハンドル (選択・ドラッグ・辺クリックで追加・Delete・Esc) と Create → 2D → Line の実クリックは未確認 (自動操作の手段が無い。コードの読み合わせとビルドまで)。Editor.exe --select で Hierarchy / Inspector に Line2D (アイコン・point0.. の欄) が出ることだけは撮って確認した
  - ABI 変更なし、snapshot 60 → 61 のみ。TypeId 88。hash 対象フィールドの append は無い (焼いたハッシュの再採取は不要)
自己採点 (1-5):
  仕様適合: 4 — やること 1〜5 と受け入れ 1・2 を満たした。lit / 帯の UV / 点編集の入り方は spec が決めていない所を最小解釈で埋めて差分に書いた。ピッキングは未実装
  正しさ: 4 — 純関数は境界・向き・継ぎ目まで selftest、描画は画素値と基点との --tol 0 で確認。Scene View の編集操作は実クリック未確認、透視カメラ / 回転・スケール付きの親での見た目は未確認
  コード品質: 3 — HandleLine2DEdit が HandleShape2DEdit と同じ 2 本の lambda を重複して持つ (既存コードを動かさない選択)。共通化は sub-19 (SpriteShape の点編集) でまとめて行う余地
  テスト: 4 — 純関数と Collect は selftest、描画は画素照合。GUI の編集は未テスト
不安・質問:
  - Line2D を lit にしたこと (Tilemap と同じ) で良いか。夜のロープなどは光で暗くなる。unlit が要るなら Line2D に lit の欄を足す (hash 外・NoHash なので snapshot は同じ v61 のまま足せる)
  - 点編集は「点を選ぶまでは変形ギズモのまま」にした。常に点ハンドルを優先する (Line2D を選ぶと変形ギズモを出さない) 方が Unity の LineRenderer / SpriteShape に近いが、エンティティを動かす手段が Inspector だけになるので避けた
触ったファイル:
  - src\Editor\App\EditorMain.cpp、src\Editor\Widgets\CreateMenu.cpp、src\Editor\Widgets\EditorComponentCatalog.cpp
  - src\Editor\Windows\Scene\InspectorWindow.cpp、SceneViewWindow.h、SceneViewWindow.cpp
  - src\Engine\Core\Ecs\Components.h、Components.cpp、src\Engine\Core\Localization\LocalizationTable.inl
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp、src\Engine\Engine\Replay\SimSnapshot.h
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h、Sprite2DRenderer.cpp
  - src\Engine\Engine\Rendering\Sprite2D\Line2DGeometry.h / .cpp、Line2DSelfTest.h / .cpp (新規)
  - plans\m94-2d\screenshots\line2d.png、line2d_log.txt (新規)、plans\m94-2d\sub-18.md (この節)
  - 新規 .meta は無し。build\*.vcxproj / filters は gen_project_files の生成物 (一覧外)
申し送り:
  - sub-19 (SpriteShape2D): 点列の三角形分割は `line2d::Triangulate` (= TriangulatePolygon2D) を使える。Scene View の点編集は `HandleLine2DEdit` が「コンポーネントのフィールドへ直接書く + UndoStack」版の手本 (HandleShape2DEdit はアセットへ書く版)。2 本の lambda (toScreen / mouseLocal) が両方にあるので、SpriteShape の点編集を足すときに共通の関数へ寄せる余地
  - sub-23: ロープは Line2D (loop なし、幅 0.1 前後)。ライトのあるシーン (2d_lights) では Line2D も lit になる。`TextureLibrary::White()` を使うので device が無い selftest で Collect する場合は Init が要る
  - sub-24 engine_spec / 限界欄: Line2D は最大 32 点、帯の継ぎ目はマイター (曲がり 120 度超はベベルで、半透明の帯は内側で二重に乗る)、端は平ら (丸いキャップ / 破線 / テクスチャの繰り返しは無し)、帯の UV は u = 全長に対する 0..1 / v = 左右、塗りの UV は ローカル座標 / 1 単位 (v は上向き)、サンプラは clamp、ライトのあるシーンでは常に lit、Scene View のクリック選択は無し (点ハンドルで選ぶ)、点編集の実クリックはユーザー目視 (checklists)
  - `cache\m94\s15\base_head` は基点 HEAD f694013 (3e54872 の sub-17 込み) を Release ビルドした状態に更新した (git 管理外)。img-diff は Runtime.exe だとハングするので Editor.exe で

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。スクショ `line2d.png` と端点の画素ログで端が切れず外にも出ないこと、凹五角形の切れ込みが塗られないことを確認。selftest (帯・マイター・ベベル・loop・境界・Triangulate・Collect) 両構成 PASS、Line2D 無しのシーンが基点と 4 組 tol 0。lit の常時化・帯の UV・既定値・点編集の入り方は採用 (spec 4.1.11)。重複 lambda の共通化は sub-19 5b へ。lit の欄を足すかは `[ユーザーに聞ける]`。
