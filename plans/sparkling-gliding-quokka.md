# M70: 地雷除去 — 保存でデータが消えるのを止め、ゲーム内 UI を解像度非依存にする

**再開手順**: `git log --oneline -5` で最後に完了した M70x を確認 → 本ファイルの該当サブの節を読む →
着手前にそのサブの前提 (版 bump の一覧 / 検証) を先に確認する。
1 サブ = 1 コミット (`M70a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルには**計画外の事実・罠・申し送りのみ**追記する。

- 基点コミット: `4516200` (M69f、master、clean)
- 材料: `plans\gleaming-strolling-swing.md` (計画 M64 — 調査結果と判断 1〜5 の記録。
  **判断 1 のキャンバスだけ M70b で差し替え済み**) / `docs\dogfooding.md`
- この M を含む全体像 (提案 25 件と着手順): <https://claude.ai/code/artifact/f4ed69be-4cdc-4018-8982-19f9111c57b6>

## Context

M69 (文書の現在地合わせ) が終わり、エンジンの記述と実装は一致した。その過程で
**「ポートフォリオとして致命的なのに、エンジン単体の回帰テストでは絶対に赤くならない穴」が 2 つ**
残っていることが確定した。M70 はこの 2 つだけを潰す。

1. **保存でデータが消える** (`docs/dogfooding.md` #10)。レジストリに無い型のコンポーネントは
   ロードで捨てられ、その状態を保存するとディスクからも消える。スキーマを触った直後や
   **GameLogic.dll のロードに失敗した状態**で Ctrl+S を押すと、シーン中のスクリプト
   コンポーネントが丸ごと消える。壊れるのは「読めなかった瞬間」ではなく「保存した瞬間」なので、
   気付くのは次の起動で調整値が既定値に戻ったとき。
2. **ゲーム内 UI が 1920x1080 決め打ち** (`engine_spec.md` §12.3、M69f)。描画は実クライアント px、
   ヒットテストとフォーカスナビは 1920x1080 固定で解く。**リポジトリ内に 1920x1080 で走る構成は
   1 つも無い** (既定 1600x900 / 撮影 960x540) ので、`anchor=0` (左上) 以外の UI は
   「見えている場所」と「押せる場所」が常にズレている。

どちらも「エンジンで実際にゲームが作れる」の土台にあたり、面接で触られると全体の信頼を削る。
逆に replay 7 ペアも golden 22 枚も緑のままなので、機械検証は 1 つも助けてくれない。

## スコープ (ユーザー決定 2026-09-07)

- **M70 で実装する** = 提案の 4-A (dogfooding #10) + 4-B (M64 計画の 3 サブ)。
- **M70 の次のタスクとして台帳に置く** = 提案の 4-C〜4-G (NavMesh / アニメーション深度 /
  カットシーン / LOD + オクルージョンカリング / ビヘイビアツリー)。本 M では実装しない。
- **ラベルは M70** (新番号)。`plans/gleaming-strolling-swing.md` (計画 M64) の中身は材料として
  引き継ぎ、同ファイル冒頭に「M70 へ移管」を記す。M69f が申し送った宿題はこれで決着。
- **進め方は通常のサブ分割**。1 サブ = 1 コミット (`M70a: ...`) = 1 セッション + /clear。harness は使わない。
- **UI キャンバスは Unity / UE 準拠の可変キャンバス**。レターボックスは採らない。
  規則は `Expand` (min) 1 つだけ実装し、Unity の match スライダ相当は「後で実装する」と
  書き残す。詳細は M70b。

---

## M70a — シリアライザを非可逆から可逆へ (データ消失の封鎖)

### 原因 (2 つのコードパスの非対称性。スキーマは引き金にすぎない)

- `src/Engine/Engine/SceneSerializer.cpp:184-188` — 型が未登録なら WARN 1 行を出して `continue`。
  **生 JSON (`fields`) はローカル変数のまま捨てられ**、`ReadEntityComponents` は `void` なので
  「unknown があった」事実すら呼び出し元へ返らない。
- `src/Engine/Engine/SceneSerializer.cpp:100-135` (`WriteEntity`) — 保存は元 JSON を一切参照せず、
  **生きているアーキタイプだけ**から `components` を作り直して丸ごと置換する。
- `src/Engine/Engine/EngineLoop.cpp:312-317` — スキーマ登録はここ 1 回だけ。
  `HotReload/ReloadHub.cpp:76` が `.component.schema.json` を「未知の .json = 何もしない」に
  分類しているので、起動後にスキーマを足しても登録されない。

**実害はスキーマ型より広い**: `EngineLoop.cpp:334-341` は GameLogic.dll のロード失敗でも続行する
(ERROR ログのみ)。その状態で保存すると全 C++ スクリプトコンポーネントが消える。C# ホストの
Init 失敗も同型。`.actor.json` / `.prefab.json` も `Prefab.cpp:1524` (`SaveEdited`) が同じ
`SaveToJson` を通るので同じ消え方をする。

### 直し方 — 「知らないから捨てる」を「知らないから触らない」へ

1. **`Scene` に側テーブル** `unknownComps_` : `fileId → std::map<型名, 生 JSON 文字列>`。
   `overrides_` (`src/Engine/Engine/Scene.h:89-140`) と同型で、**ECS の外・WorldHash 非対象**。
   `std::map` にするのは出力順を決定論にするため (`OverrideSet` が `std::set` なのと同じ理由)。
   - ★`overrides_` と違い **SimSnapshot には入れない**。override 表は「タイムトラベルで編集状態ごと
     戻す」ために撮影対象に入っている (`Scene.h:129-138`) が、パススルーは tick 中に一切変化しない
     ので撮る理由が無い。blob を太らせない。
2. **退避** — `ReadEntityComponents` (`SceneSerializer.cpp:177`) が unknown の生 JSON を退避し、
   件数を戻り値で返す。現在この関数は `World&` しか受けていないので退避先の口を引き回す
   (呼び出し元 3 本: `:367` `LoadFromJson` / `:445` `ApplyDiff` / `:560` `ApplyPartial`)。
3. **書き戻し** — `WriteEntity` (`:100`) の末尾で、その fileId の退避分を `comps` に足す。
   **すでに登録済みになった名前はスキップ** (アーキタイプ側が勝つ) — スキーマを直して開き直した
   あとに二重書きしないため。
4. **保存時の告知** — `src/Editor/EditorApp.cpp:1441` が Ctrl+S / File メニュー / 未保存モーダル /
   「保存してコミット」の**唯一の絞り**なので、ここ 1 箇所で「未知のコンポーネント N 個を保持した
   まま保存しました」をトーストで出す (Unity の Missing Script 相当)。文字列は
   `LocalizationTable.inl` に en/ja 両方。
5. **Inspector に読み取り専用の行**「未知のコンポーネント (N)」。何が保持されているのか見えないと
   ユーザーは消えたと思う。

### やらないこと

`assets/schemas` の監視とホットリロード (dogfooding の案 3)。スキーマ型は「組込みの後・
スクリプト型の前」に登録する規約 (`EngineLoop.cpp:312-317` のコメント = M48j の決定) なので、
実行中に型を足すとスクリプト型の TypeId が一斉にずれ、**既存シーンと .rep が壊れる**。
案 1 で実害は消える。

### 版

`Scene::kDocVersion` (現行 3、`Scene.h:16-22`) は**上げない**。unknown が無いシーンでは出力バイト列が
1 バイトも変わらない加算的変更で、唯一の消費者 `Prefab.cpp:1368` の v3 分岐にも影響しないため
(M63 の「上げない」判断と同型)。

### テスト

`src/Engine/Engine/SceneSelfTest.cpp` の `RunSceneSerializerSelfTest` 内に新ブロック。
★このスイートは連鎖の 2 番目 (`EditorMain.cpp:600`)、`RunSchemaSelfTest` は 20 番目 (`:609`) なので、
**実行時点でスキーマ型は 1 つも登録されていない**。`"MyeTestUnknownComp"` を手書き JSON に入れれば
「必ず未登録」が保証でき、レジストリを汚さずに unknown 経路を再現できる。検査項目:

- ロード → 保存で生 JSON が**同値で戻る** (キー順・数値表記まで)
- 同名の型が登録済みになったら退避分は出力されない (二重書きしない)
- エンティティを消したら orphan が出力に残らない
- **unknown ゼロのシーンは出力バイト列が従来と完全一致** (既存 golden シーンでの非干渉)
- `ApplyDiff` (ホットリロード) を挟んでも退避が生き残る

---

## M70b — キャンバス統一 (計画 M64a 相当)

### 現状の食い違い (実測)

| レーン | 解く解像度 | 場所 |
|---|---|---|
| 描画 (Runtime) | SwapChain 実 px | `EngineLoop.cpp:1638-1639, 1654` |
| 描画 (Editor GameView) | GameView RT 実 px | `GameViewWindow.cpp:33-34, 47-50` |
| ヒットテスト | **1920x1080 固定** | `EngineApiTable.cpp:773-774` |
| フォーカスナビ | **1920x1080 固定** | `EngineApiTable.cpp:421-422` |
| マウス (`MousePos`) | クライアント実 px | `EngineApiTable.cpp:80-83` |

`UILayout.h` の `Resolve/ResolveRect/ResolveClipRect/ResolveVisibleRect` は **全部 `screenW/H` を
引数で取る純関数**で、「描画とヒットテストが同じ関数を通る」設計自体は正しい。渡している値が
違うだけ。960x540 では `anchor=4` (中央) の基準点が描画 (480,270) とヒットテスト (960,540) で
2 倍ずれる。`anchor=0` だけが偶然一致する。

### モデル — Unity / UE に合わせる (ユーザー決定 2026-09-07)

レターボックスは**採らない**。Unity (Canvas Scaler の `Scale With Screen Size`) も
UE5 (UMG の DPI Scaling) も、**基準解像度からは「一様スケール」だけを取り出し、
キャンバス矩形そのものは画面のアスペクトに合わせて伸びる**モデルで、黒帯は作らない。
Unity のドキュメントは「キャンバス矩形は実行時に画面のアスペクトへ合わせて変わり、基準解像度の
ままではない」と明言している。UE も「ウィジェットは ピクセル ÷ DPI スケール のスレート単位で
配置され、ビューポートもその単位で伸びる」。

```
s        = min(w / 1920, h / 1080)      // Unity の Screen Match Mode = Expand 相当
canvasW  = w / s        canvasH = h / s // 黒帯なし。UI は常に本当の画面端まで届く

 1600x900 (16:9)   -> canvas 1920 x 1080   現行と同一
  960x540 (16:9)   -> canvas 1920 x 1080   golden 不動
 1920x1200 (16:10) -> canvas 1920 x 1200   下端アンカーの UI が本当の下端へ
 2560x1080 (21:9)  -> canvas 2560 x 1080   左右へ広がる
```

★**この式の一番おいしい性質**: `s = min(w/1920, h/1080)` なので、**キャンバス寸法は
アスペクト比だけの関数**になり画素数に依らない。16:9 なら 960x540 でも 1600x900 でも 4K でも
キャンバスは厳密に 1920x1080 = **既存の golden も CI も 1 ビットも動かない**。可変になるのは
非 16:9 のときだけ。

**規則は `Expand` (min) 1 つだけ実装する。** Unity の `Match Width Or Height`
(対数空間の lerp = `pow(2, lerp(log2(w/refW), log2(h/refH), match))`) と `Shrink` は
**後から足しても既存シーンを壊さない**ので、今回は入れず「後で実装する」とコードのコメントと
`engine_spec.md` に書き残す。基準解像度 1920x1080 もハードコードのままにする。

### 直し方

- `uilayout::CanvasSize(screenW, screenH) -> {scale, canvasW, canvasH}` を追加。
  **`Resolve*` のシグネチャは変えない** — 渡す `screenW/H` を canvasW/H に差し替えるだけ
  (`UISelfTest.cpp` の 40 検査が全書き換えになるのを避ける)。
- `UIRenderer.cpp` はキャンバス座標で解いてから `* scale` で実 px へ (`textScale *= scale`)。
  オフセットは無い (中央寄せしない)。`GameViewWindow.cpp:120-142` の選択アウトラインも同じ。
- **sim へ入る機種依存は 4 値だけ** — `Input::CaptureSnapshot` の中でキャンバス座標へ正規化し、
  `InputSnapshot` に `mouseCanvasX/Y` と `canvasW/H` (いずれも float) を載せる (raw px は残す)。
  こうすると .rep に記録されるのは正規化後の値なので、**再生時は窓の大きさに依らず一致する**。
  UIElement は `kComponentNoHash` (`Components.cpp:266-299`) なのでズレてもワールドハッシュに
  出ない = 記録せずに実解像度を渡す直し方は「最悪の壊れ方」になる。
  - `canvasW/H` を持つのは**レーン 0 だけ** (マウスと同じ規約。レーン 1-3 は 0)。消費側は
    `ctx.Input()` を読む。`SynthLaneInput` にも載せて `.rep` 往復照合の被覆に入れる。
- **ネット対戦**: キャンバス寸法はアスペクト比の関数なので、**アスペクトが違う 2 台は UI の
  当たり判定が食い違う**。ハンドシェイクの照合項目 (proto / API 版 / .rep 版 / 起動オプション /
  開始ワールドハッシュ) に **canvasW/H を足して不一致は接続拒否**する。16:9 同士なら解像度が
  違っても通る (キャンバスが厳密に一致するため)。
- ★**M64a (`080d5d5`) の取りこぼしを同時に直す**: `Replay.cpp:135-159` の
  `FirstDifferentInputField` に `mouseDeltaX/Y` の比較が無い。視点入力が食い違っても
  `--rep-diff` が「入力は同じ」と嘘をつく。新 4 値と一緒に 6 行足す。

### 版 bump (このサブに 1 回だけ束ねる)

| 定数 | 現行 | 新 | 理由 |
|---|---|---|---|
| `sizeof(InputSnapshot)` | 72 | 88 | `mouseCanvasX/Y` + `canvasW/H` (float x 4) |
| `kReplayFileVersion` (`Replay.h:41`) | 5 | 6 | .rep がこのビット列をそのまま持つ |
| `kSimSnapshotVersion` (`SimSnapshot.h:89`) | 11 | 12 | LOP 節の `prevTickInput` が太る |
| `kNetProtoVersion` (`NetSession.h:35`) | 3 | 4 | 1 パケットの本体長 + ハンドシェイク項目が変わる |

`NetSelfTest.cpp:111-116` の `sizeof == 72` / `== 3` の表明も同時に更新。

### golden

撮影は全 22 枚が **960x540** (`shot_verify.bat:79`) で 16:9 = キャンバスは厳密に 1920x1080、
スケールはちょうど 1/2。`ui_probe.scene.json` の UIElement 16 個と `DemoContent.cpp` の UI 数値
(`x/y/w/h/fontScale/sliceBorder`) を 2 倍すれば IEEE754 で厳密に元の絵と一致する。
★ただし**フォーカス枠の `kRing = 2.0f` (`UIRenderer.cpp:341-352`) はスケールしない**ので、
`ui_probe.scene.json:943-944` の `focused=1` の要素まわりだけ差が出うる。まず `--update` 無しで
`img-diff` を取り、動いた画素がリング幅由来だけであることを確認してから golden を更新する。

新規 golden 2 枚 (22 → **24 枚**) — この 2 枚が無いと新経路は 1 画素も被覆されない:

| 枚 | 解像度 | 何を固定するか |
|---|---|---|
| 23 | 1280x720 (16:9) | **スケール経路**。キャンバスは 1920x1080 のまま、s = 2/3 = 2 進で割り切れない倍率での描画 |
| 24 | 960x600 (16:10) | **可変キャンバス経路**。canvas 1920x1200 = 下端/右端アンカーが本当の端へ動くこと |

`UISelfTest` にも `CanvasSize` の検査を足す (16:9 の 3 解像度が厳密に 1920x1080 を返すこと /
16:10 が 1920x1200 / 21:9 が 2560x1080)。

---

## M70c — UI イベントとフォーカス駆動 (計画 M64b 相当、ABI v16)

現状、押下判定は `UIRenderer.cpp:292-300` でエンジン内部に存在するのに**表示にしか使われず捨てられて
いる**。ゲーム側で唯一動いている経路は `GameLogic/Scripts/UIButtonDemo.cpp` で、UIElement と同じ矩形を
スクリプト側にもう一度手書きする形 (`ScriptAPI.h:444-448` のコメントが「anchor=0 以外は画面サイズ依存」
と自白している)。`UIHitTest` / `UIFocusNav` のゲームからの実使用は C++ / C# とも **0 件**。

- 新規 `src/Engine/Engine/UI/UIInteraction.{h,cpp}`。状態 `hovered` / `pressed` / `focused`
  (EntityID 3 本) を **`Scene` の `TimeControl` の隣**に置く = SimSnapshot の Scene 節に入り
  **WorldHasher の対象**になる。配線が壊れたら `replay_verify` が赤くなる、が唯一の防波堤。
- 評価位置は `TickRunner.cpp:250` の `inputActions.Evaluate` → `UpdatePlayerInputMirror` の直後、
  **スクリプト層より前**。`click` = 押した要素の上で離した瞬間 (Unity 意味論)。
- フォーカスはアクションマップ (`UINavUp/Down/Left/Right/Submit`) で `uinav::FindNext`
  (`UINav.h:21-58`) を駆動し、`UIElement.focused` をエンジンが書き戻す。
- **このサブが M70 唯一の ABI bump** (`MYE_API_VERSION` 15 → **16**、スロット 104 → **110**)。
  家風どおり「ABI はマイルストーンに 1 回」なので、**M70d が使う分もここで先に入れる**:

  | 追加 | 本数 | 用途 |
  |---|---|---|
  | `UIButtonState` (bit0 hovered / 1 pressed / 2 clicked / 3 focused) | 1 | M70c |
  | `UIGetFocused` / `UISetFocused` | 2 | M70c |
  | `MouseCanvasPos` | 1 | M70c |
  | `GetUIRect` (解決済みキャンバス矩形 = UI 唯一の読み取り口) | 1 | M70c |
  | `LoadPersist(slot)` (dogfooding #16。`LoadGame` がセーブ時のシーンへ必ず飛ぶ問題) | 1 | 相乗り |

  ★`MyeScriptField` のメタデータ拡張 (M70d の中身) は**構造体レイアウトが動く**ので、
  スロット追加と**同じコミットで**入れる。分けると M70c 時点でビルドした DLL が
  `apiVersion` 一致のまま別レイアウトで受理され、静かに壊れる。
  `check_rules.ps1:583` の `$apiVersionSlots` に `16 = 110` を**同時に**足す (片方だけだと規則 11)。
  `Interop.cs` は位置ミラーなので順序・件数・引数個数を揃える。`PartSelfTest.cpp:177` の
  `== 15u` 表明も更新。
- 同時に: `UIRenderer.cpp:295-300` の自前ハイライト計算を削除して `UIInteraction` を読む /
  `MyeScript.cs` の `SetUIRect` / `SetUILayout` (C# から UI 幾何を書く口) を閉じる /
  `UIButtonDemo.cpp` を `OnUIClick` 版へ書き換える。
- `replay_verify` の `job_flow` に、パッド合成入力でフォーカス移動 + Submit を通す被覆を足す。

---

## M70d — スクリプト⇄オブジェクトの穴埋め (計画 M64c 相当、ABI 追加ゼロ)

★ABI のレイアウト変更は M70c で済ませてあるので、このサブはヘッダ内 inline とエディタ側だけ。
M70c → M70d の順序は入れ替えできない。

- `SetComponentField` の NoHash ゲート (`EngineApiTable.cpp:713`) を**削除**。Get 側 (`:691`) は
  据え置きの非対称 (読みは恒久的に閉じる = `plans/gleaming-strolling-swing.md` の決定)。
- `MyeScriptField` のメタデータを Inspector が読む (`MYE_F_JP` / `MYE_F_RANGE`)。
  スクリプトの調整フィールドに日本語表示名とスライダ範囲が付く (dogfooding #2 を実装で追い越す)。
  `FIELDS()` の上限 16 → 32。
- ★**実バグ 1 件**: `src/Shared/ScriptAPI.h:289` の `MyePlaySoundHere` が
  `GetLocalPosition()` をワールド位置として `PlaySoundAt` に渡している (子エンティティで
  鳴る場所がずれる)。

### 同時に回収する dogfooding (ABI 追加ゼロで済むものだけ)

| # | 内容 | 手当て |
|---|---|---|
| 18 | `MyeGameObject` が回転を読めない | `ScriptAPI.h:249-265` に `GetLocalRotation` / `GetLocalScale` / `SetLocalScale` の 3 本を inline 追加。**ABI スロットは 6 本とも既にある** (`EngineAPI.h:146-151`) ので糖衣のみ |
| 7 | 角度 → 四元数のヘルパが無い | `MathPod.h` は 49 行で関数が `MyeEntityIdIsNull` 1 本のみ。`ScriptAPI.h` のオーディオ糖衣 (`:271-`) と同じ形で `MyeQuatFromEuler` / `MyeForwardOf` を追加 |
| 4 | `Instantiate` の「親なし」が偶然で成立している | `TickRunner.cpp:435` の `(req.parent.index != 0 \|\| req.parent.generation != 0)` を `!MyeEntityIdIsNull(req.parent)` へ。ヘルパは `MathPod.h:41-49` に実在、同ファイルは既に `Shared/` を include 済み。**1 行** |
| 12 | `builtin://` が遅延生成で Runtime では半分しか解決しない | `GpuResources.h:67` の `Init` は `device_` 代入だけ。`Cube/Sphere/Plane/Quad/Cylinder/Capsule` の 6 アクセサをここで呼んで登録を確定させる (**数行**) |
| 3 / 5 / 6 | `Start()` で WorldMatrix が無い / CC カプセルが scale を拾う / `CharacterJump` が接地を見ない | コメント追記のみ。3 は `EngineAPI.h:195, 255` の空間クエリ節へ、5 は `Components.h:449-460` へ (このコンポーネントには現在スケールへの言及が 1 文字も無い)、6 は `EngineAPI.h:209-210` の注記を先頭へ上げる。**改名 `CharacterRequestJump` は採らない** — 規則 11 が 3 ファイルの名前を照合するので (d) 相当のコストになる |
| 8 | Skybox の cubemap が未実装 | ★**文書のほうが古い。M38b で実装済み** (`Components.cpp:394` のリフレクション / `RenderSystem.cpp:379-381, 1071-1085` / `SkyboxPass.cpp:32` の専用シェーダ / `GpuResources.cpp:631-678` の DDS cubemap ローダ / RT 側 `RtPasses.cpp:348-349`)。`Components.h:503, 505, 509` の「予約」コメント 3 行を実態へ直し、`dogfooding.md` の 8 番を「解決済み (文書の誤り)」へ |
| 9 | エンティティ参照が名前引きに寄りがち | ★**エンジン側は完備** (`MYE_FIELD_ENTITYREF` / `ScriptHost.cpp:36` / `InspectorWindow.cpp:1181, 2032` の D&D / `JsonUtil.cpp:62` の fileId 再マップ / `SearchWindow.cpp:122-147` の逆引き)。プロジェクト側の書き方の問題なので**文書化のみ** |
| 16 | `LoadGame` がセーブ時のシーンへ必ず遷移する | スロットは M70c で確保済み。ここでは `TickRunner.cpp:643-654` に「persist だけ読む」分岐を足す |

**M70 では回収しないもの** (次節の台帳へ): #11 (`--debug-draw-log`)、#13 (`builtin://wheel`)、
#17 (CC ⇄ Rigidbody の双方向)、#19 (`PhysicsEnvironment` 不在の WARN)、#20 (スクリプト間
メッセージ)。いずれも新しい実装面が要り、#20 は `MyeScriptDesc` に `onMessage` を足す =
次の ABI bump の設計から始まる。

---

## 版・定数の変更一覧 (このマイルストーンで動くもの)

| 対象 | 現行 → 新 | サブ |
|---|---|---|
| `sizeof(InputSnapshot)` | 72 → 88 | M70b |
| `kReplayFileVersion` | 5 → 6 | M70b |
| `kSimSnapshotVersion` | 11 → 12 | M70b |
| `kNetProtoVersion` | 3 → 4 (ハンドシェイクに canvasW/H を追加) | M70b |
| `MYE_API_VERSION` / スロット数 | 15 / 104 → **16 / 110** | M70c |
| `MyeScriptField` のレイアウト | メタデータ追加 (同じ bump に同梱) | M70c |
| `Scene::kDocVersion` | 3 のまま (**上げない**) | M70a |
| golden 枚数 | 22 → 24 (ui_probe を 1280x720 と 960x600 で追加) | M70b |

## 検証

全サブ共通 (`AGENTS.md` / `CLAUDE.md` の既定):

1. 8 ビルド 0 警告 (`/p:MyeWarnAsError=true` で Debug/Release × 4 プロジェクト)
2. `bin\x64\Debug\Editor.exe --selftest` / Release も (45 スイート全 PASS)
3. `pwsh -File tools\check_rules.ps1` — 0 error (規則 11 の `$apiVersionSlots` は M70c で同時更新)
4. `tools\replay_verify.bat` — 7 ペア。M70b/M70c は .rep 版と入力レイアウトを動かすので**必ず全数**
5. `tools\shot_verify.bat` — M70b だけが golden を動かす。**先に `--update` 無しで走らせて
   `img-diff` の maxDiff と画素数を記録**し、リング幅由来と説明できることを確認してから更新する
6. `tools\net_verify.bat` — M70b (プロトコル版) のあとに 1 回 (CI 対象外なのでローカルで)

サブ固有:

- **M70a**: 実機で「スキーマを消した状態でシーンを開く → Ctrl+S → JSON を diff」して
  コンポーネントが残ることを目視。加えて `GameLogic.dll` をリネームしてエディタを起動 →
  保存 → スクリプトコンポーネントが残ることを確認 (これが本当の地雷)。
- **M70b**: `Runtime.exe --scene assets\scenes\ui_probe.scene.json` を **16:9 / 16:10 / 21:9 の
  3 アスペクト**で開き、(1) 押せる場所 = 見える場所、(2) 端アンカーの UI が本当の画面端に付く、
  (3) 黒帯が出ないことを目視。`--rep-diff` が視点入力の食い違いを言い当てることも確認。
  ネットは**アスペクトの違う 2 台**で接続を拒否することを実測 (16:9 同士は解像度が違っても通る)。
- **M70c**: `--local-demo` + パッドでフォーカス移動 → Submit。desync しないことを `net_verify` で。
  C# レーンは replay 被覆の外なので、`Interop.cs` のミラーは一時 probe スクリプトで実走確認する
  (`abi-bump-verification` の手順)。
- **M70d**: `--selftest` に加えて、Inspector にスクリプトの調整フィールドが日本語表示名と
  スライダで出ることを目視。`MyePlaySoundHere` は子エンティティに付けた音源で位置を確認。

新規ファイル (`UIInteraction.{h,cpp}` 等) を足したら **`pwsh -File tools\gen_project_files.ps1`**。

## 進捗

| サブ | 内容 | 状態 |
|---|---|---|
| — | 計画確定 + 提案一覧の公開 + M64 計画への移管注記 | **完了 (2026-09-07)** |
| M70a | シリアライザのラウンドトリップ (データ消失の封鎖) | **完了 (2026-09-07)** |
| M70b | キャンバス統一 (Unity / UE 準拠の可変キャンバス) | **完了 (2026-09-07)** |
| M70c | UI イベントとフォーカス駆動 + ABI v16 | **完了 (2026-09-07)** |
| M70d | スクリプト⇄オブジェクトの穴埋め + dogfooding の回収 | **完了 (2026-09-07)** |

### M70a の実施メモ (計画との差分)

- 計画どおり `Scene::unknownComps_` + `WriteEntity` の書き戻しで実装。`kDocVersion` は 3 のまま。
- **計画に無かった追加 1**: `SaveToJson` で「生きているエンティティの分だけ残す」掃除
  (`Scene::RetainUnknownComponents`)。孤児は出力には出ないが、`ApplyDiff` を繰り返す
  ホットリロードでは表に溜まりっぱなしになるため。
- **計画に無かった追加 2**: `RemapEntityRefsInComponents` は未登録型のフィールド表を持てないので、
  **複製 / コピペした未知コンポーネント内の EntityRef は付け替わらない** (元の fileId のまま)。
  型が引ける状態で複製し直せば直る。同関数にコメントで明記した。
- 実機確認は `GameLogic.dll` をリネームして `Runtime.exe --scene assets\scenes\flow_game.scene.json`
  → `unknown component 'FlowGameDriver' (kept verbatim)` を確認 (ロード側)。
  **保存側の GUI 目視 (Ctrl+S → JSON diff) は未実施** — セルフテストの指紋一致で担保している。
- 文書更新: `docs/dogfooding.md` #10 を修正済みへ / `engine_spec.md` §8.3 に節を追加 + §12.3 の
  台帳行 / `README.md` と `docs/demo_script.md` の件数 (17 → 16)。

### M70b の実施メモ (計画との差分)

- 計画どおり `uilayout::CanvasSize` + `InputSnapshot` の 4 値 + 版 bump 4 種で実装。
  `Resolve*` のシグネチャは変えていない (UISelfTest の 40 検査は無傷)。
- **計画との差分 1**: `Input::CaptureSnapshot` は `uilayout::CanvasSize` を**呼べない** —
  Platform 層から Engine 層を include することになるため。式の正本は Engine 側に置いたまま、
  `InputCanvas` (scale/w/h の POD) を EngineLoop が計算して渡す形にした。
  正規化そのものは計画どおり CaptureSnapshot の中で 1 回だけ起きる。
- **計画との差分 2**: キャンバス寸法は `int` へ丸める (`lroundf`)。`Resolve*` の引数型を
  変えないための妥協で、誤差は最大 0.5 px、しかも描画とヒットテストが同じ整数を通るので
  **両者がズレることは無い** (損をするのは右端/下端の 0.5 px だけ)。1366x768 のような
  非 16:9 かつ端数が出る解像度でしか効かない。
- **計画との差分 3**: `NetIdentity` が 40 → **48 バイト** (canvasW/H で 8 バイト増)。
  `NetHandshakePayload` も 48 → 56 で、`NetSelfTest` の 3 つの表明を更新。
  拒否理由に `NetReject::Canvas` を足した。
- **計画との差分 4**: golden の動いた画素は**計算と厳密に一致した**。フォーカス枠
  (`kRing`) をキャンバス単位にしたので 960x540 では 2 px → 1 px になる。
  対象は `StartButton` (336x88 キャンバス = 168x44 px) だけで、
  リング面積は 2 px 時 `172*48 - 168*44 = 864`、1 px 時 `170*46 - 168*44 = 428`、
  差 **864 - 428 = 436** = 実測の `diffPixels=436` そのもの (maxDiff=206 は白枠と
  暗い背景のコントラスト)。**他の 21 枚は maxDiff=0 のビット一致**なので、
  `--update` で塗り潰したのは ui_probe.png 1 枚だけ。
- **計画との差分 5**: 資産側の 2 倍化は `ui_probe.scene.json` の 144 値 (16 要素 ×
  x/y/w/h/fontScale + sliceBorder 4) と `DemoContent.cpp` の 7 箇所。
  `UIElementComponent` の**既定値と CreateMenu の生成サイズは 2 倍しなかった** —
  golden には出ないうえ、既定値を動かすと「その値で保存された既存シーン」の解釈が
  変わりうるため (単位がキャンバスになったことはコメントで明記)。
- ★**申し送り (M70c への引き継ぎ)**: `ScriptAPI.h` の `MyeMouseInRect` /
  `MyeButtonClicked` は**まだクライアント実 px で判定する**ので、`UIButtonDemo` は
  1920x1080 以外でズレる (M70b 以前は anchor=0 なら合っていたので、ここだけは一時的な
  後退)。直すにはキャンバス座標のマウスが要り、それは ABI スロット (`MouseCanvasPos`) =
  M70c。同サブで `UIButtonDemo` ごと `OnUIClick` 版へ寄せる予定なので px のまま据え置いた。
- 実測 (すべてローカル):
  - 8 ビルド 0 警告 (`/p:MyeWarnAsError=true`)、Debug / Release の `--selftest` 全 PASS
  - `replay_verify.bat` 10 ジョブ 93.3s 全 PASS / `check_rules.ps1` 0 error
  - `shot_verify.bat` 24 枚 PASS (更新は ui_probe.png のみ + 新規 2 枚)
  - `net_verify.bat` 4 ケース + desync 注入 PASS
  - **アスペクト拒否の実測**: 960x540 (16:9) ホスト ⇄ 960x600 (16:10) 参加 →
    `host rejected the connection: UI canvas size (the two windows have different aspect ratios)`。
    960x540 ⇄ 1280x720 (どちらも 16:9) は `lockstep ready` まで通る。
  - **`--rep-diff` の実測**: .rep のバイトを 1 フィールドだけ書き換えて
    `mouseDeltaX / mouseDeltaY / mouseCanvasX / canvasW / canvasH` の 5 つとも
    `tick 5: input lane 0 differs at <名前>` と名指しできることを確認。
  - 21:9 (2560x1080) は golden にしていないが、一時スクショで
    「左端/右端/下端アンカーが本当の端に付く・黒帯なし」を目視確認した。
- 文書更新: `engine_spec.md` に **§6.11 In-game UI canvas** を新設 + §12.3 の台帳行を
  M64a-M64c → M70c / M70d へ差し替え / `README.md` に機能の項 + スクショ枚数 /
  `CLAUDE.md` の shot_verify 行 (22 → 24 枚、CI 判定 12 → 14 枚)。

### M70c の実施メモ (計画との差分)

- 計画どおり `UIInteraction.{h,cpp}` + `Scene` の状態 + ABI v16 (110 スロット) で実装。
  評価点も計画どおり `UpdatePlayerInputMirror` の直後 (スクリプト層より前)。
- **計画との差分 1**: 版が 2 つ余分に動いた。計画表は M70c を「ABI だけ」としていたが、
  Scene 節に状態を足せば **`kSimSnapshotVersion` 12 → 13**、ワールドハッシュに節を足せば
  **`kReplayFileVersion` 6 → 7** が要る (`Replay.h` 冒頭の「InputSnapshot / WorldHasher の
  レイアウトが変わったら版を上げる」がそのまま効く)。`kNetProtoVersion` は**据え置き 4** —
  パケットも NetIdentity も 1 バイトも変わっていない (版の食い違いは repVersion /
  snapshotVersion の照合で弾かれる)。
- **計画との差分 2**: **`clicked` を 4 本目の状態として持った** (計画は 3 本)。
  「押した要素の上で離した瞬間」は tick 内の派生値だが、スクリプトはその tick にしか
  読めないので状態として持つほかない。1 tick で必ず落ちることを selftest で固定した。
- **計画との差分 3**: **`adoptedAuthored` (5 本目)**。エンジンがフォーカスの正本になると
  `UIElement.focused` を毎 tick 上書きするので、**シーンが書いた `focused=1` が起動直後に
  消える**。実際 golden 3 枚 (ui_probe / _720p / _16x10) がフォーカス枠のぶんだけ割れて
  発覚した。「起動後 1 回だけ拾う」規則を入れて 3 枚とも元へ戻した (Unity の EventSystem
  "First Selected" と同型。毎 tick 拾い直すとユーザーが外した次の tick に戻ってしまう)。
- **計画との差分 4**: `OnUIClick` **コールバックは作らず**、`UIButtonState` のポーリング
  1 本にした。計画の表は `UIButtonState` をスロットとして挙げているので、コールバックを
  足すと同じことを 2 通りで表す (`MyeScriptDesc` のレイアウトも動く)。`UIButtonDemo` は
  ポーリング版へ書き換え、ついでに対象指定を名前引きから **EntityRef** にした
  (dogfooding #9 の「名前引きに寄る」を実装で外した)。
- **計画との差分 5**: `LoadPersist` は **M70c で実装まで済ませた** (計画は「スロットだけ
  確保して TickRunner の分岐は M70d」)。何もしないスロットを 1 サブ分放置すると、
  呼んだスクリプトが黙って失敗する罠になるため。dogfooding #16 はこれで解決済み。
- **計画との差分 6**: `MyeScript.cs` は `SetUIRect` / `SetUILayout` に加えて
  **`SetUIFocused` も閉じた** (フォーカスは M70c でハッシュ対象になったので、C# から
  書くと再シムで割れる = 閉じる理由が同じ)。
- ★**既存バグを 1 件回収**: `HashWorld` の `SimSources` に **`acoustic` を渡し忘れていた
  箇所が 5 か所**あった (`EngineLoop.cpp` の 4 か所 = startWorldHash / タイムトラベルの
  自己検証 / `TickEndHash` / desync ダンプ、`TimeTravel.cpp` の 1 か所)。M65a が
  TickRunner 側にだけ配線して、他は 4 引数のまま残っていた。音響の節は内容ゲート
  (波が 1 本も無ければ畳まない) なので**波の出ないシーンでは同じ値**が出ていて誰も
  気づけない — 波のあるシーンでだけ「クラッシュ .rep が全 tick MISMATCH」
  「タイムトラベルの自己検証とネットの desync 検出が波スロット表を見ない」形で出る。
  `ui` を足すついでに 5 か所とも直した。
- **計画との差分 7 (撮影の中立化)**: 決定的撮影モードで**マウスの位置とボタンも 0 に倒した**
  (M68c は生デルタだけ)。hovered / pressed がボタンのハイライトを決めるようになったので、
  **撮影中にカーソルが窓の上にあるだけで golden が割れる**。位置は -100000 という
  「どのキャンバス座標も指さない」値へ (0,0 は左上の正当な座標なので使えない)。
- **計画との差分 8 (デモ)**: 被覆のためにタイトル画面へ **focusable なボタンを 2 個**足した
  (`MakeUiButton`)。**横並び**にしたのは合成入力の都合 — `SynthLaneInput` の D-Pad 分布は
  600 tick で Left 9 / Right 5 / Up 12 / Down 2 で、縦に積むと「上端でさらに上」ばかりに
  なりフォーカスが 1 度も動かなかった (実測)。あわせて `FlowTitleDriver` から
  `"Jump"` の直接判定を外した (Space / パッド A は `UINavSubmit` にも割り当てたので、
  CLEAR BEST を選んで決定すると「消えると同時にゲームも始まる」二重発火になっていた)。
  ★ついでに **TitleHint と TitleBest の文字が重なっていた**のを直した (M70c 以前の
  golden にもそのまま写っていた)。
- **合成入力の変更**: `SynthLaneInput` が D-Pad を疎に押すようになった (8 通り中 4 通りで
  1 方向)。これが無いと `UINav*` が replay で 1 度も動かない。
- 実測 (すべてローカル):
  - 8 ビルド 0 警告 / Debug・Release の `--selftest` 全 PASS / `check_rules` 0 error
    (規則 11 の 110 スロット照合込み)
  - `replay_verify` 10 ジョブ全 PASS (148.2s)、`shot_verify` 24 枚 PASS
    (**動いたのは flow_title.png の 1 枚だけ** = ボタン 2 個とヒント位置。他 23 枚は
    maxDiff=0 のビット一致)
  - **UI 対話の replay 被覆を実測で確認**: tick 527 → 528 で
    `focused 9 → 0x0A` / `clicked 0x0A` / `FlowTitleDriver.clearClicks 0 → 1`、
    529 で `clicked` が null へ戻る (1 tick 意味論)。すべて `--hash-dump` の行で確認。
  - `net_verify` 4 ケース + desync 注入 PASS
  - C# ミラーは一時 probe で実走確認 (`abi-bump-verification` の手順)
- 文書更新: `engine_spec.md` に **§6.12 In-game UI interaction** を新設 + §12.3 の台帳行を
  M70d だけに / `README.md` に機能の項 / `CLAUDE.md` の ABI 行 (v15=104 → v16=110) /
  `docs/dogfooding.md` の #16 を修正済みへ (20 件中 5 件修正済み)。

### M70d の実施メモ (計画との差分)

- 計画の 10 件 (NoHash ゲート / メタデータ / FIELDS 32 / `MyePlaySoundHere` / #18 / #7 / #4 /
  #12 / #3・#5・#6・#8・#9 の文書) はすべて実施。ABI は**触っていない** (v16 = 110 のまま)。
- **ユーザー判断で 2 つ広げた (2026-09-07)**: (1) 計画 M64c (5) の **C# レーンの底上げ**を含める、
  (2) メタデータを**主要スクリプト 20 本すべて**へ付ける。
- **計画との差分 1**: `MYE_F_JP` / `MYE_F_RANGE` は「メタデータ付きの項目だけ括弧付きタプルへ
  展開し、`MYE_SF` が括弧の有無で分岐する」形にした (`MYE_SF_IS_PAREN`)。**既存の書き方が
  1 文字も変わらない**のが要点。/Zc:preprocessor は既に既定で入っている。
- **計画との差分 2**: `MyePlaySoundHere` の修正には**ワールド位置を読む口**が要るので
  `MyeGameObject::GetWorldPosition` を足した (計画の表には無い。WorldMatrix の汎用フィールド
  読み = ABI 追加ゼロ)。★**親が無いときはローカル位置を返す**規則にしてある —
  `WorldMatrix` は生成時から単位行列で存在するので「まだ TransformSystem が回っていない」と
  「本当に原点に居る」を行列からは区別できず、素直に行列だけを読むと **Start から呼んだ
  ルートエンティティが黙って原点で鳴る**という新しい罠を作ってしまう (実測で気づいた)。
- **計画との差分 3**: `MyeQuatFromEuler` の中身で **CRT の `sinf`/`cosf` は使えない**
  (`AeroSampling.cpp` の注記 = 実装依存でビットが動く。作った回転はハッシュ対象へ入る)。
  `WatcherFpsCamera` (M65g) が持っていた 9 次多項式を `MyeSinRad` / `MyeCosRad` として
  **1 命令も変えずに** `ScriptAPI.h` へ引き上げ、同スクリプトはそれを呼ぶ形へ寄せた。
  実際 golden 24 枚が maxDiff=0 のままなので、ビット一致は実測で確認できている。
- ★**触らなかったもの**: `Rotator.cpp` の `sinf`/`cosf`。これも同じ CRT 依存だが、
  多項式へ寄せると値が動いて **golden (fog / render / parts の Spinner) が動く**。
  「決定論の穴だが機種差でしか出ない」ので、絵を動かす価値と釣り合わない。
  **次に golden を撮り直す用事があるサブで一緒に寄せる**のが安い (申し送り)。
- **計画との差分 4**: `SetComponentField` の NoHash 開放に伴い、`SchemaSelfTest` の
  「読み書きとも遮断」の 1 検査を**非対称の 3 検査**へ (読み 0 / 書き 1 / 実際に値が入った)。
- **計画との差分 5 (C# レーン、ユーザー判断)**: `MyeScript` / `MyeEntity` へ
  CharacterController 4 本 / `Instantiate` / `FindByFileId` / `PlayEffect` / `EmitterBurst` /
  `SetEmitterPlaying` / `RestartEffect` / `SetAnimatorParam` / `SetTextMeshText` /
  `SetMeshRenderer` / `DebugDrawLine` / `Overlap*` / `SphereCast*` / `RaycastMasked` /
  `NameHash` / `WorldPosition` を公開し、**`tickIndex` を `MyeScript.Tick` として渡す**
  (`ScriptRuntime.Invoke` が受け取っておきながら捨てていた。C# には他に決定論的な時間
  カウンタが無い)。★`UISetFocused` / `LoadPersist` / `SetUIRect` は**開けていない** —
  M70c の判断 (ハッシュ対象を C# から書かせない) をそのまま守る。
- **テストの置き場**: 新スイートは作らず `SchemaSelfTest` に足した (45 スイートのまま)。
  検査は「マクロの展開 3 種 + layoutHash に混ざらないこと + エンジン側変換 + 角度ヘルパの
  DirectXMath 照合 + ワールド位置の親合成」。
- 実測 (すべてローカル):
  - 8 ビルド 0 警告 (`/p:MyeWarnAsError=true`) / C# も `TreatWarningsAsErrors` で 0 警告
  - Debug・Release の `--selftest` 全 PASS / `check_rules.ps1` 0 error
  - `replay_verify.bat` 10 ジョブ全 PASS (118.9s) / `shot_verify.bat` **24 枚すべて
    maxDiff=0 diffPixels=0** (golden は 1 枚も動かない = 挙動を変えていないことの確認)
  - `net_verify.bat` 4 ケース + desync 注入 PASS
  - **Inspector 目視**: `Editor.exe --fog-demo --select Spinner --lang ja` の一時スクショで
    `Rotator` が「回転速度 (度/秒)」「現在の角度 (度)」で出ること、アセット一覧に
    `builtin://` 6 種が並ぶことを確認
  - **C# は一時 probe で実走確認** (`abi-bump-verification` の手順)。実測値: `Tick=2` /
    子エンティティの `local=5 → world=6` / `Fog` への `SetField=True` かつ `TryGetField=False`
    (非対称そのもの) / 型サイズ違いの Get は False / `SetMeshRenderer("builtin://cylinder")=True`
    (#12 の修正が Runtime で効いている証拠) / `Instantiate` が fileId を返す。probe は削除済み
- 文書更新: `engine_spec.md` §5.2 (メタデータの節を追記 + 規約を「共有する値」の話へ) /
  §12.2 の表と見出し (M0-M70、283 コミット) / §12.3 から M70d の行を削除し dogfooding の
  残り 5 件へ差し替え / `README.md` に機能の項 + Inspector の行 /
  `docs/dogfooding.md` を **20 件中 15 件決着 (実装 10 / 文書と確認 5)** へ。

## 次のタスクの置き場 (M70 では実装しない)

`plans\` に 1 ファイルで置き、`engine_spec.md` §12.3 の「未着手」表にも 1 行ずつ足す:

| 次 | 内容 | 備考 |
|---|---|---|
| 4-C | NavMesh + 決定論 A* | 敵 AI の基礎。音響ナビ (`AcousticNav`) は特殊解 |
| 4-D | アニメーション深度 (1D/2D ブレンドツリー / 2 骨 IK / ルートモーション / アニメイベント) | 現状はクロスフェードのみ |
| 4-E | カットシーン・シーケンサ (tick ベース) | tick ベースなら replay と golden にそのまま載る |
| 4-F | メッシュ LOD + GPU オクルージョンカリング | 既存 `HzbPass` (M56c) を再利用できる |
| 4-G | ビヘイビアツリーのグラフエディタ | `AnimatorControllerWindow` のノードグラフ UI を再利用 |
| 追 | UI スケール規則の拡張 | Unity の `Match Width Or Height` (対数空間 lerp) と `Shrink`、基準解像度の project_settings 化。**後から足しても既存シーンは壊れない**ので M70b では入れない |

`docs/dogfooding.md` の台帳も更新する。M70 後の未解決は **5 件** (#11 / #13 / #17 / #19 / #20) に減る:

| # | 残る理由 |
|---|---|
| 11 | `--debug-draw-log` の CLI + tick 内ダンプの新設 (`EngineConfig` の `hashDumpPath` / `acousticDumpFrame` と同型で置ける) |
| 13 | `builtin://wheel`。`DemoContent.cpp:1565` の `RegisterWheelMesh` (46 行) を `MeshLibrary` へ移す + プリミティブ列挙の文書化 |
| 17 | CC ⇄ Rigidbody の双方向。`SolveCharacters` (`PhysicsSystem.cpp:931-936`) が `outContacts` を持たない設計判断そのものを変えるので回帰面積が大きい |
| 19 | `PhysicsEnvironment` 不在の WARN。`PhysicsSystem.cpp` には `MYE_LOG_WARN` が 1 件も無く、`ResolvePhysicsEnvironment` は毎 tick 呼ばれる = 「起動時に 1 行」の置き場を新設する必要がある |
| 20 | スクリプト間メッセージ。`MyeScriptDesc` に `onMessage` を足す = 次の ABI bump の設計から |
