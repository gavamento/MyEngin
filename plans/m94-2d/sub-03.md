# sub-03: スプライト資産 (`.meta` の sprites / filter / PPU、SpriteLibrary) とソート層

- 依存: なし
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #10, #11, #12、4.1.2、4.2.1

## やること
1. `TextureImportSettings` (`ImportMetaResolver.h:11-15`) に `filter` (int32、0 auto / 1 linear / 2 point)、`pixelsPerUnit` (int32、100)、`sprites` (可変長配列 — ImportSettings に初めて配列が入るので `std::vector<SpriteSlice>` を持たせ、`.meta` の読み書き (`AssetDatabase::ReadMeta / WriteMeta`) で `"sprites"` を**空なら書かない**。Resolve の関数ポインタ注入 (`ImportMetaThunk`) が配列を運べる形にする)。`SpriteSlice { uint32 id; char name[64]; int32 x, y, w, h; float pivotX, pivotY; int32 borderL, B, R, T; }`。
2. `SpriteLibrary` (`src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.{h,cpp}`): `Register(texPath)` が .meta を読み、`AssetID = HashStr(assetkey::SubAssetKeyPrefix(texPath) + "#sprite" + std::to_string(id))` で `SpriteDef { AssetID texture; int rect[4]; float pivot[2]; int border[4]; int ppu; int filter; char name[64]; }` を登録。テクスチャ自身の AssetID も「全面」として `Get` で解決する (遅延: `Get(texAssetId)` が未登録ならテクスチャの .meta を引いて全面を返す)。`Enumerate` (Inspector ピッカー用、"tex#name" の表示名)、`FindByName(texPath, name)`。未解決は `nullptr` を返し、呼び手 (sub-04) がマゼンタに落とす。注入は `physmat::Install` の流儀 (`SimInit.cpp:69-70`、`EngineLoop.cpp:198`、`HeadlessSim.cpp:116`)。起動走査は `RegisterAssetLibraries` (`DemoContent.cpp:3186-3269`) でテクスチャ拡張子の分岐に足す。ReloadHub の texture 再読込 (`ReloadTexture`) の後に `SpriteLibrary::Register` を呼ぶ。
3. ソート層: `src\Engine\Engine\Scene\SortingLayers.{h,cpp}` (`TagNames` / `NavAgentTypes` の流儀。`Get()`、`Load(root, force)`、`Save(root)`、`DiffersFromDisk()`、`Add(name) → id (0..31 の未使用最小)`、`Remove(id)` (id 0 は不可)、`Rename`、`Move(from, to)`、`OrderOf(id) → 配列順 (無ければ 0 = Default)`、`Count`)。`project_settings.json` の `"sortingLayers": [{"id":0,"name":"Default"}]`。ファイルに無ければ Default 1 層。`UpdateProjectSettingsFile` で他キー保持。
4. Project Settings 窓に「Sorting Layers」の CollapsingHeader (追加 / 削除 / 改名 / ↑↓)。未保存判定 (`:702-705`) に `DiffersFromDisk` を足す。文字列は `Tr()`。
5. selftest `Sprite2DAssetSelfTest` (新規、`--selftest` 末尾に append): (a) `.meta` の往復で同一 JSON、`sprites` 無しのテクスチャの .meta が従来とバイト同一 (b) 改名後も同じ AssetID、id が最大 + 1 (c) SortingLayers の id 再利用と上限 32 (d) `OrderOf` が配列順。

## やらないこと (このサブでは)
- スプライトエディタ窓 (sub-05)、Sprite2D 描画 (sub-04)

## 触る場所 (planner の見立て)
- `src\Engine\Core\Asset\ImportMetaResolver.{h,cpp}`、`src\Engine\Engine\Asset\AssetDatabase.{h,cpp}` (:201-295 ReadMeta / WriteMeta / EnsureMeta、:387-418 Thunk / InstallAsKeyResolver)、`src\Engine\Core\Asset\AssetKeyResolver.h:29` (SubAssetKeyPrefix)
- 新規 `src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.{h,cpp}`、`src\Engine\Engine\Scene\SortingLayers.{h,cpp}`、`src\Engine\Engine\Rendering\Sprite2D\Sprite2DAssetSelfTest.{h,cpp}`
- `src\Engine\Engine\Demo\DemoContent.cpp:3186-3269`、`src\Engine\Engine\HotReload\ReloadHub.cpp` (texture の後処理)、`SimInit.cpp` / `EngineLoop.cpp` / `HeadlessSim.cpp` (注入)
- `src\Editor\Windows\Project\ProjectSettingsWindow.cpp` (:219-235, :350-404, :702-705)、`src\Engine\Engine\App\ProjectSettingsFile.h`
- `src\Editor\App\EditorMain.cpp` (selftest 登録)、`build\*.vcxproj` (`gen_project_files.ps1`)、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 11) selftest (a)(b) PASS。既存 .meta の 1 つ (`assets/models/player_fp_v01/previews/first_person.png.meta`) を ReadMeta → WriteMeta してバイト同一 (ログ)。
2. (spec 12) selftest (c)(d) PASS。Project Settings のエディタスクショ `plans\m94-2d\screenshots\sorting_layers.png`。
3. (spec 1-3) 0 警告、check_rules 0 (規則 10 を含む)、`MYE_SELFTEST_ONLY=m94` に `Sprite2DAssetSelfTest` を足して PASS、版 bump なし (sim に触らない)。重い 3 本は sub-05。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
チェックポイントではない (重い 3 本は sub-05)。

## 実装メモ (coder が追記)

## フィードバック履歴
