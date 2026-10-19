# sub-03: スプライト資産 (`.meta` の sprites / filter / PPU、SpriteLibrary) とソート層

- 依存: なし
- 状態: OK (commit 03521a8)
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

SELF_EVAL: sub-03 (round 1) の要旨 (全文は司会への返答)。
- `TextureImportSettings` に filter / pixelsPerUnit / `std::vector<SpriteSlice> sprites` と `NextSpriteId()`。`.meta` は既定値と空 sprites を書かない (従来とバイト一致)。
- `SpriteLibrary` (全面 = rect の w/h が 0)、`spritelib::Install` を `SimLibraries.sprites` で注入。起動走査と ReloadTexture 後に Register。
- `SortingLayers` と Project Settings の Sorting Layers 節、`Sprite2DAssetSelfTest` を `MYE_SELFTEST_ONLY=m94` と全体の末尾へ。
- 検証: Debug / Release 0 警告ビルド (C4100 の既知癖のみ)、m94 selftest 両構成 PASS、check_rules は既存 3 error のみ・新規なし、スクショ `plans\m94-2d\screenshots\sorting_layers.png`。

### round 1 SELF_EVAL (全文、司会が転記)

```
SELF_EVAL: sub-03 (round 1)
実装:
  - ImportMetaResolver.h: SpriteSlice 新設。TextureImportSettings に filter / pixelsPerUnit (既定 100) / std::vector<SpriteSlice> sprites と NextSpriteId() (= 最大 id + 1) を追加。
  - AssetDatabase.cpp: ReadMeta / WriteMeta が tex.filter / pixelsPerUnit / sprites を読み書き。既定値と空 sprites は書かない。Thunk はコピーなのでそのまま配列を運ぶ。ReadIntArray ヘルパを追加。
  - Rendering/Sprite2D/SpriteLibrary.{h,cpp} (新規): SpriteDef、Register / Get (遅延登録) / Enumerate ("tex" と "tex#name"、表示名昇順) / FindByName / Clear / SliceId。AssetID = HashStr(SubAssetKeyPrefix(texPath) + "#sprite" + id)。spritelib::Install / Library は physmat:: と同じ流儀。
  - Loop/SimInit.{h,cpp}: SimLibraries に sprites を末尾追加。EngineLoop.cpp / HeadlessSim.cpp: SpriteLibrary を所有して InstallSimLibraries に渡す。
  - Demo/DemoContent.cpp: RegisterAssetLibraries のテクスチャ拡張子の分岐で Register。HotReload/ReloadHub.cpp: ReloadTexture の ReplaceFromFile 成功後に Register。
  - Scene/SortingLayers.{h,cpp} (新規): Get / Load / Save / DiffersFromDisk / Add (0..31 の未使用最小 id、上限 32) / Remove (id 0 不可) / Rename / Move / OrderOf / IdOf / Count / At / EditAt。壊れた表は Load で補正。Save は UpdateProjectSettingsFile で他キーを保持。
  - ProjectSettingsWindow.cpp + LocalizationTable.inl: Sorting Layers の CollapsingHeader (追加 / 削除 / 改名 / Up / Down / 保存)。HasUnsavedChanges に DiffersFromDisk。Tr() 7 本 (日英)。
  - Rendering/Sprite2D/Sprite2DAssetSelfTest.{h,cpp} (新規) と EditorMain.cpp: MYE_SELFTEST_ONLY=m94 と全体 --selftest の末尾へ追加。
  - build/Engine.vcxproj と .filters: gen_project_files.ps1 の再生成。
仕様との差分:
  - [追加] filter / pixelsPerUnit も「既定値のときは .meta に書かない」(既存 .meta のバイト同一のため)。実 .meta (first_person.png.meta) の Read → Write バイト同一は selftest で確認。
  - [追加] 「テクスチャ全面」の SpriteDef は rect = {0,0,0,0} (IsFullTexture()) で表す。sub-04 が Texture から寸法を引く。
  - [追加] SortingLayers::OrderOf(id) の「無い id」は Default の配列位置を返す。
  - [追加] SpriteLibrary の内部に std::map (規則 7 の新規警告を避け、Enumerate の順序が読み順に依存しない)。
  - [追加] SpriteLibrary の注入は SimLibraries の末尾メンバ (sprites)。描画専用で sim は引かない。
  - 見立てとの差: 起動走査の追加位置は DemoContent.cpp の else 側。ClassifyPath で Texture を判定して Register。
検証:
  - Debug / Release ビルド → 両方 exit 0。警告は既知の C4100 のみ。
  - gen_project_files.ps1 → 実行済み。
  - MYE_SELFTEST_ONLY=m94 (Debug / Release) → Physics + UI + Sprite2DAsset が ALL PASS。(a) .meta 往復と従来バイト同一、実 .meta の Read → Write バイト同一、(b) 改名後も同じ AssetID・新規 id = 最大 + 1・削除 id は nullptr・遅延全面解決・Enumerate の順序、(c) id 再利用と上限 32、(d) OrderOf が配列順 (Move 後も)、保存・読み直し・他キー保持・壊れた表の補正。
  - check_rules.ps1 → 3 error / 50 warning (既存のまま)。M94 の変更で増えた error / warning はない。
  - エディタのスクショ (一時プローブで Project Settings を開き、Release、imgui.ini を退避) → plans\m94-2d\screenshots\sorting_layers.png。Default / Background / Props / Player の 4 行と Up / Down / Remove / Add Layer / Save を目視確認。プローブは撮影後に戻し、残り 0 件。
  - 未実行 (ユーザー判断 9 で sub-05 へ): replay_verify、shot_verify、golden 30 枚、--selftest 全体。
  - 版 bump なし (kSimSnapshotVersion は 52 のまま)。
自己採点 (1-5):
  仕様適合: 4 / 正しさ: 4 (GUI の実クリック、Save 後の再起動、ホットリロード経由の再登録は未実施) / コード品質: 4 / テスト: 4 (Register の起動走査と ReloadTexture 経由は selftest の外)
不安・質問:
  - 全面スプライトを rect w/h = 0 で表す方式でよいか。
  - SortingLayers の起動時 Load の位置は sub-04 で決めてよいか。
  - ReloadTexture は GPU テクスチャが未ロードだと Skipped で Register を呼ばない。.meta 単体の変更は監視対象外のため、sub-05 の Apply が直接 Register を呼ぶ前提。
申し送り:
  - sub-04: SpriteDef は rect w/h = 0 で全面。ppu / filter は .meta から (filter 0 = linear)。未解決は Get が nullptr → マゼンタ。SortingLayers::OrderOf(id) が描画順。起動時の Load は未接続で sub-04 が呼ぶ。SpriteLibrary は spritelib::Library() で引ける。
  - sub-05: スライス追加の id は NextSpriteId()。Apply は WriteMeta のあと SpriteLibrary::Register と TextureLibrary::ReplaceFromFile を呼ぶ。
  - 撮影手順: Editor.exe は --frames N --shot-frame M --screenshot <png> の組で、--shot-frame を付けないと保存されない。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。`SpriteLibrary.h` と `AssetDatabase.cpp` の差分を読み、AssetID のキー形式 (`#sprite<id>`)、既定値を書かない WriteMeta、id 無し / 範囲外の要素を捨てる ReadMeta を確認。スクショで Sorting Layers の 4 行と Up / Down / Remove / Add / Save を確認。[追加] 5 件は全部採用 (spec 4.1.2 に反映): 全面 = rect w/h 0、OrderOf の無い id = Default の位置、std::map、SimLibraries 末尾、filter / ppu も既定なら書かない。不安 3 件の裁定: (1) w/h 0 で可、sub-04 が Texture から寸法を引く (2) SortingLayers の起動 Load は sub-04 で EngineLoop 初期化 (TagNames と同じ場所) に接続 (3) .meta 単体変更は Apply (スプライトエディタ + Inspector Import Settings) が Register を直接呼ぶ (sub-05 に明記)。nit: 重複 id の後勝ち WARN (spec R6) は sub-05 で確認。
