# sub-10: タイルパレット / タイルマップ資産、`Tilemap`、チャンク描画

- 依存: sub-04
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #7, #28、4.1.7、4.2.1、4.2.3

## やること
1. `.tilepalette.json` / `TilePaletteLibrary` と `.tilemap.json` / `TilemapLibrary` (spec 4.1.7 の形式。GUID キー、`PhysMatLibrary` の流儀、`AssetType` append ×2、`ClassifyPath` 最長サフィックス、`RegisterAssetLibraries`、ReloadHub (tilemap は「全チャンク dirty」を通知する口を持つ)、Project 窓 Create ×2、`kTypeFilters` / `TileLabel` / `FileTypeIcon`、Inspector の簡易表示 (サイズ・レイヤー数・チャンク数))。`rule` / `anim` のフィールドも**今読み書きする** (使うのは sub-13)。
2. `TilemapComponent` (hash 対象、TypeId 末尾): `tilemap / layer / mask / friction / physMaterial / showCollision (kFieldNoHash)`。登録表、Catalog (2D)、Create → 2D → Tilemap。`kSimSnapshotVersion` +1。
3. Engine 層 `src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}`: アセットとエンティティから「実効セル」を返す `GetCell(e, layer, cx, cy)` (override 表は sub-11 で足す。今は無条件にアセットの値)、チャンク列挙、座標変換 `WorldToCell / CellToWorld` (原点 = エンティティのワールド位置、スケール無視)。
4. 描画: `Sprite2DRenderer` の収集に Tilemap を足す。チャンクごとに頂点列 (セルごとに 1 クアッド、`tileId → palette → sprite → SpriteDef`、flip / 回転ビット、`destroyed` 等は無し) をキャッシュし、dirty (アセットのリロード、初回、sub-11 の override 変更、sub-13 のアニメコマ変化) のときだけ焼き直す。レイヤーの `sortingLayerId / orderInLayer` でキーを作り、同じキーの Sprite2D と並ぶ (タイブレークは entity.index → レイヤー index → チャンク)。カメラの正射影範囲外のチャンクは出さない (AABB で切る。透視のときは切らない)。
5. Scene View ギズモ: Tilemap のグリッド範囲 (使用チャンクの AABB、灰) と、2D モードでセルの格子 (薄い灰、カメラ範囲内のみ)。
6. selftest `TilemapSelfTest` (新規): (a) 2 レイヤー 3 チャンクの往復で同一 JSON、空チャンクは書かれない (b) `GetCell` / 座標変換 (c) パレットの id 割り当て。
7. 確認スクショ: 2 レイヤー (地面 / 前景) + Sprite2D 1 枚 (地面と前景の間の order) → 層順で写る、flip / 回転の 4 変種が見える。

## やらないこと (このサブでは)
- 衝突・override・破壊 (sub-11)、パレット窓 (sub-12)、ルール / アニメの評価と UI (sub-13)

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Tilemap\TilemapAssets.{h,cpp}` (2 ライブラリ)、`TilemapSystem.{h,cpp}`、`TilemapSelfTest.{h,cpp}`
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp` (収集にタイル経路)
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub.{h,cpp}`、`AssetBrowserWindow.cpp`、`AssetOps.{h,cpp}`、`EditorWidgets.cpp`、`InspectorWindow.cpp`
- `SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 20) スクショ `plans\m94-2d\screenshots\tilemap_basic.png` で層順と flip / 回転が期待どおり (画素座標のログ)。selftest (a)(b)(c) PASS。
2. 100×50×3 層のタイルを temp で作り、`--frames 300` の平均フレーム時間を Release / WARP でログ (spec 4.4 の 16 ms 予算の内訳。超えても今は REWORK にしないが記録する)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `TilemapSelfTest` を足して PASS、snapshot +1 のみ。
4. **チェックポイント (sub-06〜10 の分、spec 4-5)**: `Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`tools\replay_verify.bat` 全ジョブ PASS、`tools\shot_verify.bat` + 既存 golden 30 枚 `--tol 0` で maxDiff=0。割れたら sub-06〜10 のどれが原因かを特定して SELF_EVAL に書く。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot` と `--frames 300`
**チェックポイント (このサブで回す)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat`、`tools\shot_verify.bat` + 既存 30 枚 `--tol 0`。replay_verify は全文ログを `cache\m94\replay_verify_cp2.log` に残し、ジョブごとの PASS 行を SELF_EVAL に写す (sub-05 VERDICT should 3)。`demo_terrain_deferred` は golden でなく基点 `25fb355` の絵と `--tol 0` で比べる (spec 5. 条件 4 の例外)

## 実装メモ (coder が追記)

## フィードバック履歴
