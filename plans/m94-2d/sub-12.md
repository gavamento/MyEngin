# sub-12: タイルパレット窓

- 依存: sub-10, sub-02
- 状態: OK (commit 3a11cdb)
- 往復: 1
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

SELF_EVAL: sub-12 (round 1)
実装:
  - src\Editor\Windows\Tilemap\TilemapEditModel.{h,cpp} — 編集モデル。TilemapAsset のコピーを持ち、塗りはセル差分 (CellEdit) で 128 段の独自 Undo deque に積む。PaintCell / EraseCell / FloodFill (4 連結・同値領域・上限 65536 で超えたら何も書かず TooLarge) / FillRect (2 隅順不同・両端含む・上限 262144) / PickCell / SelectRect / EraseSelection、BeginGesture / EndGesture (ドラッグ 1 回 = 1 段)、Dirty (保存時点のレイヤーとの比較、Revision でキャッシュ)、MarkSaved、SameCells。レイヤー範囲外・座標範囲外 (-32768..32767) は Invalid
  - src\Editor\Windows\Tilemap\TilePaletteWindow.{h,cpp} — 窓 (レイアウト ID "TilePalette")。対象 Tilemap エンティティのコンボ・レイヤーのコンボ・Undo / Redo / Save・6 ツール (S/B/D/G/U/I)・flip X / Y・回転 90°・Paint in Scene View のチェック・パレットのサムネイル一覧 (DrawSpriteThumb)・未保存モーダル (別アセットへ切替時)・Play 中の無効表示。入力 (OnSceneInput) と描画 (Draw*) と保存 (DoSave) を別関数に分けた (sub-05 nit)。ブラシのドラッグは前回セルから Bresenham で補間
  - src\Editor\Windows\Scene\SceneViewWindow.{h,cpp}:HandleTilePaint — 2D モードで窓が編集可能なとき、MouseRay と Tilemap の z 平面の交点 → TilemapSystem::WorldToCell でセルにして窓へ渡し、黄のカーソルセル / 黄のドラッグ矩形 / 水色の選択範囲を ImGui の drawlist に重ねる。塗り中はピッキング・ギズモ・アイコン選択へ左ボタンを流さない
  - src\Editor\App\EditorApp.{h,cpp} — 窓の所有・Window メニュー・レイアウト登録・Play 中と 2D モードの状態を Scene View より先に渡す・Inspector の Open Tile Palette を受ける・ゲートの dirty 判定・グローバルな Delete (エンティティ削除) の前に窓へ問い合わせ (ClaimsDeleteKey)
  - src\Editor\Windows\Scene\InspectorWindow.{h,cpp} — Tilemap コンポーネントに [Open Tile Palette] ボタン (そのエンティティを対象に窓を開く)
  - src\Engine\Engine\Rendering\Sprite2D\SpriteLibrary.{h,cpp} — Epoch() を足した (Register / Clear のたびに +1)。やること 7
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp} — タイルのチャンクキャッシュのキーに SpriteLibrary の版を追加 (BakedChunk.spriteEpoch)
  - src\Editor\SourceControl\GitTransaction.{h,cpp} / SourceControlSelfTest.cpp — GateBlocker::TilePaletteDirty を追加 (未保存のタイル編集があるうちは Source Control の書き込み系 op を止める。SpriteEditorDirty と同じ形。行表の static_assert も更新)
  - src\Engine\Core\Localization\LocalizationTable.inl — 窓・ツール・状態・モーダル・Inspector ボタン・ゲート文言の日英 (Tr のみ、書式付きの Tr は使っていない)
  - src\Editor\SelfTest\TilePaletteSelfTest.{h,cpp} + EditorMain.cpp — 新 suite を MYE_SELFTEST_ONLY=m94 と全体に接続
仕様との差分:
  - [逸脱] やること 5 の「編集中の上書き」は TilemapSystem へ足さず、編集のたびに TilemapLibrary::Register (ファイルは書かない) で反映する方式にした。理由: TilemapSystem の override 表は hash / snapshot 対象で、エディタ専用の口を同居させると HashWorld・snapshot への混入リスクが出る (sub-11 申し送りの警告)。Library の Epoch は描画キャッシュが既に見ているので追加の仕組みが要らず、Save 前に Scene View に即反映される (実撮影で確認)。TilemapSystem は無改変、版 bump なし。代償: 編集のたびに全チャンクが焼き直される (epoch は全体の版)。大きな面 (数千チャンク) のドラッグ中に重くなる可能性があり未計測
  - [追加] Library が外から更新されたとき (自分の保存の ReloadHub 再読込・git の切替など) の同期 (SyncLive): 未編集なら内容を取り込み (Undo は捨てる)、編集中なら編集側を勝たせて登録し直す。保存直後に ReloadHub が古い内容へ巻き戻す競合を避けるため
  - [追加] GitTransaction に GateBlocker::TilePaletteDirty (上記)。SpriteEditorDirty に相乗りさせると文言が嘘になるため別項目にした
  - [追加] 「Paint in Scene View」チェック: 窓を開いている間は 2D モードの左クリックが塗りになりピッキングできなくなるため、外せるようにした
  - [追加] 塗り潰しは上限超過 (空白は無限に広がる) で何も書かず警告を出す仕様にした (仕様は「上限 65536 セル」とだけ)。途中までは塗らない。矩形は別上限 262144 (TilemapSystem::kMaxFillCells と同じ)
  - [追加] Pick は空でないセルを拾うとブラシ (タイル id / flip / 回転) に反映して Brush ツールへ戻る (Unity 準拠)。空セルのピックは何もしない
  - [追加] Select ツール: ドラッグで選択範囲 (水色) を作り、Delete で範囲を消去 (Undo 可)、Esc で選択を外す。移動・コピーは作らない。EditorApp のグローバルな Delete (選択中エンティティの削除 = Tilemap 自身が消える) が同時に走らないよう、選択範囲があるあいだ窓が Delete を先取りする (ClaimsDeleteKey)
  - [未実装] パレットへのタイル追加 UI (スプライトからタイルを作る) は sub-12 にも sub-13 の記述にも無く作っていない。タイル追加は今は .tilepalette.json の手編集か C++ の TilePalette::Add のみ
  - [未実装] Revert ボタン (保存時点へ戻す)。Undo で戻せる
検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true → 最終状態で 0 警告 0 エラー (初回の Release は既存癖の SelfTestUnused.kernel.hlsl の C4100 で落ち、再実行で通った)
  - pwsh tools\gen_project_files.ps1 → 実行済み (Editor.vcxproj / filters だけ差分)
  - pwsh tools\check_rules.ps1 → 3 error / 50 warning (基準と同じ、新規なし)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest Debug / Release → どちらも exit 0、FAIL 行 0、TilePalette suite PASS (0 failure)、Tilemap / SpriteEditor suite も PASS (最終ビルドで再実行)
  - TilePalette suite の中身: 4 連結 (斜めの隙間に漏れない・内側 9 セル)、同値領域 (壁 15 セル)、上限ちょうど 65536 は通り 65792 / 無限空白は TooLarge で Undo 段が増えない、矩形 (順不同・チャンク境界・負座標・上限・Invalid)、ピック (id + flip + 回転)、選択と消去と Undo、Undo / Redo 3 段・ジェスチャ 1 段・128 段上限・Dirty (保存時点比較)、JSON 往復 (空チャンクは書かれない)、SpriteLibrary::Epoch の増加
  - 実機 (一時プローブで Release Editor.exe --warp、1600x900): プローブは EditorApp / Scene View / 窓に一時コードを足して合成入力 (ツール・セル・押す / ドラッグ / 離す、カーソルは Scene View 中央の実マウス座標) を流し、撮影後に全て除去 (git diff で残りなし、再ビルド済み)。結果: Rect ツールで 8x8 を塗る → Brush ツールで斜め 3 セル → 窓の Save → .tilemap.json はチャンク (-1,-1) (0,-1) (0,0)、タイル id 1 が 64 セル / id 2 が 45 セル (床 42 + ブラシ 3) で期待どおり。Scene View 中央のカーソルは実レイ → セルの変換で (4, 2) を指し、その黄の枠が撮影で中央のセルに重なった (ray → cell と cell → 画面の往復が一致)。保存した .tilemap.json を Runtime.exe --warp --scene で読み、8x8 + 斜め 3 + 床 2 段が描かれた (sub-10 の読み込み経路)
  - エディタスクショ plans\m94-2d\screenshots\tile_palette.png: 窓 (ツール・レイヤー・パレット一覧・保存済み表示) + Scene View の黄のカーソル枠 + セル格子 + showCollision の緑の箱 (床 2 段の行ごとの箱が写る。細い線) + 8x8 の塗り (撮影後に Delete の先取りと警告文の折り返しを足したが、見た目は同じ)
  - Play 中: プローブで SetInPlayMode(true) を強制して撮影 → 窓に警告文 (黄) が出てツール / ブラシ / Undo / Redo が無効表示 (グレー) になり、Scene View では塗りが一切入らず (プローブの塗り指示を無視) 8x8 が出ない。このときの警告文は 1 行で切れていたので TextWrapped に直した (直した後の Play 表示は再撮影していない)
  - やること 7: スプライトの rect を変える (SpriteEditModel の SetRect + Save = スプライトエディタ Apply と同じ SpriteLibrary::Register) と、Scene View の草の 8x8 が茶色に変わった。比較実験として一時的に Epoch の比較を外したビルドで同じ手順を撮ると、パレットのサムネイルは茶色になるが Scene View の 8x8 は緑のまま (古い UV) = 修正前の不具合を再現し、Epoch で直ることを確認。比較用の改変は戻し済み
  - 未実行: 全体 selftest / replay_verify / shot_verify (ユーザー判断 9、sub-15)。SourceControlSelfTest の実行 (行表は static_assert でビルド時に検証済み)。Fill / Erase / Pick / Select の GUI 経由の実操作、Ctrl+S / Ctrl+Z / S B D G U I キー、Delete の先取り (ClaimsDeleteKey) の実操作、別アセットへの切替モーダル、Inspector の Open Tile Palette ボタン、Window メニュー、実マウスでの左ドラッグ (合成入力で窓のストローク経路と Scene View の座標変換は通したが、ImGui の実クリック判定 = IsMouseClicked / IsWindowHovered は通っていない)。数千チャンクの面での編集中の描画コスト
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜3 は検証済み。やること 5 は推奨と別方式 (差分に理由)、パレットのタイル追加 UI と Revert は未実装
  正しさ: 4 — 純関数と Undo は selftest、塗り → 保存 → Runtime の往復と Scene View の座標・枠・Epoch は実撮影で確認。実マウスのクリック判定とキー操作は未確認
  コード品質: 4 — モデル / 窓 / Scene View の連携を分け、sub-05 の指摘 (入力と描画を 1 関数に持たない) を守った。TilePaletteWindow.cpp が 700 行規模で Draw 系がやや長い
  テスト: 4 — 純関数・Undo・JSON・Epoch は selftest。窓と Scene View の入力経路は合成入力の一時プローブで、自動化はしていない
不安・質問:
  - 編集中の反映が「TilemapLibrary への Register のたび全チャンク焼き直し」になる。大きな Tilemap のドラッグ中の負荷は未計測。重ければ sub-13 で Library ではなく TilemapSystem の編集専用口 (hash 対象外) へ移すか、チャンク単位の dirty を足す判断が要る
  - 窓を開いている間 Scene View の 2D モードで左クリックのピッキング・ギズモが効かない (チェックで外せる)。Unity の Tile Palette と同じだが、既定を OFF にする方がよいか
  - Scene View の 2D モードのスプライト / タイルの色が淡い (緑 (70,170,70) が薄い緑に出る)。エディタカメラは ACES が掛かるため。窓のサムネイルは原色。色の契約 (spec 4.1.3) はゲームカメラの話で、Scene View は対象外の理解でよいか (撮影で気づいた。このサブでは触っていない)
触ったファイル: (本欄は SELF_EVAL 本文と同じ。絶対パスは司会への返信を参照)
申し送り:
  - sub-13 へ: パレットのタイル追加 UI が無い。sub-13 のパレット編集 (TilePaletteEditModel) で足すのが自然。レイヤー編集は TilemapEditModel の asset_.layers を触るので、編集モデルを拡張するなら layer 数が変わる前提で saved_ 比較 (LayersEqual は size も見る) と Undo の layer 添字 (CellEdit.layer) の扱いに注意 (レイヤーの削除・並べ替えを Undo に載せる設計は別途要る)
  - sub-13 へ: Tilemap の見た目に rule / anim が入ると、窓の「編集中は Register で反映」はそのまま使える (BakeTileChunk が Library を読むため)。隣接チャンクの dirty は Epoch が全体なので現状は不要
  - 版 bump なし (kSimSnapshotVersion 57 のまま)、hash 対象フィールドの追加なし、ABI 無変更、焼いた WorldHash 定数の再採取なし
  - キー: S B D G U I は窓にフォーカスがあるか Scene View 上にカーソルがあるとき (RMB 押下中・修飾キー中は無効)。Ctrl+S / Ctrl+Z / Ctrl+Y / Delete は窓にフォーカスがあるときだけ握る (Delete は選択範囲があるあいだ Scene View 上でも窓が先取り)。Scene View にフォーカスがあるときの Ctrl+Z はシーンの UndoStack のまま (タイルの Undo は窓のボタンか窓フォーカス時)
  - 撮影メモ: エディタ撮影は imgui.ini を退避して撮り、元に戻した (Git 管理外)。一時アセット (assets\_tmp_m94l)・一時プローブ・退避物は作業ツリーに残していない

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。スクショ `tile_palette.png` で窓 (コンボ・6 ツール・flip / 回転・Paint in Scene View・サムネイル) + Scene View の黄のカーソル枠・セル格子・8x8 の塗り・斜め 3 セル・床 2 段の showCollision 緑枠を確認 (sub-11 nit 5 も消化)。TilePalette suite 両構成 PASS、塗り → 保存 → Runtime 読み込みの往復、Play 中の無効表示、Epoch の故障点再現 (外すと古い UV) を確認。[逸脱] やること 5 の Register 方式は採用 (spec 4.1.7)。不安 1 (大きな面の負荷) は sub-13 3c で計測、2 (Paint in Scene View 既定 ON) は採用、3 (Scene View の色) は理解どおり。タイル追加 UI は sub-13 3b へ。
