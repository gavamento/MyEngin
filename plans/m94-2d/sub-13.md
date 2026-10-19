# sub-13: ルールタイル、アニメーションタイル、レイヤー UI

- 依存: sub-11, sub-12
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #28、4.1.7

## やること
1. ルールタイル: 純関数 `ResolveRuleVariant(palette, tileId, neighbors8) → spriteAssetId` (`variants` を順に見て最初に合う `neighbors` 8 文字パターン (NW N NE W E SW S SE、x = 同 tileId が必要 / o = 空 (0) が必要 / . = 任意) を採る。合わなければタイル既定の `sprite`)。隣接は**同じレイヤー**の実効セル (override 込み)。チャンク境界は隣チャンクを読む (隣チャンクの変更で自チャンクも dirty にする = 変更セルの 8 近傍が別チャンクならそのチャンクも dirty)。
2. アニメーションタイル: `frameIndex = (simTick / ticksPerFrame) % frames.size()`。`Sprite2DRenderer` の収集は sim tick (`TickRunner` が持つ現在 tick を描画側へ渡す既存の経路、無ければ `EngineContext` の tick) を受け、アニメタイルを含むチャンクは `frameIndex` が変わった tick だけ焼き直す (チャンクごとに「最後に焼いた frameIndex 列」を持つ)。sim 状態は持たない。
3. レイヤー UI: Tilemap の Inspector (アセット側) にレイヤーの追加 / 削除 / 名前 / sortingLayerId / orderInLayer / collision / ↑↓。パレット窓のタイル Inspector に `collider / oneWay / hp / destroyedTile / destroyEffect / rule (variants の追加・削除・8 近傍のトグル UI・sprite ピッカー) / anim (ticksPerFrame・frames の追加・削除・順序)`。両方とも独自 Undo (sub-12 の `TilemapEditModel` を拡張 / `TilePaletteEditModel` を新設)。
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

## フィードバック履歴
