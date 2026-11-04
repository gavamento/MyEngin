# M95 アクション戦闘の土台 — 仕様書

- slug: m95-action-combat
- 状態: 確定 (2026-11-04。round 1 の `[ユーザーに聞ける]` は司会が AskUserQuestion で確認済み → 台帳「ユーザー判断」欄。round 2 で反映)
- 依頼原文: M95 アクション戦闘の土台 (事前調査は plans\m95-action-combat\design-draft.md)

## 1. 目的 (なぜ作るか)

ユーザーは MyEngine で大剣主体の 3D アクション (モンハン / ソウル系のボス戦) を作る。モデル・モーションは自作。
縦切り (武器 1 種 3 段 + 溜め、ロール、ロックオン、スタミナ、ボス 1 体 = 攻撃 3 種・部位 2 箇所、円形アリーナ) を
GameLogic で書こうとすると、エンジン側に次の穴があり、ゲーム側では回避できない (またはリプレイの決定性を壊す形でしか書けない):

1. **ヒットストップ** — Animator に entity 単位の停止が無い (速度はアセットのステート側 `ControllerState.speed` だけ)
2. **部位への重なり・掃引** — 部位 (Part + PartBounds) にはレイしか当てられない。大剣の刀身は球 / カプセルの重なりと球の掃引で当てたい
3. **遷移の割り込み** — 遷移中は遷移を評価しない。被弾・回避で攻撃を途中で切ると姿勢が飛ぶ
4. **CC 同士の接触と接触イベント** — CC は剛体に対してだけ押し出され、CC 同士は素通り。接触イベントは剛体同士からしか出ない (dogfooding 17)
5. **ルートモーションの縦成分** — y を捨てている。跳び込み斬り・飛び退きが作れない
6. **イベント時刻の秒・フレーム指定** — tick 整数でしか書けない。Blender の 30 fps のフレーム番号から手で換算している

達成したい状態: 上記 6 件がエンジンの機能として存在し、**既定 off のまま既存シーン (三校・HAL Collector) がビット同一**で、
Debug / Release / WARP でリプレイが一致する。

進め方 (ユーザー判断、2026-11-04): selftest の所要時間対策を **sub-00** として先頭に置き、以降の coder は**波単位で並列** (1 coder = 1 git worktree)。
共有資源は付録 A の予約表で先に決め、波の統合は付録 B の手順で行う。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | ヒットストップ中に SkinnedMesh の姿勢が保てるか、SkinningSystem 側の時計も止める必要があるか (design-draft 論点 5) | 非アクティブ entity の分岐 `AnimatorController.cpp:1709-1722` が既に「プログラムを保ち、`poseClaim = 1`、`prevTimeQ = timeQ`、`stepQ = 0`」で凍らせている。SkinningSystem は `poseClaim` を見て旧経路の時計を進めない (`Components.h:352-354`) | — | 同じ手で足りる。SkinningSystem は触らない。LocalTransform を動かすプロパティクリップも「時刻を進めない = 書き直さない」で保たれる |
| 2 | ヒットストップに速度倍率 (スローモーション) も要るか | 時刻は整数 tick (`ControllerState.speed` = tick/進み)。倍率は分数の時刻を生み、決定性の規約 (整数比) を崩す | — | 入れない。停止だけ。ジャンル上も停止で足りる |
| 3 | ヒットストップ中に CC の重力も止めるべきか | Unity の `animator.speed = 0` も物理は止めない。重力を止めると空中で被弾した相手が浮く | — | 止めない。停止中は `rootMotionVelocity = 0` を書くので applyRootMotion の CC は水平に動かない。スクリプト駆動の移動は `AnimatorGetPause` を見てゲーム側が止める |
| 4 | 部位の重なり・掃引に物理の形状判定を再利用できるか | `shapes::Overlap` は全 6 ペア対応 (`Shapes.h:87-91`)。掃引は `SphereCastWorld` (`PhysicsQueries.cpp:166-215`) が「sphere/capsule は膨張レイの解析解、box は 32 回の保守的前進」の 2 本立て。部位は箱と球だけ (`MakePartBoundsPose`) | — | 再利用する。`SphereCastWorld` の対象 1 件分を共通関数へ切り出してよいが、**演算順を 1 つも変えない**こと (replay_verify が番人) |
| 5 | カプセルの重なり (`OverlapCapsuleParts`) も要るか | 刀身は tick ごとに前位置→今位置の球の掃引でも当たるが、刀身の長さを 1 回で覆うにはカプセルが素直 (Unity `Physics.OverlapCapsule` 相当)。`shapes::Overlap` はカプセル vs 箱 / 球に対応済み | **要る** (planner 裁定「要らない」と逆、2026-11-04) | sub-03 に `OverlapCapsuleParts` (両端点 + 半径) を足す。ABI v32 = 193 スロット、受け入れ条件 31 |
| 6 | 割り込み時に進行中の混合ポーズから混ぜ直すべきか (design-draft 論点 4) | Unity は混合ポーズを保存して混ぜ直す ([Unity Manual: Animation transitions](https://docs.unity3d.com/2020.3/Documentation/Manual/class-Transition.html))。ここでは姿勢のスナップショットが hash 対象を増やす | — | **近い側へ確定する**: `transitionTick * 2 >= transitionDuration` なら遷移先へ、未満なら元へ確定してから新しい遷移を張る。`CurrentSpriteFrame` (`AnimatorController.cpp:1636`) と同じ整数規則。飛びは最大でも重み 0.5 ぶん |
| 7 | `AnimatorPlay(duration > 0)` を遷移中に呼んだときの挙動も変えてよいか (opt-in 原則) | 今は `transitionTo` だけ上書きし元ステートはそのまま (`AnimatorController.cpp:1610-1618`) = 姿勢が飛ぶ欠陥。三校 `C:\HAL\三校\src` と HAL Collector `C:\HAL\GameEngin_Demo\src` に `AnimatorPlay` / `PlayAnimation` の呼び出しは無い (grep) | 裁定どおり (2026-11-04) | 欠陥として直し、#6 と同じ確定規則にする |
| 8 | 割り込みの `from` は何に合わせるか | Unity の Interruption Source は Current / Next / 両方。ここは「最初に条件を満たした遷移」の 1 本道 | — | `from` が Any (-1) / `currentState` / `transitionTo` のいずれかに合う `canInterrupt` 遷移を**アセット順**で評価する。`hasExitTime = 1` の遷移は遷移中は評価しない (割り込みは条件・トリガー駆動の用途) |
| 9 | CC 同士の押し出し規則 (design-draft 論点 1): 半々か、片側か | `SolveCharacters` は CC を index 昇順に**逐次**解く (`PhysicsSystem.cpp:1521-1524`)。半々は「低 index は相手の tick 頭、高 index は相手の解決後」を見るので順序依存 (低 index が余計に譲る)。Unity / UE の pawn 同士は「動いた側が止まる」ブロック | 裁定どおり (2026-11-04) | **相手の tick 頭のカプセルから自分を全量押し出す** (全 CC が同じ姿勢を見る = 順序非依存)。同じ `pushPriority` なら互いに止まる。`pushPriority` が高い側は低い側を無視し、低い側だけ押し出される (ボスの体当たり。1 tick 遅れで押し続ける) |
| 10 | CC 同士の接触で接地・天井の扱い | 障害物の接触と同じ分岐 (`ny >= cosSlope` で接地、`<= -cosSlope` で天井、`PhysicsSystem.cpp:1596-1606`) | — | 障害物と同じにする (分岐を増やさない)。頭に乗れるが半球なので滑り落ちる |
| 11 | CC の接触イベントの範囲 (design-draft 論点 2): CC↔剛体も出すか | CC + ソリッド capsule Collider の推奨パターンでは、その Collider が**静的ボディ**として収集され (`PhysicsSystem.cpp:2005`)、動的剛体との接触は既に `SolidContact` で両者へ届く。届かないのは CC↔CC と CC↔kinematic / 静的 (静的同士は拘束を捨てる `:1032`) | — | `reportContacts` の CC が押し出しのどのパスかで触れた相手 (CC / 静的 / kinematic / 動的) を**対単位**で出す。既定 off なので既存スクリプトに新しい `OnCollision*` は起きない。剛体側と key が重なれば**剛体側を残す** (二重配送しない) |
| 12 | 対単位だと opt-in していない相手にも届く | `CollisionSystem` は key の両側へ配る (`CollisionSystem.cpp:160-170`)。片側だけに配るには配送側へ新しい状態が要る | — | 対単位でよい。opt-in した CC の相手は「触られた」ことを知ってよい (剛体同士と同じ対称性) |
| 13 | CC は運動量を交換するか | 設計意図 (`PhysicsSystem.cpp:1514-1515` "CC が剛体を押す応答は無し") | — | 交換しない。押し出しと通知だけ。ADR-034 に記録 |
| 14 | ルートモーションの縦を CC に渡す印 (design-draft 論点 3) | `SolveCharacters` の冒頭 `vy = velocity.y + g·dt` (`:1526`)。`jumpSpeed` は「積んで次 tick に消費」の前例 (`:1662-1667`) | — | CC に `useVerticalInput` / `verticalInput` を append (jumpSpeed と同じ消費則)。Animator が `velocity.y` へ重力を差し引いた値を書く案は隠れた結合なので却下 |
| 15 | 縦成分の opt-in はコンポーネント (entity) かステート (アセット) か | Unity は Root Transform Position (Y) の Bake Into Pose を**クリップ単位**で選ぶ。UE は Falling 中 Z を捨てる ([ConstrainAnimRootMotionVelocity](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/GameFramework/UCharacterMovementComponent/ConstrainAnimRoo-?application_version=5.1))。歩きに付けると縦をクリップが決めて段差で浮く | 裁定どおり (2026-11-04) | **ステート単位** `"skel":{"rootMotionY":1}` (v3)。縦をクリップが決めるステート (跳び込み・飛び退き) にだけ付ける |
| 16 | 縦の適用先に非 kinematic Rigidbody も含めるか | 剛体はサブステップごとに重力を積分する (`PhysicsSystem.cpp:3108-3122`)。飛ばすには Rigidbody にも印が要る | **適用する** (planner 裁定「CC と LocalTransform だけ」と逆、2026-11-04) | Rigidbody にも `useVerticalInput` / `verticalInput` を append (CC と同名・同じ消費則)。収集時 `b.vy = verticalInput`、その tick の全サブステップで y の重力を飛ばす (x/z の env 重力は従来どおり)、書き戻しで消費。kinematic は書かない。受け入れ条件 32 |
| 17 | イベントの `fps` の置き場 | `clipEvents` はクリップ名 → 配列 (`AnimatorController.cpp:1188-1221`)。イベント単位の fps は冗長 | — | クリップ単位の `{"fps":30,"events":[...]}` 形を**追加で**受ける (配列形は従来どおり)。保存は tick 配列のまま |
| 18 | ABI v32 を M95 が取ってよいか (memory では M75h が v32 を予定) | 「末尾から取る」規則 (司会の指示) | 司会 | M95 = v32 (188 → 193 スロット)。M75h は着手時の末尾 |
| 19 | スナップショット版はマイルストーンで 1 回か、サブごとか | M94 は v56〜v65 をサブごとに上げた (`SimSnapshot.h:165-175`) | — | サブごとに予約 (付録 A): v66 (sub-01) / v67 (sub-02) / v68 (sub-05)。同じ波で衝突したら統合時に最大値へ揃える |
| 20 | 既存シーンのビット同一の検証手段 | `tools\replay_verify.bat` と三校 `C:\HAL\三校\tools\verify.bat`。HAL Collector (`C:\HAL\GameEngin_Demo`) に検証スクリプトは無い | — | replay_verify + 三校 verify.bat。HAL Collector は selftest / replay_verify で代用 (未検証として報告) |
| 21 | hash 対象かどうか | AnimatorController / CC / Rigidbody は hash 対象 (`Components.h:777-779, 851-853`、`Components.cpp:15`)。SkinnedMesh は NoHash (`Components.cpp:288`) だが生バイトは snapshot に載る | — | 追加欄は全て既定 0 / false → シミュレーションの結果 (位置・速度・分岐) は不変。**ただし WorldHash はカラムの生バイトを混ぜるので、hash 対象の struct に欄を足すと値そのものは変わる** (M94i で CC の 2D 欄を足したときと同じ)。selftest に焼き込んだ hash 定数 (CC2D / 庭 / tile / gridnav / twod の 5 件) と AcousticAudioSelfTest の snapshot 版チェックは、欄を足したサブが再採取し、波の統合後に司会が最終値で再採取する (付録 B)。レイアウト変更は snapshot 版で吸収 (sub-02 VERDICT round 1 で訂正) |
| 22 | selftest の全走が長く、往復のたびに回すと coder の時間を食う | `EditorMain.cpp:586-614` の `MYE_SELFTEST_ONLY` は 4 値 (`servernet*` / `computescript` / `twod` / `m94`) のハードコードで、他のスイートは絞れない。所要時間のログも無い | **sub-00 を先頭に足す** (2026-11-04) | 「スイート名 → 関数」の登録表に置き換え、カンマ区切りの任意名で絞る。`--selftest-list`、スイートごとの ms ログ。既存 4 値は別名として残す。製品の挙動には触れない |
| 23 | 並列実装で末尾 append が別の struct に入る誤マージ (memory `parallel-milestone-execution` #5) | 両ブランチが同じ `};` の直前に追記すると git は競合と見なさず自動解決する | 波単位の並列 (2026-11-04) | 付録 A で append 先を struct ごとに分け、sub-00 が予約マーカー (コメント行) を先に入れて各サブは自分のマーカーの下にだけ書く。統合時は付録 B の目視手順 |

## 3. スコープ

- やる:
  - sub-00 / sub-00b: selftest の 3 層 (登録表 + tier、`MYE_SELFTEST_ONLY` の汎用フィルタ、`--selftest-full` / `--selftest-list`、所要時間ログ、CI は full、計測に基づく層分けと縮小)、並列実装用の予約マーカー
  - ヒットストップ (Animator の tick 単位停止、ABI 2 本)
  - 部位への球の重なり・カプセルの重なり・球の掃引 (ABI 3 本)
  - 遷移の割り込み (`canInterrupt`、controller v3) と `AnimatorPlay` の遷移中の確定
  - CC 同士の押し出し (`collideCharacters` / `pushPriority`) と接触イベント (`reportContacts`)、dogfooding 17 の解決、ADR-034
  - ルートモーションの縦成分 (ステート単位 `rootMotionY`、CC と非 kinematic Rigidbody の `verticalInput`)
  - イベント時刻の `time` (秒) / `frame` + クリップ単位 `fps`
  - 文書 (engine_spec / ADR-034 / dogfooding / test_checklists)、回帰テスト
- やらない (明示的に外したもの):
  - CC カプセルがスケールを拾う挙動 (ユーザー判断)
  - 固定 60 Hz tick と rand() 禁止の解除 (ユーザー判断)
  - 速度倍率 (スローモーション)、混合ポーズからの混ぜ直し、カプセルの掃引 (CapsuleCast)、CC の運動量交換、kinematic Rigidbody への縦の適用
  - ダメージ通知の新設 (`BtSendEvent` / `BtGetEvent` で足りる、`EngineAPI.h:960-964`)
  - sub-00 で selftest の中身 (各スイートの内容・順序) を変えること
- 後回し: なし

## 4. 仕様

### 4.0 selftest の 3 層構成 (sub-00 / sub-00b)

ユーザー決定 (2026-11-04): 「テストを減らす」のではなく「1 回の実行で回す量を最小にする」。

| 層 | いつ回すか | 中身 | 目標時間 |
|---|---|---|---|
| 関連スイートだけ | coder の往復中 | `MYE_SELFTEST_ONLY=animatorcontroller,part` のような名前指定 (カンマ区切り) | 数秒〜数十秒 |
| `--selftest` (既定) | SELF_EVAL を返す前、サブの区切り、手元の確認 | `tier = fast` のスイート全部 | 60 秒以内 (Debug) |
| `--selftest-full` (または `MYE_SELFTEST_FULL=1`) | VERDICT OK の直前、レビュー、push 前、CI | 全スイート (fast + full) | 今の全走と同じ |

#### sub-00 (Wave 0): 登録表・フィルタ・所要時間ログ・full の入口・CI・予約マーカー

- `src\Editor\SelfTest\SelfTestSuites.h / .cpp` (新規、`tools\gen_project_files.ps1` で登録) に
  `enum class SelfTestTier { Fast, Full }` と `struct SelfTestSuite { const char* name; bool (*run)(); SelfTestTier tier; const char* note; }`、
  **今の全走と同じ順**の表 (`EditorMain.cpp:618-748` の `ok &= mye::Run*SelfTest();` 列を 1 行 1 要素に写す。順序を変えない)。
  名前は関数名から `Run` と `SelfTest` を除いて小文字化 (`RunPhysicsSelfTest` → `physics`、`RunAnimatorControllerSelfTest` → `animatorcontroller`、`RunTwoDAbiSelfTest` → `twodabi`、`RunTwoDDemoSelfTest` → `twoddemo`)。
  **sub-00 の時点では全スイートが `Fast`** (= `--selftest` と `--selftest-full` が同じ集合。層分けは計測後に sub-00b が行う)。
- 別名 (グループ) の表: `servernet` (および `servernet-` で始まる値 → `servernet` スイート。`ServerNetSelfTest.cpp:784-789` が環境変数を自分で読む仕組みは残す)、`computescript`、`twod` (= `twoddemo`)、`m94` (今の `:606-633` の並びそのまま)、`m95` (= `physics,animatorcontroller,skeleton,part,twodabi,simsnapshot,worldhasher`)。
- `MYE_SELFTEST_ONLY`: カンマ区切り。各項目は別名またはスイート名 (小文字化・前後の空白を除く)。展開して**表の順**に重複なく回す (tier を問わない)。**例外: 別名 1 個だけの指定は別名の並びで回す** (`m94` を従来の `:606-633` の順で回すため。別名 + 他の項目の混在は表の順)。未知の名前は `[selftest] unknown suite: <name>` と一覧を出して exit 2 (何も回さない)。環境変数の読み取りは 32 文字の固定バッファ (`:589-590`) をやめ、長さを問わない。`servernet` / `computescript` はスイート名そのものなので別名表には載せない (`servernet-*` は前方一致で `servernet` へ)。
- `--selftest`: `MYE_SELFTEST_ONLY` が無ければ `Fast` のスイートを表の順に回す。`--selftest-full` または `MYE_SELFTEST_FULL=1` なら全スイート (表の順)。`MYE_SELFTEST_ONLY` は両者より優先。
- `--selftest-list`: `<name>\t<fast|full>` を 1 行 1 件、続けて別名 (`<alias>\t= a,b,c`) を出して exit 0 (ウィンドウ / D3D を作らない)。`EditorMain.cpp:298` の `--selftest` と同じ場所で `--selftest-full` / `--selftest-list` を解釈。`EngineCli.cpp` の表に載せるかは既存の `--selftest` に合わせる (載っていなければ載せない)。
- 所要時間: スイートごとに `[selftest] <name> PASS|FAIL (<ms> ms, <fast|full>)`、最後に `[selftest] total <n> suites, <k> failed, <ms> ms (mode: fast|full|only)`。計測は `std::chrono::steady_clock`。ログは stdout (既存の `check(...)` 出力と同じ経路)。
- CI: `.github\workflows\ci.yml:142-148` の `--selftest` (Debug / Release) を `--selftest-full` に書き換える。
- `AGENTS.md` §7 の表 (`:144`) に `--selftest` (既定 = fast 層) / `--selftest-full` / `MYE_SELFTEST_ONLY=<名,...>` / `--selftest-list` の行を足し、`:149` の「提出前」を `--selftest-full` にする。
- 一時フォルダの扱い (`:575-584`) と `ok &=` (短絡しない) は維持。製品の挙動 (sim / 描画、`--selftest*` 以外の経路) には 1 命令も触れない。
- 予約マーカー: 付録 A の「マーカー」列のコメント行を各ファイルに入れる (中身は空)。

#### sub-00b (Wave 1、並行可): 計測に基づく層分けと縮小

- **先に計測する** (AGENTS §3.5。推測で層分けしない): sub-00 の ms ログで Debug / Release の全スイートを各 2 回回し、スイート名 / Debug ms / Release ms の表を `sub-00b.md` の実装メモに残す (2 回の差が大きいスイートはその旨を書く)。
- 層分けの規則: 所要時間 (Debug) の降順に並べ、**上位 N 本を `Full` にしたとき残り (既定層) の合計が 60 秒以内に収まる最小の N** を採る。候補は所要時間だけで選ぶ (名前や印象で選ばない)。
- 縮小の検討: `Full` 候補のうち、サイズを縮めても不変量を守れるスイート (前例: M80 の破砕ベンチで res を落とした) は、`Full` へ移す代わりにパラメータを縮めて `Fast` に残す案を 1 本ずつ検討し、採否と理由を実装メモに書く。**Debug / Release のビット一致・焼き込みハッシュ・golden を検査しているスイート (例: `twoddemo` の 3000 tick、`navdeterminism`、`simparallel`) は値を変えない** (`Full` へ移す)。縮めた場合は縮めた値で `Full` の検査も同じになるので、元の値での検査が要るなら `Full` 用の入口 (環境変数または引数) を残す。
- 分け終えたら表の `tier` を書き換える。既存 4 値の別名は tier を問わず従来の集合を回す。
- `docs\test_checklists.md` / `engine_spec.md` の記述は sub-07 (sub-00b は表と `AGENTS.md` の 1 行だけ)。

### 4.1 振る舞い

#### A. ヒットストップ (sub-01)

- `AnimatorControllerComponent` 末尾に `int32_t pauseTicks = 0` を append (hash 対象)。
- `AnimatorControllerSystem::Update` で、非アクティブ判定の**後**に `pauseTicks > 0` なら:
  1. 駆動する SkinnedMesh を非アクティブ分岐と同じ手で凍らせる (`poseClaim = 1`、各層 `prevTimeQ = timeQ`、`stepQ = 0`)
  2. 時刻・位相・遷移カウンタ・`stateEntered` を進めない。イベントを発火しない。スプライトのコマも書き直さない (残る)
  3. `rootMotionVelocity = (0,0,0)`、`rootMotionDeltaRotation = (0,0,0,1)`。`applyRootMotion` なら `ApplyRootMotion` を 0 で呼ぶ (CC の `moveInput.xz = 0`、非 kinematic Rigidbody の `velocity.xz = 0`、LocalTransform は動かない)。ヨーは回さない
  4. `--pauseTicks` して次の entity へ
- 非アクティブ entity では `pauseTicks` を減らさない (凍結が優先)。
- ABI v32 (`MYE_API_VERSION 32`、`TilemapGetBounds` の次):
  - `int (*AnimatorSetPause)(void* engine, MyeEntityId entity, int32_t ticks)` — 上書き (累積しない)。負は 0。Animator 無しは 0 を返し何もしない。成功 1
  - `int32_t (*AnimatorGetPause)(void* engine, MyeEntityId entity)` — 残り tick。Animator 無しは -1
  - `ScriptAPI.h` の inline 糖衣、`Interop.cs` の関数ポインタ (位置ミラー) と `Engine.AnimatorSetPause / AnimatorGetPause`、`MyeScript.cs` の糖衣
- Inspector: `pauseTicks` を読み取り専用で表示 (反射登録)。

#### B. 部位への重なり・掃引 (sub-03)

- `Parts::OverlapSphereParts(World&, EntityID root, uint64_t tag, const XMFLOAT3& center, float radius, std::vector<EntityID>& out)`:
  候補収集 (Part + PartBounds + WorldMatrix、tag 一致、root 配下、アクティブ、非縮退) → index 昇順 → `shapes::Overlap(球, MakePartBoundsPose)` が真のものを **index 昇順**で `out` に積む。`radius <= 0` は 0 件。
- `Parts::OverlapCapsuleParts(World&, EntityID root, uint64_t tag, const XMFLOAT3& p0, const XMFLOAT3& p1, float radius, std::vector<EntityID>& out)`:
  カプセルは両端点 `p0` / `p1` (線分の端 = 半球の中心) と半径。`ShapePose` は `shape = kCapsule`、中心 = 中点、`halfSeg = |p1 − p0| / 2`、軸の基底 = `normalize(p1 − p0)` (カプセルの軸がどの基底 (`bx/by/bz`) かは `Shapes.cpp` の `CapsuleCapsule` / `CapsuleBox` で確認して合わせる。残り 2 軸はそれに直交する任意の正規直交基底で、決定的に作る)。`|p1 − p0| < 1e-6f` なら球として判定。規約は `OverlapSphereParts` と同じ。
- `Parts::SphereCastParts(World&, EntityID root, uint64_t tag, origin, dir, float radius, float maxDist, PartRayHit& out)`:
  同じ候補列を index 昇順に走査し、sphere 部位は半径を `radius` だけ膨らませた `shapes::Raycast`、box 部位は `DistanceToShape` の保守的前進 (固定 32 回、ヒット後も回数を消化、`d < 1e-4f` で停止) — `SphereCastWorld` と同じ 2 本立て・同じ定数。厳密 `<` で最近を更新 (同 t は低 index)。`point` は元形状の表面点、`normal` は膨張面の法線。`radius <= 0` は `RaycastParts` と同じ結果。`maxDist <= 0` は `1e9f`。dir は正規化済み前提 (既存契約)。
- ABI v32 (`AnimatorGetPause` の次、この順):
  - `int32_t (*OverlapSphereParts)(void* engine, MyeEntityId root, uint64_t tag, MyeVec3 center, float radius, MyeEntityId* out, int32_t cap)` — 戻り値は**切り捨て前の総数** (`FindPartsByTag` の規約)。`out = null / cap <= 0` で数えるだけ
  - `int32_t (*SphereCastParts)(void* engine, MyeEntityId root, uint64_t tag, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist, MyeRaycastHit* outHit)` — ヒット 1 / 無し 0。`outHit->entity` = 部位
  - `int32_t (*OverlapCapsuleParts)(void* engine, MyeEntityId root, uint64_t tag, MyeVec3 p0, MyeVec3 p1, float radius, MyeEntityId* out, int32_t cap)` — `OverlapSphereParts` と同じ規約
  - C++ 糖衣 / C# ミラー / `MyeScript.cs` 糖衣 (タグ名文字列版)
- 読むのは WorldMatrix (前 tick の TransformSystem の結果) — `RaycastParts` と同条件。Start() から使えない注意も同じ。

#### C. 遷移の割り込み (sub-04)

- `ControllerTransition` に `int32_t canInterrupt = 0` (JSON `"canInterrupt"`、既定 0)。v3 で保存 (`"controller": 3`)、v1 / v2 はそのまま読める。
- `Update` の 1. を拡張: `transitionTo >= 0` のときも遷移を走査し、`canInterrupt == 1` かつ `hasExitTime == 0` かつ `from ∈ {-1, currentState, transitionTo}` かつ `to != transitionTo` かつ `to` が範囲内、かつ `AllConditionsMet` の最初の 1 本を採る。
  採用時: `ConsumeTriggers` → **確定** (`transitionTick * 2 >= transitionDuration` なら `currentState = transitionTo / stateTimeTicks = transitionToTime / statePhase = transitionToPhase`、未満なら元のまま) → `stateEntered = 0` → 新遷移を張る (`transitionTo = to`、`transitionTick = 0`、`transitionDuration = max(duration, 1)`、`transitionToTime = 0`、`transitionToPhase = 0`)。
  確定後の `currentState == to` (元へ確定して元へ戻る遷移) は採用しない (採用直前に再判定)。
- `canInterrupt == 0` のみのアセットでは状態・結果が従来と同一 (遷移中の走査は遷移リストを舐めて先頭の `continue` で抜ける形でよい。fp 演算は無く、決定性・ビット同一に影響しない。アセット側の `anyInterruptible` フラグは要らない — sub-04 VERDICT round 1 で確定)。
- `AnimatorPlay(duration > 0)` を遷移中に呼んだときも同じ確定規則 (#7)。`duration <= 0` は従来どおり即切り替え。
- Editor: `AnimatorControllerWindow` の遷移リストに「割り込み可」チェック (`Tr`、両言語)。

#### D. CC 同士の接触と接触イベント (sub-02)

- `CharacterControllerComponent` 末尾に append (hash 対象、既定 off):
  - `bool collideCharacters = false` — 両者が true の CC 同士だけ押し出す
  - `int32_t pushPriority = 0` — 高い側は低い側を無視する。等しければ互いに止まる
  - `bool reportContacts = false` — この CC が触れた相手を接触イベントとして出す
- 押し出し (`SolveCharacters`、固定パスの中、`bodies` の走査の後): `c.cc->collideCharacters` のとき `chars` を index 昇順に走査し、`other != c`、`other.cc->collideCharacters`、`CanCollide(層)`、`other.cc->pushPriority >= c.cc->pushPriority` の CC について、**相手の tick 頭のカプセル** (`other.px/py/pz`、`other.radius/halfSeg`、ワールド Y 軸) と `shapes::Collide(pose, otherPose)` → `pose += n * depth`、接地 / 天井は障害物と同じ分岐 (#10)。plane2D の CC は z を押し出さない (既存規則)。
  `collideCharacters` の CC が 1 体も無いシーンでは `chars` の走査に入らない (fp 演算ゼロ)。
- 接触の記録: `c.cc->reportContacts` の CC が押し出しのどのパスかで `Collide` が真になった相手 (剛体 `obs` と CC `other`) を対として記録する。CC↔CC は**どちらか**が `reportContacts` なら記録。接地プローブは記録しない (状態を変えない探りなので)。
  1 対 1 件: `key = (小 index << 32) | 大 index`、法線は**大 index → 小 index** (Collide の n は相手 → 自分なので、自分が小 index なら n、大なら -n)、`px/py/pz` は相手形状の表面上で自分の中心に最も近い点 (`ClosestPointOnShape`)、`impulse = 0`。
- 合流: `SolveCharacters` に `std::vector<SolidContact>* outContacts` を渡す。記録が 1 件以上あるときだけ key でソート → 重複 key を除く → `outContacts` に無い key だけ差し込み、全体を key 昇順に保つ (`CollisionSystem::FindContactNormal` の二分探索の前提)。既に剛体側にある key は剛体側を残す。記録が 0 件なら `outContacts` に触らない。
- 配送は既存の `CollisionSystem` (enter / stay / exit、両側へ)。`GetContactInfo` (v14) からも見える (impulse 0)。デバッグ描画にも載る。
- ADR-034: 「CC は運動量を交換しない。押し出し (tick 頭の姿勢、優先度) と通知だけ」。dogfooding 17 を「修正済み (M95)」へ。

#### E. ルートモーションの縦成分 (sub-05)

- `ControllerState` に `int32_t rootMotionY = 0` (JSON `"skel":{"clip":"Jump","rootMotionY":1}` / ブレンドツリーも `"skel"` 直下)。v3。
- `SkeletalSource` と `SkinnedMeshComponent::PoseLayer` に `int32_t rootMotionY` を持たせ、`BuildSkeletalSources` がステートから写す。
- Animator の 8. (`RootMotionDelta` `:745-787` と呼び出し元 `:1904-1931`): **既存の演算順は 1 つも変えない** (3D の `out` を積んで回し `deltaWorld[1] = 0` はそのまま)。それとは別に `rootMotionY` の層だけで `upDelta = Σ w · (m.delta · up)` (エンティティ空間の上方向の成分、スカラー) を積み、`rootMotionY` の層が 1 つでもあるときだけ呼び出し元で `up · upDelta` を連鎖のスケール・回転でワールドへ回して `deltaWorld[1]` に入れる。`rootMotionVelocity.y = deltaWorld[1] * kTicksPerSecond`。`rootMotionY` の層が無ければ分岐に入らず従来とビット同一。
- `SkinningSystem::StripRootMotion` (`:110-170`): 水平は従来どおり全層の `d` から抜く。縦は `rootMotionY` の層だけで `alongY = Σ ratio · ((now − start) · up)` を別に積み、層が 1 つでもあるときだけ `alongY · up` も抜く。Animator 側と同じ式 (「同じ答えを見る」1 本化)。無ければ分岐に入らない。
- `ApplyRootMotion` (この tick に `rootMotionY` の層が骨を駆動したときだけ縦を書く。駆動していなければ縦は触らない = 重力に戻る):
  - NavMeshAgent が握っていれば従来どおり何もしない
  - 非 kinematic Rigidbody: `velocity.xz` に加え `useVerticalInput = true; verticalInput = velocity.y; isSleeping = false` (#16)。kinematic は従来どおり LocalTransform へ
  - CC: `moveInput.xz` に加え `useVerticalInput = true; verticalInput = velocity.y`
  - LocalTransform: y も足す
- `CharacterControllerComponent` 末尾に append (hash 対象): `bool useVerticalInput = false`、`float verticalInput = 0`。`SolveCharacters` 冒頭: `vy = useVerticalInput ? verticalInput : velocity.y + g·gravityScale·dt`。以降 (床で `vy<0 → 0`、天井、段差、`jumpSpeed`) は従来の順。書き戻しで `useVerticalInput = false` (消費)。
- `RigidbodyComponent` 末尾に append (hash 対象): `bool useVerticalInput = false`、`float verticalInput = 0`。収集時 (`PhysicsSystem.cpp:1882`) `b.vy = useVerticalInput ? verticalInput : velocity.y` と `Body` に印を写し、その tick の**全サブステップ**で y の重力積分 (`:3119` / `:3122`) を飛ばす (x/z の env 重力は従来どおり。減衰・接触・拘束は従来どおり効く)。書き戻し (`:5765-5767`) で `useVerticalInput = false`。印が無いボディは 1 命令も変わらない (分岐ゲート)。kinematic は読まない。
- どちらも `SetComponentField` からスクリプトが書ける (ABI は増やさない)。
- 契約: `rootMotionY` のステートが骨を駆動している間、縦は**クリップが決める** (重力は効かない。y が 0 の tick は浮く)。歩き・待機に付けないこと。遷移中はもう一方の層の重み分だけ y が薄まる (線形混合、位置の飛びは無い)。

#### F. イベント時刻の秒・フレーム指定 (sub-06)

- `clipEvents` の各クリップは従来の配列、または `{"fps": 30, "events": [...]}` オブジェクト (fps は正の数、既定 60。0 以下 / 非数は警告して 60)。
- 各イベントの位置: `tick` があればそれ。無ければ `time` (秒、数値) → `tick = llround(time * 60.0)`。無ければ `frame` (数値) → `tick = llround(frame * 60.0 / fps)`。どれも無ければ 0 (`time` / `frame` が数値でないときも 0)。double で計算し `std::llround` (half away from zero) → `int32_t`。換算結果が非有限なら 0、int32 の範囲外は clamp。換算は `FromJson` の 1 箇所。オブジェクト形で `events` キーが無いクリップは読み飛ばす (sub-06 VERDICT round 1 で確定)。
- 保存 (`ToJson`) は従来どおり `tick` 配列 (手書きの `time` / `frame` は保存で tick 化される。文書に明記)。
- Editor (`AnimationPreviewWindow` のイベント帯、`AnimPrev_EvTick` のスライダ): 右に `= 0.500 s` の併記 (tick / 60、`snprintf` した文字列を `%s` で出す。`Tr()` を書式に渡さない)。内部表現は tick のまま。

### 4.2 データ・保存形式・互換性

| 項目 | 変更 | 互換 |
|---|---|---|
| ABI | v32 = **193** スロット。順: `AnimatorSetPause` / `AnimatorGetPause` (sub-01)、`OverlapSphereParts` / `SphereCastParts` / `OverlapCapsuleParts` (sub-03)。`EngineAPI.h` の版コメント、`Interop.cs` 位置ミラー、`ScriptAPI.h`、`PartSelfTest` の版・充填チェック | GameLogic.dll は再ビルド (版不一致は `ScriptHost.cpp:287` で拒否) |
| SimSnapshot | v66 (sub-01: Animator `pauseTicks`)、v67 (sub-02: CC 3 欄)、v68 (sub-05: CC 2 欄 + Rigidbody 2 欄 + SkinnedMesh `PoseLayer.rootMotionY`)。統合の規則は付録 A | 旧 blob は読まない (既存規則) |
| controller アセット | v3 (`"controller": 3`): 遷移 `canInterrupt` (sub-04)、ステート `skel.rootMotionY` (sub-05)。`clipEvents` の `time` / `frame` / `fps` 形は読みのみ (sub-06) | v1 / v2 はそのまま読める。保存は v3 |
| シーン | 追加欄は反射登録 (既定値はシーンに出ない) | 既存シーンのロード結果・hash は不変 |
| TypeId / ADR | TypeId 追加なし。ADR-034 | — |
| selftest | スイート登録表 (sub-00)。全走の順・集合は不変 | `MYE_SELFTEST_ONLY` の既存 4 値は別名として動く |

### 4.3 UI / ビジュアル

- `AnimatorControllerWindow` 遷移リスト: 「割り込み可」チェック (hasExitTime の隣)
- `AnimationPreviewWindow` イベント帯: tick スライダ右の秒併記
- Inspector: Animator の `pauseTicks` (読み取り専用)、CC の `collideCharacters` / `pushPriority` / `reportContacts` (編集可)、CC / Rigidbody の `useVerticalInput` / `verticalInput` (読み取り専用)
- 文字列は `LocalizationTable.inl` に両言語 (`###` 識別子は一致)。接頭辞は付録 A

### 4.4 非機能

- 決定性: 固定パス・index 昇順・整数比較のみ追加。CC 同士は tick 頭の姿勢を見る (順序非依存)。`SphereCastParts` は `SphereCastWorld` と同じ固定反復。ハッシュコンテナ走査なし
- 既存シーン不変: 全機能 opt-in (既定 0 / false)。opt-in が無いシーンで fp 演算を増やさない (分岐ゲート = M59f1 の流儀)
- ABI 規則: 既存スロットの引数は変えない。C ABI / POD のみ
- ローカライズ: 新規 UI 文字列は両言語
- 文書: `engine_spec.md` 新節、ADR-034、dogfooding 17、test_checklists M95 節
- **selftest の層**: 既定層 (`--selftest`) を軽くした分、**`--selftest-full` は VERDICT OK の直前・レビュー・push 前・CI で必ず回す** (Debug / Release 両方)。coder の検証欄は「往復中 = `MYE_SELFTEST_ONLY` で関連スイート名指定 / SELF_EVAL を返す前 = `--selftest` (既定層) / VERDICT OK の直前 (最後の round) = `--selftest-full` を Debug / Release で 1 回 + CR + RV + 三校」に統一。sub-00b が終わるまでは既定層 = full (全スイート) なので時間はかかるが規則は同じ
- **並列実装の罠** (memory `parallel-milestone-execution`): 両ブランチが同じ `};` の直前に追記すると git は競合マーカーを出さずに自動解決し、片方のフィールドが**別の struct に入る**ことがある (コンパイルが通ることすらある)。対策は 3 つ: (1) 付録 A の予約表で append 先を struct ごと・サブごとに分ける、(2) sub-00 が各 append 先に**サブ名入りの予約マーカー (コメント行)** を先に入れ、各サブは自分のマーカーの直下にだけ書く (= 2 本のブランチが同じ行に追記しない)、(3) 統合時に付録 B の目視手順 (構造体のメンバー一覧・ABI 表の末尾・Interop.cs の末尾・selftest 表) を必ず行う。coder は **worktree の絶対パスで作業し、master のチェックアウト (`C:\HAL\MyEngin`) へ書かない** (`git status` で漏れを確認)

## 5. 受け入れ条件

検証の略記: **ST** = `bin\x64\Debug\Editor.exe --selftest` と `bin\x64\Release\Editor.exe --selftest` (PowerShell ツールから起動)、**ST(名)** = `$env:MYE_SELFTEST_ONLY='<名>'` を付けた部分実行 (sub-00 以降)、**RV** = `tools\replay_verify.bat`、**CR** = `tools\check_rules.ps1`、**三校** = `C:\HAL\三校\tools\verify.bat` (`MYE_ENGINE=C:\HAL\MyEngin` 相当。worktree なら worktree のパス)。
**ST-full** = `--selftest-full` (Debug / Release)。
3 段の規則: 往復中 = ST(関連スイート) / SELF_EVAL を返す前 = ST (既定層、Debug) / VERDICT OK の直前 (最後の round) = ST-full Debug / Release + CR + RV + 三校を 1 回。
**「最後の round」の判定は coder が行う**: 自分で「仕様との差分に must 級の未実装が無く、これで OK を求める」と判断した round では、SELF_EVAL を返す**前**に ST-full Debug / Release + CR + RV + 三校を回し、結果を検証欄に貼る。planner は full 検証の結果が無い SELF_EVAL に OK を出さない (REWORK で「最終検証を回して次の round で返す」を指示する)。
**三校 verify の扱い** (2026-11-04、司会の切り分け): 三校の `cache\GameLogic.vcxproj` は `C:\HAL\MyEngin` (master) を固定参照し、`cache\GameLogic.dll` は API 版が合わないとロードされない (M95 基点以前から v30 のままで M94 以降ロードされていなかった = 別件、台帳に記録)。worktree のエンジンでは構造上通せない (ABI を上げる sub-01 では必ず不一致) ので、**三校 verify は司会が波の統合時に master で三校の DLL を作り直してから回す**。各サブの worktree で三校を回した結果は SELF_EVAL に貼ってよいが、DLL 不一致 (`API version mismatch` / `GameLogic.dll was not loaded`) が原因の FAIL は環境要因として採点に含めない。coder は三校を回さなくてよい (回すなら shot.log の `[dll]` 行を併記する)。下の各条件の「ST(名)」は往復中の確認手段で、OK 前の ST-full で同じ検査が全部回る。

A. ヒットストップ
1. `pauseTicks = n` の間、`stateTimeTicks` / `statePhase` / `transitionTick` / `stateEntered` が変わらず、駆動 SkinnedMesh の `poseLayers[i].timeQ` が保たれ `stepQ == 0`、`poseClaim == 1`。n tick 後に自動再開し、停止の前後で時刻の差は進めた tick 数に等しい — ST(animatorcontroller)
2. 停止中は `rootMotionVelocity == 0`、`rootMotionDeltaRotation == 恒等`、`applyRootMotion` の CC の `moveInput.xz == 0`、LocalTransform 適用先の位置が不変 — ST(animatorcontroller)
3. 停止中はイベントが 1 件も発火しない (停止をまたぐ位置のイベントは再開後の進みで発火する) — ST(animatorcontroller)
4. ABI: `AnimatorSetPause` は上書き (5 → 2 で残り 2)、負は 0、Animator 無しは 0 を返す。`AnimatorGetPause` は残り tick、Animator 無しは -1 — ST(part) (`PartSelfTest` の ABI 様式、`MYE_API_VERSION == 32u` と v32 スロットの充填チェック)
5. `pauseTicks == 0` の既存シーン: RV 全 PASS、三校 PASS — RV / 三校
6. `Interop.cs` の関数ポインタ順が `EngineAPI.h` と一致 (memory `abi-bump-verification` の機械照合) — 照合スクリプトの出力を SELF_EVAL に貼る

B. CC 同士の接触と接触イベント
7. `collideCharacters` 両 true、`pushPriority` 同値の CC 2 体が**接した状態から**歩み寄ると、両者が相手の tick 頭のカプセルから押し出され、tick 末に重ならない (深さ 0 以下)。隙間のある所から歩み寄る場合は、相手の tick 頭の姿勢を見る構造上、重なりが最大 1 歩 (速度 × dt) ぶん残る tick があり得るが、すり抜けない (ADR-034 影響欄。sub-02 VERDICT round 1 で明記) — ST(physics)
8. `pushPriority` 1 の CC が 0 の CC へ歩くと、0 側だけが動き 1 側は歩いた距離そのまま — ST(physics)
9. 片方だけ `collideCharacters` なら素通り (従来) — ST(physics)
10. `reportContacts` の CC が床 (静的) / kinematic 剛体 / 動的剛体 / 別の CC に触れた tick に `SolidContact` (impulse 0、法線は大→小、key 昇順) が出て、`CollisionSystem` 経由で両エンティティの `onCollisionEnter` → `onCollisionStay` → 離れた tick に `onCollisionExit` が届く — ST(physics) (既存の `CollisionSystem` を直接回すテスト `PhysicsSelfTest.cpp:511 / :552 / :1207` の様式)
11. CC + ソリッド Collider と動的剛体の接触は `SolidContact` が 1 件 (剛体側、impulse > 0) で二重配送されない — ST(physics)
12. `collideCharacters` / `reportContacts` が全て false のシーン: RV 全 PASS、三校 PASS — RV / 三校
13. `docs\adr\ADR-034-*.md` があり、`docs\dogfooding.md` 17 が「修正済み (M95)」— 目視 (reviewer)

C. 部位への重なり・掃引
14. `OverlapSphereParts`: 球と重なる部位を index 昇順で返す。tag / root の絞り込み、非アクティブ・縮退の除外、`radius <= 0` は 0 件、総数と cap の規約 — ST(part)
15. `SphereCastParts`: sphere 部位・box 部位それぞれにヒットし `distance` / `point` / `normal` が解析値と一致 (box は 1e-3 以内)、同距離は低 index、`radius <= 0` は `RaycastParts` と同じ結果、`maxDist` を超えると無し — ST(part)
16. ABI 3 本が C++ 糖衣 / C# / `MyeScript.cs` から呼べ、`PartSelfTest` の充填チェックに載る (v32 = 193 スロット) — ST(part) + #6 の照合

D. 遷移の割り込み
17. `canInterrupt = 1` の遷移が遷移中に採用され、`transitionTick*2 >= duration` なら遷移先へ、未満なら元へ確定してから新遷移が始まる (確定後の `currentState` / `stateTimeTicks` / `statePhase` の値を断言)。`hasExitTime = 1` の `canInterrupt` 遷移は遷移中に採用されない。`from` が `transitionTo` の遷移も採用される — ST(animatorcontroller)
18. `canInterrupt = 0` だけのアセットは遷移中に遷移を評価しない (既存テスト全 PASS)。v2 ファイルの読み (`canInterrupt` 無し = 0)、v3 で書き戻して再読 — ST(animatorcontroller)、RV
19. 遷移中の `AnimatorPlay(duration > 0)` が #17 と同じ確定規則に従う — ST(animatorcontroller)
20. 遷移リストに「割り込み可」チェックが出て保存される — 目視 (test_checklists)

E. ルートモーションの縦成分
21. `rootMotionY = 1` のステートで `rootMotionVelocity.y` がルートの縦速度 (クリップの y 差分 × 60) になり、`rootMotionY = 0` なら従来どおり 0。既存の水平・ヨーのテストは不変 — ST(animatorcontroller)
22. `StripRootMotion` が `rootMotionY` の層の縦だけ抜く (縦ありの層と無しの層を混ぜたとき、抜く量が縦あり層の寄与 × ratio に等しい) — ST(skeleton または animatorcontroller)
23. CC: `useVerticalInput = true, verticalInput = v` の tick は `velocity.y == v` (重力が足されない)、消費されて `useVerticalInput == false`、床に向かう負の v は接地で 0、`jumpSpeed` は従来どおり後段で勝つ — ST(physics)
24. applyRootMotion + CC で `rootMotionY` のステートを再生すると CC が縦に動き、クリップ終端で重力へ戻る。LocalTransform 適用先は y も足す。NavMeshAgent が握る間は従来どおり — ST(animatorcontroller)
25. `rootMotionY` が無いシーン: RV 全 PASS、三校 PASS — RV / 三校

F. イベント時刻の秒・フレーム指定
26. 読み: `tick` 優先、`time: 0.5` → 30、`time: 0.0083` → 0 (llround)、`frame: 7` + クリップ `fps: 30` → 14、`frame: 1` + `fps: 24` → 3 (2.5 を切り上げ)、`fps` 無し → 60、`fps: 0` → 警告して 60、配列形は従来どおり — ST(animatorcontroller)
27. `ToJson` は `tick` 配列で書き、再読で同じ tick。既存の v2 ファイルの round-trip が不変 — ST(animatorcontroller)
28. イベント帯の tick スライダに秒が併記される — 目視 (test_checklists)

G. 文書
29. `engine_spec.md` に M95 の節 (A〜F の契約と非対応)、§13 の ADR 一覧に ADR-034、`docs\test_checklists.md` に M95 節 (20 / 28 と実機の手動項目)、`docs\dogfooding.md` 17 の解決 — 目視 (reviewer)。文書の参照先が実在する
30. 全サブ完了後: ST-full (Debug / Release)、CR、RV 全ジョブ、三校 verify.bat が PASS — **Wave 2 統合時に司会が付録 B の 4. で回した結果を証拠とする** (ユーザー判断 2026-11-04、補足 3)。sub-07 (文書のみ) では full 検証を回さず、文書の参照先の実在と CR だけを確認する (AGENTS.md §7「文書のみの変更ではビルドは原則不要」)。台帳に Wave 2 統合の検証結果 (各 total 行・RV の全ジョブ・三校) が記録されていることが条件。reviewer が Phase 3 で自分の裁量で回すのは妨げない

H. round 2 で追加 (ユーザー判断)
31. `OverlapCapsuleParts`: 両端点と半径のカプセルと重なる部位を index 昇順で返す (球・箱の部位それぞれにヒット、軸が斜めのカプセルで箱の角に当たる / 当たらないの境界、`p0 == p1` は球と同じ結果、`radius <= 0` は 0 件、総数と cap の規約)。ABI 経由でも同じ — ST(part)
32. 非 kinematic Rigidbody: `useVerticalInput = true, verticalInput = v` (linearDamping 0) の tick は物理後に `velocity.y == v` (重力が足されない、x/z は従来どおり)、消費されて `useVerticalInput == false`、次 tick は重力が戻る。kinematic には何も書かれない。applyRootMotion + Rigidbody で `rootMotionY` のステートを再生すると `velocity.y` にルートの縦速度が入り、`isSleeping == false` — ST(physics) + ST(animatorcontroller)
33. sub-00: `MYE_SELFTEST_ONLY` 未設定の全走が、変更前と同じスイートを同じ順で回す (変更前の `EditorMain.cpp` の `Run*SelfTest` 列と登録表の名前列の diff が空) — 列の抽出コマンドと diff の出力を SELF_EVAL に貼る
34. sub-00: `MYE_SELFTEST_ONLY=physics` で physics だけ、`=part,physics` で表の順 (physics → part) に 2 件、`=m94` / `=twod` / `=computescript` / `=servernet-v1` が従来と同じ集合、`=nosuch` が exit 2 と一覧、`=m95` が §4.0 の 7 件 — 各実行のログ (スイート行) を SELF_EVAL に貼る
35. sub-00: `--selftest-list` が名前と別名を出して exit 0。ウィンドウを出さない — 出力を貼る
36. sub-00: 各スイート行に `(<ms> ms, <tier>)`、最後に total 行。製品コード (`--selftest*` 以外の経路) に差分が無い (`git diff --stat` が `EditorMain.cpp` の selftest 節 / `SelfTest\SelfTestSuites.*` / プロジェクトファイル / `ci.yml` / `AGENTS.md` / 予約マーカーのコメント行だけ) — diff の stat を貼る。RV PASS (マーカーはコメントなので当然だが、ビルドが通ることの確認)
37. sub-00: `--selftest-full` と `MYE_SELFTEST_FULL=1` が全スイートを表の順に回し (sub-00 時点では `--selftest` と同じ集合)、`MYE_SELFTEST_ONLY` がそれより優先される。`.github\workflows\ci.yml` の selftest 2 ステップが `--selftest-full` になっている — ログと diff を貼る
38. sub-00b: Debug / Release の全スイートの所要時間表 (各 2 回) が `sub-00b.md` の実装メモにあり、`Full` に移したスイートが「Debug の所要時間の降順で上位 N 本、N は既定層が 60 秒以内に収まる最小」の規則で選ばれている (表から追える)。縮小を選んだスイートは採否と理由がある — 表を貼る
39. sub-00b: 既定層 `bin\x64\Debug\Editor.exe --selftest` の total が **60 秒以内** (sub-00b の実装メモの計測と同じ PC で、2 回とも) — total 行を貼る
40. sub-00b: `--selftest-full` (Debug / Release) で回るスイートの集合と順が sub-00 着手前の全走と同じ (登録表の名前列の diff が空) で全 PASS = 検査を 1 本も落としていない。縮小したスイートは縮小前の値でも 1 回 PASS させた記録がある — 名前列の diff と PASS ログを貼る
41. sub-00b: `--selftest-list` の `fast` 件数 + `full` 件数 = 登録表の本数 — 出力と件数を貼る

## 6. サブ分割

| サブ | 波 | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|---|
| sub-00 | Wave 0 | selftest のスイート登録表 (tier 属性)・`MYE_SELFTEST_ONLY` の汎用化・所要時間ログ・`--selftest-full` の入口・CI・予約マーカー | なし | 33, 34, 35, 36, 37 | `M95 準備: selftest のスイート登録表 (tier、MYE_SELFTEST_ONLY のカンマ区切り、--selftest-full / --selftest-list、所要時間ログ、CI は full) と並列実装の予約マーカー` |
| sub-00b | Wave 1 | 計測に基づく selftest の層分けと縮小 (既定層 60 秒以内) | sub-00 | 38, 39, 40, 41 | `M95 準備 (2): 所要時間の計測に基づいて selftest を fast / full に分ける (既定の --selftest を 60 秒以内に)` |
| sub-01 | Wave 1 | ヒットストップ — Animator の tick 単位停止 (ABI v32、snapshot v66) | sub-00 | 1, 2, 3, 4, 5, 6 | `M95a: Animator のヒットストップ (pauseTicks、ABI v32 AnimatorSetPause / AnimatorGetPause、snapshot v66)` |
| sub-02 | Wave 1 | CC 同士の押し出しと接触イベント (snapshot v67、ADR-034) | sub-00 | 7, 8, 9, 10, 11, 12, 13 | `M95b: CC 同士の押し出しと接触イベント (collideCharacters / pushPriority / reportContacts、snapshot v67、ADR-034、dogfooding 17)` |
| sub-04 | Wave 1 | 遷移の割り込み (controller v3 canInterrupt、AnimatorPlay の確定) | sub-00 | 17, 18, 19, 20 | `M95d: 遷移の割り込み (canInterrupt、近い側へ確定、controller v3)` |
| sub-06 | Wave 1 | イベント時刻の秒・フレーム指定 | sub-00 | 26, 27, 28 | `M95f: アニメイベントの time / frame / fps 指定 (読み込み時に tick へ丸める)` |
| sub-03 | Wave 2 | 部位への球の重なり・カプセルの重なり・球の掃引 (ABI v32 の残り 3 本 = 193) | sub-01 (ABI スロット順)、Wave 1 統合 | 14, 15, 16, 31 | `M95c: 部位への球 / カプセルの重なりと球の掃引 (OverlapSphereParts / SphereCastParts / OverlapCapsuleParts、ABI v32 = 193 スロット)` |
| sub-05 | Wave 2 | ルートモーションの縦成分 (ステート rootMotionY、CC / Rigidbody の verticalInput、snapshot v68) | sub-02, sub-04、Wave 1 統合 | 21, 22, 23, 24, 25, 32 | `M95e: ルートモーションの縦成分 (ステートの rootMotionY、CC / Rigidbody の verticalInput、snapshot v68)` |
| sub-07 | Wave 3 | 文書 (engine_spec / ADR 索引 / dogfooding / test_checklists) + 最終統合確認 | sub-01〜06、Wave 2 統合 | 29, 30 | `M95g: 文書 (engine_spec の M95 節、ADR-034 の索引、test_checklists)` |

波の根拠:
- sub-00 は「登録表 + フィルタ + ms ログ + full の入口 + CI + マーカー」に絞り、計測を要する層分けは sub-00b に割った (Wave 0 を短くして Wave 1 を早く切る。sub-00b は `SelfTestSuites.cpp` の `tier` 列と縮小するスイートの中身だけを触るので Wave 1 の 4 本と衝突しない。縮小対象が M95 の触るスイート (physics / animatorcontroller / part) に当たる場合は「不安・質問」で planner へ)
- Wave 1 の 5 本 (sub-00b を含む) は触るファイルの重なりが「同じファイルの別の場所」に限られ (AnimatorController.cpp: sub-01 は Update 冒頭、sub-04 は ToJson / FromJson の遷移と Update 1. と AnimatorPlay、sub-06 は FromJson の clipEvents。AnimatorControllerSelfTest.cpp / Components.h / Components.cpp / SimSnapshot.h は付録 A のマーカーで分離)、同時ビルド 2〜3 本の範囲に収めるため sub-03 / sub-05 を Wave 2 へ回す
- sub-03 は ABI スロットを sub-01 の後ろに積む (順を守るため直列)。sub-05 は CC の末尾 append を sub-02 の後ろに、controller v3 を sub-04 の後ろに積む
- sub-07 は全コード完了後

## 7. 未決事項・リスク

- `SphereCastWorld` の対象 1 件分を共通関数に切り出す場合、演算順を変えると既存の掃引クエリの結果が変わる。coder は共通化するなら RV の全ジョブで確認し、変えない選択も可
- CC の接触記録は固定パス × 相手数ぶんの `Collide` 結果を見るだけで追加の判定は無いが、記録用の配列確保が毎 tick 起きる。`reportContacts` が無ければ確保しない形にする
- HAL Collector に検証スクリプトが無い。ビット同一は三校 verify.bat と RV で代用 (未検証として報告)
- `useVerticalInput` を立てたまま物理が走らない (CC が Rigidbody 併用で無効 / 剛体が kinematic) ケースは印が残り続ける。害は無いが Inspector に見えるので読み取り専用にする
- Wave 1 で `SimSnapshot.h` の `kSimSnapshotVersion` 行は sub-01 と sub-02 の両方が変える = 統合で競合マーカーが出る (出るのが正しい)。付録 A の規則で解く
- 同じ波の 2 本が `tools\gen_project_files.ps1` を回すと `build\*.vcxproj` が衝突し得る (sub-00 だけが新規ファイルを足す予定。Wave 1 / 2 は新規ファイル無し。足すなら付録 A の置き場所に従い、統合時に再生成する)

## 8. 変更履歴

- 2026-11-04 sub-02 VERDICT round 1 (出所: coder SELF_EVAL): §2 #21 の「hash 不変」を「結果は不変だが WorldHash の値は変わる (焼き込み定数は再採取)」に訂正。§5 受け入れ 7 に「接した状態から」と隙間からの 1 歩ぶんの重なりの許容を明記。付録 B の 4. に焼き込み hash の再採取手順 (統合コミット前に 1 回、根拠は --selftest-full のログ) を追加
- 2026-11-04 sub-04 VERDICT round 2 (出所: 司会の切り分け): §5 冒頭に三校 verify の扱い (司会が波の統合時に master で三校 DLL を作り直してから回す。worktree での DLL 不一致 FAIL は採点外) を追記
- 2026-11-04 sub-06 VERDICT round 1 (出所: coder SELF_EVAL の「仕様との差分」): §4.1 F に「数値でない time / frame は 0」「非有限は 0、int32 範囲外は clamp」「events 無しのオブジェクト形は読み飛ばす」を明記
- 2026-11-04 補足 3 (出所: ユーザー判断、司会の AskUserQuestion): 受け入れ 30 の証拠を「Wave 2 統合時に司会が回す full 検証 (付録 B の 4.)」に代用し、sub-07 (文書のみ) は参照先の実在と check_rules だけを確認する。sub-07.md の検証コマンド節を同じ方針に変更
- 2026-11-04 sub-04 VERDICT round 1 (出所: coder SELF_EVAL の「仕様との差分」): §4.1 C の「走査に入らない形」を「走査内の先頭 continue でよい」に緩和 (結果・状態が同一で fp 演算が無いため)。§5 冒頭の 3 段の規則に「full 検証の結果が SELF_EVAL に無い round には OK を出さない」を明記
- 2026-11-04 sub-00 VERDICT round 1 (出所: coder SELF_EVAL の「仕様との差分」): §4.0 の `MYE_SELFTEST_ONLY` に「別名 1 個だけは別名の並び」「項目は小文字化」「`servernet` / `computescript` は別名表に載せない」を明記。§4.0 と sub-00 受け入れ 34 の食い違い (表の順 vs m94 の従来順) を後者で解消
- 2026-11-04 round 2 補足 2 (出所: ユーザー決定): selftest を 3 層 (関連スイート / `--selftest` = fast 層 60 秒以内 / `--selftest-full` = 全部) にする。sub-00 に tier 属性・`--selftest-full`・CI の書き換えを足し、計測に基づく層分けと縮小を sub-00b (Wave 1 並行) に分割。受け入れ 37〜41、§4.4 に「full は OK 前に必ず回す」、全サブの検証欄を 3 段に統一
- 2026-11-04 round 2 (出所: ユーザー回答、台帳「ユーザー判断」): #5 `OverlapCapsuleParts` を sub-03 に追加 (ABI v32 = 193、受け入れ 31)。#16 非 kinematic Rigidbody に縦を適用 (sub-05、受け入れ 32、snapshot v68 に Rigidbody 2 欄)。sub-00 (selftest 登録表 + 予約マーカー、受け入れ 33〜36) を先頭に追加。波単位の並列 (§6 の波、付録 A / B)。全サブの検証コマンドを「往復中は関連スイート、完走は最後 1 回」に変更

## 付録 A. 共有資源の予約表 (並列実装用。表の外の番号・場所を取らない)

### A.1 ABI (`src\Shared\EngineAPI.h` の `MyeEngineApi` 末尾、`Interop.cs` の構造体末尾、`ScriptAPI.h`、`MyeScript.cs`)

| 順 | スロット | 版 | サブ | 備考 |
|---|---|---|---|---|
| 189 | `AnimatorSetPause` | v32 | sub-01 | `MYE_API_VERSION 32` の宣言と版コメントの行は sub-01 が書く |
| 190 | `AnimatorGetPause` | v32 | sub-01 | |
| 191 | `OverlapSphereParts` | v32 | sub-03 | sub-01 の統合後に積む |
| 192 | `SphereCastParts` | v32 | sub-03 | |
| 193 | `OverlapCapsuleParts` | v32 | sub-03 | 最終スロット数 193。`PartSelfTest` の充填チェックは sub-01 が 2 本で書き、sub-03 が 5 本に増やす |

`Interop.cs` は `// ---- v32 (M95): アクション戦闘 ----` のコメント行の下に同じ順。`ScriptAPI.h` の糖衣は `MyeAnimatorPlay` の隣 (sub-01) と `MyeRaycastParts` の隣 (sub-03)。

### A.2 `Components.h` の末尾 append (各 struct の `static inline ComponentTypeId sTypeId` の直前。sub-00 がマーカーを入れる)

| struct | マーカー (sub-00 が入れるコメント行) | 書くサブ | 欄 (この順) |
|---|---|---|---|
| `AnimatorControllerComponent` | `// ---- M95 sub-01 (末尾 append) ----` | sub-01 | `int32_t pauseTicks = 0;` |
| `CharacterControllerComponent` | `// ---- M95 sub-02 (末尾 append) ----` | sub-02 | `bool collideCharacters = false; int32_t pushPriority = 0; bool reportContacts = false;` |
| `CharacterControllerComponent` | `// ---- M95 sub-05 (末尾 append) ----` (sub-02 のマーカーの下) | sub-05 | `bool useVerticalInput = false; float verticalInput = 0.0f;` |
| `RigidbodyComponent` | `// ---- M95 sub-05 (末尾 append) ----` | sub-05 | `bool useVerticalInput = false; float verticalInput = 0.0f;` |
| `SkinnedMeshComponent::PoseLayer` | `// ---- M95 sub-05 ----` (PoseLayer の最後の欄の下) | sub-05 | `int32_t rootMotionY = 0;` |

`Components.cpp` の反射登録も同じ struct の登録の末尾 (`});` の直前) に同名のマーカー。Rigidbody / Animator / CC の 3 箇所 (PoseLayer は未登録のまま)。

### A.3 `kSimSnapshotVersion` (`src\Engine\Engine\Replay\SimSnapshot.h:175`)

| 版 | サブ | 版コメント行 (`// v66 (M95a): ...` の形) |
|---|---|---|
| 66 | sub-01 | `AnimatorControllerComponent` 末尾に `pauseTicks` |
| 67 | sub-02 | `CharacterControllerComponent` 末尾に `collideCharacters` / `pushPriority` / `reportContacts` |
| 68 | sub-05 | CC 末尾に `useVerticalInput` / `verticalInput`、`RigidbodyComponent` 末尾に同 2 欄、`PoseLayer` 末尾に `rootMotionY` |

規則: 各サブは**自分の版**を `kSimSnapshotVersion` に書き、自分の版コメント行を**既存の最後の行の次**に足す。同じ波の統合で同じ行が競合したら、定数は**最大値**、コメント行は**版の昇順に全部残す**。Wave 1 統合後の値は 67、Wave 2 統合後は 68。各サブの selftest (`RunSimSnapshotSelfTest`) は自分の worktree の版で通ればよい。

### A.4 controller アセット版 / ADR / TypeId

| 資源 | 値 | 書くサブ |
|---|---|---|
| `root["controller"]` (`AnimatorController.cpp:964`) | 3 | sub-04 が 3 にして版コメントを書く。sub-05 はコメントに `rootMotionY` を足すだけ |
| ADR | `docs\adr\ADR-034-character-contacts.md` (題名は coder 案可、番号は 034 固定) | sub-02 |
| TypeId | 追加なし | — |

### A.5 selftest (sub-00 以降)

| 資源 | 規則 |
|---|---|
| 登録表 `SelfTestSuites.cpp` | M95 では新しいスイート関数を**足さない** (全て既存スイートに入れる)。足す必要が出たら表の末尾に `// ---- M95 ---- ` マーカーの下へ、サブ番号順。**`tier` 列を書き換えるのは sub-00b だけ** (Wave 1 の他サブは触らない) |
| `AnimatorControllerSelfTest.cpp` | sub-00 が `RunAnimatorControllerSelfTest` の本体末尾 (`return` の前) に 4 行のマーカー `// ---- M95 sub-01 ----` / `sub-04` / `sub-05` / `sub-06` をこの順で入れる。各サブは自分のマーカーの直下に書く |
| `PhysicsSelfTest.cpp` | 同様に `// ---- M95 sub-02 ----` / `sub-05` |
| `PartSelfTest.cpp` | 同様に `// ---- M95 sub-01 (ABI) ----` / `sub-03` |
| `SkeletonSelfTest.cpp` | `// ---- M95 sub-05 ----` |
| グループ `m95` | `physics,animatorcontroller,skeleton,part,twodabi,simsnapshot,worldhasher` |

### A.6 ローカライズ (`src\Engine\Core\Localization\LocalizationTable.inl`)

sub-00 が末尾に `// ---- M95 ----` マーカーを入れ、その下にサブ番号順で足す。接頭辞:

| サブ | StrId |
|---|---|
| sub-04 | `Anim_CanInterrupt` ("canInterrupt" / "割り込み可") |
| sub-05 | `Anim_RootMotionY` (ステート編集 UI に足す場合のみ。"rootMotionY" / "縦のルートモーション") |
| sub-06 | 無し (秒の併記は単位だけなので翻訳不要。足すなら `AnimPrev_EvSeconds`) |

### A.7 新規ファイルの置き場所

| サブ | ファイル |
|---|---|
| sub-00 | `src\Editor\SelfTest\SelfTestSuites.h / .cpp` (`gen_project_files.ps1` を回す) |
| sub-02 | `docs\adr\ADR-034-character-contacts.md` |
| その他 | 新規ファイル無し。必要になったら「不安・質問」で planner へ |

### A.8 その他の共有行

| 行 | サブ | 規則 |
|---|---|---|
| `EditorMain.cpp` の selftest 節 / `.github\workflows\ci.yml` | sub-00 だけ | 以降のサブは触らない |
| `AGENTS.md` §7 | sub-00 (フラグの行)、sub-00b (60 秒の目安と層の説明を 1 行) | sub-07 は確認のみ |
| 縮小するスイートの中身 (sub-00b) | sub-00b だけ | M95 のテストを足すスイート (physics / animatorcontroller / part / skeleton) のパラメータを縮める場合は planner へ確認してから |
| `docs\dogfooding.md` / `docs\adr\` | sub-02 だけ (Wave 1) | sub-07 は索引と表現の確認 |
| `engine_spec.md` / `docs\test_checklists.md` | sub-07 だけ | Wave 1 / 2 は触らない |
| `AnimatorController.cpp` の `ToJson` / `FromJson` | sub-04 (遷移)、sub-06 (clipEvents)、sub-05 (ステートの skel) | それぞれ別のブロック。版コメント行は sub-04 が書き sub-05 が追記 |
| `AnimatorController.cpp` の `Update` | sub-01 (冒頭の停止分岐)、sub-04 (1. 遷移チェック)、sub-05 (6. / 8. ルートモーション) | 別の番号付きブロック。同じブロックを 2 サブが触らない |

## 付録 B. 波の統合手順 (司会が行う。sub-07 の最終確認でも同じ目視を行う)

1. 各 worktree で `git status` が clean (コミット済み) で、master のチェックアウト (`C:\HAL\MyEngin`) に未コミット差分が無いことを確認する
2. 波のサブを**サブ番号順**に master へ merge する (`git merge --no-ff`)。競合マーカーが出たら付録 A の規則で解く (`kSimSnapshotVersion` は最大値 + コメント行は全部)
3. **競合が出なかったファイルほど疑う**。以下を目視し、各サブの欄が自分の struct / 表に入っていることを確認する:
   - `Components.h`: `AnimatorControllerComponent` / `CharacterControllerComponent` / `RigidbodyComponent` / `SkinnedMeshComponent::PoseLayer` のメンバー一覧 (マーカーと欄の対応)
   - `Components.cpp`: 同 3 struct の反射登録の末尾
   - `EngineAPI.h` の `MyeEngineApi` 末尾 5 本の順と `Interop.cs` の末尾の順 (memory `abi-bump-verification` の機械照合)
   - `SimSnapshot.h` の定数と版コメント行
   - `AnimatorController.cpp` の `Update` の番号付きブロック (1. / 6. / 8. と冒頭) が入れ子崩れしていない
   - `AnimatorControllerSelfTest.cpp` / `PhysicsSelfTest.cpp` / `PartSelfTest.cpp` のマーカーの下に各サブのテストがある
   - `LocalizationTable.inl` の `// ---- M95 ----` の下
4. `tools\gen_project_files.ps1` (新規ファイルがあった波だけ) → Debug / Release ビルド → **焼き込み hash の再採取** (下記) → `Editor.exe --selftest-full` (Debug / Release) → `tools\check_rules.ps1` → `tools\replay_verify.bat` → 三校の DLL を master で作り直して `verify.bat`
   - 焼き込み hash の再採取 (hash 対象の struct に欄を足した波 = Wave 1 (sub-01 / sub-02)、Wave 2 (sub-05) で必要。統合コミットの**前**に 1 回): 統合後のソースで Debug ビルド → `--selftest-full` (Debug) を 1 回回し、FAIL したスイートのログに出る実測値で定数を書き換える。対象は `PhysicsSelfTest.cpp` (CC2D)、`NavAgentSelfTest.cpp` (庭 `kExpectedYardHash`)、`TilemapSelfTest.cpp` (`kTileScenarioExpectedHash`)、`GridNav2DSelfTest.cpp` (`kExpectedScenarioHash`)、`TwoDDemoSelfTest.cpp` (`kExpectedHash`) の 5 件と `AcousticAudioSelfTest.cpp` の `kSimSnapshotVersion == N` (N は統合後の最大値)。各サブが自分の worktree で再採取した値は統合で競合する (出るのが正しい) ので、統合後の実測値で上書きする。値の根拠 = `--selftest-full` のログ行を台帳に貼る。Release の `--selftest-full` が同じ値で PASS することが「Debug / Release の一致」の確認 (値を Release から取らない)
   ★ Wave 2 の統合では、この 4. の結果 (各 total 行・check_rules の error 数・RV の全ジョブ・三校) を台帳に行単位で残す — 受け入れ 30 の証拠として sub-07 と reviewer が参照する (補足 3)
5. 統合コミット (`M95 統合: Wave N (sub-xx, sub-yy)`) を作り、台帳に記録。次の波の worktree はこの統合コミットから切る (`git worktree move` + `checkout -B` で obj キャッシュを使い回してよい)
