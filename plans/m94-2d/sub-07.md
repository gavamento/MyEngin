# sub-07: スプライト列クリップ (`.spriteanim.json`、コントローラの `"sprite"`、イベント、窓)

- 依存: sub-04
- 状態: 実装中
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

SELF_EVAL: sub-07 (round 1)
実装:
  - src/Engine/Engine/Animation/SpriteAnim.{h,cpp} (新規) — SpriteAnim (frames / 導出値 frameEnds・lengthTicks・nameHash)、SpriteAnimFrameAt (frameEnds の upper_bound = 整数の二分探索、範囲外は端のコマ)、SpriteAnimLibrary (HashForPath / LoadFromFile / Register / Get / Enumerate (名前昇順) / ToJson / FromJson / Sanitize、ticks は [1, 100000]・コマは 4096 まで)、spriteanim:: の Install / Library / Resolve。anims_ は std::map (check_rules rule 7 の警告を増やさないため)。name は JSON の "name"、無ければファイル名の stem
  - AssetDatabase.{h,cpp} — AssetType::SpriteAnim を末尾 append、ClassifyPath (".spriteanim.json")、TypeName / ParseTypeName ("spriteanim")
  - Loop/SimInit.{h,cpp}、EngineLoop.cpp、HeadlessSim.cpp — SimLibraries に spriteAnims を足し、spriteanim::Install / Uninstall、所有は EngineLoop と HeadlessSim の各 1 個 (注入 3 箇所)
  - Demo/DemoContent.cpp:RegisterAssetLibraries — ".spriteanim.json" の起動走査登録
  - HotReload/ReloadHub.{h,cpp} — ReloadKind::SpriteAnim、kAssetKinds に rank 6 の行 (controller は表に行が無く未知 .json = rank 9 なので「controller より前」を満たす)、ReloadSpriteAnim (登録済みだけ読み直す。physmat と同じ)
  - Animation/AnimatorController.{h,cpp} — ControllerState.spriteHash、StateDrivesSprite、ToJson / FromJson の "sprite" (あれば skel を書かない / 併記は WARN して sprite を採り skelClip・ブレンドを全部捨てる)、ControllerStateLengthTicks がスプライト列の Σticks を返す (未登録なら従来の経路へ)、FireStateEvents に sprite 分岐 (clipEvents のキーは SpriteAnim.nameHash、長さは Σticks、位置はコントローラのエンティティ。mainModel をポインタ化し、手順 4 のゲートから mainModel != nullptr を外した)、手順 4 の層の重みはスプライトも骨と同じ扱い、手順 6b WriteSpriteFrame (CurrentSpriteFrame = 遷移中は transitionTick · 2 >= duration で先 / 元の整数比較、SpriteAnimFrameAt、FindSpriteTarget = 自身 → 前順で最初の子孫を毎 tick 探索)
  - Editor: ControllerStateEdit.{h,cpp} (StateSkelKind::Sprite、排他、SetStateSkelKind に fallbackSprite)、AnimatorControllerWindow.cpp (kind コンボに Sprite、Sprite Animation のピッカーと長さ、ノード 2 行目 "sprite: <name>"、DrawPoseLayers に現在コマ "frame i / n")、AnimationPreviewWindow.{h,cpp} (SkinnedMesh の無いキャラでスプライトのステートがあれば DrawSpritePreview = ステート選択・再生・|<・-1・+1・スクラブ・コマの絵)、新規 Windows/Animation/SpriteAnimEditor.{h,cpp} (DrawSpriteThumb / DrawSpritePicker / SpriteAnimEditState / Inspector のコマ編集: 追加・削除・順序・ticks・スプライトのピッカー・スライスをまとめて追加 (自然順)・ステップ再生・保存 / 戻す)、InspectorWindow.{h,cpp} (SpriteAnim の編集キャッシュと 2 箇所の分岐)
  - Editor: AssetBrowserWindow.cpp (Create → Sprite Animation、kTypeFilters、TileLabel "spriteanim")、AssetOps.{h,cpp} (CreateSpriteAnimAsset、kCompound に ".spriteanim.json")、EditorWidgets.cpp (FileTypeIcon = ICON_FA_FILM)、LocalizationTable.inl (Asset_SpriteAnim / SprAnim_* / Anim_Sprite* など en / ja 両方)
  - selftest: AnimatorControllerSelfTest.cpp に M94 節 (a)〜(f) + 書き込み先 + JSON の往復 (18 項目)、AnimatorControllerEditSelfTest.cpp に Sprite の排他 4 項目、AssetDatabaseSelfTest / ReloadHubSelfTest / AssetOpsSelfTest に新種別の行、EditorMain.cpp の MYE_SELFTEST_ONLY=m94 分岐に AnimatorController / AnimatorControllerEdit / AssetDatabase / ReloadHub / AssetOps を追加
仕様との差分:
  - [追加] FindSpriteTarget は、別の AnimatorController を持つ子孫の部分木を辿らない (SkipChildren) — spec 4.1.4 は「最初の子孫 (DFS 順)」のみ。骨の CollectDrivenSkinnedMeshes と同じ「入れ子の Animator はそちらが駆動する」原則。外したいなら 1 行
  - [追加] 遷移中に選んだ側のステートがスプライトを駆動しない / 列が未登録・空なら何も書かない (Sprite2D は直前のコマのまま)。spec に無い境界の裁定
  - [追加] ControllerStateLengthTicks は spriteHash が立っていても列が未登録 (または長さ 0) ならプロパティクリップの長さへ落ちる (骨クリップがモデルから引けないときと同じ)
  - [追加] "sprite" と "skel" の併記は sprite を採り、skelClip・ブレンドの子・パラメータ番号も全部捨てる (ブレンドの子が空でも blendType が残って phase 計算に入らないように)。spec は「ブレンドは警告してスプライトを優先」
  - [追加] イベントの層の重みは、スプライトのステートも StateDrivesSkeleton と同じ側として数える (遷移中は骨と同じ線形の tick / duration、重み 0 の元は発火しない)。spec 4.1.4 の「骨と同じ関数を通す」の解釈。手順 4 のゲートから mainModel != nullptr を外したので、clipEvents が非空でモデルの無いコントローラも毎 tick このブロックに入る (FireStateEvents が即 return する)
  - [追加] プレビュー窓のスプライト表示は「SkinnedMesh を駆動しない」キャラだけ。骨とスプライトを両方持つキャラは従来どおり骨のプレビュー。スプライトのステートではイベントの編集 (帯) は出さない (spec は「コマを表示 (ステップ / スクラブ)」まで)
  - [追加] Inspector のコマ編集に「スライスをまとめて追加」(スライス済みテクスチャを選ぶと、そのスライスを数字を数として比べた順で末尾へ足す)。シートからの作成を現実的にするため
  - [追加] ReloadHub の rank は 6 (physmat と同格)。controller は表に行が無いので「controller より前」は未知 .json (rank 9) より前、で満たす
  - [追加] SpriteAnimLibrary の保持は PhysMat の unordered_map ではなく std::map (rule 7 の警告が 1 件増えるのを避けた)
  - [追加] MYE_SELFTEST_ONLY=m94 に AnimatorControllerEdit / AssetDatabase / ReloadHub / AssetOps も足した (今回触った新種別の行を回すため。sub-07 受け入れ条件 3 は AnimatorController のみ)
  - [未実装] Project 窓のダブルクリックでの Inspector 編集 (spec 4.1.4 の書き方) は、既存の physmat と同じ「選択すると Inspector に出る」だけ。ダブルクリック専用の処理は足していない
検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true → 両構成 0 警告 0 エラー (Release は初回が既知の SelfTestUnused.kernel.hlsl C4100 で落ち、再実行で通った)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Debug / Release) → exit 0。AnimatorController の M94 節 18 項目 (a: 6/6/12 の 60 tick 毎コマ、b: hasExitTime が tick 24、c: tick 12 のイベントは 12 と 36 の 2 回だけ、d: duration 4 で 1 tick 目 = 元 101、2〜3 tick 目 = 先 201、終わる tick = 202、e: sprite を手で書いても HashWorld 不変 + 次 tick で戻る、f: 2 ワールド 120 tick HashWorld 一致、書き込み先、JSON の往復と "sprite"+"skel" 併記、v2 の骨のステートの読み) + Edit / AssetDatabase / ReloadHub / AssetOps 全 PASS。既存の v1 / v2 のコントローラ読みの項目も同じ suite で PASS のまま
  - pwsh tools\check_rules.ps1 → 3 error / 50 warning (着手前と同数。error は EditorMain.cpp の #ifdef _DEBUG と tools/engine_dist/*.bat の LF = 既存の別件)。途中で SpriteAnim.cpp の Enumerate が warning を 51 に増やしたので std::map へ変えて 50 に戻した
  - pwsh tools\gen_project_files.ps1 → 実行 (Engine / Editor の vcxproj と filters が更新)
  - --anim-demo の hash-diff: 着手前 (HEAD a169aa8 を git worktree で Release ビルド) と着手後を同じ引数 (--warp --no-audio --anim-demo --frames 700 --replay-record --hash-dump-tick 599) で撮り Editor.exe --hash-diff → 構造は同じ 581 行、差は UIInteraction.prevSurfX / prevSurfY の 2 フィールドだけ (sim の値の差は 0)。ただし着手前の exe は 3 回とも 0、着手後の exe は毎回違う値 (0x4244C000 など) で、この 2 つはマウス座標 (UIInteraction.cpp:473 prevSurfX = in.mouseSurfX) 由来に見える (sub-01 / sub-02 の記述と同じ)。マウスが原因かは未確証。一時 worktree は削除済み
  - エディタのスクショ plans\m94-2d\screenshots\animator_sprite.png (Release Editor.exe --warp、一時プローブ経由、1600x900): Animator 窓にノード "Walk" の 2 行目 "sprite: Walk"、ステート編集の kind = sprite animation と Sprite Animation ピッカー (Walk)、"24 ticks / 4"、ポーズ層の "Walk frame 1 / 4" が出ている。同じ一時プローブで Inspector の .spriteanim.json 編集 (プレビューの絵・コマ一覧のサムネイル・ピッカー・ticks・^ v x・Add Frame・Add Slices Of・Save / Revert) とプレビュー窓 (再生中のコマが切り替わる) の描画も確認したが、画像は撮影確認のみで保存していない
  - 一時物の除去: 一時プローブ (EditorApp.cpp / AnimatorControllerWindow.{h,cpp}) は除去済みで grep で 0 件、EditorApp.cpp と AnimatorControllerWindow.h は git diff 空。一時アセット assets\_tmp_m94g (PNG・.meta・.spriteanim.json・.controller.json) は削除済み。imgui.ini は撮影前に退避して元に戻した。除去後に Debug / Release を再ビルドして上の selftest を回した
  - 未実行 (ユーザー判断 9 で sub-10): 全体 --selftest、replay_verify.bat、shot_verify.bat と golden 30 枚
  - 未確認: GUI の実クリック (ピッカーの選択、Add Frame / ^ v x / ticks のドラッグ、Create → Sprite Animation のメニュークリック、コントローラ窓のコンボ切り替え) は自動操作の手段が無く、描画結果とライブラリ / モデルの単体テストだけで確認した。Play 中の Editor / Runtime で Sprite2D の絵が実際に切り替わる実走は未実施 (sim が sprite を書くところまでは selftest、Sprite2D の描画は sub-04 / 06 で確認済み)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜4 を満たす。ダブルクリック専用の処理が未実装で、境界の裁定が [追加] として複数ある
  正しさ: 4 — (a)〜(f) を tick の数え方を先に予測して書き、すべて初回で一致した。hash-diff の prevSurf の由来が未確証
  コード品質: 4 — 既存の physmat / BT 窓の流儀に揃えた。FindSpriteTarget は毎 tick の部分木走査 (仕様どおり)。Inspector 側の編集 UI は新規ファイルへ分けて InspectorWindow.cpp を 2 箇所に抑えた
  テスト: 4 — 受け入れ条件の (a)〜(f) と排他・JSON を selftest で固定。GUI の実クリックと Play 中の描画は未確認
不安・質問:
  - 書き込み先で「別の Animator の部分木を辿らない」を採ってよいか (差分の 1 つ目)。仕様どおり単純に最初の子孫にするなら SkipChildren の分岐を消す
  - ダブルクリック (spec 4.1.4) は「Project 窓で選択 → Inspector で編集」で足りるか、専用の開く動作が要るか
  - --anim-demo の hash-diff で prevSurfX / Y が着手前 (0) と着手後 (毎回違う値) で分かれる理由。sub-10 の replay_verify で割れたらここを疑う。コードが原因の可能性は低いと見るが、確証は無い
触ったファイル:
  build/Editor.vcxproj, build/Editor.vcxproj.filters, build/Engine.vcxproj, build/Engine.vcxproj.filters (gen_project_files の出力。ファイル追加に伴い要コミット),
  src/Engine/Engine/Animation/SpriteAnim.h, src/Engine/Engine/Animation/SpriteAnim.cpp, src/Engine/Engine/Animation/AnimatorController.h, src/Engine/Engine/Animation/AnimatorController.cpp, src/Engine/Engine/Animation/AnimatorControllerSelfTest.cpp,
  src/Engine/Engine/Asset/AssetDatabase.h, src/Engine/Engine/Asset/AssetDatabase.cpp, src/Engine/Engine/Asset/AssetDatabaseSelfTest.cpp,
  src/Engine/Engine/Loop/SimInit.h, src/Engine/Engine/Loop/SimInit.cpp, src/Engine/Engine/Loop/EngineLoop.cpp, src/Engine/Engine/Loop/HeadlessSim.cpp, src/Engine/Engine/Demo/DemoContent.cpp,
  src/Engine/Engine/HotReload/ReloadHub.h, src/Engine/Engine/HotReload/ReloadHub.cpp, src/Engine/Engine/HotReload/ReloadHubSelfTest.cpp, src/Engine/Core/Localization/LocalizationTable.inl,
  src/Editor/App/EditorMain.cpp, src/Editor/Asset/AssetOps.h, src/Editor/Asset/AssetOps.cpp, src/Editor/Asset/AssetOpsSelfTest.cpp, src/Editor/SelfTest/AnimatorControllerEditSelfTest.cpp, src/Editor/Widgets/EditorWidgets.cpp,
  src/Editor/Windows/Animation/ControllerStateEdit.h, src/Editor/Windows/Animation/ControllerStateEdit.cpp, src/Editor/Windows/Animation/AnimatorControllerWindow.cpp, src/Editor/Windows/Animation/AnimationPreviewWindow.h, src/Editor/Windows/Animation/AnimationPreviewWindow.cpp,
  src/Editor/Windows/Animation/SpriteAnimEditor.h, src/Editor/Windows/Animation/SpriteAnimEditor.cpp, src/Editor/Windows/Asset/AssetBrowserWindow.cpp, src/Editor/Windows/Scene/InspectorWindow.h, src/Editor/Windows/Scene/InspectorWindow.cpp,
  plans/m94-2d/screenshots/animator_sprite.png, plans/m94-2d/sub-07.md (実装メモ節のみ)
申し送り:
  - (sub-08 以降 / sub-10) SpriteAnim の AssetID は GUID (.meta)。Sprite2D の書き込みは WriteSpriteFrame 1 箇所なので、タイルのアニメーションタイル (sub-13) が同じコマ選びを使うなら SpriteAnimFrameAt (SpriteAnim.h) を流用できる
  - (sub-10) m94 分岐は今 Physics / UI / Sprite2DAsset / SpriteEditor / AnimatorController / AnimatorControllerEdit / AssetDatabase / ReloadHub / AssetOps。hash-diff の prevSurfX / Y は着手前後で値が割れるので、replay_verify で見るなら除外して読むこと (上記の確証無し)
  - (sub-24) engine_spec に書くこと: スクリプトが Sprite2D.sprite を書いても次 tick で Animator が書き戻す。書き込み先は自身 → 前順で最初の子孫 (別の Animator の部分木は除く)。遷移中のコマは transitionTick · 2 >= duration で先に切り替わる。イベントのキーは .spriteanim.json の name
  - 設計上の観察: Update 手順 4 のイベントは骨とスプライトで時計 (StateClock) を共有しているので、将来「スプライトの描画補間」を足すなら StateClock.timeStep が使える (今回は対象外)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。スクショ `animator_sprite.png` で Walk ノードの 2 行目 "sprite: Walk"、kind コンボ = sprite animation、Sprite Animation ピッカー、"24 ticks / 4"、ポーズ層 "Walk frame 1 / 4" を確認。selftest (a)〜(f) 18 項目が両構成 PASS (受け入れ 1・2)、既存 v1 / v2 コントローラの読みも同 suite で PASS (3)。hash-diff の差は `UIInteraction.prevSurfX/Y` だけで、これは `InputSnapshot.mouseSurfX` (記録される入力) のミラーなので run 間で違って当然、replay は入力ごと再生するので割れない。Q1 (入れ子 Animator の部分木を辿らない) は採用、Q2 (ダブルクリック) は「選択 → Inspector」で足りると裁定、Q3 は上のとおり。spec 4.1.4 に反映。
