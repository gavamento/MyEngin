# M75: ゲーム内 UI を Unity uGUI 相当へ — RectTransform / Canvas / Layout / ウィジェット / InputField / Rect Tool

**実装開始時**: 本ファイルを `plans\m75-ugui.md` へ複写してコミット対象にする。
1 サブ = 1 コミット (`M75a: ...` 形式の日本語件名) = 1 セッション + /clear。進捗の一次情報は git log。

**再開手順**: `git log --oneline -5` で最後に完了した M75x を確認 → 本ファイルの該当節を読む →
共通検証 (Debug/Release ビルド `/p:MyeWarnAsError=true` 0 警告 → `Editor.exe --selftest` → `pwsh tools\check_rules.ps1`
→ `tools\shot_verify.bat` (Release 要) → `tools\replay_verify.bat`)。ソース追加時は `pwsh tools\gen_project_files.ps1`。

- 基点: master `2baf538` (M74b の後。作業ツリーには音響/ImpactSound の未コミット変更があるが UI とは無関係)
- 現行の版: Scene `kDocVersion=3` / `.rep` v7 / `kSimSnapshotVersion=17` / `kNetProtoVersion=4` / ABI v17 = 111 スロット / 末尾 TypeId 50

## Context

現状のゲーム内 UI は `UIElementComponent` 1 種 (kind=panel/text/button) に 9-grid 単点アンカー + x/y/w/h を直接持ち、
Canvas Scaler は Expand (1920x1080 固定) のみ、Layout Group もウィジェットも無い。M70b/M70c で「描画とヒットテストが
同じ純関数 (`uilayout::Resolve`) を通る」「対話状態 `UIInteractionState` だけがハッシュ対象」という構造ができており、
それに Unity uGUI の RectTransform / Canvas / Auto Layout / Selectable / ウィジェット / EventSystem 相当を載せる。

**ユーザー決定 (2026-09-12)**: 4 領域すべて (RectTransform+Canvas / Layout / ウィジェット / エディタ視覚編集) + InputField を含める。
構成は **RectTransform を UIElement から分離** (Unity と同じ形)。テキスト計測は **プロジェクトフォントの計測表をアセット化**。

**不変条件 (M75 全期間)**:
- 既存 golden `ui_probe` / `ui_probe_720p` / `ui_probe_16x10` / `flow_title` は **maxDiff=0** (`Editor.exe --img-diff A B --tol 0` を手で当てる)。動いてよいのは新設 `ui_widgets*` だけ。
- 描画 / HitTest / FocusNav の 3 者は **同じ `uilayout::` 関数**で矩形を解く。sim レーンから呼ぶ経路は scalar のみ (SIMD 禁止)。
- 版 bump は **M75b (sim 形式) と M75h (ABI) の 2 回だけ** + M75a の Scene v4。他のサブで版を触ったら設計ミス。
- 新 UI コンポーネントは必ず `kComponentUiAux` 付きで登録 (A5)。忘れると `IsUiOnlyEntity` が偽になり screen UI が丸ごとワールド追従に落ちて消える。

## 設計判断 (要点。詳細は M75j の ADR-020 へ)

### A. RectTransform (M75a)
- `RectTransformComponent` (NoHash + UiAux、TypeId 51): `anchorMin/anchorMax/pivot/anchoredPosition/sizeDelta` Float2、`rotation` Float (deg、Z)、`scale` Float2、`basis` Int32 (0=親 UI 矩形 / 1=キャンバス矩形)。新規作成の既定は Unity 風 (anchor (0.5,0.5)、pivot (0.5,0.5)、size (160,40))。
- **座標系は左上原点・y 下向きを維持** (Unity は y 上向き)。回転正 = 画面上で時計回り。全消費者が y 下向きで、反転すると golden 不変の証明ができない。
- 旧フィールド (`anchor/x/y/w/h/space`) は **UIElement から削除**。ロード互換は `SceneSerializer::LoadFromJson` 内で生 JSON から拾い、親リンク適用 (`SceneSerializer.cpp:417-428` 付近) の後に `MigrateLegacyUiRects()` で RectTransform を追加する。**トリガーは版番号でなく「UIElement あり && RectTransform なし」** (プレハブのミニシーンは v2 のまま = `Prefab.cpp:1501`、外部プロジェクトの手書き JSON も拾える)。`kDocVersion` 4 は「この形式で保存した」の宣言。
- 変換式: `ax = {0,0.5f,1}[anchor%3]`, `ay = {0,0.5f,1}[anchor/3]`, `anchorMin=anchorMax=(ax,ay)`, `pivot=(0,0)`, `anchoredPosition=(x,y)`, `sizeDelta=(w,h)`, `basis = (space==0 && UI 祖先あり) ? 1 : 0`。
- 解決式は現行 `ResolveImpl` (`UILayout.cpp:154-159`) と**加算順を揃える**のがビット一致の根拠:
  `w = base.w*(anchorMax.x-anchorMin.x) + sizeDelta.x*scale` / `x = (base.x + base.w*anchorMin.x) + anchoredPosition.x*scale - pivot.x*w`。
  一致アンカー・pivot 0 では `+0` / `*1` / `-0` の恒等演算だけが増える。UISelfTest に「旧式 (AnchorOrigin+オフセット) と新式を 9 アンカー × 乱数で memcmp」を足す。
- 回転/スケール: `UIResolved` に 2x3 アフィン `xform` + `hasXform`。**恒等ゲート** (自分と全祖先が rotation 0 / scale (1,1) なら変換を一切通さない) で既存経路に触れない。HitTest は点を逆変換してローカル軸平行判定、Renderer は `UIRenderer.cpp:286` の px 変換直後で 4 頂点に適用。シザーは回転矩形の AABB (制限として明記)。sin/cos は `<cmath>` float 版 (`BuildSimWorldContext` の `std::tan` と同じ扱い)。
- ABI `SetUIRect` / `SetUILayout` は署名不変のまま RectTransform へ書く (9-grid → プリセット、x/y → anchoredPosition、w/h → sizeDelta、space → basis、負値 keep も維持) = bump 不要。RectTransform を持たない UIElement は既定 RectTransform 相当で解決 (落とさない)。
- **`kComponentUiAux = 1u<<4`** を `ComponentRegistry.h:13-18` に追加。`IsUiOnlyEntity` (`UILayout.cpp:37-58`) は「基本 4 種 + FileId/Active/Prefab* + ScriptState + UiAux」を許容。PartSelfTest 同型で「`UI` で始まる or `RectTransform`/`Canvas` の登録型は全部 UiAux」を機械検査。

### B. 入力 = サーフェス記録 + 文字キュー + drag 状態 (M75b、版 bump 集約)
- `InputSnapshot` (`Platform/Input.h:29-75`) の `mouseCanvasX/Y, canvasW/H` を **`mouseSurfX/Y` (float、ゲーム面 px) + `surfW/surfH` (int32)** に置き換え、`uint16_t chars[8] + uint8_t charCount + pad[7]` を末尾に追加 → **88 → 112B**。キャンバスごとの座標は sim 内で `CanvasSize(surfW,surfH,desc)` と `mouseSurf/scale_c` の純関数で導く (描画側と**同じ関数・同じ引数**)。Runtime では surface = クライアント矩形なので既定キャンバスのマウス座標は旧 `mouseCanvasX` と同ビット。ヘッドレス/合成入力は 1920x1080 固定、0 = 未確定 → 基準解像度へ倒す規則は据え置き。
- `WM_CHAR` は `Win32Window.cpp:106` の `TranslateMessage` で既に届いている。`Input::HandleMessage` (`Input.cpp:27-100`) に case を足すだけ (制御文字 <0x20 とサロゲートは v1 では捨てる、IME は非対応)。`CaptureSnapshot` がレーン 0 で消費して 0 に戻す (wheel と同規約)。`SynthLaneInput` はレーン 0 に決定論の文字列を周期的に載せる (被覆)。
- `UIInteractionState` (`UIInteraction.h:25-50`) に `changed` EntityID (値が変わった tick だけ立つ、clicked 同型) / `pressSurfX/Y` / `prevSurfX/Y` Float を追加。WorldHasher UI 節 (`WorldHasher.cpp:430-446`) / SimSnapshot Scene 節 (`SimSnapshot.cpp:58-67,128-132`) / `Clear()` に同時反映。
- `Evaluate(world, in, prevIn, actions, state)` に前 tick 入力を渡す。**`TickRunner.cpp:254-256` で `prevTickInput` は Evaluate (`:266`) より前に上書きされる**ので、上書き前にコピーを取る。
- `UINavCancel` アクション (`assets/input/actions.json`: `Escape` / pad `B`)。`UIInteraction.h:57-61` に `kActionNavCancel`。
- 版: `.rep` 7→8 / `kSimSnapshotVersion` 17→18 / `kNetProtoVersion` 4→5 (`NetSession.cpp:396` memcpy のサイズ追従、`:73` の照合を surface 由来へ、fingerprint に `referenceW/H` と**フォント計測表ハッシュ**の欄を確保)。`Replay.cpp:144-166` の `--rep-diff` 欄名、`InputOverride` (文字は上書きしない)、`EngineApiTable.cpp:45-46` `UiCanvasOf` / `MouseCanvasPos` をサーフェス経由に。

### C. Canvas (M75c)
- `UICanvasComponent` (NoHash + UiAux、52): `referenceW/H` Int32、`scaleMode` Int32 (0 Expand=min / 1 Shrink=max / 2 MatchWidthOrHeight)、`match` Float、`sortOrder` Int32。renderMode は Overlay のみ (World キャンバスは既存のワールド追従 UI で代替)。**ConstantPixelSize は実装しない** — 「キャンバス寸法はアスペクトだけの関数」を破りヒットテストが画素数依存になる。Match は `canvasW = rW^(1-m) · rH^m · aspect^m` (対数空間 lerp)。
- Canvas の無い要素は**暗黙の既定キャンバス** (project_settings の基準解像度 + Expand) = 現行と完全互換。`CanvasSize(w,h)` は既定版として残し `CanvasSize(w,h,const CanvasDesc&)` を正本に。入れ子 Canvas 非対応 (最寄り祖先が勝つ)。Canvas 要素自身の矩形は常に (0,0,cw,ch)。
- 描画/ヒットのキー = `(canvas.sortOrder, element.order, entity.index)`。Renderer 昇順、HitTest 最大。px 変換はキャンバスごとの scale。
- 基準解像度: `assets\project_settings.json` に `"ui": {"referenceW":1920,"referenceH":1080}`。読みは EngineLoop 起動時 (particleBackend と同じ場所)、書きは `ProjectSettingsWindow.cpp` に UI 節。`uilayout::kCanvasRefW/H` は既定値に格下げ、実効値は起動時 1 回だけ書く静的 (sim 中不変)。.rep には載せない (actions.json と同じ扱い)。ネットの fingerprint に載せる。

### D. フォント計測表アセット (M75d、ユーザー決定)
- 目的: Layout / Fitter / HitTest がテキスト幅を要するが、実グリフ幅は機種の TTF 依存で sim に入れられない。**プロジェクトフォントの advance 表をコミット済みアセットにして sim が読む**。
- `assets\fonts\<font>.fontmetrics.json`: `{"font":"<name>","basePx":32,"lineH":<baseLineHPx を 1/256 固定小数>,"ranges":[[start,end,[adv,...]],...]}`。adv = 行高比 (1/256 固定小数、uint16)。float を JSON に書かない (整数だけ = パーサ非依存)。
- cook: `Editor.exe --cook-font-metrics [--project DIR]` と ProjectSettings の UI 節にボタン。FontAtlas と同じ選択規則 (`assets\fonts\*.ttf/.ttc` 名前順の先頭) で stb_truetype を開き、0x20..0xFFFF を `stbtt_FindGlyphIndex` で走査して存在するものだけ書く。生成物は **cache ではなく assets (コミット対象)**。
- 読み: `uitext::FontMetrics` (新規 `Engine/UI/UITextMetrics.h/.cpp`、純データ・D3D 非依存)。「プロジェクト → エンジンリポジトリ」の 2 ルート解決 (`FindEngineShaderDir` と同型)。**表が無い/文字が表に無いときは固定メトリクス** (ASCII 0.5 行高 / それ以外 1.0 行高 × fontScale) へ倒す = エンジンリポジトリ (assets\fonts 無し、`--font-embedded`) はこの経路で golden を撮る。
- `uitext::Measure(text, fontScale, wrap, maxW, metrics)` = Layout 用の幅・高さ・行数。`static_assert(uitext::kLineH == FontAtlas::kUILineH)`。**Renderer の既存折返し (実グリフ幅) は変えない** (golden 不変。Group/Fitter の無いシーンは経路が 1 バイトも変わらない)。
- ネット: 表のハッシュ (FNV、ファイル内容) を fingerprint に載せる (M75b で欄を確保、値はここで実装)。表が違う 2 台は `NetReject::FontMetrics`。
- 選択されたフォントと表の `font` 名が違うときは WARN 1 行 (箱と文字がずれる旨)。

### E. Layout Group (M75e、最大論点)
- **純関数 + 呼び出し単位のメモ (`uilayout::LayoutScratch`)** を採る。tick キャッシュ案はフレームレートの Renderer とワールド追従 UI の補間 (prevWorld/alpha) と噛み合わず、駆動 (子の RectTransform へ書く) 案は「駆動値を保存しない印 / Inspector read-only / Undo 干渉 / 非 Play 中の走らせ役」の 4 つを抱えるので却下。
- `LayoutScratch` = entity.index → {min,pref,flex}×2 軸の `std::vector` 線形探索 (unordered を使わない = 規則 7)。Renderer はフレームごと、HitTest は tick ごとに新規。`nullptr` の単発呼び出し (GetUIRect 等) はローカル生成。結果は変えず再計算を省くだけなので snapshot に載せるものは無い。
- 兄弟順のキーは Hierarchy の `firstChild → nextSibling` 連鎖 (= Hierarchy 窓の順 = 保存順)。**SimSnapshot に載る** (`World::SnapshotWrite` がアーキタイプ列を生バイトで書く、`SimSnapshot.cpp:431`) が **WorldHash には載らない** (`WorldHasher.cpp:181`)。壊れれば hovered/pressed で表面化する (UIElement 幾何と同じクラスの NoHash 入力)。非 Active と `ignoreLayout` の子は飛ばす。
- コンポーネント (NoHash + UiAux): `UILayoutGroupComponent` (53: kind H/V/Grid、padding Float4、spacing Float2、childAlignment、controlChildWidth/Height、forceExpandWidth/Height、reverseArrangement、Grid 用 cellSize/startCorner/startAxis/constraint/constraintCount) / `UILayoutElementComponent` (54: ignoreLayout、min/preferred/flexible W/H (負 = 未指定)、layoutPriority) / `UIContentSizeFitterComponent` (55: horizontalFit/verticalFit 0 unconstrained / 1 min / 2 preferred)。AspectRatioFitter は入れない。
- アルゴリズムは Unity と同じ 2 パス (幅→高さ、preferred は bottom-up、配分は top-down)。`PreferredSize(e)` = LayoutElement 指定 > Group なら子集計 + padding/spacing > テキストなら D の Measure > sizeDelta。配分 = min 保証 → preferred まで比例 → flexible 重み。
- Unity の「駆動プロパティ read-only」は「Inspector に解決済み矩形を read-only 表示 + Rect Tool でハンドルを出さない」で代替。

### F. Selectable / ウィジェット (M75f, g, h)
- `UISelectableComponent` (56、NoHash + UiAux): `interactable`、`transition` (None/ColorTint/SpriteSwap)、5 状態色 + `colorMultiplier`、4 sprite AssetRef、`navigationMode` (None/Horizontal/Vertical/Automatic/Explicit)、`selectOnUp/Down/Left/Right` EntityRef、`targetGraphic` EntityRef。Selectable 無しボタンは現行ハードコード (hover ×1.25 / press ×0.8) のまま = golden 不変。フォーカス候補 = `focusable != 0` または `navigationMode != 0`。`interactable == 0` はヒットを吸うが pressed/clicked/focus を立てず disabledColor。
- ウィジェット状態は **ハッシュ対象の別コンポーネント** (通常フラグ + UiAux): `UIToggle` (57: isOn, graphic EntityRef, group) / `UISlider` (58: value/min/max, wholeNumbers, direction, fillRect/handleRect) / `UIScrollRect` (59: content, horizontal/vertical, scrollX/Y, wheelStep, scrollbar ×2。慣性無し) / `UIDropdown` (60: value, expanded, captionText, list) / `UIInputField` (61: text String256、caret、selectionAnchor、editing、characterLimit、contentType、textGraphic/placeholder、repeatTicks)。union 型 1 個は却下 (FieldDesc の反映でハッシュ/シリアライズ/Inspector が無償なのは型ごとに分けたときだけ)。**M60′ の Cloth/SoftBody 予約は 62/63 へ繰り下げ** (CLAUDE.md を M75j で更新)。
- `uiwidgets::Update()` を `uiinteract::Evaluate` の末尾で呼ぶ (スクリプト層より前)。**バブリング**: clicked/pressed の要素から祖先を辿って最初のウィジェットが受ける。
- Dropdown の展開リストは**常設の子** (`list`)。`expanded==0` のとき Renderer/HitTest/FocusNav が純規則 `IsUiHidden(e)` で飛ばす (tick 内 Spawn/Destroy 不要)。展開中は描画/ヒットのキーに `+10000` の order バンプ (両側同じ規則)。
- InputField: `text` は WorldHasher が 256B 全部畳むので**エンジン側の編集でも `ZeroStringTail` 規約** (`InspectorWindow.cpp:265-272` の罠)。表示は `textGraphic` の `UIElement.text` へ毎 tick ミラー (focused ミラーと同型。スクリプトが SetUIText で書いても次 tick で戻る、と spec に書く)。Backspace/Delete/←/→/Home/End は keys のエッジ、リピートは `repeatTicks` (30 tick 待ち → 3 tick 刻み)。キャレット位置は D の Measure で決める。

### G. ABI v18 (M75h、1 回)
- 12 スロット追加 (111 → 123、`check_rules.ps1` の `$apiVersionSlots` に `18 = 123`): `SetRectTransform(id, const MyeRectTransform*)` / `SetToggle` / `GetToggle` (-1 = 非所持) / `SetSlider` / `GetSlider` / `SetScroll` / `GetScroll` / `SetDropdown` / `GetDropdown` / `SetInputText` / `GetInputText(id, buf, cap)` (GetSceneName 規約) / `SetInteractable`。`UIButtonState` に bit4 `kDragging` / bit5 `kValueChanged` (スロット不変)。
- 線引き: ハッシュ対象のウィジェット状態は**読める** (Get 5 本は C# へも公開)。UIElement/RectTransform/Selectable の見た目は write-only のまま (GetRectTransform は作らない)。**C# からは Set* を全部閉じる** (`MyeScript.cs:83-93` の SetUIRect と同じ理由)。`Interop.cs` は位置ミラーなので 12 本並べる。外部プロジェクト (HAL Collector) は Rebuild Scripts が要る。

### H. Rect Tool (M75i)
- `EngineContext.gameSurface` {valid, offX, offY, dispW, dispH, rtW, rtH} を GameViewWindow が毎フレーム書き、`EngineLoop.cpp:1358-1374` の入力確定が `InputSurface` に変換して `CaptureSnapshot` へ渡す (無効なら swapChain)。**これで「GameView 矩形 → キャンバス座標の換算が無い」(`EngineLoop.cpp:1364-1367`) が sim 側ごと直る**。換算の正本は `uilayout::SurfaceToCanvas / CanvasToSurface` の 1 対。
- ハンドル: 4 隅 + 4 辺 + 中央ドラッグ + アンカー 4 点 + pivot。逆算は `uilayout::RectToTransform(rect, parentRect, anchors, pivot)` (純関数、往復検査)。アンカー/pivot ドラッグは矩形を保ったまま逆算 (Unity 挙動)。Shift/Alt 修飾は v1 では無し。
- Undo: 押下で `BeginRecord + CaptureBefore`、離しで `CaptureAfter + EndRecord` (`InspectorWindow.cpp:938-949` の 9-grid ピッカーと同型)。Escape でキャンセル。LayoutGroup に駆動される要素と Canvas 要素はハンドル無し・点線 + ツールチップ。Play 中の編集は Inspector と同じ経路 (M72a/M73a の Fork に乗る)。

## サブ分割 (実行順。TypeId は登録順なのでコンポーネントを登録するサブは直列)

| サブ | 内容 | 版 | golden |
|---|---|---|---|
| **M75a** | RectTransform + 自動変換 + Inspector + SetUIRect 再実装 + `kComponentUiAux` | Scene 3→4 | 不変 (動いたら A の加算順ミス) |
| **M75b** | 入力拡張: サーフェス記録 / 文字キュー / drag 状態 / Cancel / fingerprint 欄 | .rep 8 / snap 18 / net 5 | 不変 |
| **M75c** | Canvas + Scaler 3 モード + 基準解像度 project_settings 化 + `--ui-demo` 骨格 + golden 25 枚目 `ui_widgets` | — | `ui_widgets` 新規 |
| **M75d** | フォント計測表アセット (cook CLI/ボタン + ローダ + `uitext::Measure` + 固定メトリクス fallback + net fingerprint 値) | — | 不変 |
| **M75e** | Layout Group / LayoutElement / ContentSizeFitter + `LayoutScratch` | — | `ui_widgets` 更新 |
| **M75f** | Selectable + Toggle + Slider + バブリング + `changed` + replay 8 ペア目 `--ui-demo` (決定論の入力台本) | — | `ui_widgets` 更新 |
| **M75g** | ScrollRect + Dropdown + `IsUiHidden` | — | `ui_widgets` 更新 |
| **M75h** | InputField + ABI v18 (12 本) + C# ミラー + `UiWidgetsDemo.cpp` | ABI 17→18 | `ui_widgets` 更新 |
| **M75i** | Rect Tool + GameView → サーフェス換算 (**M75a 以降なら並列 worktree 可**) | — | 不変 |
| **M75j** | `--ui-demo` 最終形 (`ui_widgets` / `ui_widgets_16x10`) / engine_spec §6.11-6.15 / ADR-020 / README / CLAUDE.md / dogfooding.md | — | 確定 |

### M75a — 触るファイル
`Core/Components.h/.cpp` (51 登録、UIElement から 6 フィールド削除) / `Core/ComponentRegistry.h` (UiAux) / `UI/UILayout.h/.cpp` (ResolveImpl、xform、IsUiOnlyEntity) / `UI/UIInteraction.cpp` (HitTest 逆変換) / `UI/UIRenderer.cpp` (頂点変換、シザー AABB) / `Engine/SceneSerializer.cpp` (`MigrateLegacyUiRects`) / `Engine/Scene.h` (v4) / `Engine/DemoContent.cpp` (MakeUiText/MakeUiButton 等 15 か所) / `Editor/CreateMenu.cpp` / `Editor/Windows/InspectorWindow.cpp` (アンカープリセット 4x4 ピッカー、Pos/W/H ⇄ Left/Right/Top/Bottom 表示、`kEnumFields` に basis、解決済み矩形 read-only) / `Editor/EditorComponentCatalog.cpp` / `Script/EngineApiTable.cpp` / `LocalizationTable.inl` / `assets\scenes\{ui_probe,flow_title,flow_game}.scene.json` (エディタで開いて v4 保存し直す = 自動変換の実走確認)。
テスト (UISelfTest +): 旧式/新式 memcmp、ストレッチ、pivot、basis、回転 45° のヒット/非ヒット、恒等ゲート、v3 JSON からの自動変換 (SceneSerializerSelfTest)、UiAux 機械検査。検証: 共通 + golden 4 枚 `--tol 0` + replay_verify (flow シーンが v3 → 変換経路を通る)。

### M75b — 触るファイル
`Platform/Input.h/.cpp` / `Engine/EngineLoop.cpp:1358-1374` / `Replay/Replay.h/.cpp` / `Replay/SimSnapshot.h/.cpp` / `Replay/WorldHasher.cpp` / `Net/NetSession.h/.cpp` / `UI/UIInteraction.h/.cpp` / `TickRunner.cpp:254-266` / `Script/EngineApiTable.cpp` / `Replay/InputOverride.*` / `assets/input/actions.json`。
テスト: `sizeof == 112`、文字キュー 取り込み→消費→ゼロ戻し、Synth に文字が乗る、SimSnapshot 往復で新欄が戻る、WorldHasher UI 節が変化を拾う、`MouseCanvasPos` が旧値と同ビット (960x540 / 1600x900)。検証: 共通 + `net_verify.bat`。

### M75c 〜 M75h — 触るファイル (共通部)
各サブで `Components.h/.cpp` (登録) / `InspectorWindow.cpp` (`kEnumFields`) / `CreateMenu.cpp` (Create > UI に Canvas / H・V・Grid Group / Toggle / Slider / Scroll View / Dropdown / Input Field = 子構成込み) / `DemoContent.cpp` (`--ui-demo` を積み増し) / `UISelfTest.cpp`。
新規ファイル: `UI/UITextMetrics.h/.cpp` (d) / `UI/UILayoutGroup.h/.cpp` (e) / `UI/UIWidgets.h/.cpp` (f〜h) / `GameLogic/Scripts/UiWidgetsDemo.cpp` (h) / `Editor/…FontMetricsCook` (d、`--cook-font-metrics`)。
`--ui-demo` の入力台本 (`UiDemoInput(tick)`) は `--ui-demo` 時だけ InputOverride の後段で適用する決定論のマウス/キー列。`replay_verify.bat` の 8 ペア目 (f で追加)、`shot_verify.bat` の 25 枚目 (c で追加、960x540、CI tol=3)。
プレハブ往復: CreateMenu が子構成ごと作る Toggle/Slider/ScrollView/Dropdown をプレハブ化 → 展開して EntityRef (graphic/fillRect/content/list) が再マップされる検査を f/g に 1 本ずつ。

### M75i — 触るファイル
`Editor/Windows/GameViewWindow.h/.cpp` (Rect Tool トグル、`gameSurface` 書き込み、ハンドル描画/操作) / `Engine/EngineLoop.h` (`EngineContext.gameSurface`) / `UI/UILayout.h/.cpp` (`SurfaceToCanvas/CanvasToSurface`、`RectToTransform`) / `LocalizationTable.inl` / `EditorSettings`。テスト: 往復 (全アンカー種別 × pivot)。エディタ操作は一時プローブ + `--screenshot` で絵を撮って確認 (ImGui は backbuffer に載る)。

### M75j — 文書
`engine_spec.md` (§6.11 追記: サーフェス記録・3 モード・project_settings・複数キャンバス / §6.12 追記: バブリング・drag・changed・Cancel / §6.13 RectTransform と自動レイアウト / §6.14 ウィジェットと InputField / §6.15 Rect Tool / §11.3 .rep v8 / ABI 表 v18 / §12.3 の UI スケール項を消す) / `docs/adr/ADR-020-ui-layout-determinism.md` (純関数+メモ vs 駆動、計測表アセット + 固定 fallback、y 下向き、サーフェス記録、ConstantPixelSize 非採用、兄弟順キー、状態の別コンポーネント化、C# の閉じ方) / `README.md` / `CLAUDE.md` (末尾 TypeId 61、Cloth/SoftBody 62/63、検証表の枚数・ペア数、ABI v18=123、「UI コンポーネントを足す」チェックリスト = UiAux) / `docs/dogfooding.md` (HAL Collector: 初回ロードで v4 化、Rebuild Scripts、fontmetrics の cook)。

## 申し送り (計画外の事実)

- **M75a (2026-09-12)**: 着手時の作業ツリーに ImpactSound (計画 `ImpactSoundDesign.md`) の未コミット変更が
  あり、`WaveSound` が TypeId 51 を先に取っていた。RectTransform は**その後ろ**に登録した
  (コミット単体では 51、WaveSound を含む作業ツリーでは 52)。TypeId は登録順で決まりシーンは型名で
  保存するので、どちらの順で master に入っても壊れない — ただし `.rep` は跨げない (どうせ両方
  未 push)。以降の UI コンポーネントは常に「ファイル末尾へ append」で番号を固定しない。
- **M75a の golden 4 枚**: Debug の Runtime で撮って `--img-diff --tol 0` を当て、`ui_probe` /
  `ui_probe_720p` / `ui_probe_16x10` / `flow_title` が **maxDiff=0**。Release の `shot_verify.bat` と
  `replay_verify.bat` は、着手中に Release の `Editor.exe` (三校プロジェクト) が起動していて
  `bin\x64\Release` がロックされ**回せていない** — エディタを閉じてから回すこと (M75b の頭で)。
- **旧シーンの v4 化**: `assets\scenes\ui_probe.scene.json` は Python で v4 へ書き換えた (nlohmann の
  `dump(2)` と同じ並び = sort_keys + indent 2)。`flow_title` は gitignore の生成物で `--flow-demo` が
  毎回 v4 で書く。ロード時変換 (v3 → RectTransform) は UISelfTest の「旧形式シーンのロード」が固定する。
- **basis の仮置き**: `ReadEntityComponents` は親リンク前に走るので `basis=1` (キャンバス) で仮置きし、
  `LoadFromJson` の末尾で「UI 祖先なし → basis=0」に戻す。ApplyPartial (プレハブ展開) 経由は
  仮置きのまま = 旧挙動と同値なので実害なし。
- **RectTransform の既定値は旧 UIElement 互換 (左上・pivot 0・160x40)**。Unity 風 (中央) は
  CreateMenu が明示的に書く。理由は `Components.h` の RectTransform のコメント。
- **anchoredPosition は pivot 点の位置** (Unity と同じ)。pivot (0.5,0.5) の要素を (100,100) に置くと
  中心が (100,100) に来る — 自己検査を書いたときに 1 度踏んだ。

## 各サブに共通する罠
- `IsUiOnlyEntity` の許容漏れ (UiAux で構造的に潰す)。
- `ZeroStringTail` (InputField の text)。
- `prevTickInput` の上書き順 (`TickRunner.cpp:254`)。
- 新 UI コンポーネントの EntityRef はプレハブ内参照の再マップを通ること (f/g で検査)。
- golden `flow_title` は C# レーンが操作説明を暗くする — `MyeScripting.dll` Release を先に作る (`tools\build_managed.bat Release`)。
- `--frames 6` の撮影 run では GpuTimer が 0 を返す (計測は別 run)。
- `replay_verify.bat --job` を手で叩くときは `chcp 437` 前置 (CLAUDE.md)。

## 検証 (全体)
- 各サブ: 共通検証 (ビルド 0 警告 / `--selftest` / `check_rules` / `shot_verify` / `replay_verify`) + 該当サブの `net_verify` (b, h)。
- golden 4 枚の `--tol 0` は a〜j の全サブで手で当てる。
- 最終 (j): `--ui-demo` を Runtime.exe で実走し、マウス/キー/パッドで Toggle / Slider / Scroll / Dropdown / InputField を触る。外部プロジェクト (HAL Collector) を `--project` で開き、v3 シーンが自動変換されて UI が同じ位置に出ること + Rebuild Scripts 後にスクリプトが動くことを目視。
