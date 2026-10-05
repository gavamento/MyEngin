# AI ロードマップ M83〜M86 (知覚 → NavMesh 拡張 → BT → Smart Objects)

- 承認: 2026-10-05 (プランモード、ユーザー承認)。原本は `C:\Users\akita\.claude\plans\mellow-splashing-floyd.md`、ここはその写しと進捗
- 進め方: harness は使わず、メインセッションで直接実装する (2026-10-05 ユーザー指示「ハーネス使わずに」)。1 マイルストーン = 数コミット (M83a: …)

## 進捗

再開手順: この表の最初の未着手から始める。着手時に `Components.cpp` の末尾 TypeId (M83 後 77) と `EngineAPI.h` の版 (M83 後 v25 = 144)、`kSimSnapshotVersion` (M83 後 30) を確認する。

| マイルストーン | 状態 | コミット | メモ |
|---|---|---|---|
| M83 知覚 | 完了 (2026-10-05) | ade5d68 | ADR-024。ABI v25 = 144、TypeId 76 AIPerception / 77 AIStimulusSource、kSimSnapshotVersion 30 |
| M84 NavMesh 拡張 | 着手 (2026-10-05) | | 下の「M84 で決めたこと」 |
| M85 ビヘイビアツリー | 未着手 | | |
| M86 Smart Objects | 未着手 | | |

### M83 で計画から変えたこと

- ReportNoise は Distance モード専用。Acoustic モードへは流さない (ADR-024 決定 5)
- 結果も報告の保留欄もコンポーネントに置き、SimSnapshot の節を足さない (ADR-024 決定 2)

### M84 で決めたこと

- **Surface の接続は UE 式** (2026-10-05 ユーザー回答)。計画の「Unity と同じ」は誤り: Unity は別の Surface を自動でつながず、NavMesh Link を要求する。UE は Agent ごとにナビメッシュを 1 つ持ち、複数の NavMeshBoundsVolume の範囲を合わせて焼く。M84-B では、同じ Agent Type の Surface をすべて範囲の指定として扱い、それらを合わせた 1 つのナビメッシュとして焼く (Bake は Agent Type 単位、同じ型の Surface は同じ .mnav を共有する)
- M84b の細部: グループ = 同じ agentTypeId の有効な Surface (`NavCollectSurfaceGroups`)。leader (エンティティキー最小) のセル・タイル・エリアのコストと navAsset をグループ全体に使い、実行時に読み込むのも leader だけ (Nav 節の書式は不変)。ベイクの範囲は全 Surface を合わせた AABB、歩行面は Surface の箱の中だけに切り詰める (`kNavBakeVersion` 2。単独の Surface でも端のタイルがはみ出さなくなった)。三角形は Surface ごとの collectLayerMask で集め、1 コライダー 1 回。.mnav の名前は Agent Type の名前。ADR-023 の決定 (Surface ごとに 1 つの dtNavMesh) の改訂は文書のサブでまとめて書く
- M84c の細部: `.navfilter.json` はエリアごとの「コストの上書き (0 = Surface のまま、1..1000)」と「通らない」のビット。計画の「コスト倍率」ではなく Unity / UE と同じ上書きにした。資産の扱いは `.physmat.json` と同じ (NavFilterLibrary を GUID で引く、起動走査・ReloadHub・Inspector 編集、中身はワールドハッシュに入れず provenance の contentHash が守る)。Agent に `navFilter` (AssetRef) を末尾追加して kSimSnapshotVersion 31。dtCrowd のフィルタは (areaMask, navFilter) の組ごと (16 を超えたら最後を共有)。クエリ 4 種は navFilter 引数を持つが、ABI からは 0 を渡す (ABI に足すのは M84d の bump でまとめて)
- M84d1 の細部 (2026-10-05 ユーザー回答 2 点): updatePosition = false は「moveInput を書かず、crowd は毎 tick 実位置から取り直す」(Unity のように nextPosition が離れたまま進むことはしない)。avoidancePriority は Unity の「高い側は無視」ではなく分担の重み付け (Recast パッチ 6)。isStopped は最高速度 0 + 回避のサンプリングを外す (vmax 0 で 1/vmax が FLT_MAX になるため)。Warp は一度きりのフィールドではなく `NavSystem::Warp` (その場で Transform と crowd を置き直す)。kSimSnapshotVersion 32。CalculatePath / SetPath と ABI v26 (クエリの navFilter 引数を含む) は M84d2
- M84a の Agent Type の表は `project_settings.json` の `navAgentTypes` (エディタ専用、`src\Editor\Project\NavAgentTypes.h`)。sim は Surface に写した寸法だけを見る。型を選ぶと写し、Project Settings 側の変更は Bake の時に写す (Inspector で食い違いを警告)。Agent の radius / height は Agent 自身の値のまま (Unity と同じ)

---

# AI 基盤の続き: 知覚 (M83) → NavMesh 拡張 (M84) → ビヘイビアツリー (M85) → Smart Objects (M86)

## Context

M82 で NavMesh を作った (Recast、Surface / Agent / Obstacle / Modifier / Link、ABI v24)。
ユーザーの次の要望 (2026-10-05) は 3 つ。

1. キャラクターごとに「どこまで見えるか」「どんな音が聞こえるか」を簡単に設定したい (知覚)
2. NavMesh で動くキャラの行動ルーチンを、UE / Unity のようなビヘイビアツリーで組みたい
3. ほかにも Unity / UE にある NavMesh の機能を足したい

事前計画 `C:\Users\akita\.claude\plans\imperative-scribbling-shore.md` で M-B (知覚) と M-C (BT) は概要と BT のノード一覧を決めてある。
この計画はそれを現在のコード (M82 後) に合わせて具体化し、3 つ目の NavMesh 拡張を加える。

**進め方**: どれもマイルストーン規模なので `/harness` で 1 本ずつ回す。この計画の該当節を `plans\<slug>\design-draft.md` として planner に渡す (出発点。planner の裁定で変えてよい)。
順番は M83 → M84 → M85 → M86 の直列にする。M83 と M84 はどちらも TypeId・`kSimSnapshotVersion`・ABI 版を動かすので、並行させると番号が衝突する。BT は知覚と NavMesh 拡張の両方の上に乗り、Smart Objects は BT のタスクから使う。合計は約 32 サブ。

## 確定した判断

| 項目 | 決定 | 出所 |
|---|---|---|
| 陣営 | AIPerception 系に専用の `faction` と、敵対・友好のマスクを持たせる (UE の Team / Attitude 相当)。TagComponent では代用しない | 2026-10-05 回答 |
| 聴覚 | 距離ベースが基本。AcousticVolume があるシーンでは音響伝播モードも選べる | 事前計画 |
| BT ノード | Selector / Sequence / Parallel、Decorator 5 種 + Abort、Task 5 種、AI 4 種、PlayAnimation / SendEvent、SubTree、C++ / C# Task | 事前計画 (2026-10-03 指定) |
| 既存 AgentBrain | 残して共存する | 事前計画 |
| NavMesh の追加機能 | Agent の細かい制御 / Agent・クエリごとのエリアコスト / Link の自動生成 / Agent Type の一元管理と Surface の接続 | 2026-10-05 回答 |
| 入れないもの | Obstacle の強化、Smart Link、実行時の再ベイク、ベイク入力の選び方。AI ツール 4 種 (AI デバッガ / EQS / 経路テスト / Nav Invoker) と BT の Service ノード、StateTree。どれも 2 回聞いて選ばれなかった | 2026-10-05 回答 |
| 知覚の追加感覚 | ダメージ、接触、予測 (UE の AISense_Damage / Touch / Prediction) | 2026-10-05 追加回答 (「まだ足りない」) |
| 行動の追加 | 巡回ルート (M85 に入れる)、Smart Objects (M86 として新設) | 2026-10-05 追加回答 |

## 番号の予約状況 (先着順)

- TypeId の末尾は 75 (NavMeshLink、`src\Engine\Core\Ecs\Components.cpp:1476`)。M75h の InputField は 76 に「予定」とあるが未登録。新しい型は**登録コミットの時点の末尾**から振る。
- `kSimSnapshotVersion = 29` (`src\Engine\Engine\Replay\SimSnapshot.h:112`)。上げるときは `AcousticAudioSelfTest.cpp:129` の `== 29` も直す。
- ABI は `MYE_API_VERSION 24` = 139 スロット (`src\Shared\EngineAPI.h:41`)。M75h が v25 を予定している。**各マイルストーンで bump は 1 回**にし、着手時点の次の番号を使う。外部プロジェクトの GameLogic.dll は bump のたびに再ビルドが要る。

---

## M83 知覚 (AIPerception) — 約 6 サブ

### 作るもの (UE の AIPerception / Unity の一般的なセンサーの形)

**`AIPerceptionComponent`** (見る側。Add Component で付ける)
- 視覚: `sightRadius`、`loseSightRadius` (見失う距離。sightRadius 以上)、`fovDeg` (視野角)、`eyeHeight`、`autoSuccessRange` (この距離以内は角度・遮蔽に関係なく見える)、`losLayerMask` (視線を遮るレイヤー)
- 聴覚: `hearingRange`、`hearingThreshold`、`hearingMode` (Distance / Acoustic)
- 陣営: `faction` (0..31)、`detectMask` (どの陣営を知覚するか。既定は敵対だけ)
- 記憶: `forgetTicks` (最後に知覚してから忘れるまでの tick)
- 結果 (読み取り専用、固定 8 スロット): 対象 entity、感覚の種類、`lastSensedPos`、`lastSensedTick`、強さ、今見えているか

**`AIStimulusSourceComponent`** (見られる側)
- `faction`、見られる点の高さ、視覚と聴覚それぞれの「知覚される」可否
- 付いていない entity は視覚の対象外。プレイヤーにも付ける

**ノイズ**
- スクリプト / ABI の `ReportNoise(pos, loudness, range, instigator)` で鳴らす (UE の ReportNoiseEvent)。足音や衝撃音のような既存の音響イベントも、Acoustic モードでは聞こえる
- Distance モード: 鳴った tick に距離の減衰で判定する。ノイズの列はその tick の中で消費して空にするので、tick 境界に残らず、スナップショットは不要
- Acoustic モード: `AcousticField::Emit` で波を立て、同じ entity の `AcousticListenerComponent` の鏡 (`lastHeardTick` / `lastHeardPos` / `lastLoudness` / `lastSourceEntity`、`Components.h:1574-1590`) を読む。壁の回り込みと遮蔽は音響に任せる

**追加の感覚 (UE の AISense と同じ考え方)**
- **ダメージ**: エンジンに汎用のダメージ処理は無い (`damage` は破壊物理の蓄積値だけ、`Components.h:1860`)。そこで UE の `ReportDamageEvent` と同じく、ゲーム側が ABI の `ReportDamage(victim, instigator, amount, hitPos)` で知らせる。見えていなくても攻撃者を知覚し、攻撃者の位置を記録する
- **接触**: 物理の接触ペア (前の tick の solidContacts。音響の `DrainImpacts` と同じ入力) から、AIPerception を持つ entity に触れた相手を知覚する。トリガーは含めるかどうかを planner が決める
- **予測**: 知覚した相手の速度を記録し、見失った後は `lastSensedPos + 速度 × predictionTicks` を予測位置として結果に出す。BT の SearchArea がこれを捜索の起点に使う
- それぞれの感覚に on/off と記憶の長さを持たせる。結果のスロットには「どの感覚で知ったか」のビットを持たせる

### 設計の要点
- **Tick の位置**: TickRunner のフェーズ 3.4 (音響 + AgentSystem) の後、3.4b (NavSystem) の前に、独立したゲートで新設する (3.4 は `ts.acoustic` が無いと走らないので相乗りしない。前例は 3.4b、`TickRunner.cpp:388-395`)。Acoustic モードは同じ tick に配られた鏡を読める
- **前方** = その entity の WorldMatrix の +Z (前の tick の行列。`RaycastWorld` と同じ基準)。AgentSystem は向きを回さないが、NavMeshAgent は `angularSpeedDeg` で回すので問題ない
- **視線**: `RaycastWorld` (`src\Engine\Engine\Physics\Rigid\PhysicsSystem.h:201`) で目の高さから相手の点へレイを撃つ。呼ぶたびにコライダーを全部集め直すので、(a) 距離と角度で絞った後だけ撃つ (b) 1 tick に撃つ本数の上限を**件数で**決める (時間で決めると決定論が崩れる)。上限を超えた分は entity キー順に次の tick へ回す。必要なら、1 回の収集で複数のレイを撃つ `RaycastWorldBatch` を足す (計測してから決める)
- **順序**: 見る側も対象も entity キー (index、同値は generation) 順。スロットが溢れたら「強さ → 距離 → entity キー」で決める
- **存在ゲート**: AIPerception が 0 個なら RNG・ハッシュ・スナップショットのどれも変えない (AgentSystem の作法、`AgentSystem.cpp:145-185`)
- **結果は固定長の POD** なので World 節にそのまま入る。`kSimSnapshotVersion` を上げる
- **既存の AI と**: AgentBrain は変えない (LightSeeker と AcousticListener のまま)。後で BT が AIPerception を読む

### エディタとデバッグ
- SceneView のギズモ (選択中だけ): 視野の扇形 (sightRadius と loseSightRadius の 2 本の弧)、聴覚の円、目の位置。扇形と円のヘルパは無いので、NavMeshObstacle の円柱 (`SceneViewWindow.cpp:878-890`) と同じく `AddLine` のループで書き、`gizmo` 名前空間に定数を置く
- 実行時のデバッグ線: 見えている相手への緑の線、見失った最終位置の印、聞こえた位置の印。`navSystem->AppendDebugLines` と同じ形で `debugLines` に足す (`TickRunner.cpp:480-487`)
- Inspector: 今知覚している相手の一覧 (Play 中のライブ表示)
- 文字列は `LocalizationTable.inl` (en / ja)

### ABI (1 回 bump)
`PerceptionGetCount` / `PerceptionGet` (POD の配列を呼び出し側バッファへ) / `ReportNoise` / `ReportDamage` / `PerceptionCanSee` (2 entity 間の即時判定)。C# ミラー (`src\Scripting\Interop.cs`、`MyeScript.cs`)、`check_rules.ps1` の版表。

### サブ分割案
1. コンポーネント 2 種 + 視覚 (距離・角度・LOS・陣営・記憶) + SelfTest + ギズモ
2. 聴覚 Distance モード + `ReportNoise` + 記憶の統合
3. 聴覚 Acoustic モード (AcousticListener の鏡の取り込み)
4. ダメージ・接触・予測の感覚
5. ABI + C# + デモシーン (`--perception-demo`) + replay_verify のジョブ + golden
6. 文書 (ADR-024、engine_spec、feature guide) と全体検証

---

## M84 NavMesh 拡張 — 約 7 サブ

| # | 機能 | 中身 | 主な論点 |
|---|---|---|---|
| A | **Agent Type の一元管理** | Project Settings に Agent Type (名前・半径・高さ・段差・傾斜) を定義し、Surface と Agent はそれを名前で選ぶ (Unity の Agent Types / UE の Supported Agents)。`agentTypeId` は残す | project_settings はリプレイの外にある (M82 spec 2. #10)。ベイク寸法は Surface と .mnav に**写して**持ち、sim は写した値だけを見る。設定を変えたら、該当する Surface に「再ベイクが必要」と出す |
| B | **Surface の接続** | 同じ Agent Type の Surface を 1 つの dtNavMesh にまとめ、Surface をまたいで経路が引けるようにする (Unity と同じ)。M82 の「同じ型の Surface が複数あれば index 最小が勝つ」(spec 2. #11) を置き換える | Agent Type ごとにタイルの格子 (原点・tileSize・cellSize) を揃える必要がある。範囲が重なる Surface をどうするか (エラーにするか、後勝ちか) は planner が決める。NavTileStore のスロット表と salt の決定性 (ADR-023 の F3) を保つ |
| C | **エリアコストを Agent / クエリごとに** | `.navfilter.json` アセット (エリアごとのコスト倍率と通行可否、UE の NavigationQueryFilter)。Agent が参照し、ABI のクエリにも渡せる | dtCrowd のフィルタは 16 種まで (`DT_CROWD_MAX_QUERY_FILTER_TYPE`)。既存の `filterMasks_` の溢れ処理を (areaMask, filter) の組へ広げる。アセットの中身は sim 入力なので、.mnav と同じく GUID と内容ハッシュで管理する |
| D | **Agent の細かい制御** | Warp、`isStopped`、`autoBraking`、`avoidancePriority`、`separationWeight`、経路の事前計算 (`CalculatePath`) と `SetPath`、`updatePosition` / `updateRotation` (オフにすると Nav は望む速度だけを公開し、ルートモーションやスクリプトで動かす)、読み取り専用の `desiredVelocity` / `nextPosition` | 優先度は dtCrowd に無いので、Recast へのパッチになる (`PATCHES.md` に記録)。SetPath は `dtPathCorridor::setCorridor` を使い、経路の保存先は固定長にする。新フィールドは末尾に追加し、スナップショットの版を上げる |
| E | **Link の自動生成** | ベイク時に NavMesh の縁を調べ、飛び降り (`dropHeight` 以内) と飛び越え (`jumpDistance` 以内) の Link を自動で作る (Unity の Generate Links)。Surface の設定で on/off | 生成した Link は .mnav に焼く (ベイクの導出値なので、M82 の「Link は焼かない」とは別物)。Debug / Release でバイト一致させる (M82 の N4)。手置きの NavMeshLink と同じ渡り方 (Jump / Linear) を使う |

### サブ分割案
A → B → C → D (2 サブ) → E → 文書・全体検証。ABI の bump は C と D の分を 1 回にまとめる。

---

## M85 ビヘイビアツリー — 約 15 サブ

### 構成 (UE の BehaviorTree + Blackboard を手本にする)
- **アセット**
  - `.bt.json` (木構造 + ノード位置) と `.bb.json` (Blackboard のキーと型)。SubTree の「親の Blackboard を共有する」を型で検査できるよう、Blackboard を独立したアセットにする (UE と同じ)
  - 追加の手順は ControllerLibrary と同じ: `AssetType` (`AssetDatabase.h:13-34`)、`ClassifyPath`、`kCompound` (`AssetOps.cpp:192`)、起動時の読み込み (`DemoContent.cpp:3182-3228`)、ReloadHub
- **`BehaviorTreeComponent`** (POD): BT アセットの GUID、有効フラグ、読み取り専用の状態 (実行中のノード id など)
- **実行状態と Blackboard**: コンポーネントは可変長を持てない (`Components.h:10-14`) ので、`BehaviorTreeSystem` が表で持つ。SimSnapshot に BT 節を新設し (Nav 節が手本、`SimSnapshot.cpp:490-525`)、`SimSources` にハッシュを足す。中身が空なら何も畳まない
- **Tick の位置**: 知覚 (M83) の後、NavSystem の前。MoveTo が書いた目的地を同じ tick の NavSystem が拾う。BT は NavMeshAgent を通してだけ動かし、`moveInput` は直接書かない (AgentBrain との競合は M82 と同じ警告で扱う)
- **時間**はすべて tick 単位。乱数は `World::Rng()` だけを使い、BT が無い tick では引かない

### ノード (事前計画どおり)
- Composite: Selector / Sequence / Parallel (UE の Simple Parallel)
- Decorator: Blackboard Condition (Abort: None / Self / LowerPriority / Both) / Invert / Cooldown / Repeat / Timeout
- Task: MoveTo (M84 のフィルタ指定可) / Wait / RotateTo / SetBlackboard / ClearBlackboard
- AI: FindRandomPoint (`NavSystem::QueryRandomPoint`) / FindNearestTarget (AIPerception の結果から選ぶ。同距離なら entity キー) / SearchArea / FindTarget (陣営・タグの条件)
- Gameplay: PlayAnimation / SendEvent
- Tree: SubTree
- Custom: C++ Task / C# Task
- 追加 (2026-10-05): **Patrol** (巡回ルートを順に回る)

### 巡回ルート
- `PatrolRouteComponent`: ウェイポイントを固定長の配列 (最大 32 点、ローカル座標) で持つ POD。点ごとに待ち時間 (tick)。回り方は Loop / PingPong / Once
- SceneView で点をギズモでドラッグして編集する (1 回のドラッグで 1 Undo)。点と点の間を線で結び、向きの矢印を出す
- 「次に向かう点」は BT の Blackboard ではなく、巡回する側の状態として BT 節に持つ (同じルートを複数の敵が共有できるように)。BT の Patrol タスクは、その点へ MoveTo して待ち、次の点へ進む。発見で Abort されて戻ってきたら、一番近い点から再開する
- BT を使わない簡単な用途向けに、Agent に直接ルートを持たせるかは planner が決める

### 新設が必要な基盤 (調査で無いと分かったもの)
- **sim 側の汎用イベントキュー** (SendEvent 用): tick 境界で配送し、順序は「送信元の entity キー → 送信順」。tick をまたいで残る分はスナップショットに入れる。受け取り手は BT (Blackboard へ反映し、Abort で反応する)、C++ (ポーリング API。`NetGetSystemEvent` と同じ形)、C#
- **アニメのステートを強制する API** (PlayAnimation 用): 今は `SetAnimatorParam` しかない (`EngineAPI.h:259`)。ステート名から index を引いて (`ControllerAsset::states[].name`) 遷移させる関数を Engine と ABI に足す
- **ユーザー定義ノードの登録**: `REGISTER_SCRIPT` (`ScriptAPI.h:302`) と同じ静的 Registrar で `REGISTER_BT_TASK` を作り、名前のハッシュ (`MyeNameHash`) で引く。C ABI と POD だけを使う。C# のノードはスナップショットとリプレイの対象外 (`SimSnapshot.h:39-46`) なので、使っているシーンは決定論の保証から外れることを Inspector と文書に書く

### グラフエディタ
- ノードエディタのライブラリは入っていない。AnimatorControllerWindow (`src\Editor\Windows\Animation\AnimatorControllerWindow.cpp`) と同じく ImGui の DrawList で手書きする。BT は木なので、上から下へのレイアウトにする
- Animator に無いものを足す: ノード位置の保存、アセット編集の Undo (窓の中に独自のスタックを持つ。UndoStack はシーン専用、`UndoStack.h:25-33`)、Blackboard エディタ
- ライブ表示: Play 中に、選んだ entity の実行中ノードと Abort の経路を強調する (Animator の `:223-229` の流儀)。リプレイ / What-if の再生中も同じ表示を出す

### サブ分割案
1. アセット (bt / bb) + 実行器 + Composite 3 種 + BT 節 (snapshot / hash)
2. Decorator 5 種 + Abort
3. Task 5 種 (MoveTo は NavMeshAgent 経由)
4. AI ノード 4 種 (知覚と Nav のクエリ)
5. 汎用イベントキュー + SendEvent
6. PlayAnimation (ステート強制 API) + SubTree
7〜10. グラフエディタ (配置と接続 → Undo → Blackboard 編集 → ライブ表示とリプレイ中の表示)
11. C++ Task + C++ のイベント受信 (ABI bump はここで 1 回)
12. C# Task + C# のイベント受信 (temp プローブで実走確認)
13. 巡回ルート (コンポーネント + ギズモ編集 + Patrol タスク)
14. デモ (知覚 + NavMesh + BT の敵: 巡回 → 発見 → 追跡 → 見失ったら予測位置を捜索) + replay_verify のジョブ + golden
15. ADR-025、engine_spec、feature guide、全体検証

---

## M86 Smart Objects — 約 5 サブ

UE5 の Smart Objects を手本にする。「使い方」を AI の側ではなく物の側が持ち、AI はそれを探して予約し、使う。

### 作るもの
- **`SmartObjectComponent`** (物の側): スロットを固定長 (最大 8) で持つ。各スロットは、ローカルの立ち位置と向き、タグ (「座る」「レバー」など)、使うときの振る舞い (BT の SubTree アセット、または Animator のステート名)、使用中かどうか、有効 / 無効
- **予約**: `SmartObjectSystem` が「スロット → 予約した entity」の表を持つ。同じ tick に複数の AI が同じスロットを取りに来たら、entity キーの小さい方が勝つ。予約したまま AI が消えたり無効になったりしたら解放する。表は SimSnapshot の節とハッシュに入れる
- **BT ノード**:
  - FindSmartObject (範囲・タグ・陣営で探し、空いているスロットを Blackboard へ書く。同点なら距離 → entity キー)
  - ClaimSmartObject
  - UseSmartObject (立ち位置へ MoveTo → 向きを合わせる → スロットの振る舞いを SubTree として実行 → 解放)
  - ReleaseSmartObject
  - Abort されたときも必ず解放する
- **エディタ**: スロットの立ち位置と向きのギズモ、予約中のスロットの色分け (Play 中)、Inspector で予約者を表示
- **ABI**: 検索・予約・解放 (C++ / C#)。M85 の bump とは別に 1 回

### サブ分割案
1. コンポーネント + 予約の表 (snapshot / hash) + ギズモ
2. BT ノード 4 種と Abort 時の解放
3. スロットの振る舞い (SubTree / Animator のステート)
4. ABI + C# + デモ (座る椅子・引くレバー) + replay_verify のジョブ
5. 文書 (ADR-026) と全体検証

---

## 全体のリスク
- **視線のコスト**: エージェント数 × 対象数のレイ。件数の上限で抑え、M83 のサブ 1 で 50 体 × 50 体を計測する
- **Surface の接続 (M84-B)** は M82 の復元方式 (NavTileStore のスロット表、ADR-023) に一番深く触る。M84 の中で最も危ない。planner が最初に試作で確かめる
- **avoidancePriority** は Recast 本体へのパッチになる。dtObstacleAvoidance の決定論を崩さないこと
- **番号の衝突**: M75h (InputField) と TypeId・ABI を先着で取り合う。どのマイルストーンも、着手時に `Components.cpp` の末尾と `EngineAPI.h` の版を確認する
- **C# の BT ノード**はリプレイの被覆外

## 検証 (各マイルストーン共通。AGENTS.md 7 章)
- `bin\x64\Debug\Editor.exe --selftest` と Release の `--selftest` (新しい `*SelfTest.cpp`: PerceptionSelfTest / NavFilterSelfTest / BehaviorTreeSelfTest など)
- `tools\replay_verify.bat`: 新しいジョブ (`perception` / `bt`) が Debug・Release・Server.exe と snapshot stress で PASS。既存のジョブが変わらない (存在ゲート)
- `tools\shot_verify.bat`: 新しい golden (視野のギズモ、BT デモ)。既存の golden が変わらない
- `tools\check_rules.ps1`: ABI の版とスロット数、ローカライズ
- 実際の操作で確認: エディタで視野の扇形が見える、Play 中に敵が見つけて追う・見失って捜索する、BT エディタのライブ表示 (`--screenshot` の一時プローブで撮る)
