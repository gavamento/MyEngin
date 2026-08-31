# M64: ゲーム内 UI の操作系 — キャンバス統一 / UI イベント / スクリプト⇄オブジェクトの穴埋め

**再開手順**: `git log --oneline -5` で最後に完了した M64x を確認 → 本ファイルの進捗表と突き合わせ →
次のサブの節を読む → 着手前にそのサブの「冒頭確認」を先に潰す。
1 サブ = 1 コミット (`M64a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルの進捗表には**計画外の事実・罠・申し送りのみ**書く。

> ★実装セッションの最初の作業: **本ファイルをリポジトリの `plans\<3語のslug>.md` へコピーして
> コミットに含める** (家風。`plans\vivid-spinning-ember.md` (M63) と同じ体裁)。

---

## Context

ユーザー報告: **「スクリプトとインスペクター/オブジェクトの関連が今のところ少なく、
スクリプトからボタンを検知したりができない」**。

現 HEAD (M63e 完了、ABI v14 = 102 スロット) で実測した結果、報告は 3 つの独立した構造欠落だった。

### 欠落 1 — UI ボタンは「押せる場所」と「見える場所」が座標系ごと食い違う

| レーン | 解決する解像度 | 場所 |
|---|---|---|
| 描画 (`UIRenderer`) | **クライアント実 px** | `UIRenderer.cpp:267` ← `EngineLoop.cpp:1569` が `swapChain.Width()/Height()` を渡す |
| ヒットテスト (`UIHitTest`) | **1920x1080 固定** | `EngineApiTable.cpp:773-774` |
| フォーカスナビ (`UIFocusNav`) | **1920x1080 固定** | `EngineApiTable.cpp:421-422` |
| マウス (`MousePos`) | **クライアント実 px** | `EngineApiTable.cpp:80-83` ← `InputSnapshot.mouseX/Y` (`Input.h:21-22`) |

960x540 で `anchor=4` の要素は、描画では基準点 (480,270)、ヒットテストでは (960,540)。
**両者を繋ぐ「画面サイズ」の ABI スロットは 102 本のどこにも無い。**
結果、`UIHitTest` は C++/C# 両方に配線済みなのに**ゲームスクリプトからの実使用が 0 件** (grep 実測) で、
唯一動く経路は `UIButtonDemo.cpp:8-17` の「UIElement と同じ矩形をスクリプト側にもう一度手で書く」
= anchor=0 かつ 960x540 専用。`ScriptAPI.h:448` のコメントが制限を自白している。

原因は M51 の判断 `plans\luminous-cooking-nygaard.md:59`
「Canvas スケーリング見送り (ウィンドウ実寸読取は恒久禁止事項に抵触)」。

### 欠落 2 — 押下状態はエンジン内で計算されているのに捨てられている

`UIRenderer.cpp:292-300` は hover/press ハイライトを**自分で計算して描いている**が表示専用。
sim には出ない。`focused` も表示専用でスクリプトが書く前提 (`Components.h:400-401`)、
`UINav::FindNext` (`UINav.h:23`) は純関数として置いてあるだけで**誰も駆動していない**。

### 欠落 3 — 汎用フィールドアクセスが「描画コンポーネント」を丸ごと弾いている

`GetComponentField`/`SetComponentField` (v11) は登録済み全コンポーネントを名前ハッシュで
読み書きできる汎用口だが、入口に `if (desc.flags & kComponentNoHash) { return 0; }`
(`EngineApiTable.cpp:691`, `:713`) がある。この 1 行のせいで **9 コンポーネントが実行時に
1 バイトも触れない**:

`SkinnedMesh` / `SpriteRenderer` / `TrailRenderer` / `Skybox` / `Fog` / `CameraPostFx` /
`Terrain` / `Decal` / `ReflectionProbe`

**演出でフェード・被弾フラッシュ・ヒットストップの絵を作る口 (`CameraPostFx` 40 フィールド) が
一切無い**のはこれが理由。逆に、ここを開けるだけで新スロット 0 本で全部が動く。

### 出口の姿

- ボタンは **UIElement を EntityRef で指すだけ**でクリックが取れる。矩形の手写しは無くなる。
- **光って見えるボタンと反応するボタンが構造的に必ず一致**する (描画とヒットテストが同じ解決式)。
- パッド/キーボードでメニューが操作できる (`UINav` をエンジンが駆動)。
- 演出系コンポーネントがスクリプトから動かせる。
- スクリプトのフィールドがインスペクタで**日本語表示名 + スライダ範囲 + ツールチップ**を持つ。
- **既存 17 枚の golden は 1 枚も絵が変わらない** (maxDiff=0)。

### ユーザー決定 (2026-08-31)

1. 座標系 = **1920x1080 キャンバスに統一** (描画もスケールする)
2. 配り方 = **状態 + コールバックの両方**
3. パッドフォーカス = **今回の範囲に入れる**
4. golden = **シーン数値を 2 倍して maxDiff=0 を保ち、別サイズの新規 1 枚を足す**
5. 粒度 = **3 サブ = 3 コミット**。番号は **M64** (M63 完了済み、M64 は未使用)

---

## 全体設計 (5 つの判断と根拠)

### 判断 1 — キャンバスは **1920x1080**。sim 側は 1 行も変えない

`UIHitTest` / `UIFocusNav` は**既に 1920x1080 で解決している**。キャンバスを同じ 1920x1080 に
選ぶと、**変えるのは描画側だけ**になり、3 者が自動的に一致する。

「ウィンドウ実寸読取は恒久禁止」に抵触しない根拠: 禁止対象は **sim レーン (WorldHash 対象)**。
`UIRenderer` は既に `EngineLoop.cpp:1569` で `swapChain.Width()` を読んでいる = 描画レーンでの
実寸読取は現に行われている。今回もそこから 1 歩も出ない。

### 判断 2 — マウスだけが機種依存。**記録側 (Platform 層) でキャンバス座標を作る**

sim が触れる唯一の実寸依存値がマウス px。これを **`Input::CaptureSnapshot` の中で
キャンバス座標へ正規化して `InputSnapshot` に載せる**。`WM_MOUSEMOVE` の `lParam` は既に
クライアント座標 (`Input.cpp:36-39`) で、ここが**実寸を読んでよい唯一の正当な場所**。

これで sim は「記録済み入力の純関数」のままになり、`.rep` を別解像度で再生してもビット一致する。

**raw px は消さずに残す** (`mouseX/mouseY` は現状維持、`mouseCanvasX/mouseCanvasY` を追加)。
消すとマウスルック感度が解像度で変わる副作用が入る — 今回の依頼と無関係な挙動変更はしない。

```
InputSnapshot: 64B → 72B   (int32 mouseCanvasX, mouseCanvasY を末尾に追加)
```

### 判断 3 — 版 bump は **M64a の 1 回だけ**に束ねる

家風 (`SimSnapshot.h:66-68` の M63a 注記、`plans\wistful-tumbling-lantern.md:650-657` 決定台帳 3)。
M64b/M64c で使う枠も M64a で確保する。

| 版 | 現行 | 新 | 場所 |
|---|---|---|---|
| `kReplayFileVersion` | 4 | **5** | `Replay.h:35` |
| `kSimSnapshotVersion` | 7 | **8** | `SimSnapshot.h:69` |
| `MYE_API_VERSION` | 14 | **15** | `EngineAPI.h:65` (M64b で bump。`$apiVersionSlots` と同時) |

**コミット済みの `.rep` は 1 本も無い** (`git ls-files | grep .rep` = 空)。golden は PNG のみ。
旧ファイル互換を考える必要は実質ゼロ。

### 判断 4 — ボタン状態は **tick 頭に確定し、描画もそれを読む**

置き場所は `TickRunner.cpp:247` の `inputActions.Evaluate` の**直後**。理由は同じ形だから —
「記録済みスナップショット 2 枚の純関数なので record/verify に透過」(`TickRunner.cpp:244-246`)。
ここで確定した状態を:

- スクリプトへ → `OnUIClick` コールバック + `UIButtonState` ポーリング ABI
- `UIRenderer` へ → **ハイライトを自前計算せずこの状態を読む** (`UIRenderer.cpp:295-300` を置換)

これが「見える所と反応する所が一致する」の**構造的な**根拠になる (M51e が描画とヒットテストを
`uilayout` 1 本に寄せたのと同じ手口)。

★**C# レーンから UI 幾何を書く口を閉じる**。sim がボタン矩形を読むようになるので、
record/verify 中に走らない C# が矩形を動かすと**タイムトラベル再シムでだけ結果が変わる**
(UIElement は NoHash なのでハッシュでも捕まらない = 最悪の壊れ方)。
`MyeScript.cs:49-53` の `SetUIRect`/`SetUILayout` を削除する — **実使用は 0 件**
(`assets\scripts\FlowMenu.cs` は `SetUIColor` だけ) なので代償ゼロ。色/テキスト/塗り率は
sim が読まないので C# から書けるまま残す。

### 判断 5 — golden は **数値 2 倍で maxDiff=0**、被覆は 1280x720 の 18 枚目で足す

撮影は `--width 960 --height 540` 固定 (`shot_verify.bat:78`) = キャンバス 1920x1080 に対し
**s = 0.5 ちょうど**。UI の `x/y/w/h` と `fontScale` を 2 倍すれば

```
rx = (2 * x) * 0.5 + 0 == x          (IEEE754 で厳密。2 の冪なので丸め無し)
textScale = (2 * fontScale) * 0.5 == fontScale
```

で**既存 2 枚が maxDiff=0 のまま通る**。UI が 1 個も無い残り 15 枚は無風。

★**スケールする px とスケールしない px を分ける**。ここを間違えると golden が割れる:

| 値 | 扱い | 根拠 |
|---|---|---|
| `x` / `y` / `w` / `h` | **キャンバス px = `* s`** | レイアウトそのもの |
| `fontScale` | **`textScale *= s`** | 文字の大きさもレイアウトの一部 |
| `sliceBorder` (9-slice) | **キャンバス px = `* s`** → シーン側も 2 倍 | 枠の相対的な太さを保つ |
| フォーカス枠 `kRing = 2.0f` (`UIRenderer.cpp:342-351`) | **スケールしない (実 px 据え置き)** | ①`ui_probe.scene.json:943-944` に `focusable=1, focused=1` の要素が **1 個ある** — 掛けると 2px→1px で **maxDiff=0 が崩れる**。②`UIRenderer.cpp:320` が既に「枠の太さは見た目の一貫性を優先し距離スケール対象外」と同じ判断をしている |

`ui_probe` に `sliced != 0` の要素は 0 件 (実測) なので、9-slice の判断は golden には出ない
= **`Build9Slice` の被覆は `UISelfTest` 側で足す**こと。

ただし s=0.5 だけでは**スケール経路そのものが検証されない**ので、
**`ui_probe` を 1280x720 (s = 2/3) でもう 1 枚撮る** (18 枚目)。fontScale を 2 倍してあるので
8x8 グリフは `8 * 2 * 0.667 = 10.7px` = **拡大側**になり、潰れない。

---

## サブ計画

### M64a — キャンバス統一 + 入力の記録  ★ここだけが .rep / snapshot の版と golden を動かす

**冒頭確認**: `Input` クラスは `Win32Window` を持たない (`Input.h:44-70`)。
`CaptureSnapshot(lane)` にクライアント実寸を引数で渡す形に変える必要がある。
呼び出し元は `EngineLoop.cpp:1103-1121` (ライブ) の 1 箇所。

**触るもの**

| ファイル | 作業 |
|---|---|
| `src\Engine\Engine\UI\UILayout.h` | `uilayout::CanvasTransform(int screenW, int screenH) -> {float s, ox, oy}` を追加 (純関数、inline)。`s = min(w/1920, h/1080)`、`ox/oy` は中央寄せ (レターボックス)。**`Resolve`/`ResolveRect`/`ResolveClipRect`/`ResolveVisibleRect` のシグネチャは 1 文字も変えない** — 変えると `UISelfTest` の 40 check が全部書き換えになる |
| `src\Engine\Engine\UI\UIRenderer.cpp` | ①`:267`/`:277-278` へ `kCanvasW/kCanvasH` を渡す ②先頭で `CanvasTransform` を取り、`rect`/`clip` を実 px へ写す小関数を 1 つ ③`:274` `textScale *= s` ④`:282-285` のクリップ判定をキャンバス基準へ ⑤`:260` `fullScissor` / `:381` `inv` / `:389-393` viewport は**実 px のまま** → **`assets\shaders\ui.hlsl` は無変更** |
| `src\Engine\Platform\Input.h/.cpp` | `InputSnapshot` へ `int32_t mouseCanvasX/mouseCanvasY` を末尾追加 (64→72B)、`static_assert` を 72 へ。`CaptureSnapshot(lane, clientW, clientH)` で `CanvasTransform` の逆変換を掛けて充填。`SynthLaneInput` (`:130-165`) にも**非ゼロの決定値**を入れる (入れないと `job_mp` が新フィールドを 1 ビットも検査しない) |
| `src\Engine\Engine\Replay\Replay.h` | `kReplayFileVersion` 4→5 + 版履歴コメント |
| ★`src\Engine\Engine\Replay\Replay.cpp:135-159` | **`FirstDifferentInputField` に `mouseCanvasX/Y` の比較行を追加**。忘れると `--rep-diff` と `net_verify` が「新フィールドだけ違う 2 本」を `identical` と報告する = **決定論検査そのものが嘘をつく** |
| `src\Engine\Engine\Replay\SimSnapshot.h` | `kSimSnapshotVersion` 7→8 + 版履歴。LOP 節は `sizeof` 依存なので自動追従 |
| `src\Engine\Engine\Net\NetSelfTest.cpp:111-114` | `sizeof(InputSnapshot) == 64` → 72、`kNetMaxPacket == 64 + 8*72 = 640B` (MTU 1400B 以内、`kNetRedundancy=8` 据え置きで可) |
| `src\Editor\Windows\GameViewWindow.cpp:120-142` | 選択 UI のアウトラインも `CanvasTransform` を通す (`ResolveRect` に RT 実寸を渡す前提が壊れる) |
| `assets\scenes\ui_probe.scene.json` | UIElement 16 個の `x/y/w/h/fontScale/sliceBorder` を**全部 2 倍**。972 行だが機械的 |
| `src\Engine\Engine\DemoContent.cpp:563-568, 633-655, 739` | `BuildFlowTitleScene` / `BuildFlowGameScene` / `BuildNetDuelScene` の UI 数値を 2 倍 |
| `src\Editor\CreateMenu.cpp:113-153` | UI 生成の既定サイズを 1920x1080 想定へ (パネル 240x160 → 480x320 等) |
| `src\Engine\Core\Components.cpp:254-266` | UIElement の px フィールドのツールチップに「キャンバス 1920x1080 基準」と明記 |
| `tools\shot_verify.bat` | 18 枚目 `ui_probe_720` (`--width 1280 --height 720`) を追加。撮影条件行のコメントを更新 |
| `src\Engine\Engine\UI\UISelfTest.cpp` | 新規 check: `CanvasTransform` の 16:9 / 4:3 / 1:1 / 極端アスペクト、キャンバス↔実 px の往復 |
| `engine_spec.md` / `docs\adr\ADR-014` / `CLAUDE.md` | 撮影枚数 (17→18) と UI 座標系の記述 |

**検証**: 両構成 0 警告 / selftest 全 PASS (特に #6 UI, #25 SimSnapshot, #26 TimeTravel,
#27 CrashRing, #29 Net) / replay_verify 9 ジョブ PASS /
**shot_verify で既存 17 枚が maxDiff=0** (ここが本サブの主張) + 18 枚目を `--update` で採取。

---

### M64b — UI イベント (状態 + コールバック) + フォーカス駆動 + ABI v15

**冒頭確認**: `ScriptHost` と `ManagedHost` が**別々の api コンテキストを持つか**を先に確認する
(`ScriptHost.h:107` / `ManagedHost.h:144` / `EngineApiTable.h:74`)。分かれているなら
「C# レーンだけスロットを拒否する」が 1 フラグで書ける。分かれていないなら
`MyeScript.cs` から糖衣を削るだけに留める (`Interop.cs` の `Engine` は `internal` で
ユーザースクリプトから直接呼べないため、実効的には同じ)。

**新規**: `src\Engine\Engine\UI\UIInteraction.h/.cpp`

- 状態: `hovered` (EntityID) / `pressed` (EntityID) / `focused` (EntityID) の 3 本。
  **置き場所は `Scene` の TimeControl の隣** — SimSnapshot の Scene 節に入り、
  WorldHasher の対象にもなる (= ハッシュに載る = replay_verify が配線を検査できる)。
- 評価は `TickRunner.cpp:247` の `inputActions.Evaluate` 直後。入力は
  `ctx.Input().mouseCanvasX/Y` + 前 tick の値 + `uilayout::ResolveVisibleRect(w, e, 1920, 1080)`。
- 押下判定: `press` = ボタン上で押した瞬間、`click` = **押した要素の上で離した瞬間**
  (Unity と同じ意味論。押しっぱなしで外へ出て戻ると成立)。
- フォーカス: アクションマップ名 `UINavUp/Down/Left/Right/Submit` を引き、
  `UINav::FindNext` (`UINav.h:23`) を駆動して `focused` を更新。`Submit` は `focused` に対する
  click として同じ経路へ流す。`UIElement.focused` (表示専用) はエンジンが書き戻す。

**ABI v15 (`EngineAPI.h` 末尾へ append)**

| スロット | 用途 |
|---|---|
| `uint32_t (*UIButtonState)(void* engine, MyeEntityId id)` | bit0=hovered 1=pressed(押下中) 2=clicked(この tick) 3=focused |
| `MyeEntityId (*UIGetFocused)(void* engine)` / `int (*UISetFocused)(void* engine, MyeEntityId)` | フォーカスの取得/設定 |
| `void (*MouseCanvasPos)(void* engine, int32_t* x, int32_t* y)` | キャンバス座標のマウス |
| `int (*GetUIRect)(void* engine, MyeEntityId, float* x, float* y, float* w, float* h)` | **解決済みキャンバス矩形**。UI 唯一の読み取り口。「C++ sim レーンのみ」と明記 |

**コールバック** (`ScriptTypes.h` の `MyeScriptDesc` 末尾へ append → `MYE_API_VERSION` bump が必須)

```cpp
void (*onUIClick)(void* state, MyeUpdateContext* ctx, MyeEntityId button);
```
`ScriptAPI.h` に `GetUIClickFn<T>()` を `requires` 検出付きで追加 (`GetCollisionExitFn` `:137-147` が雛形)、
`MakeDesc` で結線、`ScriptHost.h:87-91` / `ScriptHost.cpp:219-223` (バインド) /
`:236-240` (orphan 時 nullptr — **忘れると解放済み DLL を呼ぶ**) / 配信関数を追加。

**同時にやる**
- `UIRenderer.cpp:295-300` のハイライト自前計算を削除し、`UIInteraction` の状態を読む。
- `MyeScript.cs:49-53` の `SetUIRect`/`SetUILayout` を削除 (判断 4)。
- `tools\check_rules.ps1:514` の `$apiVersionSlots` へ **`15 = 107`** を追加 (102 + 上表の 5 本。
  **bump と同時**。片方だけだと規則 11-c で止まる。スロットを増減したらこの数も直す)。
- `src\Editor\PartSelfTest.cpp:177` の `MYE_API_VERSION == 14u` → `15u`、
  `:179-211` の充填チェックへ v15 の 5 スロットを追加。
- `src\Scripting\Interop.cs` の `MyeEngineApi` へ**同じ位置**に 5 本 append (位置ミラー)。
- `src\GameLogic\Scripts\UIButtonDemo.cpp` を **`OnUIClick` 版へ書き換え** (矩形フィールド 4 本を
  `MyeEntityId button` 1 本に置換 = この milestone の成果物そのもの)。
- `assets\input\actions.json` に `UINav*` / `UISubmit` を追加。
- replay 被覆: `--flow-demo` のタイトルに**押せるボタン**を足し、`replay_verify` の
  `job_flow` が UI クリックとフォーカス移動を 600 tick 通るようにする
  (`--synth-input` でマウスが動かないので、**フォーカス移動 + Submit をパッド合成入力で駆動**する)。

**検証**: selftest 全 PASS + `UISelfTest` に `UIInteraction` の純関数 check (press→drag out→
release で click しない / focus の上下左右 / Submit) / replay_verify 9 ジョブ /
**shot_verify 18 枚 maxDiff=0** (ハイライトの計算元を替えても絵が変わらないこと = 置換が正しい証拠。
frame 3 でマウスは原点なので hover は成立しない見込み — 割れたら評価順を疑う)。

---

### M64c — スクリプト⇄オブジェクトの穴埋め (ABI 追加ゼロ) + インスペクタのメタデータ

**(1) `SetComponentField` の NoHash ゲートを開ける** — `EngineApiTable.cpp:713` の
`if (desc.flags & kComponentNoHash) { return 0; }` を**削除**。`Get` 側 (`:691`) は**据え置き**。

- 安全な理由: 書き込みは sim の決定論的な副作用で、値はハッシュに載らず、
  **NoHash コンポーネントも World のカラムとして SimSnapshot に入る** (`SimSnapshot.h:22-24`)
  ので巻き戻しも復元される。危険なのは読み取り側だけ (C# が書いた値が sim へ漏れる)。
- 効果: `CameraPostFx` (露出/ブルーム/ビネット/DOF …40 フィールド) / `Fog` / `Decal` /
  `TrailRenderer` / `SpriteRenderer` / `Skybox` / `Terrain` / `ReflectionProbe` / `SkinnedMesh`
  が**新スロット 0 本で実行時操作可能**になる。
- `EngineAPI.h` の v11 節に「Set は NoHash 可 / Get は不可」の非対称とその理由を明記。

**(2) `MyeScriptField` にインスペクタ用メタデータ** (`ScriptTypes.h`)

`ScriptHost.cpp:193-198` が `flags = kFieldNone` 固定で捨てているのが原因。
`FieldDesc` 側 (`Reflection.h:42-58`) は `displayName` / `minVal` / `maxVal` / `tooltip` /
`flags` を既に持っており、**インスペクタは組込みもスクリプトも同じ `DrawField` を通る**
(`InspectorWindow.cpp:628-640`) ので、**渡すだけで日本語表示名もスライダも出る**。

```cpp
// 既存はそのまま動く (後方互換)
REGISTER_SCRIPT(PlayerController, FIELDS(moveSpeed, jumpCount));
// 新: 括弧で包んだエントリにメタデータを付ける
REGISTER_SCRIPT(PlayerController,
    FIELDS(MYE_F_JP(moveSpeed, "移動速度"), MYE_F_RANGE(jumpPower, "跳躍力", 0.0f, 20.0f)));
```
`MyeScriptField` へのメンバ追加は **`MYE_API_VERSION` bump が必要** (`EngineAPI.h:12`)。
M64b の v15 に**同梱する** (= M64c 単独では bump しない。3 サブで版が上がるのは 1 回だけ)。
→ **M64b と M64c の順序は入れ替えない**。
★`ScriptAPI.h:149-168` の `LayoutHash` は `name`/`type`/`offset` の 3 項目しか混ぜていないので、
**メタデータを足しても移行は走らない** (= 表示名を変えただけで状態が飛ばない。正しい挙動)。

**(3) `FIELDS()` の上限 16 → 32** — `ScriptAPI.h:202-224` の `MYE_SF_N` 連鎖を延長。
`AudioDemo.cpp:35` が「登録フィールドは最大 16 個なので」を理由に 9 キーのエッジ検出を
`int32_t` へビットで畳んでいるのが実害。

**(4) 糖衣束 (ABI 追加ゼロ。全部 `ScriptAPI.h` / `MyeScript.cs` 内で完結)**

| 追加する糖衣 | 実現方法 (既に届いている口) |
|---|---|
| `MyeGetWorldPosition` / `MyeGetWorldMatrix` | `WorldMatrix.value` (Float4x4) を汎用で読む。**NoHash ではないので今も読める** |
| `MyeGetParent` / `MyeFirstChild` / `MyeNextSibling` / `MyeForEachChild` | `Hierarchy.parent/firstChild/nextSibling` を汎用で読む |
| `MyeSetActive` / `MyeIsActive` | `Active.enabled`。★`ActiveComponent` は**無ければ有効**なので、初回は `AddComponentByName` → **次 tick で** `SetComponentField` の 2 tick 手順になる (`EngineAPI.h:419-423`)。糖衣でこれを吸収する |
| `MyeGetName` / `MyeSetName` | `Name.value` (String64)。★`EntityNaming.h` の正規化を通らないので `/` を弾く検査を糖衣側に入れる (通すと `FindPart` のパス解決が壊れる) |
| `MyeSetPostFx*` / `MyeSetFogDensity` 等 | (1) で開いた `SetComponentField` |
| `MyeQuatFromAxisAngle` / `MyeQuatMul` / `MyeQuatLook` | `MathPod.h` に純関数を追加。`Rotator.cpp:18-21` が角度を自前積分している原因 |
| `MyeRandomInt(lo, hiExclusive)` | `RandomRange` の境界事故を防ぐ整数版 |
| `MyeFindPartByTag` (単数) の C# 版 | C# に単数版が無い |

★`MyePlaySoundHere` (`ScriptAPI.h:288-291`) は `GetLocalPosition()` を world 位置として渡している
**実バグ** — 親を持つエンティティで定位がずれる。`MyeGetWorldPosition` へ差し替える。
C# 側も同じ (`MyeScript.cs:280-281`)。

**(5) C# レーンの底上げ**

`Interop.cs:246` の `internal static unsafe class Engine` により、ユーザースクリプト
(別アセンブリ `MyeGameScripts_N`、`InternalsVisibleTo` はリポジトリに 1 件も無い) は
**`MyeScript` / `MyeEntity` の public 面しか使えない**。102 スロットのミラーがあるのに半分が届かない。

- `MyeScript` / `MyeEntity` へ不足糖衣を追加: `MousePos`(キャンバス版) / `CharacterMove` 4 本 /
  `Instantiate` / `FindByFileId` / `EmitterBurst` / `SetAnimatorParam` / `SetTextMeshText` /
  `SetMeshRenderer` / `DebugDrawLine` / `OverlapSphere` 系 / **`NameHash`** (これが無いと
  汎用フィールドアクセスが codegen 型でしか使えない)。
- **`tickIndex` を C# へ渡す**: `ScriptRuntime.cs:505` でネイティブから受け取っているのに
  `:239-249` の `inst.Update(dt)` で捨てている。`MyeScript.Tick` プロパティとして公開する
  (C# には現状**決定論的な時間カウンタが 1 つも無い**)。

**(6) インスペクタ: `EntityRef` のドラッグ&ドロップ + 型フィルタ**

`InspectorWindow.cpp:2006-2064` の `DrawEntityRef` は全エンティティのドロップダウンのみ。
Hierarchy からの `ImGui::AcceptDragDropPayload` を受ける + `MYE_F_REQUIRE(button, "UIElement")`
のようにフィールド側で必要コンポーネントを宣言して候補を絞る。

**(7) デモの更新**
- `AudioDemo.cpp:58-72` の voiceLo/voiceHi 分割を**削除** — `MYE_FIELD_UINT64` は最初から存在する
  (`ScriptTypes.h:14` / `ScriptAPI.h:30` / `ScriptHost.cpp:28`)。**コメントが間違っているだけ**。
- `LocalPlayerDemo.cpp:38-47` を v13 の `MyeAxisFor`/`MyeActionPressedFor` へ (v12 時代のまま)。
- `PlayerController.cpp:47-50` / `WalkerDemo.cpp:32-41` のデッドゾーン `0.3f` 直書きを
  アクションマップへ寄せる。

**検証**: selftest 全 PASS + `SchemaSelfTest` に「NoHash への Set が通り Get は 0」の
識別テストを追加 / replay_verify 9 ジョブ / shot_verify 18 枚 maxDiff=0 /
**C# は replay 被覆の外なので一時 probe スクリプトで実走確認** (`ABI bump の検証レシピ` の家風)。

---

## 全サブ共通の検証チェックリスト

1. 両構成 (Debug/Release) 0 警告 — `MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true`
2. `bin\x64\Debug\Editor.exe --selftest` (**実測 42 本**。`CLAUDE.md:33` の「40」は古い)
3. `tools\replay_verify.bat` — 9 ジョブ
4. `tools\shot_verify.bat` — **既存 17 枚 maxDiff=0** + 18 枚目
5. `pwsh -File tools\check_rules.ps1` — 0 error (規則 11-c は ABI bump と `$apiVersionSlots` の同時性)
6. `tools\net_verify.bat` — M64a の `InputSnapshot` 変更後に 1 回 (CI 対象外だが `kNetMaxPacket` が動く)
7. `tools\build_managed.bat Debug` / `Release` の両方 (C# は sln の外)

### 失敗の切り分け表 (今回特有)

| 症状 | 一次的な原因 |
|---|---|
| `--rep-diff` が `identical` と言うのに desync する | `Replay.cpp:135-159` に `mouseCanvas*` の比較行を足し忘れた |
| `job_mp` だけ stress で赤・素の verify は緑 | SimSnapshot の LOP 節に新フィールドが載っていない |
| `job_mp` だけ赤 | `SynthLaneInput` を更新して `.rep` を録り直していない |
| selftest #19 `PartSelfTest` | `MYE_API_VERSION == 14u` のハードコード未更新 |
| `rules` 11-c | `$apiVersionSlots` に `15` を足し忘れ |
| `rules` 11-a `slot #N name differs` | `Interop.cs` の append 位置ずれ |
| `ui_probe.png` だけ赤 | 2 倍が漏れたフィールドがある (`sliceBorder` / `fontScale` を忘れやすい)。差が細い枠 1 本なら **`kRing` をスケールしてしまった** (判断 5 の表) |
| `flow_title.png` が赤 | `DemoContent.cpp` 側の 2 倍漏れ (シーン JSON は生成物なので直す場所はコード) |
| `ttdebug`/`ttrelease` だけ赤 | `UIInteraction` の状態を Scene 節へ入れ忘れた (再シムで焦点が復元されない) |

---

## 実装しない / できないもの

- **`GetComponentField` の NoHash 開放** — C# が書いた値が sim へ漏れる。恒久的に閉じたまま。
- **UIElement の汎用読み取り** — 上と同じ。読みは `GetUIRect` (解決済み矩形) 1 本に絞る。
- **アスペクト非 16:9 でのワールド追従 UI と 3D の完全一致** — 3D は実 target のアスペクトで
  描く (`RenderSystem.cpp:367`) ので、レターボックスの黒帯の外は原理的に一致しない。
  キャンバスと同じ 16:9 で動かす前提を仕様に明記する。
- **フォントアトラスの DPI 追従** — `kBasePx = 32.0f` 固定 (`FontAtlas.cpp:20`) なので 4K で拡大
  するとぼやける。今回は触らない (別マイルストーン候補)。
- **シーンの加算ロード / プレハブ overrides の実行時 API / スクリプトからの全エンティティ列挙** —
  今回の依頼と独立。`World::ForEachArchetype` は DLL 境界にコールバックを通せないため
  別設計が要る。

---

## 進捗表 (完了時に更新。計画外の事実・ハマった所・申し送りだけ書く)

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M64a キャンバス統一 + 入力の記録 | 未着手 | | |
| M64b UI イベント + フォーカス + ABI v15 | 未着手 | | |
| M64c 穴埋め + インスペクタのメタデータ | 未着手 | | |
