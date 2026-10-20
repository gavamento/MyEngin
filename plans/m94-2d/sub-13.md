# sub-13: ルールタイル、アニメーションタイル、レイヤー UI

- 依存: sub-11, sub-12
- 状態: OK (commit ec7a3a0)
- 往復: 1
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #28、4.1.7

## やること
1. ルールタイル: 純関数 `ResolveRuleVariant(palette, tileId, neighbors8) → spriteAssetId` (`variants` を順に見て最初に合う `neighbors` 8 文字パターン (NW N NE W E SW S SE、x = 同 tileId が必要 / o = 空 (0) が必要 / . = 任意) を採る。合わなければタイル既定の `sprite`)。隣接は**同じレイヤー**の実効セル (override 込み)。チャンク境界は隣チャンクを読む (隣チャンクの変更で自チャンクも dirty にする = 変更セルの 8 近傍が別チャンクならそのチャンクも dirty)。
2. アニメーションタイル: `frameIndex = (simTick / ticksPerFrame) % frames.size()`。`Sprite2DRenderer` の収集は sim tick (`TickRunner` が持つ現在 tick を描画側へ渡す既存の経路、無ければ `EngineContext` の tick) を受け、アニメタイルを含むチャンクは `frameIndex` が変わった tick だけ焼き直す (チャンクごとに「最後に焼いた frameIndex 列」を持つ)。sim 状態は持たない。
3. レイヤー UI: Tilemap の Inspector (アセット側) にレイヤーの追加 / 削除 / 名前 / sortingLayerId / orderInLayer / collision / ↑↓。パレット窓のタイル Inspector に `collider / oneWay / hp / destroyedTile / destroyEffect / rule (variants の追加・削除・8 近傍のトグル UI・sprite ピッカー) / anim (ticksPerFrame・frames の追加・削除・順序)`。両方とも独自 Undo (sub-12 の `TilemapEditModel` を拡張 / `TilePaletteEditModel` を新設)。
3b. (sub-12 の申し送り) パレットのタイル追加 UI: パレット窓 (またはパレットアセットの Inspector) に「スプライトからタイルを追加」(ピッカー、id は最大 + 1) と「スライス済みテクスチャから全スライスを追加」、タイルの削除。`TilePaletteEditModel` の独自 Undo に載せる。レイヤーの追加・削除・並べ替えを `TilemapEditModel` に載せるときは `LayersEqual` の size 比較と `CellEdit.layer` 添字のずれに注意 (レイヤー操作は Undo の 1 段として layer 配列ごとスナップショットでよい)。
3c. (sub-12 の不安 1) 100×50×3 層の Tilemap で Brush を 60 フレームドラッグしたときのエディタのフレーム時間をログ (Release / WARP)。1 フレーム 33 ms を超えたら、描画キャッシュの dirty をチャンク単位にする (TilemapLibrary に「変更チャンクの集合」を持たせ、Sprite2DRenderer が epoch 全体でなくそれを見る)。
4. selftest (`TilemapSelfTest` に節追加): (a) 地面の縁 4 種 (左端 / 中 / 右端 / 単独) のルールが隣接で正しく選ばれる (b) チャンク境界の隣接 (c) アニメの frameIndex が tick 8 で進む (d) レイヤー 3 枚の順序入れ替えの往復 JSON。
5. 確認スクショ 2 枚 (tick 3 と tick 11 で `--shot-frame` を変えてアニメタイルのコマが違う。ルール変種の縁)。

## やらないこと (このサブでは)
- ルールの回転 / 反転対称 (Unity の Rule Tile の Rotated / Mirror)、ランダムタイル、Isometric / Hex

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}`、`TilemapAssets.{h,cpp}`、`TilemapSelfTest.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp` (焼き直し条件、tick の受け取り)
- `src\Editor\Windows\Tilemap\TilePaletteWindow.cpp`、`TilemapEditModel.{h,cpp}`、`InspectorWindow.cpp` (Tilemap / Palette アセットの編集)
- `LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 25) selftest (a)〜(d) PASS。スクショ 2 枚 (`plans\m94-2d\screenshots\tile_rule_anim_t3.png` / `_t11.png`) の差分がアニメタイルの画素だけ (`--img-diff` のログ)。レイヤー UI のエディタスクショ 1 枚。
2. アニメタイルを含むシーンで、描画を 2 回 (SceneView + GameView) 走らせても sim の `HashWorld` が変わらない (selftest か、`--replay-verify` の既存経路で代替)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` の Tilemap suite PASS、版 bump なし (アセットの形だけ。sub-10 で `rule` / `anim` を既に読み書きしている)。重い 3 本は sub-15。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --shot-frame 3` / `--shot-frame 11`

## 実装メモ (coder が追記)

SELF_EVAL: sub-13 (round 1)
実装:
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapAssets.{h,cpp}: ResolveRuleVariant (variants を順に見て最初に合う 8 文字 x/o/. を採る、合わなければ既定 sprite) / TileAnimFrameIndex ((tick / ticksPerFrame) % frames、frames 0 は -1、ticksPerFrame < 1 は 1) / ResolveTileSprite (anim > rule > sprite) / TileNeighbors 型。TileVariant・TileDef に defaulted operator== (パレット編集モデルの Dirty / Undo 比較用)
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}: BakeTileChunk が「def->sprite を引く」1 箇所を ResolveTileSprite に差し替え。ルールを持つパレットのときだけ 3x3 チャンクの実効セル (override 込み、同じレイヤー) を読み、純関数 TileNeighborsAt (NW N NE W E SW S SE、境界は隣チャンク) で隣接を作る。BakedChunk に AnimStamp (チャンクが含むアニメタイルごとの tileId / ticksPerFrame / コマ数 / 焼いたコマ) を持たせ、EmitTileChunk は今の tick のコマが 1 つでも違うときだけ焼き直す (コマが変わる tick だけ)。Collect に simTick 引数を追加。sim には 1 バイトも書かない
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.cpp: Collect へ既存の RenderSystem::simTick (EngineLoop が ctx.tickIndex から毎フレーム設定する経路) を渡す
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilemapEditModel.{h,cpp}: レイヤー操作 AddLayer / RemoveLayer / MoveLayer / SetLayerProps。Undo の 1 段を「セル差分」か「layer 配列ごとの前後スナップショット」の Step に拡張 (CellEdit.layer 添字は段ごとの配列に対するもの = LIFO なので整合)。LayersEqual が名前 / ソート層 / order / collision も比べる (SameCells → SameLayers に改名)。連続変更 (ドラッグ) は BeginGesture / EndGesture で 1 段に統合。最後の 1 枚は消せない、上限 8
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteEditModel.{h,cpp} (新規): パレットの編集モデル。AddTile (id = 最大 + 1) / AddTiles (複数を 1 段) / RemoveTile (他のタイルの destroyedTile が指していれば 0 にする) / SetTile (Sanitize の範囲へ丸める) と、tiles 配列ごとのスナップショット Undo (128 段、ジェスチャ統合)
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TileAssetEditUi.{h,cpp} (新規): DrawLayerPanel (追加 / 削除 / 上下 / 名前 / ソート層コンボ / 層内の順序 / 衝突) / DrawPaletteToolbar (スプライトからタイル追加ピッカー、スライス済みテクスチャの全スライス追加、タイル削除 = 3b) / DrawTileInspector (名前 / スプライト / collider / oneWay / hp / destroyedTile / destroyEffect (プレハブ) / rule (変種の追加・削除・↑↓・3x3 の 8 近傍トグル・sprite ピッカー) / anim (ticksPerFrame・コマの追加・削除・↑↓))。窓は描画だけで、編集は全部モデル経由
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteWindow.{h,cpp}: paletteModel_ を対象 Tilemap のパレットから読み込み、編集中は TilePaletteLibrary::Register で即時反映 (SyncPaletteLive、タイルマップの SyncLive と同じ規則 = TilemapSystem に口は作らない)。Ctrl+S / Save はタイルマップを書き、パレットは変更があれば .tilepalette.json も書く。未保存モーダル・Discard・HasUnsavedChanges はパレットも見る。Ctrl+Z / Ctrl+Y / ボタンは直前に変わった側から戻す。下半分を DrawBody (スクロールする子窓: Layers / Tiles / Selected Tile) に整理
  - C:\HAL\MyEngin\src\Editor\Windows\Animation\SpriteAnimEditor.{h,cpp}: SpriteDisplayName と ListSlicedTextures (スライス済みテクスチャ → 自然順のスライス) を公開 (コマ追加ポップアップと共用)
  - C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl: TilePal_* を 40 本弱追加 (両言語、### 識別子つき)
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapSelfTest.cpp: 「ルールとアニメ (M94m)」節 = (a) 縁 4 種 (左端 / 中 / 右端 / 単独) と先勝ち・フォールバック・同種判定が flip / 回転 / damage ビットに依らない (b) チャンク境界 (E 隣 / W 隣 / 対角の NE・SW) (c) アニメ frameIndex が tick 8 で進み 24 で戻る・空 frames・anim > rule・パレット JSON 往復
  - C:\HAL\MyEngin\src\Editor\SelfTest\TilePaletteSelfTest.cpp: (h) レイヤー 3 枚の入れ替えの往復 JSON + Undo / Redo (i) レイヤーの追加 / 削除をまたぐ Undo の添字・設定ドラッグ 1 段 (j) パレット編集 (追加 / スライス一括 1 段 / 設定 / 丸め / ドラッグ 1 段 / 削除と参照の掃除 / Undo / 往復)
仕様との差分:
  - [逸脱] レイヤー UI とタイル Inspector は「Inspector (アセット側)」ではなくタイルパレット窓の中 — 編集モデル (独自 Undo / Dirty / 即時反映 / 保存) が窓にあり、Project 窓で選んだ資産には対象の Tilemap エンティティが無く Scene View へ反映できないため。Inspector の TileAssetInspector は読み取り専用の要約のまま
  - [逸脱] アニメのコマ選びは SpriteAnimFrameAt を流用せず (tick / ticksPerFrame) % frames を直接 (spec #28 の式)。SpriteAnimFrameAt は可変 ticks でクランプする別の意味 (台帳の「流用可」は不採用)
  - [逸脱] 3c: 描画キャッシュの dirty をチャンク単位にしなかった。計測の結果と根拠は「検証」と「不安・質問」(planner の判断を仰ぐ)
  - [逸脱] 4 (d) 「レイヤー 3 枚の順序入れ替えの往復 JSON」は Engine の TilemapSelfTest ではなく Editor の TilePalette suite (TilemapEditModel の MoveLayer が要るため)。両方とも MYE_SELFTEST_ONLY=m94 に入っている
  - [追加] anim と rule を両方持つタイルは anim が優先 (spec は評価順を決めていない)。パレット UI は anim 有効時に「ルールと既定のスプライトは使われない」と表示
  - [追加] Sprite2DRenderer::TileNeighborsAt (3x3 チャンクから隣接を読む純関数) を public static にした — GPU 無しで境界の読みを selftest するため
  - [追加] Ctrl+Z の宛先: タイルマップとパレットが別々の Undo deque なので「直前に変わった側から戻す」 (TrackLastEdited)。線形履歴ではない (例: マップ編集 → パレット編集 → パレット Undo → もう 1 回で再びパレットを戻す)
  - [追加] TilemapEditModel::SameCells → SameLayers の改名 (レイヤー設定も比べるようになったため。呼び元は窓と selftest だけ)
  - [追加] UI の上限: 変種 64 / アニメのコマ 256 / 名前 63 文字 (資産の上限ではなく操作画面だけ)。最後の 1 枚のレイヤーは削除不可
  - [追加] RemoveTile は他タイルの destroyedTile (削除した id を指すもの) を 0 にする。マップのセルに残る id は消さない (パレットに無い id は描かれないだけ)
  - [追加] Play 中はレイヤー / パレット編集も不可 (パレット差し替えが衝突キャッシュを作り直し sim へ影響しうるため。タイルの塗りと同じ扱い)
検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true → 最終状態で 0 警告 0 エラー (Release の初回は既存癖の SelfTestUnused.kernel.hlsl C4100 で落ち、再実行で通った。Debug は初回で通過)。ビルドログ cache\m94\s13\buildB_*.log
  - pwsh tools\check_rules.ps1 → 3 error / 50 warning = 基準どおり (途中で TilePal_AnimHelp の '% f' が規則 10 を踏んで 4 error になったので文面を直して基準へ戻した。残り 3 は既存: EditorMain の #ifdef _DEBUG と Editor.bat / Setup.bat の 14-b)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest Debug / Release → どちらも exit 0。Tilemap suite ALL PASS、TilePalette suite PASS (0 failure)。新節の PASS 行は両構成で rule / anim 23 本 + layers / palette 38 本。stderr の「[ERROR] failed to load .mfrac」「[WARN] material json parse failed」は既存の否定系テストの出力 (FAIL 行は 0)
  - 受け入れ条件 1 のスクショ (temp シーン = assets\_tmp_m94m の PNG 8 枚 + palette + tilemap + scene、撮影後に除去済み。Release Runtime --warp、960x540、64 px/unit、tonemapMode 0、カメラ x = 13.5): plans\m94-2d\screenshots\tile_rule_anim_t3.png / tile_rule_anim_t11.png。--img-diff (tol 0) は差分 28672 画素 = 水のタイル 7 枚 × 64 x 64 = 28672 とちょうど一致し、差分の bbox は水の帯 (y 142..205) の内側だけ (帯の外の差分 0 画素)。水は tick 3 = コマ 0 (波線が上)、tick 11 = コマ 1 (別の青で波線が下)。チャンク境界 (セル 15 / 16) をまたぐ水 3 枚も同時に変わった。地面の縁: 単独 (x = 8) は両側が暗い縁、10..18 の列は左端 (10) が左だけ暗い・右端 (18) が右だけ暗い・中は濃い緑の帯で、境界 (15 | 16) をまたぐ中も正しく選ばれた (画像を目視)
  - 受け入れ条件 1 のレイヤー UI のエディタスクショ plans\m94-2d\screenshots\tile_layer_ui.png (Release Editor.exe --warp、一時プローブで窓を開いて対象と選択を設定。撮影後に除去): Layers (追加 / 削除 / 上 / 下、一覧、名前 / ソート層 / 層内の順序 / 衝突)、Tiles (サムネイル、スプライトから追加 / テクスチャの全スライス追加 / タイル削除)、Selected Tile (名前 / スプライト / 衝突 / 一方通行 / HP / 壊れた後 / エフェクト / ルール変種 4 つの 3x3 グリッド・sprite ピッカー・↑↓x) が描かれる。x は青、o は黄の色付きで ground_left は W = o・E = x、ground_right は W = x・E = o、ground_mid は両側 x、ground_single は両側 o と JSON どおりに出た
  - 受け入れ条件 2: Runtime.exe (描画 1 経路) と Editor.exe --autoplay (Game View を描画、Scene View も既定レイアウトで開いている) で同じ temp シーンを tick 60 まで走らせ --hash-dump → Editor.exe --hash-diff → identical (57 行、total 52FD311597DD2DF9)。ただし「SceneView と GameView の 2 回描画」を個別に確かめたわけではない (Editor の 2 ビューが両方描かれたかはログで未確認)。描画側が World を const で読むだけで sim に書かない構造 (Sprite2DRenderer は World& を ForEachArchetype / GetPtr で読むのみ) でも裏が取れる
  - 3c の計測 (一時プローブで EditorApp 内から合成入力、Release Editor.exe --warp --no-vsync、100x50x3 層 = 84 チャンク、Brush を 60 フレームドラッグ = 1 フレーム 1〜2 セル変更。プローブは除去済み): (1) 毎フレーム全 84 チャンクを焼き直す (60 フレームで 5040 回) CPU 時間は、ルール 4 変種 x 別テクスチャ 4 枚の最悪構成で 2.8 ms / フレーム、単一テクスチャ・ルール無しで 1.2 ms / フレーム (BakeTileChunk を計測)。(2) フレーム時間はドラッグ 平均 42〜47 ms (最大 105〜129 ms) に対し、ドラッグ前の待機 60 フレームは平均 36〜46 ms (最大 49〜81 ms) で同等 = 絶対値が 33 ms を超えるのはドラッグが原因ではなく、この WARP 環境のエディタ全体の基準 (背景の揺れが大きく、同条件でも 5 ms と 45 ms のフレームが混在)。(3) Epoch だけ増やす bare Register のフレームは 47 / 49 ms (待機と同等) で、Register 自体は 2 ms 未満。(4) ドラッグ開始の最初の 1 フレームだけ約 105〜125 ms (4 回の実行すべて) 出るが、2 回目のストロークの開始は 40〜46 ms で再現せず (初回だけの一過性)。この初回の原因は特定できていない (Register / 再焼きではないことだけ確認)
  - 版: 触っていない (kSimSnapshotVersion は 57 のまま、ABI 不変、kCookVersion 不変)。hash 対象フィールドの append なし = 焼いた WorldHash 定数の再採取は不要
  - 未実行 (ユーザー判断 9): Editor.exe --selftest 全体、replay_verify.bat、shot_verify.bat + golden 照合 (次はチェックポイント sub-15)
  - 未確認 (合成入力で通せない GUI の実操作): ポップアップ (スプライトから追加 / 全スライス追加) の実クリック、8 近傍トグルの実クリック、InputText の入力、キー (Ctrl+Z でマップ / パレットのどちらが戻るか)、パレットを編集して Ctrl+S で .tilepalette.json を書く往復 (書き出しは selftest で JSON 往復までのみ)、Scene View の 2D モードでパレット編集直後の絵の変化 → ユーザー目視 (sub-24 checklists)
自己採点 (1-5):
  仕様適合: 4 — やること 1 / 2 / 3 / 3b / 4 / 5 を実装、受け入れ条件 1・3 を満たした。差分は Inspector → 窓、3c で dirty のチャンク化を見送り (計測の根拠つきで planner 判断待ち)、条件 2 の 2 回描画は個別に確かめていない
  正しさ: 4 — selftest 両構成 PASS、実描画でルールの縁・チャンク境界・アニメの tick 差が画素レベルで期待どおり、描画後の hash 一致。GUI のクリック操作は未確認、3c の初回 100 ms の原因は未特定
  コード品質: 4 — 純関数 (ResolveRuleVariant / TileAnimFrameIndex / TileNeighborsAt) は GPU 非依存でテスト可能、窓は描画 (TileAssetEditUi) とモデルを分離、sim 無変更、日本語コメントでヘッダ付き。TilePaletteWindow は OnImGui の分担が増えて長く、Ctrl+Z の宛先は線形履歴ではない
  テスト: 4 — 純関数と両モデル (レイヤー / パレット) を selftest、画素差分と hash を実測、3c を実計測。GUI の実クリックと 2 ビュー描画の個別確認は未
不安・質問:
  1. (3c の判断) 文面の「1 フレーム 33 ms を超えたら dirty をチャンク単位に」を字義どおりに読むと発動する (ドラッグ中の平均 42〜47 ms) が、待機でも同じ 36〜46 ms で、全チャンク焼き直しの CPU は 1.2〜2.8 ms (最悪構成) しか食わない。baseline の主因はこの WARP 環境の GPU / 背景で、dirty の粒度ではないと判断して見送った。それでもチャンク化 (TilemapLibrary に変更チャンク集合を持たせる) が要るなら、規模は「Register が差分を取る + Sprite2DRenderer が集合を見る + 隣接 8 近傍のチャンクも dirty」で中程度 — planner 判断で追補できる
  2. (3c の観測) ドラッグ開始の最初の 1 フレームだけ約 105〜125 ms の一過性の遅延。Register でも再焼きでもなく、2 回目の開始では出ない。実機 (非 WARP) で出るかは未確認
  3. (Ctrl+Z の宛先) タイルマップとパレットを別の Undo にした (線形履歴にしていない)。窓全体で 1 本の履歴にするなら両モデルを 1 つの Step 列で包む作り直しが要る。「直前に変わった側」で足りるか
  4. (Edit 中のアニメ) sim の tick が進まないエディタの Edit モードでは、アニメタイルは tick 固定でコマ 0 の絵のまま動かない (Play 中と Runtime では動く)。Edit 中にも動いて見せたいなら描画専用の時計 (決定性に影響しない) を別に持つ必要があり、spec 4.4 の「描画側は tick から導出」とは別の判断
  5. (レイヤー UI の置き場) Inspector (アセット側) の編集は窓へ寄せた (差分欄)。Inspector の資産要約からは [Open Tile Palette] のような導線を作っていない
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapAssets.h
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapAssets.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Tilemap\TilemapSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilemapEditModel.h
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilemapEditModel.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteEditModel.h (新規)
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteEditModel.cpp (新規)
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TileAssetEditUi.h (新規)
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TileAssetEditUi.cpp (新規)
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteWindow.h
  - C:\HAL\MyEngin\src\Editor\Windows\Tilemap\TilePaletteWindow.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Animation\SpriteAnimEditor.h
  - C:\HAL\MyEngin\src\Editor\Windows\Animation\SpriteAnimEditor.cpp
  - C:\HAL\MyEngin\src\Editor\SelfTest\TilePaletteSelfTest.cpp
  - C:\HAL\MyEngin\build\Editor.vcxproj (生成)
  - C:\HAL\MyEngin\build\Editor.vcxproj.filters (生成)
  - C:\HAL\MyEngin\plans\m94-2d\screenshots\tile_rule_anim_t3.png
  - C:\HAL\MyEngin\plans\m94-2d\screenshots\tile_rule_anim_t11.png
  - C:\HAL\MyEngin\plans\m94-2d\screenshots\tile_layer_ui.png
  - C:\HAL\MyEngin\plans\m94-2d\sub-13.md (実装メモのみ)
申し送り:
  - (sub-14 以降) アニメ / ルールの評価は描画側だけ。C++ の TilemapSystem / ABI には何も足していない。スクリプトが SetCell したセルのルール変種は、sim の override を重ねた実効セルを描画側が読むので自動で追従する (MarkAllDirty が全チャンクを焼き直す)
  - (sub-15 チェックポイント 3) RenderSystem / Sprite2DRenderer の Collect 引数を 1 つ足した (simTick、既定 0)。Collect の呼び元は RenderSystem.cpp の 1 箇所だけ。全体 selftest / replay_verify / shot_verify (golden 30 枚) は未実行
  - (sub-24) engine_spec / checklists に書く: ルールの評価順 (変種の先勝ち、anim > rule > sprite)、隣接は同じレイヤーの実効セルでチャンク境界も読む、アニメは sim tick から導出で Edit モードでは進まない、変種の neighbors の順 (NW N NE W E SW S SE、N = +y)、レイヤー / パレット編集は Play 中不可、Ctrl+Z の宛先。GUI の実クリック (上記の未確認) はユーザー目視
  - 窓の編集でパレットから消した tileId を使うマップのセルは残る (描かれないだけ)。削除前に使用中セルを警告する機能は作っていない
  - 一時アセット・プローブは作業ツリーに残していない (assets\_tmp_m94m は削除済み、プローブはコピーから元のファイルへ戻して md5 一致を確認し、再ビルドして最終のバイナリで検証)。cache\m94\s13\ にログ・スクショ・ダンプ (git 管理外)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-21)。スクショ `tile_rule_anim_t11.png` (水のコマ違い、地面の縁 左端 / 中 / 右端 / 単独、境界をまたぐ中) と `tile_layer_ui.png` (Layers / Tiles / Selected Tile、ルール変種 4 つの 3x3 が JSON どおり) を確認。t3 / t11 の差分 28672 画素 = 水 7 枚ちょうど。selftest 両構成 PASS、描画後の hash 一致。3c はチャンク単位 dirty を見送る判断を採用 (焼き直し CPU 最悪 2.8 ms、ドラッグと待機のフレーム時間が同等 = 33 ms 超は WARP のエディタ全体の基準。文言「33 ms を超えたら」は焼き直しが原因で超える場合の意図)。[逸脱] 4 件・[追加] 8 件は採用 (spec 4.1.7)。Edit 中のアニメ静止は `[ユーザーに聞ける]`。
