# sub-12: タイルパレット窓

- 依存: sub-10, sub-02
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #22、4.1.7、4.3

## やること
1. `src\Editor\Windows\Tilemap\TilePaletteWindow.{h,cpp}` (窓の登録手順は sub-05 と同じ。レイアウト ID "TilePalette")。Window メニュー、Tilemap の Inspector に「Open Tile Palette」ボタン。
2. 編集モデル `TilemapEditModel` (`.tilemap.json` の全レイヤーのセルを持つ、128 段 Undo deque、保存時 JSON との比較で Dirty、`BeginGesture / EndGesture` でドラッグ 1 回 = 1 段)。純関数 (selftest 対象): `PaintCell`、`EraseCell`、`FloodFill` (4 連結、同値領域、上限 65536 セル)、`FillRect`、`PickCell`、`SelectRect`。
3. UI: 対象 Tilemap (シーン内の Tilemap エンティティのコンボ) とレイヤーのコンボ、ツール (選択 / ブラシ / 消去 / 塗り潰し / 矩形 / ピック。ショートカットは Unity と同じ S / B / D / G / U / I)、flip X / Y と回転 90° のボタン (ブラシの値に反映)、パレットのタイル一覧 (`.tilepalette.json` の各タイルのスプライトのサムネイル、クリックで選択)、Save (Ctrl+S)。
4. Scene View との連携: 窓がアクティブで Scene View が 2D モードのとき、Scene View 上のマウスをセル座標に変換 (`TilemapSystem::WorldToCell` にレイの XY 交点 (z = Tilemap の z)) し、ツールを適用。カーソルのセルをハイライト (黄の矩形)。矩形ツールはドラッグで範囲表示。Play 中 (`EngineContext` の play 状態) は塗れず、窓に「Play 中は編集できません」を表示。
5. Save → `TilemapLibrary` のアセットを書き → ReloadHub が拾って全チャンク dirty (sub-10 の口)。描画は編集中もモデルの内容を即反映したい → 窓がモデルを持っている間は `TilemapSystem` に「編集中の上書き (エディタ専用、sim には無関係 = Play 中は使わない)」を渡すか、1 ストロークごとにアセットを書き換えて ReloadHub を待つか。**前者を推奨** (Save 前に見える)。採った方式を差分に書く。
7. (sub-10 の申し送り Q3) `SpriteLibrary` に `Epoch()` (Register のたびに +1) を足し、タイルのチャンクキャッシュのキーに (Tilemap epoch, TilePalette epoch, SpriteLibrary epoch) を含める。スプライトエディタの Apply 後に焼いたチャンクが古い UV のままにならないことを、窓で Apply → Scene View の絵が変わることで確認。
6. selftest `TilePaletteSelfTest` (純関数): 塗り潰しの 4 連結と上限、矩形、Undo 3 段、ピック。

## やらないこと (このサブでは)
- ルール / アニメの編集 UI (sub-13)、カスタムブラシ、プレハブブラシ

## 触る場所 (planner の見立て)
- 新規 `src\Editor\Windows\Tilemap\TilePaletteWindow.{h,cpp}`、`TilemapEditModel.{h,cpp}`、`src\Editor\SelfTest\TilePaletteSelfTest.{h,cpp}`
- `src\Editor\App\EditorApp.{h,cpp}`、`SceneViewWindow.{h,cpp}` (マウス → セル、ハイライト、ツール適用の呼び出し)、`InspectorWindow.cpp` (ボタン)
- `src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}` (編集中の上書きの口)、`LocalizationTable.inl`、`EditorMain.cpp`、vcxproj
- 参考: `SceneViewWindow.h:80` `HandlePatrolRoute` (Scene View 上の独自操作)、`BehaviorTreeEditModel.h`

## 受け入れ条件 (このサブ)
1. (spec 24) 純関数 selftest PASS。
2. (spec 24) 実機: 8×8 を塗って Save → `.tilemap.json` が変わり、Runtime で読める (sub-10 の経路)。Play 中は無効の表示。エディタスクショ `plans\m94-2d\screenshots\tile_palette.png` (窓 + Scene View のハイライト)。
3. (spec 1-3) 0 警告、check_rules 0 (規則 10)、`MYE_SELFTEST_ONLY=m94` に `TilePaletteSelfTest` を足して PASS、版 bump なし。重い 3 本は sub-15。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
チェックポイントではない (重い 3 本は sub-15)。

## 実装メモ (coder が追記)

## フィードバック履歴
