# MyEngine ドッグフーディング記録

このエンジンを **外部プロジェクトの作者の視点**で使い、仮ゲーム（HAL Collector）を作る過程で
分かったことの記録。エンジン側のショーケース（`--physics-demo` / `--joint-demo` 等）は
エンジンリポジトリの C++（`DemoContent.cpp`）から組まれているため、
「プロジェクトを開く → スクリプトを書く → アセットを置く → ゲームにする」という経路だけが
検証されていなかった。ここはその経路で踏んだ穴と、良かった点を貯める場所。

書式は **何をしようとした / 何が無かった / どう回避した / エンジンをどう直すべきか**。

- 対象エンジン: M63e 時点 (`8313eb9`) から開始、API v14 →
  **1 番を直して M64a / API v15**、さらに **14 / 15 番を直して M64b**
  (M64b は ABI を触っていないので API は v15 のまま。snapshot 版だけ 8 → 9)
- 対象プロジェクト: 仮ゲーム「HAL Collector」。`--project` で開く**エンジンリポジトリの外**の
  プロジェクト（作業時は `C:\HAL\GameEngin_Demo`）。シーン 7 枚、`main` が 39 エンティティ、
  タイトル → ゲーム → リザルトの一周
- 進捗: Phase 1（歩く・視点切替・拾う）/ 1.5（マウスルックのエンジン修正）/ 2（撃つ）/
  **3（車）/ 4（タイトル・ポーズ・リザルト・ハイスコア・デバッグ表示）まで完了**

## 状態の台帳 (M70a 時点)

**20 件中 4 件が修正済み、16 件が未解決。** 番号は下の節に対応する。

| # | 内容 | 状態 |
|---|---|---|
| **10** | **スキーマ未登録のコンポーネントがエディタ保存で黙って消える** | **修正済み (M70a)** |
| 1 | マウスの視点操作が書けない | 修正済み (M64a / API v15) |
| 14 | `Active{enabled:0}` が階層に伝播しない | 修正済み (M64b) |
| 15 | 2 つ目以降のスクリプトの `Start()` が呼ばれない | 修正済み (M64b) |
| 2 | スクリプトの調整フィールドに Inspector のメタ情報を付けられない | **文書で決着** — 「調整値はスキーマコンポーネントへ」を規約として `engine_spec.md` §5.2 に明記した |
| 3 | `Start()` の時点では WorldMatrix が無い（空間クエリが使えない） | 未解決（`engine_spec.md` §5.3 に罠として明記済み） |
| 4 | `Instantiate` / `PlayEffect` の「親なし」の渡し方が罠 | 未解決（1 行修正） |
| 5 | CharacterController のカプセルが Transform のスケールを拾う | 未解決（コメント追加で足りる） |
| 6 | `CharacterJump` は接地を見ない | 未解決（コメント追加で足りる） |
| 7 | 角度 → 四元数のヘルパがスクリプト側に無い | 未解決（`ScriptAPI.h` の糖衣。ABI bump 不要） |
| 8 | Skybox の cubemap モードが未実装 | 未解決 |
| 9 | エンティティ参照が名前引きに寄りがち | 未解決 |
| 11 | スクリプトのレイキャストをヘッドレスで追えない | 未解決（`--debug-draw-log` 案） |
| 12 | `builtin://` プリミティブが遅延生成で Runtime では半分解決しない | 未解決 |
| 13 | プロジェクト側に車輪メッシュを用意する手段が無い | 未解決（`builtin://wheel` 案） |
| 16 | `LoadGame` がセーブ時のシーンへ必ず遷移する | 未解決（`LoadPersist(slot)` 案） |
| 17 | CharacterController と Rigidbody が一方通行 | 未解決 |
| 18 | `MyeGameObject` は回転を書けるのに読めない | 未解決（1 行） |
| 19 | `Vehicle` があるのに `PhysicsEnvironment` が無いと黙って柔らかい車になる | 未解決（起動時 WARN 案） |
| 20 | スクリプト間で値を渡す手段が名前引きしかない | 未解決 |

---

## 1. マウスの視点操作が書けない → **修正済み (M64a / API v15)**

**やろうとしたこと** — 一人称視点をマウスで回す。

**無かったもの** — `InputSnapshot`（`src/Engine/Platform/Input.h`）が持つマウスの情報は
**クライアント座標の絶対値だけ**で、フレーム間デルタが無い。ABI（`src/Shared/EngineAPI.h`）にも
`GetMouseDelta` に相当するスロットも、カーソルを画面内に閉じ込める口も無い。
エンジン本体は `ImGui::GetIO().MouseDelta`（`SceneViewWindow.cpp:1128`）でエディタカメラを
回しているが、これはゲーム側からは触れない。

**Phase 1 での回避** — 視点入力を `LookX` / `LookY` の**軸アクション**にし、矢印キーと
右スティックで回した。絶対座標の差分を自前で取る手もあるが、カーソルを戻せない以上
窓の端で視点が止まるので採らなかった。

**Phase 1.5 で実際に直したもの** — 以下を 1 度の bump で束ねた（「ABI は各マイルストーン末に
1 回」の運用）。

| 層 | 変更 |
|---|---|
| `Platform/Input.h` | `InputSnapshot` に `mouseDeltaX` / `mouseDeltaY`(int32)。64 → **72 バイト** |
| `Platform/Input.cpp` | `WM_INPUT`（Raw Input）で生カウントを積む / `AttachRawInput` / `ApplyCursorLock` |
| `Replay.h` | `kReplayFileVersion` 4 → **5** |
| `SimSnapshot.h` | `kSimSnapshotVersion` 7 → **8**（LOP 節の `prevTickInput` が太る） |
| `NetSession.h` | `kNetProtoVersion` 2 → **3**（1 パケットの本体長が変わる） |
| `Shared/EngineAPI.h` | `GetMouseDelta` / `SetCursorMode` の 2 スロット、`MYE_API_VERSION` 14 → **15** |
| `EngineApiTable.*` / `EngineLoop.cpp` | スロット充填、`CursorLockState` の配線、フレーム末の適用 |
| `Interop.cs` / `MyeScript.cs` | 位置ミラー + 作者向けラッパ |
| `check_rules.ps1` | `$apiVersionSlots` に `15 = 104` |
| `PartSelfTest.cpp` / `InputActionsSelfTest.cpp` / `NetSelfTest.cpp` | 版とサイズの表明、新フィールドの回帰 |

**直しながら分かったこと（ここが本題）**

- **絶対座標の差分では原理的に成立しない。** カーソルをロックすると絶対座標は動かなくなる
  （矩形に貼り付く）ので、差分方式は「ロックした瞬間に視点が止まる」。
  Raw Input の生カウントは**カーソル位置と無関係**なので、ロックと共存する唯一の入力源。
  ついでにポインタ加速が掛からないので OS のマウス設定にも依存しない
  （代わりに **DPI が機種依存**になるので、感度は調整値（`GameTuning.mouseSensDeg`）で割る）。
- **`RIDEV_NOLEGACY` を付けてはいけない。** 付けると `WM_MOUSEMOVE` / ボタンメッセージが
  止まり、ImGui とエディタのヒットテストが同時に死ぬ。生デルタは**追加で**受けるだけにする。
- **カーソルロックには「エンジン側の逃げ道」が要る。** エディタで Play 中にロックすると
  Stop ボタンが押せなくなる。`Escape` でエンジンがロックを手放す作りにし、
  **ゲームが `SetCursorMode(0)` を出し直すまで再ロックしない**ようにした
  （毎 tick `1` を書き続ける実装に Escape を握り潰させないため）。
- **バッチ実行を明示的に除外する必要があった。** `--frames` / `--screenshot` の実行は
  record/verify を通らないので、既存の抑止条件（記録中・検証中・フォーカス喪失）だけでは
  **CI とスクショ検証がデスクトップのカーソルを奪う**。`config.maxFrames > 0 ||
  !config.screenshotPath.empty()` を条件に足した。人が座っていない実行の判定が
  出力レーンの抑止条件に必要、というのは振動（M51h）のときには出てこなかった観点。
- **`ShowCursor` は内部カウンタ。** 同じ向きに 2 回呼ぶと戻らなくなるので、状態が
  変わった tick だけ 1 回呼ぶ。終了時のリセットも要る（`ApplyVibration(0,0)` の隣）。
- **合成入力に載せて初めて被覆になる。** `SynthLaneInput` はマウスの**位置**を今も動かさない
  （動かすと GameView のヒットテストが誤爆する）が、**デルタは載せた** — どのヒットテストにも
  入らない純粋な視点入力なので、これで `.rep` と SimSnapshot の往復照合に新フィールドが乗る。
- **積分される入力を合成するときは平均を 0 にする。** 最初 `((h >> n) & 15) - 7` と書いたら
  範囲が `[-7, +8]` で**平均 +0.5 カウント**になり、これを積分する視点角が 1400 tick 後に
  ピッチ上限 80 度へ張り付いた（合成入力の実行が「ずっと真下を向いて歩くだけ」になり、
  絵としても診断としても読めない）。独立な 3bit を 2 本引いて差を取る形に直し、
  自己検査に「4096 tick の平均が 0.15 カウント未満」の回帰を足した。
  ★**押しっぱなしのキー**では起きない種類の事故で、デルタ型の入力を足したときに初めて出る。

**検証** — `--selftest` 全パス / `check_rules.ps1` 0 error / デモゲームで
`VERIFY PASS: 600 ticks hash-identical`（Debug・Release 双方）。
さらに `mouseSensDeg` を 0.12 と 0.0 にして同じ合成入力で録った `.rep` が
**別のバイト列になる**ことを確認した（= 生デルタが本当にワールドハッシュまで届いていて、
上の VERIFY PASS が自明な合格ではないことの証明）。

---

## 2. C++ スクリプトの調整フィールドに Inspector のメタ情報を付けられない

**やろうとしたこと** — 移動速度やカメラ距離を Play 中に Inspector で詰める。

**無かったもの** — `MyeScriptField`（`src/Shared/ScriptTypes.h:30`）は `{name, type, offset}` の
3 つだけ。エンジン内蔵コンポーネントが持つ `MYE_JP("表示名", ...)` のような**表示名・
ツールチップ・min/max・ドラッグ速度**を、スクリプトのフィールドには一切付けられない。
結果、Inspector には英語のフィールド名と素のドラッグウィジェットしか出ない。

**回避** — 調整値を**スキーマコンポーネント**へ逃がした
（`assets/schemas/game_tuning.component.schema.json` の `GameTuning`）。スキーマ側は
`display` / `tooltip` / `min` / `max` / `speed` を持てるので、日本語ラベル付きのスライダが
自動生成される。スクリプトは `MyeGetField` で読むだけにした。
**結果的にこれは良い設計**（データとロジックが分かれ、複数スクリプトから同じ値を読める）
なので、回避というより「そう書くのが正解」だった。ただしそれが分かる文書はどこにも無い。

**直すなら** — `MyeScriptDesc` にフィールドのメタ配列を足す（ABI 追加）か、
**「調整値はスキーマへ」を規約として README / engine_spec に明記**する。後者のほうが安い。

---

## 3. `Start()` の時点では WorldMatrix がまだ無い（空間クエリが使えない）

**やろうとしたこと** — ゲーム開始時にアイテムを撒く。真下へレイを飛ばして、床でも箱の上でも
その面の上に置きたかった。

**踏んだ罠** — `Start()` はスクリプト層（フェーズ 3）で走り、`TransformSystem`（フェーズ 4）は
その**後**（`TickRunner.cpp:332`）。シーンを読み込んだ直後の最初の tick では
`WorldMatrixComponent` がまだ確定しておらず、`RaycastMasked` が**何にも当たらない**。
エディタでは Play 前に毎フレーム描画が走っていて行列が埋まっているので**気付きにくく、
Runtime.exe（描画前に tick が回る）でだけ壊れる**という一番たちの悪い形になる。

**回避** — 散布を `Start()` ではなく `Update()` に置き、`elapsedTicks >= 1`（= 2 tick 目）まで
待ってから撒く（`GameDirector::SpawnPickupsOnce`）。

**直すなら** — `Start()` を呼ぶ直前に一度 `TransformSystem::Update` を回すか、
できないなら `EngineAPI.h` の空間クエリ群のコメントに
「Start では使えない（Transform 未確定）」と明記する。

---

## 4. `Instantiate` / `PlayEffect` の「親なし」の渡し方が罠

**踏んだ罠** — `Instantiate(engine, key, pos, parent)` の `parent` に「親は無い」を渡したい。
自然に書けば `MyeEntityId{}` だが、これは `index = 0xFFFFFFFF`（null id）であって
**0 ではない**。drain 側の判定は `parent.index != 0 || parent.generation != 0`
（`TickRunner.cpp:386`）なので、null id は「親あり」に分類される。
今は `Scene::EnsureFileId` が死んだエンティティに 0 を返す（`Scene.cpp:47`）ので
結果的にルート生成になるが、**2 段階の偶然に依存**している。

**回避** — `constexpr MyeEntityId kNoParent{ 0u, 0u };` を用意して明示的に渡した。

**直すなら** — drain の判定を `MyeEntityIdIsNull(req.parent)` に変えるのが正しい
（`MathPod.h` に既にこのヘルパがある）。ABI 変更を伴わない 1 行修正。

---

## 5. CharacterController のカプセルが Transform のスケールを拾う

**踏んだ罠** — プレイヤーの見た目を出すために本体へ `scale [0.7, 1.8, 0.7]` を入れると、
`CharacterControllerComponent.height` に `scale.y` が掛かって
（`PhysicsSystem.cpp:1319`）当たり判定が 1.8 倍に伸びる。

**回避** — 本体は**無スケール**にし、見た目は子エンティティ（`PlayerBody`）へ逃がした。
`DemoContent.cpp` の車が「車体は無スケール」と同じ理由で同じ形になっている。

**直すなら** — 罠の内容は車と同一なので、Components.h の `CharacterControllerComponent` の
コメントにも `VehicleComponent` と同じ注意書きを入れる。

---

## 6. `CharacterJump` は接地を見ない

`EngineAPI.h:200` に「接地可否に関わらず消費される」と明記はされているが、名前からは
「ジャンプする API」に読める。素で呼ぶと空中で何度でも跳べてしまう。
呼ぶ側で `CharacterIsGrounded` を確かめるのが正しい使い方
（`PlayerController::Update` はそうしている）。

**直すなら** — 名前を `CharacterRequestJump` にする、あるいは
「接地判定は呼び出し側の責任」をコメントの先頭に上げる。

---

## 7. 角度 → 四元数のヘルパがスクリプト側に無い

`Shared/` は C ABI + POD のみで DirectXMath を持ち込めないため、
`XMQuaternionRotationRollPitchYaw` 相当を**ゲーム側で手で書く**必要がある
（`GameCommon.h` の `QuatFromYawPitch`）。式を間違えるとカメラだけが静かに壊れる。

**検算方法** — エンジンが `SetLocalRotationEuler(50, -30, 0)` で作った値が
シーンの Sun の四元数 `[0.40823, -0.23457, 0.10937, 0.87543]` として残っているので、
自前の式に同じ角度を入れて一致するかで確かめられる（実際これで一発で合った）。

**直すなら** — `ScriptAPI.h` に `MyeQuatFromEuler` / `MyeForwardOf` を inline で足す。
ABI 追加ではなくヘッダ内の糖衣なので `MYE_API_VERSION` の bump は要らない。

---

## 8. Skybox の cubemap モードが未実装

`SkyboxComponent.mode = 1`（Cubemap）は予約で、実際は Gradient にフォールバックする
（`Components.h:502`）。プロジェクトには `assets/textures/test_sky_cubemap.dds` が入っているのに
使えない。Gradient で足りたので実害は無かったが、アセットだけあって経路が無いのは紛らわしい。

---

## 9. エンティティ参照が名前引きに寄りがち（改善余地・未対応）

カメラ / HUD / GameRoot を毎 tick `FindByName` で引いている。`MyeScriptField` は
`MYE_FIELD_ENTITYREF`（`MyeEntityId` 型のフィールド）を扱えて、シーン JSON では fileId で
シリアライズされるので、**Inspector で参照を張って持たせるのが本来の作法**のはず。
今回は Phase 1 の範囲を広げないため名前引きのままにしてある。
規模が大きくなったら EntityRef フィールドへ移す（そのときに Inspector から実際に
参照を張れるかも検証対象）。

---

## 10. スキーマ未登録のコンポーネントは、エディタ保存で**黙って消える**（データ消失）

**踏んだ罠** — Phase 1.5 の作業中、エディタを開いたまま `GameTuning` スキーマに
フィールドを足した。その後ユーザーがエディタを保存して閉じたら、
**シーンから `GameTuning` コンポーネントが丸ごと消えていた**。

**原因** — `schema::RegisterSchemaComponents` は `EngineLoop.cpp` の起動時に**一度だけ**
呼ばれ、`assets/schemas` の監視・再読込は無い。したがって

1. エディタ起動時点で未登録だったコンポーネントは、シーンのホットリロードで読んでも
   `SceneSerializer.cpp:186` の `unknown component '%s' (skipped)` で捨てられ、
2. その状態のシーンを保存すると、**捨てた事実がそのままディスクに書き戻る**。

WARN は出ているが、起動時ログの奔流に紛れる 1 行で、実害（保存で消える）とは
結び付かない。**壊れるのは「読めなかった」瞬間ではなく「保存した」瞬間**なので、
気付くのは次にゲームを起動して調整値が全部既定値に戻ったときになる。

**回避** — スキーマを触ったらエディタを開き直す。今回は手でコンポーネントを書き戻した。

**修正 (M70a)** — 上の案 1 + 案 2 を実装した。案 3（`assets/schemas` の監視）は**採らない** —
スキーマ型は「組込みの後・スクリプト型の前」に登録される規約（M48j）なので、実行中に型を足すと
スクリプト型の TypeId が一斉にずれて既存シーンと `.rep` が壊れる。案 1 で実害は消える。

- `Scene` に `unknownComps_`（`fileId → 型名 → 生 JSON`）の側テーブルを置き、
  `ReadEntityComponents` が捨てる代わりに預け、`WriteEntity` が書き戻す。
  **ECS の外・WorldHash 非対象**で、`SimSnapshot` にも入れない（tick 中に変化しないため）。
  同名が登録済みになっていたらアーキタイプ側が勝つ = 二重書きしない。
- 保存時にトースト「未知のコンポーネント N 個を保持したまま保存しました」、
  Inspector に読み取り専用の「未知のコンポーネント (N)」行（Unity の Missing Script 相当）。
- `Scene::kDocVersion` は 3 のまま。未知がゼロのシーンは出力バイト列が 1 バイトも変わらない。

**★実害はスキーマ型より広かった** — `EngineLoop.cpp` は `GameLogic.dll` のロード失敗でも
ERROR ログだけ出して起動を続ける。その状態で保存すると**全 C++ スクリプトコンポーネントが
消えていた**（C# ホストの Init 失敗も同型）。`.actor.json` / `.prefab.json` も同じ
`SaveToJson` を通るので同じ消え方をする。M70a はこの 3 経路をまとめて塞いでいる。

**直すなら**（当時の検討。軽い順）

1. **読めなかったコンポーネントの生 JSON を保持して保存時に書き戻す**（ラウンドトリップ）。
   シリアライザとして本来こうあるべきで、`unknown` を「知らないから捨てる」ではなく
   「知らないから触らない」に変えるだけ。ハッシュ対象外の側テーブルで済む。
2. 読み込み中に unknown があったシーンは**保存時に確認を出す**（Unity の Missing Script 相当）。
3. `assets/schemas` を監視してホットリロードする。ただしスキーマ型は
   「組込みの後・スクリプト型の前」に登録される規約なので、実行中に足すと
   スクリプト型の TypeId が動く。**3 は一番重く、1 で実害は消える**。

---

## 11. スクリプトのレイキャストを目で追う手段がヘッドレスに無い

**やろうとしたこと** — 「撃っても敵に当たらない」の切り分け。

**分かったこと** — 原因は 3 つ重なっていた。(a) 合成入力のマウスデルタに直流バイアスが
あって視点がピッチ上限に張り付いていた、(b) テストのために `Fire` を `Space` に割り当てた
せいで**撃つたびにジャンプして**弾が敵の頭上を越えていた、(c) 敵はプレイヤーを追うので
移動中は常に背後にいる。どれも「当たらない」という同じ症状に見える。

**無かったもの** — `DebugDrawLine`（v7）はあるが**描画レーン**なので、
`--warp --no-audio` のヘッドレス検証では絵が残らない。結局
`MyeLogf` で始点・方向・ヒット有無・相手 index を吐いて突き合わせるしかなかった。

**回避** — 一時的にログを仕込んで実測 → 原因を 1 つずつ潰した。
`hitent=100 hasHealth=1 hp=30 → 18 → 6 → kill` まで確認してからログを外した。

**直すなら** — `DebugDrawLine` した線を `--replay-record` 中に**テキストで吐く**
オプション（例 `--debug-draw-log`）があると、ヘッドレスでも射線を後から追える。
描画レーンなのでハッシュには入らず、決定論には無関係。

---

## 12. `builtin://` プリミティブは**遅延生成**で、Runtime では半分が解決しない

**やろうとしたこと** — 車輪に円柱を使う。`builtin://cylinder` は `MeshLibrary::Cylinder()` に
あり、エディタの Create メニューにも並んでいる。シーン JSON には
`HashStr("builtin://cylinder") = 2351109162603831618` と定数で書けるはずだった。

**何が無かったか** — **実体が無い**。`MeshLibrary` の 6 つのプリミティブはすべて
遅延生成 (`GpuResources.cpp:233-453`、初回の `Cube()` / `Cylinder()` 呼び出しで作られる) で、
`builtin://` を無条件に登録する場所がどこにも無い。プロジェクトを開いた Runtime で
生きているのは **cube と sphere だけ** — しかもそれは
`RuntimeMain.cpp:60` 以降が全ショーケースの材質登録を無条件に呼んでおり、その中の
`res.meshes.Cube()` / `Sphere()` が**副作用で**作っているからにすぎない。
`cylinder` / `capsule` / `plane` / `quad` はエディタで Create メニューを開いた回だけ存在する
（`CreateMenu.cpp:42-62` と `AssetPreviewCache.cpp` が唯一の呼び手）。

つまり **「エディタで作った円柱を含むシーンが、Runtime では黙って描画されない」** が成立する。
プロジェクト作者から見れば、使える組込みメッシュの一覧がどこにも書いておらず、
しかも実行環境によって変わる。

**回避** — 車輪は cube / sphere で代用できない（下の 13 番）ので、
エンジンのデモが焼く `jdemo_wheel` に相乗りした。

**直すなら** — `MeshLibrary::Init` で 6 つとも登録してしまう（各数百頂点で、
遅延にする価値が無い）。せめて `AssetID` の解決に失敗したときに
`unknown mesh builtin://cylinder` を 1 行 WARN で出す。

---

## 13. プロジェクト側に**車輪メッシュを用意する手段が無い**

**やろうとしたこと** — Phase 3 の車に車輪を付ける。

**何が要るか** — `RenderSystem.cpp:67 ApplyWheelVisual` は車輪の**ローカル空間**で
①車軸まわりの転がり ②切れ角 ③サスの伸縮 を合成する。したがって車輪メッシュは
**軸が X 向き**でなければならず、しかも

- 車輪エンティティを寝かせて回転で辻褄を合わせることは**できない** —
  サスのレイ方向 (ローカル −Y) と前方向 (ローカル +Z) が姿勢からそのまま読まれるため
  (`PhysicsSystem.cpp:1934` 付近)。
- 非一様スケールで潰すことも**できない** — 合成はスケールが掛かる**前**に効くので、
  回すと歪む（一様スケールなら安全、というのは実測で確かめた）。

**何が無かったか** — 軸 X の円柱を作る手段。builtin は軸 Y（しかも 12 番で解決しない）、
プロジェクトに同梱のモデルは箱と人型だけ、`.obj` をその場で書く経路も無い。
エンジンのデモは `DemoContent.cpp:1557 RegisterWheelMesh` で**実行時に焼いて**いる —
これは C++ から呼ぶ関数で、プロジェクト側からは触れない。

**回避** — `RegisterJointShowcaseContent` が Editor / Runtime とも**無条件に**呼ばれる
(`RuntimeMain.cpp:109`) ことを利用し、そこで焼かれる `jdemo_wheel`
（半径 0.35 / 幅 0.25、ハッシュ `8592444148444852898`）をシーン JSON から直接引いた。
**エンジンのデモ資産への相乗り**であって、外部プロジェクトの正しい姿ではない。

**直すなら** — `builtin://wheel`（軸 X の単位円柱、半径 0.5）を `MeshLibrary` に足す。
一様スケールは安全なので、半径 0.35 の車輪はスケール 0.7 で作れる。
12 番と合わせて「組込みプリミティブは常に全部ある」にするのが本筋。

---

## 14. `Active{enabled:0}` が階層に伝播しない → **修正済み (M64b)**

**やろうとしたこと** — 車に乗っているあいだプレイヤーを止める。計画どおり
`Player` に `Active{enabled:0}` を書いた。スクリプトも衝突も止まり、
狙いどおり「運転中は歩けない・車に轢かれない」が成立した。

**何が起きたか** — **見た目だけ残った**。運転中もプレイヤーの箱が地面に立っている。
`ActiveComponent` の注記どおり「現状は自エンティティのみ判定」で、
`MeshRenderer` を持っているのは子の `PlayerBody` / `PlayerNose` なので、
親を止めても描画は止まらない。sim（スクリプト・物理・パーティクル）は親で止まるのに
描画だけ止まらないので、**「消えるはず」と思って書くと必ず踏む**。

**回避（当時）** — `VehicleDriver::ApplyPlayerActive` で子の `Active` も一緒に倒していた。

**修正 (M64b)** — `IsEntityActive` を**自分と祖先すべて**を見る形にした
（`Engine/Core/Components.cpp`）。1 つでも無効なら無効、という Unity と同じ規約。

```cpp
for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
    const auto* a = world.GetComponent<ActiveComponent>(cur);
    if (a != nullptr && a->enabled == 0) {
        return false;
    }
}
return true;
```

- 親子の循環は `World::ApplySetParent` が拒否するので走査は必ず終わる。祖先を辿る形は
  エンジン内に前例がある（`PhysicsSystem` の車輪 → 剛体探索、`Parts::RaycastParts` の root 判定）。
- 全エンティティが `HierarchyComponent` を必ず持つ（`World` の基本アーキタイプ）ので、
  ルート 1 段なら従来 + 1 回の `FindTypeIndex` で済む。
- 検証: `SceneSelfTest` に 6 ケース追加（部分木が落ちる / 兄弟に漏れない /
  子は自分で有効に戻せない / 祖先を戻すと復活する）。スクショ回帰 17 枚は
  `maxDiff=0` のまま — エンジン同梱のシーンには「無効な親 + 子」が 1 つも無いので、
  既存の絵は 1 画素も動かない。

プロジェクト側の回避コードは削除し、親を 1 つ倒すだけに戻した。

---

## 15. 2 つ目以降のスクリプトの `Start()` が呼ばれない → **修正済み (M64b)**

**やろうとしたこと** — `GameRoot` に `GameDirector` と `PauseMenu` を同居させる
（どちらも「シーン全体の面倒を見る」役なので、置き場は同じでよい）。

**何が起きたか** — `PauseMenu::Start()` のログだけが出ない。`Update()` は毎 tick 走っている。

**原因** — `ScriptHost.cpp:40`

```cpp
uint64_t StartedKey(EntityID e) { return (index << 32) | generation; }
```

`started_` は **エンティティ ID だけ**をキーにしている。同じエンティティの 2 つ目の
スクリプトは、1 つ目が既にキーを入れているので `Start` が飛ばされる。
`Update` / `LateUpdate` は無条件に呼ばれるため、**「初期化だけ効かない」**という
一番デバッグしづらい形で出る。

**修正 (M64b)** — キーを `(エンティティ, スクリプト型)` の 2 語にした。

エンティティ側は `index<<32 | generation` で 64bit を使い切っているので、型を同じ語へ
詰めることはできない。`ScriptStartedKey { uint64_t entity; uint64_t script; }` を作り、
`started_` を `std::unordered_set<uint64_t>` から **`std::set`** へ変えた
（走査順そのものが決定論になるので、`SimSnapshot` 側の「書く前に昇順へ整列する」
という約束が 1 つ消える）。

- `SimSnapshot` の SCR 節は 1 語 → 2 語交互になるので **`kSimSnapshotVersion` を 8 → 9**。
  古い blob は版で弾かれる（ハッシュ不一致という分かりにくい形にならない）。
  `.rep` のワイヤ形式は変わらないので `kReplayFileVersion` は据え置き。
- 検証: このゲームの `GameRoot`（`GameDirector` + `PauseMenu` の 2 本）で
  `[game] PauseMenu start` が出るようになった。エンジン側は selftest 2888 PASS /
  replay 9 ジョブ / スクショ 17 枚が全てグリーン。

---

## 16. `LoadGame` は**セーブ時のシーンへ必ず遷移する**ので、ハイスコアだけ読めない

**やろうとしたこと** — タイトル画面で前回のハイスコアを出す。

**何が起きたか** — セーブファイルは `PersistStore` と**現シーンパス**の両方を持ち
(`SaveGame.h`)、`LoadGame` は読み込んだシーンパスを `pendingScene` に積む
(`TickRunner.cpp:570`)。つまり本編中にセーブすると、次回起動のタイトルで
`LoadGame(0)` した瞬間に**本編へ飛ばされる**。「永続値だけ読み戻す」ができない。

**回避** — **セーブをタイトルに居るあいだに書く**ことにした。復元先もタイトルになるので、
`LoadGame` は「自分自身の読み直し」で済む。その 1 回の読み直しが無限ループに
ならないよう、`PersistStore` に `saveLoaded` の旗を立てて番をしている
（旗はロードで上書きされるが、セーブ時に必ず 1 が立っているので結果として安定する）。
時間切れ → リザルト → タイトル → そこで初めてディスクへ書く、という流れ。

**直すなら** — シーンを切り替えない読み込み（`LoadPersist(slot)`）を 1 本足す。
あるいは `SaveGame(slot, includeScene=false)` で「シーンを記録しないセーブ」を選べるようにする。

---

## 17. CharacterController と Rigidbody は**一方通行**（車で轢く判定が書けない）

**やろうとしたこと** — 走っている車で敵（`CharacterController`）を轢く。
素直に `OnCollisionEnter` で書こうとした。

**何が起きたか** — **一度も飛ばない**。物理は剛体を `bodies`、キャラを `chars` という
別の配列で解いており (`PhysicsSystem.cpp:1287` 付近)、接触イベントは剛体同士
(`SolidContact`) からしか生まれない。キャラは車に当たって止まるのに、
**車は何も感じない**。押し返しも無いので、車で人だかりに突っ込むと素通りに見える。

**回避** — `VehicleDriver` が自前で近傍を見る。「水平速度が閾値以上のときだけ、
進行方向の前方に置いた球で `OverlapSphereMasked` して Health を削る」。
最初は球を**車の中心**に置いたが、車体は前後 3.6m あるので半径 2.4m では
前バンパーの 0.6m 先までしか届かず、敵の集団へ全速で突っ込んでも 1 体も轢けなかった。
**判定を進行方向へ 1.6m ずらして解決**（向きは速度から取るので後退でも正しい）。

**直すなら** — キャラ ↔ 剛体の接触もイベントとして報告する。運動量の交換まで
やらなくても、「触った」ことさえ通知されれば轢く・押されるはゲーム側で書ける。

---

## 18. `MyeGameObject` は回転を**書けるのに読めない**

`MyeGameObject` は `GetLocalPosition` / `SetLocalPosition` / `SetLocalRotation` を持つが、
**`GetLocalRotation` が無い**（`ScriptAPI.h:250`）。ABI にはスロットが存在する
（`EngineAPI.h:148`）ので、`ctx.api->GetLocalRotation(...)` を直に叩けば済むが、
糖衣が非対称なのは気付きにくい。車の向きは物理が決めるので**読むしかない**
（自分でヨーを積んでいる `PlayerController` と違って）、Phase 3 で初めて踏んだ。

**直すなら** — 1 行足すだけ。ついでに `GetLocalScale` も無い。

---

## 19. 車を置くと `PhysicsEnvironment` が必要になり、**シーン全体の物理が変わる**

車輪のばね（`stiffness` 60000 N/m）は 1 tick 1 回積分では硬すぎるので、
`PhysicsEnvironment.substeps` を上げる必要がある（エンジンのデモは 16）。
ところが `PhysicsEnvironment` は**存在ゲート**で、置いていないシーンは substeps 1 相当の
経路を通る。つまり **車を 1 台足すために、既にあるプレイヤーや敵の物理まで変わる**。

**回避** — Phase 3 で `substeps = 8` の `PhysicsEnv` を置き、`.rep` を録り直した
（決定論は保たれるが、以前の記録とはハッシュが変わる）。

**直すなら** — 車輪だけサブステップを増やす、は物理として筋が悪い。せめて
「`Vehicle` があるのに `PhysicsEnvironment` が無いシーン」で起動時に 1 行 WARN を出す。
今は**何も言わずに柔らかい車**になるので、原因に辿り着きにくい。

---

## 20. スクリプト間で値を渡す手段が「名前引き + フィールド書き込み」しかない

轢いた撃破を得点に足すとき、得点を持っているのは `PlayerController` で、
乗車中のプレイヤーは止まっているので自分では数えられない。結果、
`VehicleDriver` が `MyeSetField(player, "PlayerController", "score", ...)` と
**他エンティティのスクリプト状態へ書き込む**しかなかった。
Phase 1 で「読みは一方向」と決めた設計が、ここで崩れている。

エンティティ参照型が `FIELDS()` に無い（9 番）ことと同根で、
「このスクリプトはあのスクリプトに話しかける」を**シーン上で結べない**のが原因。

**直すなら** — 9 番（EntityRef フィールド）に加えて、
イベント／メッセージの仕組みが 1 段あると素直に書ける。最小なら
`SendMessage(entity, nameHash, int32)` のような 1 スロットでも、
「誰が誰に何をした」をスクリプトの外に出せる。

---
## 良かった点（そのまま動いた）

- **プロジェクト内 C++ スクリプトのビルド経路が完全に自動**。
  `<project>\src\GameLogic\Scripts\*.cpp` を置くだけで、エディタが `cache\GameLogic.vcxproj` と
  `GameLogicMain.cpp` を生成し、エンジンの `build\Common.props` を import して
  同じフラグでビルドする。エンジンリポジトリの `.vcxproj` に一切触らなくてよい。
  スクリプト 3 本 + 共有ヘッダ 1 本が**一発でコンパイルを通った**。
- **シーン JSON を環境非依存で手書きできる**。builtin メッシュは
  `HashStr("builtin://cube") = 1506918697593860217` と定数で、ファイル資産は同伴 `.meta` の
  `guid` が尊重される（`AssetDatabase.cpp:230`）ので、`.mat.json` と `.meta` を自分で書けば
  チェックアウト先に依存しない ID になる。
- **スキーマコンポーネントが強力**。JSON にフィールドを書くだけで
  Inspector UI / シーン保存 / ワールドハッシュ / `SetComponentField` が全部通る。
  実行時デバッグ UI がこれだけで手に入る。
- **プレハブ + `Instantiate` がそのまま動く**。`.actor.json` に
  スキーマコンポーネントもスクリプトも書けるので、拾えるアイテム 1 個の定義が 1 ファイルで済む。
- **決定論がプロジェクト側スクリプトでも成立した**。`std::sin/cos/sqrt` を使い、
  `Instantiate` で 24 体を生成し、トリガーで破棄する 600 tick を録って、
  **Release で記録した `.rep` が Debug でハッシュ一致**した（`VERIFY PASS: 600 ticks
  hash-identical`）。エンジンの一番大きな主張が、外部プロジェクトでもそのまま効いている。
- `--synth-input` が「人がいなくてもプレイヤーが歩き回る」検証手段になり、
  トリガー回収の実地確認に使えた。
- **スキーマコンポーネントが「ゲーム固有の型」の受け皿として完成している**。
  敵の体力はエンジン同梱の見本 `health.component.schema.json` をそのまま使えて、
  `Health` を持つかどうかだけで「撃てる相手か」を判定できた
  （スクリプトの有無で判定しないので、プレハブを差し替えても壊れない）。
- **`PlayEffect` + `Effect.autoDestroy` が使い捨てエフェクトとして過不足ない**。
  着弾も敵の死亡も `PlayEffect("prefabs/hit.actor.json", pos, 親なし)` の 1 行で、
  寿命が来たら自分から消えるので回収コードが 1 行も要らない。
- **車が ABI 追加ゼロで組めた**（Phase 3）。運転入力 `steer` / `throttle` / `brake` は
  `Vehicle` の sim 状態フィールドなので `MyeSetField` で書ける。エンジンが M60 で主張した
  「車両のために専用スロットを足さない」が、外部プロジェクトから見ても本当に成立していた。
  おまけに入力が sim 状態なので、`.rep` とスナップショットが**何もしなくても運転操作を運ぶ**。
- **車両の諸元は GameTuning へ逃がす必要が無かった**。`Vehicle` / `Wheel` は
  コンポーネント側に Inspector メタ（表示名・レンジ・ツールチップ）を持っているので、
  車体を選べばそのままスライダで触れる。2 番の問題は**スクリプトのフィールドだけ**の話だと
  はっきりした。
- **`ApplyWheelVisual` の設計が効いている**。転がり・切れ角・サスの伸縮を描画側で合成する
  ので、車輪が回ってもワールドハッシュは 1 バイトも動かない。実際、車を足しても
  `--replay-verify` は素通しで通った。
- **`PersistStore` がシーン跨ぎの受け渡しにそのまま使えた**（Phase 4）。
  `Scene::Clear` で消えず、しかもハッシュ対象なので、リザルト画面へ成績を渡す経路が
  そのまま record/verify に載る。「シーンを跨ぐ値」の置き場として過不足ない。
- **ポーズがスクリプト層を止めない**のが正しかった。止まるのはアニメ/物理/衝突/パーティクル
  だけなので、ポーズメニュー自身が自分でポーズを解除できる。各スクリプトが
  `MyeIsPaused` を見て自分で降りる、という規約さえ守れば素直に書ける。
- **決定論がシーン遷移を跨いで成立した**。タイトル → 本編 → リザルト → タイトルと
  `LoadScene` で回しても、`--replay-verify` は 600 tick ハッシュ一致のまま
  （Release で録って Debug で照合）。

---

## 検証に使ったコマンド

```
rem スクリプトのビルド (エディタの Rebuild Scripts と同じ。CI 用に直接叩く場合)
MSBuild cache\GameLogic.vcxproj /p:Configuration=Release /p:Platform=x64

rem 決定的スクリーンショット (エディタ UI 無し)
Runtime.exe --project <dir> --warp --no-audio --frames 240 --shot-frame 239 --screenshot shot.png

rem 人がいなくても歩く
Runtime.exe --project <dir> --warp --no-audio --synth-input --frames 700 ...

rem 決定論の記録と照合 (Debug/Release 相互)
Runtime.exe --project <dir> --warp --no-audio --synth-input --replay-record cache\game.rep --replay-ticks 600 --replay-fast
Runtime.exe --project <dir> --warp --no-audio --replay-verify cache\game.rep

rem エンジン本体に手を入れた回 (Phase 1.5) に追加で回すもの
bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\replay_verify.bat

rem 本編以外のシーンを直接確かめる (bootScene はタイトルなので --scene が要る)
Runtime.exe --project <dir> --scene <dir>\assets\scenes\main.scene.json --warp --no-audio ...
```

### 人が居ないところで「車」を確かめる方法

`--synth-input` が叩くのは **WASD / Space / パッド A** だけで、`E`（乗降）もマウスも
押されない。つまり合成入力だけでは**車に一度も乗らないまま** `VERIFY PASS` が出てしまう。
そこで Phase 3 では 3 つの入口を用意した。

1. `GameTuning.carAutoDrive = 1` — 無人のまま S 字を走る。人が居なくても車両ソルバが
   ハッシュを動かすので、「車を足したのに何も変わっていない」を検出できる。実際、
   `carAutoDrive` を 0 / 1 で録った 2 本の `.rep` は**別バイト列**になった。
2. シーン JSON の `VehicleDriver.aboard` を 1 にして始める — 追従カメラ・プレイヤーの
   非表示・運転入力の経路が、合成入力の WASD だけで一通り走る。
3. `actions.json` の `Interact` に一時的にパッド A を足す — 合成入力が乗降をトグルし続けるので、
   状態遷移（乗る → 降りる → 位置と視点の引き継ぎ）が回る。検証が済んだら戻すこと。

`GameTuning.carAutoDrive` と `GameDirector.debugOn`（F3 のオーバーレイ）をシーン JSON で
立てられるようにしてあるのは、この 3 つを**エディタ無し**でやるため。

### 「合格が自明でないこと」の確かめ方

`--replay-verify` は、新しい入力フィールドがどこにも読まれていなくても PASS する。
そこで **調整値を変えた 2 本の `.rep` が別バイト列になること**を見て、その入力が本当に
sim を動かしていることを先に証明してから VERIFY PASS を主張している。

```
rem 同じ合成入力で、マウス感度だけ 0.12 / 0.0 に変えて録る
Runtime.exe --project <dir> --warp --no-audio --synth-input --replay-record cache\a.rep --replay-ticks 400 --replay-fast
rem  (シーンの GameTuning.mouseSensDeg を 0.0 にして)
Runtime.exe --project <dir> --warp --no-audio --synth-input --replay-record cache\b.rep --replay-ticks 400 --replay-fast
rem  a.rep と b.rep のハッシュが違えば、入力はワールドハッシュまで届いている
```
