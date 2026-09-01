# M65: 音響伝播 — 音が世界を描き、敵がそれを聞く (7 サブ)

**再開手順**: `git log --oneline -5` で最後に完了した M65x を確認 → 本ファイルの進捗表と突き合わせ →
次のサブの節を読む → 着手前にそのサブの「冒頭確認」を先に潰す。
1 サブ = 1 コミット (`M65a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルの進捗表には**計画外の事実・罠・申し送りのみ**書く。

> ★実装セッションの最初の作業: **本ファイルを `plans\hushed-rippling-beacon.md` へコピーして
> コミットに含める** (家風。`plans\vivid-spinning-ember.md` (M63) と同じ体裁)。

---

## Context

### なぜやるか

`三校企画.md` は「世界は真っ暗。自分の行動が音の波となって世界を照らす。だがその波は敵にも
自分の居場所を教える」を中核に据えた暗闇ステルス FPS の企画書。現 HEAD (`080d5d5`) で
**企画の中核 5 機能が 1 行も存在しない**ことを実測で確認した。

| 企画の要求 | 現状 |
|---|---|
| §3-1 角を回り込む音響伝播 | **無い**。音のオクルージョン / ポータル / リバーブゾーンも皆無 |
| §3-1 音波による地形描画 | **無い**。ワールド空間の光の場そのものが無い |
| §3-5 世界空間に残る残光 | **無い**。蓄積は画面空間履歴 3 例 (TAA / froxel temporal / RT SVGF) のみで、視点を回すと壊れる形 |
| §6 聴覚 AI | **無い**。AI / NavMesh / パス探索 / ステアリング / BehaviorTree / 知覚 が全部ゼロ |
| §3-4 音響床材 | **無い**。`PhysMat` は摩擦・反発・密度・粘着まで |

一方で**下地はほぼ揃っている**。ゼロから作るのは「場」と「判断」だけ:

- `VolumeTexture` (`src\Engine\Renderer\VolumeTexture.h`) — Texture3D の所有者。froxel が実戦投入済み
- `shapes::Overlap` / `ComputeAabb` / `MakePoseFromMatrix` (`Physics\Shapes.h`) — ボクセル化の下請け
- `Shapes.cpp:2344` `RayTerrain()` — **決定論的セル DDA の唯一の前例**
- `ColliderComponent.physMaterial` → `physmat::Resolve` — 床材の解決経路が既に通っている
- `PhysMat` は**末尾 append が明文で許可**されている (`PhysMatLibrary.h:16-17`)
- `SolidContact.impulse` — ヘッダのコメントが**「衝撃音 (将来)」を消費者として名指ししている**
- `XpbdBackend` — ECS 外 sim 状態の「3 点セット契約」(池 + `SimSources` + `SimRefs`) の写し元
- `PhysicsDebugDraw.h` — 「World を読んで `DebugLineCmd` へ追記するだけの純出力関数 + グローバルフラグ」の型
- ABI v15 の `GetMouseDelta` / `SetCursorMode` (`080d5d5` で追加、**利用者 0 件**) — 一人称カメラの材料

### 決定論の分水嶺 (この計画で最初に押さえたこと)

既存オーディオ (`AudioListener`/`AudioSource`) は**全部 `kComponentNoHash`** で、`AudioSourceSystem` は
`world.Rng()` に触らない専用 `Pcg32` を持ち、drain は tick 末ハッシュの**後**。つまり
**オーディオは決定論レーンの完全な外**。AI が読む伝播結果をここに載せることはできない
(載せても replay_verify が一切検査しないので、壊れても誰も気づかない)。

→ **音の伝播は既存オーディオとは無関係な、新しい sim レーンのシステムとして作る。**

### ユーザー決定 (2026-09-01)

1. **エンジン組込み + ゲームは薄く** — 伝播 / 残光 / 床材 / 聴覚センサーは Engine 層。
   一人称カメラ・光の運用・投擲だけ `src\GameLogic\Scripts\` の C++ スクリプト。
2. **sim のボクセル場を描画にも使う** — シャドウマップは使わない。
3. **伝播グリッドを流れ場に転用** — NavMesh は作らない。
4. **1 マイルストーン (M65) × 7 サブ**。

### 出口の姿

- 暗闇の部屋で歩くと、**波が L 字の廊下を曲がって隣の部屋の壁を描く**。
- 敵は**その波が届いた経路と同じ道を通って**寄ってくる (聞こえた所と来る道が同一の配列から出る)。
- カーペットの上では波が数歩ぶんしか広がらず、金属板では部屋を突き抜ける。
- 一度照らした輪郭が**視点を回しても残っている** (世界空間に貯まっている)。
- **既存 17 枚の golden は M65d まで maxDiff=0、既存 6 replay ペアは全サブで無風。**
- **ABI 追加ゼロ** (v11 の `Get/SetComponentField` で足りる。M65g の冒頭確認で確定させる)。

---

## 全体設計 (7 つの判断と根拠)

### 判断 1 — 中核の着想: **1 本の波面が描画・AI・残光の 3 役を同時に果たす**

音イベント 1 個につき、ボクセルグリッド上の波面を **1 tick 1 リング**前進させる。
その 1 本の frontier が:

- **描画** = 薄いシェル (r±δ) そのもの → 3D テクスチャに書けば「壁を貫通せず角を回り込む波」
- **AI** = リスナーのセルに到達した tick に「どちらから・どれだけの大きさで」が確定する
- **残光** = 触れたセルの強度を残光ボリュームへ `max` で焼き、毎 tick 減衰させる

企画 §11-1 は「見た目と挙動を分離する」と書いているが、**分離すると企画の中核が壊れる**。
シャドウマップは直線遮蔽なので**角を回り込まない**し、絵と AI が別式なら
「見えている所と敵に届く所が一致する」という §3-1 の約束が保証されない。
同一の場から両方を導けば、その一致が**構造的に**成立する。

★実装規約: **frontier の 3 つの書き込みは同じ 1 ループの中でやる**。後から別ループに
分かれた瞬間に「聞こえる場所と光る場所がずれる」種類のバグが入る余地ができる。

### 判断 2 — 伝播は **整数チャンファ距離の bucket Dijkstra**。素の BFS ではない

26 近傍 + Borgefors の最適重み **(面 11 / 辺 16 / 角 19)** を `uint16` で持つ。
距離[m] = `dist * cellSize / 11`。

- 6 近傍 BFS の等距離面は**菱形 (L1)**、26 近傍の等コスト BFS は**立方体 (L∞)**。
  どちらも球から 40% 級に外れ「音の波」に見えない。`<11,16,19>` は**最適スケールを取ったとき**の
  最大相対誤差が 1.56% (M65a 実測)。★16/11 と sqrt(2) を直に比べる (2.85%) のは誤読 —
  チャンファ距離は「真の距離の s 倍」の近似で s は自由に選べる。詳細は M65a の申し送り 1。
- ★**キーが整数であることが決定論の核心**。物理の float は 1 ulp ずれても剛体が微動するだけだが、
  **伝播は順序比較が 1 ulp ずれると訪問順が入れ替わり、親リンクが変わり、AI が聞く方向が変わる**。
  規約は「**順序を決めるものは全部整数。float は整数から導く末端の 1 式だけ**」。
- リング前進 = `dist ∈ [ring*11, (ring+1)*11)` を 1 tick で pop。`ticksPerRing` で分周し、
  伝播速度 = `cellSize * 60 / ticksPerRing` (0.5m / 1 → 30 m/s)。
  **材質で速度は変えない** (変えると同一グリッドの再利用が崩れる)。金属の「遠くまで」は
  `maxRing` と `amplitude` で表す。
- 各セルに `parentDir` (uint8、26 方向) を持つ。**リスナーに届いたときの「どこから来たか」がタダで、
  しかも角を曲がった経路の方向として正しい** — 企画 §6-3 と §3-1 を同じ 1 バイトが満たす。

### 判断 3 — sim 状態は **波スロット表 (1KB 未満) だけ**。セル配列は全部「導出値」

本計画で最大の判断。

| データ | 分類 | ハッシュ | snapshot | 再構築の契機 |
|---|---|---|---|---|
| 波スロット表 (`kMaxWaves=16` × 約 56B) | **sim 状態** | ○ (内容ゲート) | ○ (v10 の新節) | — |
| 占有グリッド (uint8/セル) | 導出 | × | × | 静的コライダ署名 fold の変化 |
| 波ごとの距離 + 親方向サブグリッド | 導出 | × | × | `Invalidate()` (復元 / シーンロード) |
| 航法 (粗) グリッドと流れ場 | 導出 | × | × | **毎 tick 全再計算** (判断 6) |
| 残光ボリューム + 3D テクスチャ | **描画レーン** | × | × | 呼び手の `ResetVisual()` |
| リスナーの受聴結果 / FSM 状態 | **sim 状態** | ○ (ECS) | ○ (ECS) | — |

**波の全状態が (原点セル, 現リング, 振幅, 材質) に閉じる**のは、
**エネルギーを整数距離の純関数にする**から。伝播中に材質で減衰させると経路依存になり、
セル配列そのものが sim 状態に落ちて snapshot が数 MB になる。
→ **床材は「発音時の振幅と到達上限」にだけ効かせ、伝播中の吸収は v1 では実装しない**
(企画 §3-4 の表はこの範囲で完全に表現できる)。

復元時は `Invalidate()` → 次の `Update` で ring 0 から現リングまで**引き直す**。
★**「増分で育てた場」と「引き直した場」がビット同一であること**が本計画で最も重要な不変条件で、
これ 1 本に selftest を当てる (M65b)。ここが割れると
「リプレイは通るのに巻き戻しでだけ割れる」= 最悪の型のバグになる。

### 判断 4 — グリッドは **コンポーネントが定義する固定 AABB**。次元は整数、範囲は導出値

`AcousticVolumeComponent` に `dimX/dimY/dimZ` (Int32) と `cellSize` (Float) を持ち、
`halfExtents` は `dim*cellSize/2` の**導出値**。原点はそのエンティティの `WorldMatrix` の
平行移動 (**回転は無視 = 常に軸平行**)。

- **プレイヤー追従を却下する理由は決定論より「残光の再アドレス」**。追従グリッドはスライドの
  たびに残光を平行移動かリセットしなければならず、「一度照らした輪郭が薄れていく」という
  §3-5 の中核が原点更新のたびに壊れる。froxel がカメラ相対でよいのは**履歴が毎フレーム
  再投影される前提**だから。残光は再投影できない。
- 「次元を整数で持ち extent を導出する」のが要点。逆 (extent から次元を割る) にすると
  **float→int の丸めがグリッド形状そのもの**になり、丸めが 1 変われば波の到達セルが全部ずれる。
- 想定 64×16×64 m / `cellSize=0.5` → `dim = 128×32×128 = 524,288` セル / 占有 512 KB。
- ★**存在ゲートの実体はこの 1 コンポーネント**。シーンに 1 個も無ければ `AcousticField::Update` は
  先頭で return し、占有配列を `resize` すらしない。既存 17 golden / 6 replay ペアが
  1 ビットも動かない根拠がここに集約される。

### 判断 5 — 描画は **3D テクスチャ 1 枚 + Deferred の空席 t13 / Forward の t8**

`deferred_light.hlsl:92-94` を実測確認: **t13 は SSR の予約席だったが SSR (M56d) が
光パスの出力を読む別パスになったので空いたまま**。ここを取ると
**`gbSrvs[16]` / `nullSrvs[16]` の本数が 1 つも変わらない** = M57d/e が 3 回踏んだ
「SRV 剥がし忘れ」の罠を構造的に回避できる。Forward 側の空きは **t8**
(`forward_lit.hlsl:77-84` で t7 まで使用済み)。

- `R8_UNORM` の Texture3D (SRV のみ / UAV なし)。128×32×128 で **512 KB**。
  → `VolumeTexture::Create` に **`bool withUav = true`** を追加する
  (現状 `BIND_SRV | BIND_UAV` 固定で、R8_UNORM は FL11_0 の typed UAV 必須リスト外なので
  **UAV 生成が落ちて Create が false を返す**)。引数作法は `RenderTexture::Create` に揃える。
- サンプル位置は **`P + N * (0.75 * cellSize)`**。占有セルは BFS が絶対に訪れないので、
  壁面は「開セルと閉セルの境界」にある。法線方向へ押し出して開セル側を引くのが
  「壁に当たった面だけが光る」の正体。
- 既定でビット同一の**二重ゲート**: ① `view.acousticSRV == nullptr` なら CB の `enabled = 0`
  ② シェーダは `enabled != 0` の分岐の**内側**でしかサンプルにも定数にも触れない
  (`* 1.0` も `+ 0` も通らない = M63a/M63c の作法)。

### 判断 6 — 流れ場は **整数比で粗くした別グリッドを毎 tick 全再計算**し、**キャッシュを持たない**

音響セル 0.5 m に対し `navCellRatio = 2` → 1 m セル。64×16×64 m で 65,536 セル。
目標 4 個ぶんの Dijkstra を毎 tick 回しても 26 万 pop で 1 ms 未満。

★**キャッシュを持たないことが判断の本体**。LRU / 遅延構築 / 予算分割を入れると
「場が間に合ったか」が敵の `moveInput` (= ハッシュ対象) を変えるので、
**導出値のつもりのキャッシュが隠れた sim 状態に化ける**。これは巻き戻しでだけ割れる。
毎 tick 全再計算なら場は (占有, 目標セル集合) の純関数で、履歴が存在しない。
遅いときは**解像度を落とす方向にしか逃げない** (`navCellRatio` 2→4)。

### 判断 7 — 敵 FSM は **組込みコンポーネント + Engine のシステム**。`.json` 資産にはしない

`AnimatorController` が `.json` なのは「クリップが content で本数が読めない」から。
本件は**状態 5 個・遷移固定**で、変わるのは (a) どのセンサーが発火するか (b) 閾値と時定数だけ。
資産化すると `ControllerLibrary` 相当のローダ / 版 / エディタ窓 /「資産値はハッシュ外」問題が
全部ついてきて、得るものが無い。3 種目の敵が要求されたら再検討する、と申し送る。

- `AgentBrainComponent` — 共通 FSM。**性格づけ = このコンポーネントのフィールド値**。
- `AcousticListenerComponent` — 聴覚センサー。エンジンが毎 tick 書く鏡。
  ★**`PlayerInputComponent` と全く同じ型**で、`kFieldNoSerialize` を付けないことが
  ハッシュ被覆の条件 (CLAUDE.md:88-89 の明文の罠)。
- `LightSeekerComponent` — 光センサー。**光源は既存の `LightComponent`** (既にハッシュ対象) を
  そのまま使う。新しい「光」概念を作らない。

---

## サブ計画

### M65a — 共有契約: グリッド / ボクセル化 / コンポーネント 5 本 / snapshot v10
★**ここだけが TypeId / hash / snapshot の版を動かす**

**狙い**: 「グリッドが在るシーンでだけ占有が焼ける」ところまで。波は 1 本も出さない。

**冒頭確認 (着手前に潰す)**
1. ★**TypeId 45/46 の取り合いを解決する**。`plans\supple-weaving-loom.md:36,287` が
   **45=Cloth / 46=SoftBody を予約**しているが、M60′h/k は**未実装で登録もされていない**
   (`Components.cpp` の末尾は Rope=44)。登録順 = TypeId なので飛ばし登録は不可能。
   → **M65 が 45〜49 を取り、`supple-weaving-loom.md` の予約を 50=Cloth / 51=SoftBody へ
   書き換える**。まだ登録されていないのでデータは 1 バイトも壊れない。**この書き換えを
   M65a のコミットに含める**こと (別コミットにすると M60′ 再開時に食い違う)。
2. `git log --oneline -5` が `080d5d5` のままか (M60′ が動いていたら 1 に戻る)。
3. `assets\physmats\*.physmat.json` 5 種が無傷か (M65c の前提)。

**触るもの**

| ファイル | 作業 |
|---|---|
| `src\Engine\Engine\Acoustic\AcousticGrid.h/.cpp` **(新規)** | 純関数のみ。`AcousticGridDesc{int dimX,dimY,dimZ; float cellSize,minX,minY,minZ;}` / `WorldToCell` / `CellToWorldCenter` / `CellIndex` / `InBounds` / `kFaceCost=11 kEdgeCost=16 kCornerCost=19` / 26 近傍オフセット表 (**固定順 = 決定論のタイブレーク**) |
| `src\Engine\Engine\Acoustic\AcousticField.h/.cpp` **(新規)** | 場の所有者。M65a では `Sync(World&)` (ボリューム探索 + 占有ベイク) と空の波表のみ。`Reset()` / `Invalidate()` / `Waves()` — **`XpbdBackend.h` の写し** |
| `src\Engine\Engine\Acoustic\AcousticSelfTest.h/.cpp` **(新規)** | `RunAcousticSelfTest()` |
| `src\Engine\Core\Components.h` / `.cpp` | 5 型を POD で追加 → `RegisterBuiltinComponents()` の**末尾に append** (TypeId 45〜49)。`MYE_JP` + `MYE_FIELD_RANGE` / `_TIP` |
| `Replay\WorldHasher.h/.cpp` | `SimSources` に `const AcousticField* acoustic` を**末尾 append** / `HashAcousticWaves()` (`HashXpbdPools` の写し) を**内容ゲート付き**で畳む (active な波が 0 本なら節ごと畳まない) |
| `Replay\SimSnapshot.h/.cpp` | `SimRefs` に `AcousticField* acoustic` 末尾 append / **`kSimSnapshotVersion` 9→10** + 履歴コメント / `kAcousticMagic='ACU1'` の節を **XPB 節の直後**に / ★**読み側は復元後に `Invalidate()` を呼ぶ** |
| `Engine\TickRunner.h/.cpp` | `TickServices` に `AcousticField* acoustic` / `:334` の直前に**新フェーズ 3.4** を作り `if (stepSim && ts.acoustic) ts.acoustic->Update(...)` / **ハッシュ呼び出し 7 箇所** (`:422,441,449,456,479,498` 付近) の `SimSources` 初期化子 / シーン遷移ブロック (`:597-613`) に `Reset()` |
| `Engine\EngineLoop.cpp` | `:118` 付近に `AcousticField acoustic;` の実体 / `:455` の `SimRefs` / `:707` 付近の `tickServices.acoustic = &acoustic;` |
| `src\Editor\EditorMain.cpp` | `:593` の `&&` 連鎖**末尾**に `&& mye::RunAcousticSelfTest()` (実測 42 → **43 スイート**。CLAUDE.md の「40」は古いので同時に直す) |
| `engine_spec.md` / `CLAUDE.md` | 新章の骨子 + 11.3 のハッシュ対象表に波表 / selftest 本数 |
| `pwsh -File tools\gen_project_files.ps1` | 新規 6 ファイル |

**追加コンポーネント (TypeId 45〜49)** — M65f までしか使わないフィールドも**ここで全部確保する**

| 型 | 主フィールド | 消費 |
|---|---|---|
| `AcousticVolumeComponent` (45) | `dimX/dimY/dimZ` Int32=64/16/64 / `cellSize` Float=0.5 / `navCellRatio` Int32=2 / `blockLayerMask` UInt32 / `enabled` Bool | a |
| `AcousticEmitterComponent` (46) | `pendingLoudness` / `pendingRadiusM` / `pendingTone` / `ticksPerRing` / `autoFootstep` / `stepDistanceM` / `travelAccum` / `cooldownTicks` / `cooldown` | b, c |
| `AcousticListenerComponent` (47) | `threshold` / **鏡**: `lastHeardTick` UInt64 / `lastHeardX/Y/Z` / `lastLoudness` / `lastSourceEntity` EntityRef / `lastTone` | b(書), f(読) |
| `LightSeekerComponent` (48) | `attractRadius` / `minIntensity` / **鏡**: `nearestLight` EntityRef / `nearestX/Y/Z` / `nearestStrength` | f |
| `AgentBrainComponent` (49) | `state` / `stateTicks` / `homeX/Y/Z` / `targetX/Y/Z` / `alertTicks` / `searchTicks` / `loseTicks` / `memoryTicks` / `walkSpeed` / `runSpeed` / `emitEveryTicks` / `emitLoudness` / `emitPhase` | f |

**既定でビット同一の根拠**
- 既存シーンに `AcousticVolumeComponent` が 0 個 → `Sync` が最初の `ForEachArchetype` で 0 件を見て return。
- 波表が空 → 内容ゲートで**節ごと畳まない** = `HashWorld` の値が現行と厳密一致
  (`XpbdBackend` が M60′b で通した論法そのもの)。
- snapshot は空の節が 1 個増えるのでレイアウトが変わる → 版 9→10。
  ★`golden_*.rep` は `cache\` (gitignore) に**毎回録り直される**ので**コミット対象の再生成はゼロ**。
- 描画は 1 行も触らない → golden 17 枚 maxDiff=0。

**検証**: selftest (43 本目) に — `WorldToCell`↔`CellToWorldCenter` の往復 / 境界セルの包含 /
チャンファ重みが `sqrt` 比に対し誤差 2% 未満 / 既知の箱 1 個のボクセル化がセル数一致 /
静的署名 fold が「同 world で 2 回呼んで同値・1 コライダを動かすと変化」 /
★**`SimSources{...,&emptyField}` の `HashWorld` が `{...,nullptr}` と等値** ―
replay_verify 6 ペア / shot_verify 17 枚 maxDiff=0 / check_rules 緑。

**リスク**
- **冒頭確認 1 (TypeId) を飛ばすと M60′ 再開時に不可逆**。
- 占有ベイクは `ForEachArchetype({Collider, WorldMatrix})` + `shapes::ComputeAabb` で
  **コライダの AABB 内セルだけ** `shapes::Overlap` に掛ける。全セル×全コライダは 52 万×数百で即死。
- `SimRefs` / `SimSources` の**片方だけ配線するのが 3 点セット契約違反**。
  `--snapshot-stress` は同一 tick 往復しか見ないので、片落ちはタイムトラベルでしか出ない。

---

### M65b — 波面伝播 (整数 Dijkstra) + デバッグ線 + `--acoustic-demo` + replay 7 ペア目

**狙い**: 「1 tick 1 リング進む波が壁を貫通せず角を回り込む」を**線で見せ、ハッシュ列で証明する**。

**冒頭確認**: M65a の selftest 43 本と replay 6 ペアが緑。`kMaxWaves=16` の根拠 (波ごとのローカル箱の
メモリ合計) をコメントに書けるか。

**触るもの**

| ファイル | 作業 |
|---|---|
| `AcousticField.h/.cpp` | `struct Wave{uint32 active; EntityID source; int32 ox,oy,oz; uint32 ring,maxRing,ticksPerRing,phase; float amplitude; uint32 tone; uint64 bornTick;}` / `Emit()` (**最小 index の空きスロット** = 決定論) / `Advance()` (bucket Dijkstra 1 リング) / **波ごとのローカルサブグリッド** (`uint16 dist` + `uint8 parentDir`、`maxRing` から寸法を決めて誕生時に確保・**プールして使い回す**) / `Rebuild()` |
| `AcousticGrid.h` | `EnergyAt(uint32 chamferDist, float amplitude, float cellSize)` — **整数距離の純関数**。`Audio\SpatialMath.h::RolloffGain` を流用できるか確認し、できるなら共有する (音と光が同じ減衰式になる) |
| `Acoustic\AcousticDebugDraw.h/.cpp` **(新規)** | `AcousticDebugFlags{bool frontier, occupancy, waveOrigin, listener;}` / `GetAcousticDebugFlags()` / `BuildAcousticDebugLines(...)` — **`Physics\PhysicsDebugDraw.h` の型を丸写し** |
| `TickRunner.cpp` | フェーズ 3.4 に `Emit` (エミッタ走査) → `Advance`。フェーズ 4 の `BuildPhysicsDebugLines` の**直後**に `if (!ts.resim && GetAcousticDebugFlags().Any()) BuildAcousticDebugLines(...)` |
| `DemoContent.h/.cpp` | `RegisterAcousticShowcaseContent` / `BuildAcousticShowcaseScene`。材質接頭辞 **`adem_`** (既使用 `rdemo_ tdemo_ pdemo_ jdemo_ fdemo_ vdemo_` と重複しないこと)。**L 字の廊下 + 2 部屋**を置く |
| `EditorMain.cpp` / `EditorApp.h/.cpp` / `RuntimeMain.cpp` / `CLAUDE.md` | `--acoustic-demo` (デモ CLI の 7 ファイルの舞踏) |
| `tools\replay_verify.bat` | **6 箇所**: `:81` 件数 echo / `:84` ジョブ名に `acoustic` / `:87` PASS 文言 / `:130-217` に `:job_acoustic` (`call :chain cache\golden_acoustic.rep "--acoustic-demo" "--acoustic-demo"`) / `:95-118` の `:failed` diagnose ブロック / `:279` コメント |
| `EditorApp.cpp` + `LocalizationTable.inl` | View メニューに音響デバッグ 4 チェック (`MYE_STR` を en/ja 両方) |

**既定でビット同一の根拠**: `Emit` の入口が `AcousticEmitterComponent` の走査で、既存シーンには 0 個。
波が 0 本なら `Advance` はループに入らず、ハッシュ節は畳まれない。デバッグ線は既定 off + `!ts.resim`。

**検証 (この計画の心臓)**
- ★**selftest — 引き直しの同値性**: 手組みの迷路占有で、(a) 増分で ring 0→20 まで育てた
  `dist[]`/`parentDir[]` と (b) `Invalidate()` 後に `Rebuild()` した結果が **`memcmp` で同一**。
- selftest — 壁の向こうが到達不能 / L 字の角の先が**直線距離より大きいチャンファ距離**で到達 /
  `parentDir` を辿ると原点に戻る / 同じ波を 2 回同じ ring まで進めたら同値 /
  `kMaxWaves` 満杯時の `Emit` が**最古を潰さず false を返す** (満杯時の挙動が到着順に依存しない)。
- `replay_verify` **7 ペア目**が Debug/Release/snapshot 往復で緑。
- 目視: `--acoustic-demo` + 音響デバッグ線で波が L 字を曲がる。

**リスク**
- **性能**。r=40 セルのシェルは約 2 万セル、26 近傍で 52 万テスト/tick/波。16 波同時で 800 万 = 破綻。
  ★**縮退の順序を先に決めてコメントに書く**: ① `ticksPerRing` を上げる ② `cellSize` 0.5→0.75
  (セル数は 1/s³) ③ `kMaxWaves` を下げる ④ 26→6 近傍 (**波が菱形になるので最後の手段**)。
  `MYE_PROFILE_SCOPE("acoustic")` を必ず入れる。

---

### M65c — 床材と発音源 (PhysMat 末尾 append / 足音 / 衝撃 / 投擲)

**狙い**: 企画 §3-2 / §3-4 (行動と床材が波の大きさを決める) を数値で成立させる。

**冒頭確認**: `PhysMatLibrary.h:16-17` の「末尾 append 可 / 値はハッシュ非対象」が現 HEAD でもそのままか。
`DemoContent.cpp:855` の `FindPhysMat(const char*)` が使える形か (**絶対パスからハッシュを組んではいけない**)。

**触るもの**

| ファイル | 作業 |
|---|---|
| `Physics\PhysMatLibrary.h/.cpp` | `PhysMat` **末尾**に `acousticLoudness` (0=無音) / `acousticRadiusM` / `acousticTone` (Int32 0..3)。`FromJson` は `contains` + 既定値の前方互換読み |
| `assets\physmats\*.physmat.json` **(新規 6 種)** | `carpet` `wood` `gravel` `water` `metal` `glass` (企画 §3-4 の表そのまま)。**既存 5 種は触らない** |
| `Physics\PhysicsSystem.h` | `SelectAcousticLoudness/RadiusM/Tone` を `SelectFriction` (`:84-96`) と同じ形の純関数で追加。override ビットは `Components.h:186-191` の末尾へ |
| `AcousticField.cpp` | **足音**: `CharacterControllerComponent` の水平移動距離を `travelAccum` に積み、`stepDistanceM` 超で真下へ `RaycastWorld` → ヒット先の `ColliderComponent::physMaterial` → `PhysMat` → `Emit`。★`maxRing = (int)(acousticRadiusM / cellSize)` の変換は**1 式に閉じ、切り捨てを明記** |
| `AcousticField.cpp` | **衝撃音**: `std::vector<SolidContact>` を受け、`impulse` が閾値超えのペアだけ `Emit`。`key` 昇順の走査 = 決定論。★`PhysicsSystem.h:21` のコメント「衝撃音 (将来)」を「M65c で消費」に更新 |
| `TickRunner.cpp` | フェーズ 3.4 に `solidContacts` を渡す。★**物理 (3.6) より前なので参照するのは前 tick の接触**。1 tick 遅れであることをコメントに明記 |
| `DemoContent.cpp` | ショーケースに 6 材質の床タイルを敷き、キャラを歩かせる |

**既定でビット同一の根拠**: 既存 5 プリセットは JSON にキーが無い → `acousticLoudness = 0` → `Emit` しない。
PhysMat の値はハッシュ非対象 (M59 決定台帳 5) なので既存物理も無風。足音・衝撃の入口は
両方 `AcousticVolumeComponent` の存在ゲートの内側。

**検証**: selftest — 材質→振幅→`maxRing` が整数の純関数 / 6 材質の相対順
(carpet < wood < gravel < water < metal ≒ glass) / `physMaterial` 未設定の既定が**無音** ―
`RunPhysMatSelfTest` に前方互換読みの check を追加 / replay 7 ペア。

**リスク**: 地形 (`TerrainCollisionData.splat`) からの材質解決は**このサブでは実装しない**。
`terraincol::Resolve` + `kMaxLayers=4` の最大重みレイヤ、という延長線だけコメントに残す。
★**描画側 `TerrainSystem` のキャッシュを読むのは禁止** (`TerrainColliderLibrary.h:11-18`)。

---

### M65d — 残光ボリューム + 3D テクスチャ転送 (まだライティングには入れない)

**狙い**: sim の場を GPU に上げ、**読み戻して数値で正しさを主張する**。
M57c が「積分結果を読む者が居ない段階で golden を撮らない」とした流儀に揃える。

**冒頭確認**: ★**`R8_UNORM` の Texture3D SRV が WARP で通るかは机上で決まらない**。
froxel が M57a で `--froxel-probe` を先に書いたのと同じく、`--acoustic-probe` を先に書いて
30 分で実測してから設計を確定する判断がありうる。

**触るもの**

| ファイル | 作業 |
|---|---|
| `Renderer\VolumeTexture.h/.cpp` | `Create/Resize` に **`bool withUav = true`** を追加 (`RenderTexture::Create` と同じ引数作法)。false で `BindFlags` から UAV を落とす。既存 froxel の呼びは既定引数で無風。**SRV 作成失敗もログに残す** |
| `AcousticField.h/.cpp` | `std::vector<uint8_t> glow_` + `DecayVisual(float perTick)` + `WriteShell(...)` (**リング前進と同じループ**で `glow = max(glow, energy)`) + `ResetVisual()` + `visualSerial_`。★**ハッシュ・snapshot 対象外であることをヘッダに明記**し、`Waves()` と別アクセサにする |
| `Renderer\AcousticVolumePass.h/.cpp` **(新規)** | `VolumeTexture` の所有 + `Upload(GraphicsDevice&, const AcousticField&)` (`UpdateSubresource` **全域転送で始める**) + `SRV()` + `LastUploadMs()` |
| `Engine\RenderSystem.h/.cpp` | `const AcousticField* acousticField = nullptr;` — ★**`reflectionProbes` (`RenderSystem.h:186`) の写し**: 非所有 / 所有者 (EngineLoop・EditorApp) が毎フレーム埋める / **`AssetPreviewCache` は埋めない** = サムネイルに音の光が漏れないことが構造的に保証される |
| `Renderer\RenderTypes.h` | `RenderView` **末尾 append**: `acousticSRV` / `acousticGridMin[3]` / `acousticInvSize[3]` / `acousticIntensity` / `acousticNormalPush` |
| `EngineLoop.cpp` / `EditorApp.cpp` | `renderSystem.acousticField = &acoustic;` / 破棄前に `nullptr` へ戻す (`EditorApp.cpp:819` と同型) |
| `TickRunner.cpp` | フェーズ 3.4 の末尾に `DecayVisual` (**sim 相の中で描画レーンのデータを触るが sim は絶対に読まない**旨をコメント) |
| `Windows\ProfilerWindow.cpp` | 転送 ms / セル数 / アクティブ波数 |
| `EditorMain.cpp` / `RuntimeMain.cpp` | `--acoustic-dump N` (`ReadbackAll` で統計をログ。`--froxel-dump` と同じ調査専用の型) |

**既定でビット同一の根拠**: `acousticField == nullptr` または波が空 → `Upload` を呼ばず
`view.acousticSRV` は null のまま。**この時点でシェーダは 1 本も変わっていない**ので golden 17 枚は完全無風。

**検証**: selftest — `glow_` を書き換えても `HashWorld` が動かない / 減衰が単調で 0 に収束 /
`WriteShell` が閉セルに絶対書かない / ★**`ReadbackTexel` の 1 点照合**
(`SysMemPitch`/`SysMemSlicePitch` を取り違えると **Z がずれた絵**になり、しかも絵は出る) ―
`--acoustic-demo --acoustic-dump 120` で「非ゼロセルが波の到達領域と一致 / 壁の向こうがゼロ」を
数値で主張 / `--warp` で転送 ms を実測して本ファイルへ書き戻す。

**リスク**: R8_UNORM が WARP で期待どおりサンプルされない可能性。縮退は
`R16_FLOAT` → `R8G8B8A8_UNORM` の順。部分更新 (`D3D11_BOX`) は「残光が全域で毎 tick 減衰する」以上
非ゼロ領域全部が dirty なので、**汚れ AABB を追跡しない限り効かない** — 必要になったら同サブ内で切る。

---

### M65e — ライティング差し込み (Deferred t13 / Forward t8) + golden 18/19 枚目
★**ここで初めてピクセルが動く**

**冒頭確認**: `deferred_light.hlsl:92-94` の「t13 は空席のまま。詰めないこと (統合契約 予約 2)」に対し、
**M65 が t13 を占める**ことを契約の更新としてコメントに書く。
`PSSetShaderResources(1, 7, ...)` の **4 箇所** (`ForwardPath.cpp:307,381` / `DeferredPath.cpp:1293,1463`) を洗い出す。

**触るもの**

| ファイル | 作業 |
|---|---|
| `assets\shaders\acoustic_common.hlsli` **(新規)** | **register 宣言ゼロ** (`common.hlsli` の契約)。`AcousticSample(Texture3D, SamplerState, float3 posW, float3 N, float3 gridMin, float3 invSize, float push)` と `AcousticTint(float e, int tone)` の**正本 1 本** |
| `deferred_light.hlsl` | `Texture3D gAcoustic : register(t13);` + 空席コメントを更新。CB **末尾**に `gAcousticGridMin` / `gAcousticInvSize` / `gAcousticParams` (x=intensity y=normalPush z=gamma **w=enabled**)。合成は `if (gAcousticParams.w != 0.0)` の**1 分岐だけ** |
| `forward_lit.hlsl` / `_instanced` / `forward_skinned.hlsl` / `forward_terrain.hlsl` | `register(t8)` + `PerFrame` CB 末尾に同 3 本 + 同じ 1 分岐。**床がタダで乗る** |
| `DeferredPath.cpp` | `:1218` の `gbSrvs[13]` を `nullptr` → `view.acousticSRV` (★**本数は 16 のまま**) / `:1293` の `fwdSrvs[7]`→`[8]`・`(1,7)`→`(1,8)` / `:1463` の null も 8 |
| `ForwardPath.cpp` | `:306` の `static_assert` の隣に `kAcousticForwardSrvSlot == 8` / `frameSrvs[7]`→`[8]`・`(1,7)`→`(1,8)` / **`:381` の null も 8** |
| `Renderer\RenderTypes.h` | `namespace acoustic { constexpr int kSrvSlot = 13; constexpr int kForwardSrvSlot = 8; }` + `struct AcousticCB` + `MakeAcousticCB(const RenderView&)` (**CPU 側 1 本を両パスが共有** = `MakeFroxelForwardCB` の型) + `static_assert(sizeof(...))` |
| `tools\check_rules.ps1` | `$constGroups` に **2 エントリ** (`kSrvSlot`/`MYE_ACOUSTIC_SRV_SLOT`、`kForwardSrvSlot`/`MYE_ACOUSTIC_FWD_SRV_SLOT`)。HLSL 側に `#define` を置いて機械照合できる形にする |
| `RenderSystem.cpp` | `:1091` 付近の froxel ブロックの隣で `acousticPass_.Upload` → `view` を充填。ボリュームが無ければ全部 0/null |
| `DemoContent.cpp` | ★ショーケースに**弱い環境光 + 設置光 1 個**を必ず置く (下のリスク) |
| `tools\shot_verify.bat` | `:108-110` 枚数コメント / 末尾に `call :shot acoustic_forward --acoustic-demo` と `acoustic_deferred --acoustic-demo --deferred` (**frame 120 の型**、`TOLNOW` は既定 3) / `MYE_SHOT_SKIP_ACOUSTIC` の囲い / `:308-312` PASS 文言 |
| `.github\workflows\ci.yml` | `:5-36` と `:64-68` の**両方**に `MYE_SHOT_SKIP_ACOUSTIC` を登録 |
| `CLAUDE.md` | golden 枚数と CI 判定枚数 |

**既定でビット同一の根拠 (二重ゲート)**: ① `acousticSRV == nullptr` → `gAcousticParams.w = 0`
② シェーダは `w != 0` の分岐の**内側**でしか定数にもサンプルにも触れない。

**検証**: shot_verify の**既存 17 枚が maxDiff=0** + 新規 2 枚 / replay 7 ペア (描画は sim に触らない) /
selftest で `AcousticSample` の CPU ミラーが「閉セル境界で法線押し出しにより開セル値を拾う」ことを固定。

**リスク**
- ★★**「暗いゲームだから真っ黒な golden」が最大の罠**。全画素が黒だと、機能が壊れて何も出なくても
  golden 一致で通る = **回帰検出がゼロ**になる。デモには必ず (a) 弱い環境光で見える壁と
  (b) 設置済みの光 1 個 を置き、音の帯が明確に加算されている画にする。
  **意図的にシェーダの合成を殺して赤くなることを 1 度確かめる**こと。
- SRV は張り忘れではなく**剥がし忘れ**が M57d/e で 3 回起きている。Deferred は t13 が既存の
  `nullSrvs[16]` に含まれるので無風だが、**Forward の null 側を 8 にし忘れると次フレームで生き残る**。
- `deferred_terrain.hlsl` は GBuffer パスなので**触らない** (触ると二重加算)。

---

### M65f — 流れ場 + 共通 FSM + センサー 2 種

**狙い**: 「AI が聞いた場所」と「光った場所」が同じ配列から出ることを、動く敵で示す。

**冒頭確認**: M65e までの golden 19 枚と replay 7 ペアが緑。
`CharacterControllerComponent.moveInput` が「水平 m/s・保持」であることを `WalkerDemo.cpp` (唯一の実例) で再確認。

**触るもの**

| ファイル | 作業 |
|---|---|
| `Acoustic\AcousticNav.h/.cpp` **(新規)** | 粗グリッド (`navCellRatio` で間引き、**サブセルの過半が開なら開**) / `BuildFlowField(target, out dist[])` = 同じチャンファ Dijkstra / `SampleGradient(pos)` → 単位方向。★**キャッシュを持たない** (判断 6) |
| `Acoustic\AgentSystem.h/.cpp` **(新規)** | センサー充填 → FSM 遷移 → `moveInput` 書込 → 自分の音の `Emit`。`ForEachArchetype` + entity.index 昇順 |
| `AcousticField.cpp` | リング前進のループ内で、`AcousticListenerComponent` を持つエンティティのセルに今リングで到達したら鏡を書く。方向は `parentDir` から復元 |
| `AgentSystem.cpp` | 光センサー: `LightComponent` を index 昇順に走査し `attractRadius` 内で最強を選ぶ。**タイブレークは entity.index** (規則 7) |
| `TickRunner.h/.cpp` / `EngineLoop.cpp` | `TickServices` に `AgentSystem*` / フェーズ 3.4 の `Advance` の**直後**に `agentSystem.Update(...)`。物理 (3.6) より前なので `moveInput` は**同じ tick で効く** |
| `DemoContent.cpp` | 音の敵 1 体 + 光の敵 1 体 + 設置光を追加 |
| `engine_spec.md` | FSM の状態表と遷移条件 |

**既定でビット同一の根拠**: `AgentBrainComponent` が 0 個 → `Update` が最初の `ForEachArchetype` で return。
流れ場も目標 0 件で構築しない。★`world.Rng()` の draw も**エージェントが居るときだけ**引く
(探索状態の不規則な波)。

**検証**: selftest — 迷路で勾配降下が目標に到達 / 壁を通り抜けない / **目標が同じなら 2 回計算してビット同一** /
FSM 5 状態 × 入力の遷移表を全網羅 / `emitEveryTicks` の分周で**「警戒中は 1 波も出ない」** (企画 §6-3) ―
replay 7 ペア (**敵が動くのでハッシュ列が濃くなる = AI の決定論の唯一の証明**) /
golden 2 枚を `--update` して差分を目視。

**リスク**
- 流れ場が 1 ms を超えたら `navCellRatio` 2→4。**キャッシュを入れない**のが判断 6 の本体。
- `moveInput` を毎 tick 上書きするので、同じエンティティにスクリプトが付いていると喧嘩する。
  `AgentSystem` はフェーズ 3.4 = スクリプト (3) の**後**なので AI が勝つ。これを仕様として明記。
- ★**敵の自発音が自分のリスナーを叩く**。`Wave::source == self` の波は無視する。
  忘れると**全敵が永久に追跡状態**になる。

---

### M65g — ゲームの薄皮 (一人称カメラ / 光の設置・回収 / 投擲)

**狙い**: 企画のゲームループ 7 段を最小構成で通す。**エンジンには 1 行も足さない**。

**冒頭確認**
1. ★C++ スクリプトから `SetComponentField` で組込みコンポーネント
   (`AcousticEmitterComponent::pendingLoudness` 等) を**書けることを使い捨ての probe で実測**する。
   新コンポーネントは NoHash ではないので `EngineApiTable.cpp` の NoHash ゲートには当たらない見込み。
   **書けるなら ABI v15 据え置きが確定**。書けなければここで初めて v16 = 105 スロットの判断に入る
   (`EngineAPI.h` + `Interop.cs` の位置ミラー + `$apiVersionSlots` + `PartSelfTest.cpp` を**同時に**)。
2. `GetMouseDelta` / `SetCursorMode` (`EngineAPI.h:492,506`) の利用者が 0 件のまま = **初実装**。

**触るもの**

| ファイル | 作業 |
|---|---|
| `Scripts\WatcherFpsCamera.cpp` **(新規)** | `GetMouseDelta` → yaw/pitch を**スクリプト状態フィールドに保持** (ホットリロードで生き残る)。`CharacterMove` へ yaw 回転した入力。`SetCursorMode(1)` は 1 回だけ |
| `Scripts\WatcherLightTool.cpp` **(新規)** | 所持数 / 設置 2〜3 秒・回収 5 秒 (整数 tick) / 設置中は光の `intensity` を線形に上げる (**ゲージを出さない** = 企画 §4-3) / Prefab 生成で `LightComponent` 付きを置く |
| `Scripts\WatcherThrowTool.cpp` **(新規)** | 石/瓶を Rigidbody + `AcousticEmitterComponent` 付き Prefab で射出。着弾は M65c の衝撃音が拾う |
| `assets\prefabs\*.prefab.json` **(新規)** | 設置光 / 石 / 瓶 |
| `DemoContent.cpp` | ショーケースにプレイヤー (CharacterController + 上記 3 スクリプト) |

★**スクリプト名の罠**: スクリプトの ComponentTypeId は **DLL 内の名前 `strcmp` 昇順**
(`ScriptHost.cpp:165-177`)。既存の最後が `WalkerDemo` なので **`Wat...` 以降**にすれば
既存スクリプトの相対順が動かない。`AcousticXxx` と名付けると `AudioDemo` より前に入る。

**既定でビット同一の根拠**: 新スクリプトは `--acoustic-demo` のシーンにしか付かない。
既存 6 replay ペアと golden 17 枚は無風。

**検証**: replay 7 ペアが `--synth-input` で緑 / 手動でゲームループ 7 段が通ること。

**リスク**: `SetCursorMode` は**出力レーン**。`resim` はスクリプト層をゲートしないので、
カーソルロックは「1 回きり + エンジンが Escape で強制解除する既存の逃げ道」に依存する設計にする。
`GetMouseDelta` は `InputSnapshot` 由来の決定論レーンなので、yaw/pitch をスクリプト状態
(= ハッシュ対象) に持つのは正しい (むしろ replay 被覆になる)。

---

## 全サブ共通の検証チェックリスト

1. `pwsh -File tools\gen_project_files.ps1` (新規 .h/.cpp を足したサブのみ)
2. Debug / Release 両構成 0 警告 (`MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true`)
3. `bin\x64\Debug\Editor.exe --selftest` — **43 スイート**全緑 (現行は実測 42。CLAUDE.md の「40」は古い)
4. `tools\replay_verify.bat` — M65b 以降は **7 ペア** / 10 並列ジョブ
5. `tools\shot_verify.bat` — **既存 17 枚 maxDiff=0** (M65e 以降は 19 枚)
6. `pwsh -File tools\check_rules.ps1` — 0 error
7. `tools\build_managed.bat Debug` / `Release` (C# は sln の外。M65 は C# を触らないが構成は保つ)

**このマイルストーン固有の追加チェック**
- ECS 外 sim 状態を触ったら **3 点セット** (`AcousticField` の member / `WorldHasher` の節 /
  `SimSnapshot` の ACU 節) が揃っているか。**片落ちは `--snapshot-stress` では出ない**。
- 新しい配列を足したら「これは sim 状態か導出値か」をヘッダに 1 行で書く。
  導出値なら `Invalidate()` の呼び手を全部列挙する。
- 順序を決める比較に float が混入していないか (`grep`)。
- SRV を張ったら**同じ関数内で剥がしているか**。

---

## 失敗の切り分け表

| 症状 | 最初に疑うもの | 確かめ方 |
|---|---|---|
| `replay_verify` が M65a で割れる | `SimSources` を足した 7 箇所の漏れ / 内容ゲートが効いていない | `--hash-diff` に `Acoustic` 行が出るか |
| `--snapshot-stress` は通るがタイムトラベルで割れる | 復元後の `Invalidate()` 忘れ = 引き直していない | `Rebuild()` を毎 tick 強制する実験ビルドで消えるか |
| 波の形が Debug と Release で違う | 順序決定に float が混入 | チャンファ距離が `uint16` のままか grep |
| 壁を音が貫通する | 占有ベイクのポーズ不一致 / `blockLayerMask` | `--acoustic-demo` + 占有デバッグ線 |
| **音が届く先と光る先が食い違う** | `WriteShell` とリスナー判定が別ループに分かれた | 同じ 1 ループ内で両方やる設計に戻す (**企画の中核**) |
| golden が真っ黒で常に緑 | デモに環境光と設置光が無い | 合成を意図的に殺して赤くなるか試す |
| Forward だけ影/霧が消えた | `fwdSrvs` の**本数**を 8 にし忘れ (4 箇所のどれか) | `DeferredPath.cpp:1288` のコメントが同型の警告 |
| 敵が全員永久追跡 | 自分の発した波を自分で聞いている | `Wave::source == self` の除外 |
| 巻き戻すと敵の動きが変わる | 流れ場にキャッシュを入れた | 判断 6 に戻す (毎 tick 全再計算) |
| WARP でフレームが落ちる | 転送ではなくライトパスのサンプル | `--acoustic-dump` の転送 ms と ProfilerWindow の GPU ms を分けて読む |

---

## 実装しない / できないもの (v1 の境界)

| 項目 | 理由 / やるなら何が要るか |
|---|---|
| **伝播中の材質吸収** (カーペットの部屋を通ると減衰) | エネルギーが経路依存になり**セル配列そのものが sim 状態に落ちる** (snapshot が数 MB / rollback 破綻)。やるなら「コストを積む Dijkstra」+ セル配列の snapshot 節 |
| **動的コライダの占有反映** (ドア・箱) | v1 は静的のみ。署名 fold が変わると全再ベイクなので動く物を入れると毎 tick スパイクする。やるなら「静的ベイク + 動的オーバーレイ」の二層化 |
| **地形スプラットからの床材解決** | `terraincol::Resolve` + 最大重みレイヤ、の筋だけ残す。**描画側 `TerrainSystem` のキャッシュは絶対に読まない** |
| **回転したボリューム / 複数ボリューム** | グリッドは常に軸平行、シーンに 1 個だけ (2 個目以降は最小 entity.index が勝ち + 警告ログ) |
| **FSM の `.json` 資産化** | 判断 7。3 種目の敵が要求されたら `ControllerLibrary` の型で導入 |
| **NavMesh / 敵同士の回避** | 流れ場 + CharacterController の collide-and-slide のみ。敵同士は重なる |
| **音響の GPU 計算** | AI と絵が同じ場から出ることが要件なので、場は決定論レーン (CPU) から動かせない |
| **実際に鳴る音との統合** | `AudioSourceSystem` は決定論レーンの外。`Emit` と同時に `PlaySound` を鳴らすのは**出力レーン (tick 末ハッシュの後)** の別作業。`SpatialMath.h::RolloffGain` を共有すれば減衰は一致する |
| **残光のロールバック整合** | 残光は描画レーン。巻き戻し後の見た目は `ResetVisual()` を呼び手 (TimeTravel) が決める — GPU パーティクル / VfxRenderer トレイルと**同じ扱い**で、新しい制約ではない |
| **ABI 拡張** | v11 の `Get/SetComponentField` で足りる見込み (M65g の冒頭確認 1 で確定)。足りなければそこで初めて v16 |

---

## 進捗表 (完了時に更新。計画外の事実・罠・申し送りだけ書く)

| サブ | 状態 | 版 / 契約の変更 | メモ |
|---|---|---|---|
| M65a 共有契約 + ボクセル化 | **完了** | TypeId 45〜49 / snapshot v10 / phase 3.4 新設 / M60′ 予約を 50/51 へ | 下の「M65a の申し送り」参照 |
| M65b 波面伝播 + デバッグ線 | 未着手 | replay 7 ペア目 / `--acoustic-demo` | |
| M65c 床材と発音源 | 未着手 | PhysMat 末尾 append 3 本 / physmat 6 種 | |
| M65d 残光ボリューム + 転送 | 未着手 | `VolumeTexture::Create(withUav)` | |
| M65e ライティング差し込み | 未着手 | Deferred t13 / Forward t8 / golden 18-19 枚目 / `$constGroups` 2 件 | |
| M65f 流れ場 + FSM | 未着手 | なし (描画・版とも無風) | |
| M65g ゲームの薄皮 | 未着手 | ABI 据え置きを確定 (要冒頭確認) | |

---

## M65a の申し送り (計画外の事実・罠だけ)

**実測**: 両構成 0 警告 / selftest **43** 本 ALL PASS / replay_verify 9 ジョブ PASS (81.9s) /
shot_verify **17 枚すべて maxDiff=0** / check_rules 0 error。存在ゲートは主張どおり効いた。

1. ★**チャンファ重みの「誤差 2%」は最適スケールを取ったときの話**。計画本文の
   「16/11 = 1.4545 vs sqrt(2) で誤差 1.2%」は**誤り**で、素の比は **2.85%** ずれている。
   チャンファ距離は「真の距離の s 倍」の近似で s は自由に選べるので、`s = sqrt(lo*hi)`
   (幾何平均) を取ったときの最大相対誤差 **1.56%** が正しい実力値。
   `ChamferToMeters` は **s = 11 (面方向が厳密)** を採ってあり、対角が 2.9% 長く出るのは
   「到達距離 [m] が軸方向でぴったり」を優先した意図的な取引。selftest (2) がこの 3 点を固定する。
2. `SimSources` のブレース初期化は **6 箇所** (計画は 7 と書いていた)。全部
   `{&particleSystem.Cpu(), &scene.Time(), &scene.Persist(), ts.xpbd}` の同一文字列なので機械置換で足りた。
3. **フェーズ 3.4 はスクリプト層 (3) の直後・アニメ (3.5) の前**に置いた。計画は
   「`:334` の直前 = フェーズ 4 の直前」と書いていたが、そこだと M65f の `AgentSystem` が書く
   `moveInput` を物理 (3.6) が**次の tick まで消費できない** = 敵の操作が 1 tick 遅れる。
   代償として読む `WorldMatrix` は前 tick のものになるが、これは物理がコライダを
   tick 頭の位置で判定しているのと同じ扱いで、コメントに明記した。
4. `AcousticField.cpp` で署名用の FNV を自前で書いたら **`mye::kFnvOffset` (Core/Hash.h) と
   あいまいシンボル** (C2872)。Core の `HashCombine` / `HashBytes` をそのまま使うのが正解
   (どのみち同じ FNV-1a)。★**構造体の生バイトを丸ごと畳まないこと** — パディングは
   未初期化なので構成で違いうる (スカラーを 1 つずつ畳む)。
5. `OppositeNeighbor` は**表が要らない**。26 近傍を (dz,dy,dx) の辞書順で中心だけ抜いて並べると、
   27 セル並びの点対称「26 - k」がそのまま「**25 - i**」に落ちる。
6. selftest の実測は **42 → 43** 本。`CLAUDE.md` の「40 スイート」は古かったので同時に直した
   (`plans\gleaming-strolling-swing.md` (M64) が「実測 42」と書いていたのが正しい)。
7. `$constGroups` (規則 9) への追加は **M65a では不要**。C++/HLSL 共有定数が出るのは
   M65e (SRV スロット 2 本) から。
8. ★**Bash ツールから `cmd /c "exe ..."` は実行されない** (cmd が対話起動して即終了する)。
   気づかず古い `cache\selftest.log` を読んで「テストが走っていない」と誤診しかけた。
   **exe の実行は PowerShell ツールから `cmd /c`** で行うこと (GUI サブシステムなので
   `& exe` は待たない、という CLAUDE.md の罠とは別問題)。
9. `BakeOccupancy` は署名を作る走査と**別に**候補を集め直している。二度手間だが、
   「焼く条件」と「署名の条件」が将来ずれると *署名は変わらないのに occupancy が変わる*
   = 焼き直されない静かな壊れ方になるため。走るのは署名が変わった tick だけなので代償は無い。
10. 占有ベイクは **box / sphere / capsule のみ**。shape=3/4/5 (メッシュ / 地形 / 凸包) は
    `meshcol` / `terraincol` / `convexcol` の実体注入が要り、注入しないと `meshData` が null で
    全判定が「衝突なし」に落ちる (安全側)。壁は box で組む前提。
