# sub-07: スプライト列クリップ (`.spriteanim.json`、コントローラの `"sprite"`、イベント、窓)

- 依存: sub-04
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #2、4.1.4、4.2.1、4.2.3

## やること
1. `.spriteanim.json` と `SpriteAnimLibrary` (`src\Engine\Engine\Animation\SpriteAnim.{h,cpp}`、GUID キー、`PhysMatLibrary` の流儀。`AssetType` 末尾 append、`ClassifyPath` (最長サフィックス)、`TypeName`、`RegisterAssetLibraries`、ReloadHub (`kAssetKinds` に controller より前の rank で)、Project 窓の Create → Sprite Animation (`CreateSpriteAnimAsset` in AssetOps、`kCompound` にサフィックス)、`kTypeFilters` / `TileLabel` / `FileTypeIcon`、Inspector のアセット編集 (コマの追加 / 削除 / 順序 / ticks / sprite ピッカー、ステップ再生のプレビュー)、注入は physmat と同じ 3 箇所)。
2. `ControllerState` に `spriteHash` (AssetID) を足し、`FromJson / ToJson` で `"sprite"`。`"skel"` と両方あれば `"sprite"` を採って WARN。`ControllerStateLengthTicks` (`AnimatorController.cpp:1542-1561`) がスプライトの長さ (Σticks) を返す。
3. `AnimatorControllerSystem::Update` (:1596-1841) の手順 6 (骨のポーズプログラム) の隣に「スプライトの書き込み」: 現在コマを `stateTimeTicks` から二分探索 (遷移中は spec 4.1.4 の整数比較で元 / 先を選ぶ)、書き込み先の Sprite2D (自身 → 無ければ DFS 最初の子孫。探索結果はキャッシュせず毎 tick 探す = 構造変更に追随。コストは部分木の大きさ) の `sprite` に書く。イベント (:1731-1757) は `mainModel != nullptr` 条件を「スプライトでも通る」ように広げ、`clipEvents` のキーに `.spriteanim.json` の `name` を使う。
4. Animator Controller 窓: ステート編集に Sprite Animation のピッカー (「skel」と排他の表示)、ノード 2 行目 "sprite: <name>"、`DrawPoseLayers` でスプライトのとき現在コマ番号。`ControllerStateEdit.h` の種類 enum に `Sprite` を足す。プレビュー窓 (M89n) はスプライトのステートでコマを表示 (ステップ / スクラブ)。
5. selftest (`AnimatorControllerSelfTest.cpp` に節を足す): (a) 3 コマ (6/6/12) のループで tick ごとのコマ (b) `hasExitTime` 遷移が tick 24 で発火 (c) `clipEvents` の tick 12 のイベントが 1 回だけ (d) 遷移 duration 4 の中間でコマが切り替わる tick (e) Sprite2D.sprite を手で別値にしても `HashWorld` が変わらない (f) 2 ワールド並走の HashWorld 一致。
6. `kSimSnapshotVersion` は触らない (コンポーネントの形は変わらない。ControllerLibrary はアセット)。

## やらないこと (このサブでは)
- ブレンドツリーのスプライト版、スプライトの描画補間、`"joint"` 指定の位置

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Animation\SpriteAnim.{h,cpp}`
- `src\Engine\Engine\Animation\AnimatorController.{h,cpp}` (:56-57 ControllerState、:959-1209 JSON、:1542 長さ、:1596-1841 Update、:1731 イベント)
- `src\Editor\Windows\Animation\AnimatorControllerWindow.cpp` (:398-417, :473-484)、`ControllerStateEdit.h:18-42`、プレビュー窓 (M89n の `AnimationPreviewWindow`)
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub.{h,cpp}`、`AssetBrowserWindow.cpp`、`AssetOps.{h,cpp}`、`EditorWidgets.cpp:205-229`、`InspectorWindow.cpp:2972-3041`
- `AnimatorControllerSelfTest.cpp`、`LocalizationTable.inl`、vcxproj
- 参考: `engine_spec.md:3577` (M89h のイベント発火規則)、`plans\m89-skeletal-animation.md:18-27` (原則)

## 受け入れ条件 (このサブ)
1. (spec 16) selftest (a)〜(d)(f) PASS。Animator 窓のエディタスクショ `plans\m94-2d\screenshots\animator_sprite.png` (ピッカーとコマ番号)。
2. (spec 17) selftest (e) PASS。
3. `.controller.json` v1 / v2 の既存アセットが読める (`AnimatorControllerSelfTest` の既存項目。この suite を `MYE_SELFTEST_ONLY=m94` に足す)。`--anim-demo` の `--hash-diff` が無差分 (replay_verify 全体は sub-10)。
4. (spec 1-3) 0 警告、check_rules 0、版 bump なし。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- `Runtime.exe --anim-demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-10)。

## 実装メモ (coder が追記)

## フィードバック履歴
