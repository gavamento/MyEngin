# M85 ビヘイビアツリー (BT + Blackboard + 巡回ルート) — 仕様書

- slug: m85-behavior-tree
- 状態: 確定 (2026-10-05。planner 裁定 5 件を司会経由でユーザーが確認し、4 件は裁定どおり、MoveTo の Stuck だけユーザー回答で変更 (2. #17、8. 変更履歴)。確定と sub-01 からの着手はユーザー承認済み)
- 依頼原文: M85の実装
- 出発点: `C:\HAL\MyEngin\plans\ai-roadmap-m83-m86.md` の「M85 ビヘイビアツリー — 約 15 サブ」節 (2026-10-05 承認)。
  BT の Service ノードと StateTree は範囲外 (ユーザー選択済み)。巡回ルートは M85 に含む (ユーザー選択済み)。
- 基点: HEAD baf29bf (M84f)。確認した番号: TypeId 末尾 77 (`AIStimulusSource`、`src\Engine\Core\Ecs\Components.cpp:1574`)、
  `MYE_API_VERSION 26u` (`src\Shared\EngineAPI.h:46`、151 スロット)、`kSimSnapshotVersion = 33` (`src\Engine\Engine\Replay\SimSnapshot.h:117`)。
  ADR は 024 まで (BT は ADR-025)。

## 1. 目的 (なぜ作るか)

NavMesh (M82/M84) で歩け、知覚 (M83) で敵を見つけられるようになったが、「見張る → 見つけたら追う → 見失ったら探す → 諦めて巡回へ戻る」
という行動の組み立ては、今は C++ / C# のスクリプトを毎回手で書くしかない (既存の `AgentBrain` は 5 状態固定の FSM、
`src\Engine\Core\Ecs\Components.h:1611-1626`)。
UE の Behavior Tree + Blackboard と同じ考え方で、**行動をアセット (木) として組み、エディタで見ながら直せ、
しかも決定論 (Debug / Release / Server のビット一致、リプレイ、What-if、ロールバック) を崩さない** 状態を作る。
後続の M86 Smart Objects は BT のタスクと SubTree の上に乗る。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | BT の実行状態は M83 の知覚と同じく「全部コンポーネントに置く」(ADR-024 決定 2) べきでは? 節を足すと SimSnapshot・SimSources・SimRefs の 3 点セットが要る | ADR-024 決定 2 は「固定長に収まる」から成立した。BT は木のノード数・Blackboard のキー数・SubTree の入れ子・C++ タスクの状態サイズがアセットごとに違い、固定長にすると上限が設計を縛る (例: 64 ノード × 数十バイト + BB 32 キー × 16 B + C++ タスク状態 ≥ 2 KB/体、シーンファイルにも実行状態のゼロ列が並ぶ)。Nav 節 (`SimSnapshot.cpp:493-523`) と `HasHashableState` の中身ゲート (`WorldHasher.cpp:549-553`) という前例がある | 裁定どおり (2026-10-05 ユーザー回答、司会経由) | **ロードマップどおり `BehaviorTreeSystem` が表で持ち、SimSnapshot に BT 節を新設、SimSources にハッシュを足す。** コンポーネントは設定と読み取り専用の表示値 (実行中ノード id・状態) だけ。却下: 全部コンポーネント (上限が木の大きさを縛る)。 |
| 2 | 実行モデル: 毎 tick 根から評価し直す (Unity の一般的な自作 BT) か、UE のように実行中タスクを持ち続けて Decorator の監視で Abort するか | Abort モード (None / Self / LowerPriority / Both) は UE のイベント駆動モデルでしか意味を持たない (根から毎回評価するなら Abort は不要)。ロードマップで Abort 付き Decorator が確定済み | ロードマップで確定済み | **UE 方式**: 実行中のタスクを持ち続け、Blackboard Condition の監視 (observer) で Abort する。ただし監視はコールバックでなく **毎 tick の BT フェーズ冒頭で、監視中の Decorator を木の優先順に全部評価する (ポーリング)**。BB の書き換え元 (スクリプト / ABI / イベント / 他ノード) と書いたタイミングに結果を依存させないため。却下: BB 書き込み時の即時コールバック (同じ tick の中で書いた順に Abort が走り、スクリプトの実行順で結果が変わる)。 |
| 3 | UE の公式仕様を当たったか (ユーザーの要望) | この planner 環境には Web 取得の道具が無い。下の UE の規則は UE 5 の公式ドキュメント (Behavior Tree Node Reference: Composites / Decorators / Tasks、Behavior Tree Overview の「Observer Aborts」) の記述を**記憶から**書いている (未検証) | — | 仕様 4.1 に「UE ではこう」を併記し、UE と違う所は理由を書く。**sub-01 / sub-02 の coder は、Web が使えるなら公式ドキュメントで 4.1 の UE 欄を照合し、違っていれば SELF_EVAL の不安・質問に出す** (planner が直す)。reviewer も同じ照合をしてよい |
| 4 | Blackboard を BT アセットの中に持たず `.bb.json` を独立アセットにする必要はあるか | SubTree が親の BB を共有するので、型の一致を検査する単位が要る。UE も BlackboardData は独立アセット | ロードマップで確定済み | 独立アセット `.bb.json`。BT は使う BB を 1 つ参照する。SubTree は**同じ BB アセット (GUID 一致)** を要求する (UE は親 BB の派生も許すが、BB の継承は作らない) |
| 5 | PlayAnimation に必要な「ステートを強制する API」は本当に無いか | 無い。`SetAnimatorParam` (`EngineAPI.h:291-293`) は params[0..3] を書くだけ。`AnimatorControllerComponent` (`Components.h:706-716`) の `transitionTo` 等を汎用フィールド ABI で書く抜け道はあるが読み取り専用フィールドへの直書き。アニメの状態は sim の中 (`TickRunner.cpp:410-415`、World 節) にある | — | Engine に `AnimatorPlay(world, e, stateIndex, durationTicks)` (遷移を開始する。duration 0 = 即切り替え) を足し、BT の PlayAnimation と ABI の `AnimatorPlay` (ステート名のハッシュ) が使う。**付記**: `ControllerAsset::defaultState` が実行時に適用されていない (`AnimatorController.cpp:300-302` で 0 に丸めるだけ) のを見つけたが、M85 の範囲外として触らない (7. のリスクへ) |
| 6 | 汎用イベントキューは既にあるか | 無い。あるのは用途別 (衝突 / 破壊 / UI / 知覚の保留欄 / エフェクト / 音 / Net の SystemEvent) だけ。ポーリング ABI の手本は `NetSystemEventCount` / `NetGetSystemEvent` (`EngineAPI.h:734-736`) | — | 新設 (4.1.6)。tick N に送った分を tick N+1 に配る。配送待ちは BT 節に入れる |
| 7 | C++ のユーザー定義タスクの状態をどこに置くか | `REGISTER_SCRIPT` は状態を POD としてエンジン側に置き (`ScriptTypes.h:49-72`、`ScriptAPI.h:188-211`)、だからスナップショットとリプレイに入っている | — | 同じ形: `REGISTER_BT_TASK(T, FIELDS(...))` で POD の状態型を登録し、**状態はノードのインスタンスごとにエンジンの BT 表が持つ** (BT 節に入る = 決定論の被覆内)。フィールドは BT エディタでノードのパラメータとして編集し `.bt.json` に名前で保存する。登録は `MyeScriptModule` に `btTaskCount` / `btTasks` を足す (レイアウト変更なので ABI の bump に入れる) |
| 8 | C# のタスクは決定論を保てるか | 保てない。C# レーンは状態が World の外にあり、記録・検証・Net・再シムでは走らない (`TickRunner.cpp:333-341`、`SimSnapshot.h:39-46`) | ロードマップで「保証外と明記」に確定済み | 作る。C# レーンが止まっているとき (記録・検証・Net・再シム) は C# タスクは**即 Failure** を返し、1 回だけ警告する。BT エディタと Inspector に「この木は C# タスクを含むので決定論の保証外」と出す |
| 9 | `.bt.json` にノードの表示位置を持たせると、位置を動かしただけで provenance の contentHash (`Provenance.cpp:176-245`) が変わり、Net の接続が拒否され、リプレイのヘッダ差分が出る | contentHash は `assets\` 以下の全ファイルを畳む。位置を別ファイル (`.bt.layout.json`) に分けても同じく `assets\` 以下なら畳まれる | 裁定どおり (2026-10-05 ユーザー回答、司会経由) | **`.bt.json` に位置を持たせる** (UE もグラフはアセットに入る。ファイルが 1 つで済み、Git の差分も 1 か所)。見た目だけの変更でも contentHash は変わる、と文書に書く。却下: 別ファイル (どのみち contentHash は動くので利点が無い)。 |
| 10 | Agent に直接ルートを持たせる (BT 無しの巡回) か | ロードマップで planner 裁定と指定。BT 無しで巡回したい用途は「Patrol 1 個だけの木」で足り、NavMeshAgent に巡回を足すと Nav と BT の 2 か所が目的地を書く | 裁定どおり (2026-10-05 ユーザー回答、司会経由) | **持たせない。** 巡回は BT の Patrol タスクだけ。最小の巡回用 BT を `assets\ai\patrol_only.bt.json` としてデモに同梱する。却下: NavMeshAgent に `patrolRoute` を足す (目的地を書く主体が 2 つになる) |
| 11 | 巡回の「次の点」をどこに持つか | ロードマップ: ルートを複数の敵が共有できるよう、ルート側ではなく巡回する側の状態にする | ロードマップで確定済み | Patrol ノードのインスタンス状態 (BT 表) に「次の点の index と向き (PingPong)」を持つ。Abort されて戻ってきたら**現在位置から一番近い点** (同距離は index 小) から再開 |
| 12 | 1 tick の中で即終了するノードが無限に回る木 (Repeat 無限 + 即成功タスク) はどうなるか | UE でも起きる。時間で打ち切ると決定論が崩れる | — | 1 体 1 tick の**ノード実行の手数**の上限 `kBtMaxStepsPerTick = 256` (名前付き定数)。超えたらその tick はそこで止めて次の tick に続きから進め、1 回だけ警告する |
| 13 | 番号 (TypeId / ABI / snapshot) を M75h (InputField) と取り合うか | M75h は未着手 (TypeId 76 を予定していたが M83 が 76/77 を使用済み) | — | 「登録コミット時点の末尾から振る」を守る。予定: TypeId 78 `BehaviorTree` (sub-01)、79 `PatrolRoute` (sub-07)。ABI は sub-11 で 1 回だけ v27。snapshot はコンポーネント / 節を変えるサブごとに +1 (同じサブ内では 1 回) |
| 14 | BT が NavMeshAgent と AgentBrain の両方と同じエンティティで動いたら? | `moveInput` を書くのは NavSystem と AgentBrain。Inspector に既に警告がある (`InspectorWindow.cpp:1874-1875`) | — | BT は `moveInput` を書かない (Nav 経由だけ)。MoveTo / RotateTo / Patrol / SearchArea は NavMeshAgent が無ければ即 Failure。BT と AgentBrain の同居も Inspector に警告を出す |
| 15 | ライブ表示でリプレイ / What-if の再生中も見せられるか | TimeTravel は同じ World へ `RestoreSimSnapshot` して再シムする。毎フレーム ctx.scene から取り直す窓は巻き戻した tick の状態を出せる (Animator 窓が前例)。What-if の分岐側は World として読めない (`TimeTravel.h:85-90`) | — | BT 節が復元されるので、BT 窓が毎フレーム BehaviorTreeSystem を引けばタイムライン操作中も正しく出る。**What-if の分岐 (非ライブ側) の BT 表示はやらない** (ロードマップの「What-if の再生中も」は、ライブ側へ分岐を採用した後に見えることで満たす)。 |
| 17 | MoveTo が Stuck のとき Failure にするか | NavSystem の Stuck は「経路の残りを一定 tick 縮められない」表示と通知だけで、押し続けて前進が戻れば Moving に戻る (`Components.h:1940`)。M82l で同じ目的地の渋滞を Stuck にしないよう直した経緯がある。UE の MoveTo は詰まっても失敗しない | **ユーザー回答 (2026-10-05)**: 「MoveToにチェックボックスで詰まった時にFailureするかを選ばせる」(planner 裁定の「常に Running」は不採用) | MoveTo のパラメータ `failOnStuck` (bool) をノードごとに選ぶ。**既定は false** — UE と同じで、一時的な渋滞や押し合いで追跡が途切れない方が安全側。既存の木 (まだ無い) やデモで明示しなければ従来の想定どおりになる。true は「詰まったら別の行動へ切り替えたい」木で使う |
| 18 | 移動を含む SearchArea / Patrol が Stuck のとき | ユーザーは MoveTo に「詰まったら Failure にするか」のチェックボックス (2. #17) を選んだ。SearchArea / Patrol も内部で同じ移動をするので、同じ問題 (詰まり続けると終わらない) を持つ | 裁定どおり (2026-10-05 ユーザー回答「MoveTo と同じチェック」、司会経由) | **同じ名前・同じ意味の `failOnStuck` (既定 false) を SearchArea と Patrol にも持たせる** (true ならノード全体が Failure)。却下: SearchArea だけ「詰まった点を飛ばして次の点へ」(捜索としては便利だが、同じ名前のチェックボックスが種類ごとに違う意味になる) |
| 19 | Patrol を初めて始めるとき、点 0 からか一番近い点からか | ノードの状態は終了・Abort で消えるので、初回と復帰を区別するには持ち越しの状態が要る | 裁定どおり (2026-10-06 ユーザー回答、司会経由) | **入るたびに一番近い点から** (初回も同じ。同距離は index 小)。却下: 初回だけ点 0 (持ち越しの状態と snapshot の書式変更が要り、置いた位置が点 0 から遠いと最初に引き返す) |
| 20 | BB の Entity キー (Patrol の route、追う相手の初期値など) をシーンの物で埋める手段が無い。`.bb.json` の初期値はアセットなのでシーンのエンティティを指せない。このままでは `patrol_only.bt.json` がコード無しで動かず、2. #10 の「BT 無しの巡回は Patrol 1 個の木で足りる」が成り立たない | sub-07 SELF_EVAL | 裁定どおり (2026-10-06 ユーザー回答、司会経由) | **`BehaviorTreeComponent` に「Entity キーの初期値」を固定 4 組 (`bbEntityKey[4]` = キー名 String64、`bbEntityValue[4]` = EntityRef) 持たせ、木を始める / やり直すときに BB へ書く** (UE で Pawn ごとに BB を初期化するのと同じ役)。Inspector では BB の Entity キーを選ぶ欄にする。sub-08 で入れる (snapshot +1)。却下: Patrol の route 未設定時に自分の PatrolRoute を使う (ルートの共有ができず Patrol 専用の抜け道になる)、ABI でしか書けないまま (コード無しで巡回できない) |
| 16 | Animator の窓が「位置を保存しない・Undo 無し・パン/ズーム無し」なので、それを手本に BT エディタを作ると同じ穴が残る | `AnimatorControllerWindow.cpp:25-37` (位置はメモリだけ)、Undo 無し、`NoScrollbar | NoMove` | — | BT エディタは新規の窓。位置の保存・窓内の Undo (アセット全体の JSON を前後で持つ単純な方式)・パンとズームを持つ。Animator 窓は直さない |

## 3. スコープ

- やる:
  - アセット `.bt.json` (木 + ノードの位置) / `.bb.json` (キーと型) と、そのライブラリ・起動時の読み込み (Editor / Runtime / Server)・ReloadHub・Asset Browser (作成・アイコン・ダブルクリックで BT 窓)
  - `BehaviorTreeComponent` (TypeId 78)、`BehaviorTreeSystem` (TickRunner フェーズ 3.4a2 = 知覚の後・Nav の前)、SimSnapshot の BT 節、SimSources のハッシュ
  - ノード: Composite 3 (Selector / Sequence / SimpleParallel)、Decorator 5 (BlackboardCondition + Abort 4 種 / Invert / Cooldown / Repeat / Timeout)、Task 5 (MoveTo / Wait / RotateTo / SetBlackboard / ClearBlackboard)、AI 4 (FindRandomPoint / FindNearestTarget / SearchArea / FindTarget)、Gameplay 2 (PlayAnimation / SendEvent)、Tree 1 (SubTree)、Custom 2 (C++ Task / C# Task)、Patrol
  - sim 側の汎用イベントキュー (BT・C++・C# が受信)
  - Engine の `AnimatorPlay` (ステート強制)
  - `PatrolRouteComponent` (TypeId 79) と SceneView のギズモ編集
  - BT グラフエディタ (配置・接続・パラメータ・保存・Undo・Blackboard 編集・検査エラー・Play 中とタイムライン操作中のライブ表示)
  - ABI v27 (1 回): BB の読み書き、イベントの送信と受信、AnimatorPlay、C++ タスクの登録 (モジュール記述子の拡張)。C# ミラーと糖衣
  - デモ `--bt-demo` (巡回 → 発見 → 追跡 → 見失ったら予測位置を捜索 → 巡回へ戻る)、replay_verify の `bt` ジョブ、shot_verify の golden `bt`
  - ADR-025、engine_spec、engine-feature-guide、test_checklists、ロードマップの進捗表
- やらない (明示的に外したもの):
  - BT の Service ノード、StateTree (ユーザー選択)
  - EQS / AI デバッガ窓 / 経路テスト / Nav Invoker (ロードマップで不採用)
  - Blackboard の継承 (親 BB)・インスタンス同期キー (UE の Instance Synced)、Decorator の「OnValueChange」通知 (OnResultChange だけ)
  - Agent が直接ルートを持つ巡回 (2. #10)
  - What-if の非ライブ分岐の BT 表示 (2. #15)
  - Animator の `defaultState` 未適用の修正、Animator 窓の改修
  - `AgentBrain` の変更 (共存のまま)
- 後回し:
  - Smart Objects のノード (M86)
  - BT の実行時のホットリロードで「実行中の位置を保ったまま差し替える」こと (M85 では木を最初からやり直す、4.1.9)

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 共通の実行モデル (UE 方式、2. #2)

- 各ノードの結果は `Running` / `Success` / `Failure`。時間は**すべて tick**。
- `BehaviorTreeSystem::Update` はフェーズ 3.4a2 (知覚 3.4a の後、Nav 3.4b の前。`TickRunner.cpp:389-404` の間。コメントで M85 用と予約済み) で走る。
  - 存在ゲート: 有効な `BehaviorTreeComponent` が 0 個なら走査だけで何もしない (RNG を引かない、BT 節は空、ハッシュに何も畳まない)。
  - 処理順: エンティティキー (index → generation) 昇順。1 体ずつ最後まで処理する。
  - 1 体の 1 tick: (1) 配達されたイベントを BB へ反映 (4.1.6) → (2) 監視中の Decorator を優先順 (木の深さ優先・前順) に評価し Abort を決める → (3) 実行中タスクを 1 回 tick → (4) 結果を親へ返しながら次のタスクまで進む。(2)〜(4) で消費したノード実行の手数が `kBtMaxStepsPerTick` (256) に達したら止める (2. #12)。
  - 根が Success / Failure で終わったら、**次の tick に**根からやり直す (UE は同じ tick で再開するが、即終了する木が 1 tick に何周もするのを避ける)。
- Composite:
  - Selector: 子を左から順に。最初に Success した子で Success。全部 Failure で Failure。
  - Sequence: 子を左から順に。最初に Failure した子で Failure。全部 Success で Success。
  - SimpleParallel: 子は 2 つ。左 = メインのタスク (Task ノードでなければエディタで検査エラー)、右 = 背景の部分木。両方を同じ tick に進める (メインが先)。`finishMode` = Immediate (メインが終わったら背景を Abort) / Delayed (メインが終わっても背景が終わるまで待つ)。結果はメインの結果。背景が先に終わったら背景を最初からやり直す (UE と同じ)。
- Decorator はノード (Composite / Task) に複数付き、上から順に評価する。条件の Decorator (BlackboardCondition / Cooldown) がどれか偽ならそのノードは入らずに Failure。
- Abort (UE の Observer Aborts。**OnResultChange のみ**):
  - `None`: 入るときに 1 回評価するだけ。
  - `Self`: このノードの部分木が実行中に条件が偽になったら、部分木を Abort してこのノードを Failure にする。
  - `LowerPriority`: このノードより優先度の低い (同じ親 Selector の右側の兄弟とその子孫が) 実行中に条件が真になったら、それを Abort してこのノードから実行し直す。
  - `Both`: 両方。
  - UE と同じく LowerPriority / Both は **親が Selector のときだけ**有効。親が Sequence / Parallel のときはエディタで検査エラーにし、実行時は Self として扱う。
- Abort の後始末: Abort されたノードは子孫を深い方から順に `OnAbort` を受ける (MoveTo は停止、C++ タスクは onAbort、Cooldown の計時開始など)。M86 の「Abort でも必ず解放」はこの経路に乗る。

#### 4.1.2 Decorator 5 種

| ノード | パラメータ | 振る舞い | UE との対応 |
|---|---|---|---|
| BlackboardCondition | key、query (IsSet / IsNotSet / == / != / < / <= / > / >=)、値 (Int / Float のとき)、abort | Bool: IsSet = true。Entity: IsSet = 生きている有効なハンドル。Vector: IsSet = 書かれた印あり。Int / Float は比較 | UE の Blackboard Based Condition。Enum / Name / Rotator / Class / Object 系の型は持たない |
| Invert | なし | 子の Success ↔ Failure を入れ替える (Running はそのまま) | UE は条件 Decorator の Inverse Condition フラグ。Unity Behavior の Inverter と同じく独立ノードにする (ロードマップどおり) |
| Cooldown | ticks | ノードが終わった (Success / Failure / Abort) tick から ticks の間、入れない (条件が偽扱い) | UE の Cooldown と同じ |
| Repeat | count (0 = 無限) | 子が Success するたびに子をやり直し、count 回の Success で Success。子が Failure したら Failure で抜ける | UE の Loop に相当 (本仕様の定義を正とする) |
| Timeout | ticks | 入ってから ticks 経っても子が終わらなければ子を Abort して Failure | UE の TimeLimit |

#### 4.1.3 Task 5 種

| ノード | パラメータ | 振る舞い |
|---|---|---|
| MoveTo | target key (Vector / Entity)、acceptanceRadius、observeTarget (bool)、navFilter (AssetRef、null = Agent のまま)、failOnStuck (bool、既定 false) | NavMeshAgent の `destination` / `hasDestination` を書く (`NavSetDestination` と同じ書き方、`EngineApiTable.cpp:1360-1368`)。Entity は毎 tick その位置。observeTarget なら目標が 0.5 m 以上動いたら目的地を書き直す (UE の Observe Blackboard Value)。終了: 目標まで acceptanceRadius 以内 か `status == Arrived` → Success、`NoPath` / `Inactive` → Failure、`Stuck` は failOnStuck が false (既定) なら Running のまま (諦めは Timeout で書く)、true ならその tick に Failure (Abort と同じく `hasDestination = false` にして止める)。Abort / 終了時に `hasDestination = false` (UE の StopMovement)。navFilter 指定時は実行中だけ Agent の navFilter を差し替え、終わったら戻す。NavMeshAgent が無ければ Failure。**同じ地点への MoveTo を続けて 2 回しても 2 回目がちゃんと Success で終わること** (到着済みの Agent は destination が変わらないと再探索しない、`NavSystem.cpp:1460-1476`) |
| Wait | ticks、randomDeviation (ticks) | ticks ± randomDeviation (World::Rng、偏差 0 なら引かない) 待って Success |
| RotateTo | target key (Vector / Entity)、angularSpeedDeg (0 = Agent の値)、toleranceDeg | Y 軸まわりに LocalTransform を回して目標の方向へ向ける。toleranceDeg 以内で Success。実行中は Agent の `updateRotation` を false にし、終わったら戻す (Nav との奪い合いを防ぐ) |
| SetBlackboard | key、値 (型に合わせる。Entity は「自分」/ 別キーのコピー、Vector は定数 / 自分の位置 / 別キーのコピー) | 書いて Success |
| ClearBlackboard | key | 未設定に戻して Success |

#### 4.1.4 AI ノード 4 種

| ノード | パラメータ | 振る舞い |
|---|---|---|
| FindRandomPoint | 中心 (自分 / Vector キー)、radius、出力 Vector キー | `NavSystem::QueryRandomPoint` (`NavSystem.h:243`、World::Rng) で到達可能な点を出して Success。見つからなければ Failure |
| FindNearestTarget | sense マスク (Sight/Hearing/Damage/Touch)、currentlySensedOnly (bool)、出力 Entity キー、出力 Vector キー (最後の位置、任意) | 自分の `AIPerception.percepts` から条件に合う相手のうち一番近い (`lastSensedPos` までの距離、同距離は entity キー小) を選ぶ。名乗らない音 (target が null) は Entity キーへは書けないので Vector 出力だけに使う。無ければ Failure |
| SearchArea | 起点 key (Vector。既定は FindNearestTarget が書いた最後の位置)、usePrediction (bool。真なら知覚の `predictedPos` を起点に)、radius、pointCount、検索の終了キー (Entity。この相手が今見えたら Success)、failOnStuck (bool、既定 false。MoveTo と同じ意味、2. #17) | 起点へ MoveTo → 起点の radius 内の到達可能点を pointCount 個 (上限 32) 順に回る。点は列で持たず、向かい始める時に 1 つずつ FindRandomPoint と同じ方法で生成する。途中で対象が視覚で見えたら Success、全部回ったら Failure。予測位置はナビメッシュへ吸着 (`QuerySamplePosition`) してから使う。Stuck は failOnStuck が false なら Running のまま、true ならその tick に Failure (目的地を倒して止める) |
| FindTarget | 範囲 radius、陣営の条件 (敵 / 中立 / 味方のビット、自分の `AIPerception` の態度判定 `PerceptionAttitudeOf` を使う)、タグの条件 (`TagComponent.mask` の AND マスク)、出力 Entity キー | `AIStimulusSource` を持つ有効なエンティティから条件に合う一番近いもの (同距離は entity キー小) を選ぶ。移動しない。知覚 (見えているか) は問わない。無ければ Failure |

#### 4.1.5 Gameplay / Tree / Patrol

- PlayAnimation: state 名、durationTicks、waitForEnd (bool)。`AnimatorPlay` で遷移を始め、waitForEnd ならそのステートのクリップが 1 周するまで Running、でなければ即 Success。Animator が無い・名前が無い → Failure。
- SendEvent: イベント名、宛先 (Entity キー / 自分 / 全体)、ペイロード (Vector キー / Float 定数 / Int 定数)。積んで即 Success (4.1.6)。
- SubTree: BT アセット。**同じ BB アセット**でなければエディタで検査エラー・実行時は Failure。親の BB をそのまま使う。入れ子は 8 段まで (超えたら Failure + 警告。自分自身を含む循環もこれで止まる)。
- Patrol: route キー (Entity。`PatrolRouteComponent` を持つエンティティ)、acceptanceRadius、failOnStuck (bool、既定 false。MoveTo と同じ意味、2. #17)。現在の点へ MoveTo → 点の waitTicks だけ待つ → 次の点へ。Loop は無限に Running、PingPong は端で折り返して無限、Once は最後の点で待ち終えたら Success。Abort されて戻ったら一番近い点から (2. #11)。

#### 4.1.6 汎用イベントキュー

- イベント = POD `{ nameHash (u64), sender (Entity), target (Entity、null = 全体), vec3, float, int32, seq (u32) }`。
- 送る手段: BT の SendEvent、ABI `BtSendEvent` (C++ / C#)。tick N のどのフェーズで送っても**tick N+1 に配る**。
- 配送の順序: 送信元の entity キー → 送信順 (seq)。1 tick の上限 `kBtMaxEventsPerTick = 256`、溢れた分は捨てて 1 回だけ警告 (件数で決めるので決定論)。
- 受け手:
  - BT: `.bb.json` のキーに `eventName` を書いておくと、その名前のイベントが自分宛て (または全体) に届いたら、そのキーへ書く (Bool = true、Entity = sender、Vector = vec3、Float = float、Int = int32)。同じ tick に複数届いたら配送順の最後が勝つ。書いた後は Abort の監視でそのまま反応する。
  - C++ / C#: ABI `BtEventCount(engine, self)` / `BtGetEvent(engine, self, index, out)` で「tick N+1 に自分宛て + 全体に配られたもの」を読む (`NetGetSystemEvent` と同じ形)。読める期間は tick N+1 の中だけ。
- **配達の時点は tick の頭** (TickRunner のフェーズ 3 = スクリプトの Update より前、`stepSim` のゲートの中)。こうしないと BT より前に走る C++ / C# の Update が tick N+1 の配達分を読めない (sub-05 VERDICT)。BT は同じ tick のフェーズ 3.4a2 で配達済みの分を BB へ反映する。一時停止中 (stepSim が偽) の tick は配達しない (配送待ちは残る)。
- 配送待ち (tick N に積まれた分) は tick 境界をまたぐので **BT 節に入れる**。配達済みの分は tick N+1 の中だけ有効で、次の配達で入れ替わる。tick 末のスナップショットには入らない (復元後は空 = 次の配達で入れ替わるので、tick の中で読む限り連続実行と同じ)。
- 宛先・名前が正しくキューが溢れて捨てられた場合も、SendEvent ノードは Success (送りっぱなしの意味。警告 1 回で分かる)。
- イベントが 1 件も無い tick にハッシュは何も変わらない (中身ゲート)。

#### 4.1.7 C++ タスク / C# タスク

- C++: `REGISTER_BT_TASK(T, FIELDS(...))`。T は POD (trivially copyable、align ≤ 16)。`int32_t OnStart(MyeBtTaskContext&)` / `int32_t OnTick(MyeBtTaskContext&)` / `void OnAbort(MyeBtTaskContext&)` を持てる (戻り値 = Running / Success / Failure)。状態はノードのインスタンスごとに BT 表が持ち、BT 節に入る。`.bt.json` のノードは `{ "type": "CppTask", "params": { "task": "<名前>" }, "fields": {...} }` (sub-11 で task を params へ。C# タスクも同じ形で `params.class`)。状態 T は 112 バイトまで (ノードの追加状態は固定 128 バイト = ヘッダ 16 + 状態)、FIELDS に書かないメンバとパディングはコールバックの前後で 0 に戻る (ハッシュの一致のため)。ホットリロードは `REGISTER_SCRIPT` と同じく名前で引き直し、layoutHash が変わったら実行中のその木を最初からやり直す。DLL に無い名前は Failure + 1 回警告。
- C#: `[BtTask]` 属性のクラス (`MyeScript.cs` に基底)。C# レーンが止まる場面では即 Failure (2. #8)。

#### 4.1.8 Blackboard

- 型: Bool / Int / Float / Vector / Entity。キーは 1 アセット 64 個まで。名前は 1..63 バイト。ABI からはキー名のハッシュ (`MyeNameHash`) で引く。
- 値は「未設定」を区別する (各キーに set ビット)。初期値は `.bb.json` に書け、無ければ未設定。
- BB は `BehaviorTreeComponent` 1 個につき 1 つ (BT 表が持つ)。SubTree と共有。

#### 4.1.9 ライフサイクル

- `BehaviorTreeComponent` が付いた・有効になった tick に BB を初期値にして根から始める。無効になったら実行中を Abort してから止める (BB は保持)。外れた / エンティティが消えたら表から消す。
- BT アセットが ReloadHub で読み直されたら、その木を使っている全エンティティを Abort → BB を保ったまま根から (構造が変わっても安全)。BB アセットが読み直されたら BB も初期値に戻す。
- アセットが見つからない (GUID が未登録): 何もしない + 1 回警告 + コンポーネントの status を `AssetMissing`。

#### 4.1.10 巡回ルート

- `PatrolRouteComponent` (TypeId 79): `points[32]` (ローカル座標の XMFLOAT3)、`waitTicks[32]`、`pointCount`、`mode` (Loop / PingPong / Once)。ワールド位置 = エンティティのワールド行列 × 点。
- SceneView: 選んでいる間、点の球・点の間の線・向きの矢印・番号を描く。点をドラッグで動かせる (1 回のドラッグ = 1 Undo)。Inspector で点の追加・削除・並べ替え。Play 中は選ばなくても Patrol 中の Agent が向かっている点を強調 (デバッグ線、`drawDebug` 相当の NoHash フラグ)。

### 4.2 データ・保存形式・互換性

- `.bt.json`: `{ "engine": "MyEngine", "behaviortree": 1, "blackboard": "<bb の GUID>", "root": <node id>, "nodes": [ { "id", "type", "params": {...}, "decorators": [ {...} ], "children": [id...], "keys": { "<種類表のキー欄名>": "<BB のキー名>" }, "pos": [x, y] } ] }` (`keys` は sub-03 で追加。空文字 = 未指定で実行時 Failure)。種類キー必須 (`NavFilterLibrary.cpp:120-147` と同じ)。読み込みで Sanitize (未知の type・壊れた参照・循環は読み込み失敗として扱い、そのアセットは未登録 = AssetMissing)。
- `.bb.json`: `{ "engine": "MyEngine", "blackboard": 1, "keys": [ { "name", "type", "initial"?, "eventName"? } ] }`。
- 追加の手順はナビフィルタ (M84c) と同じ 24 か所 (AssetType の末尾追加 `BehaviorTree` / `Blackboard`、ClassifyPath、TypeName、kCompound、SimLibraries、EngineLoop / HeadlessSim、起動走査、ReloadHub、StageClassifier、DrawAssetRef、Create メニュー、アイコン、文字列、SelfTest)。
- コンポーネント:
  - `BehaviorTreeComponent` (TypeId 78): `tree` (AssetID)、`enabled`、`drawDebug` (NoHash)、読み取り専用 `status` (Idle / Running / Succeeded / Failed / AssetMissing)、`activeNodeId`、`lastAbortTick`。シーンに保存されるのは設定だけで十分だが、既存の作法 (知覚の結果と同じ) に従い全フィールドを World 節に置く。
  - `PatrolRouteComponent` (TypeId 79): 4.1.10。
- SimSnapshot: BT 節 (`'BT01'`) = エンティティキー順の (木の GUID、BB の値、実行中ノードの列、ノードごとのインスタンス状態、C++ タスクの状態バイト列) + イベントの配送待ち。空でも節は必ず書く (Nav と同じ)。`SimRefs` / `SimSourcesOf` / `SimSources` (末尾追加) に BT を足し、`HasHashableState` が偽なら何も畳まない。
- `kSimSnapshotVersion`: sub-01 で 34 (コンポーネント + BT 節)、以降コンポーネント・節を変えたサブで +1。`AcousticAudioSelfTest.cpp:129` の版の検査も同時に直す。
- ABI v27 (sub-11、1 回): 末尾に追加 (既存スロットの順序・シグネチャは変えない、`EngineAPI.h:13-15`)
  - `BtGetBlackboard(engine, e, keyHash, MyeBbValue* out)` / `BtSetBlackboard(engine, e, keyHash, const MyeBbValue*)` (`MyeBbValue` = POD `{ type, isSet, b, i, f, vec3, entity }`)
  - `BtSendEvent(engine, sender, target, nameHash, const MyeBtEventPayload*)` / `BtEventCount(engine, self)` / `BtGetEvent(engine, self, index, MyeBtEvent* out)`
  - `AnimatorPlay(engine, e, stateNameHash, durationTicks)`
  - `BtRestart(engine, e)` (根からやり直し)
  - `MyeScriptModule` に `btTaskCount` / `btTasks` を末尾追加 (レイアウト変更なので同じ bump に含める)、`MyeBtTaskDesc` と `MyeBtTaskContext` を `ScriptTypes.h` に。
  - C# ミラー (`Interop.cs` の位置ミラー、`MyeScript.cs` の糖衣)、`check_rules.ps1` の版表 (`27 = 151 + n`)、`docs\history\api-scripting-tools.md`。
  - 外部プロジェクトの GameLogic.dll は再ビルドが要る (bump の常)。

### 4.3 UI / ビジュアル

- BT 窓 (`src\Editor\Windows\AI\BehaviorTreeWindow.*`、新規。Window メニューと `.bt.json` のダブルクリックで開く)
  - 左: ノードのパレット (種類ごとに分類) と、選んだノードのパラメータ (Decorator の追加・並べ替え・削除を含む)。右: キャンバス (ImGui DrawList、パンとズーム)、上から下の木。ノードの箱の中に Decorator を帯で積む (UE と同じ見せ方)。
  - 操作: パレットからドラッグで追加、親の下端から子の上端へドラッグで接続、子の順序は x 座標の左から (UE と同じ。表示に番号)、Delete で削除、ノードのドラッグで移動、保存ボタンと未保存の印。
  - Undo / Redo: 窓の中に独自のスタック (Ctrl+Z / Ctrl+Y はこの窓がフォーカスを持つときだけ)。1 回の操作 (ドラッグ 1 回、パラメータの編集確定 1 回) = 1 段。
  - Blackboard パネル: 参照している `.bb.json` のキーの追加・削除・改名・型・初期値・eventName。BB 側も同じ Undo に乗る (保存は BT と BB を別々に)。
  - 検査エラー: SubTree の BB 不一致、LowerPriority の位置、Parallel の左がタスクでない、子の数の違反、未設定のキー参照、C# タスクを含む (警告)。ノードを赤枠にし、一覧を窓の下に出す。
  - ライブ表示: Play 中 (タイムライン操作中も) に選んだエンティティの実行中の経路を強調 (実行中 = 緑の太枠、直前に Abort したノードからの矢印 = 橙、30 tick で薄れる)、BB の現在値を BB パネルに並べる。
- SceneView: `drawDebug` が立った BT は、MoveTo の目的地・SearchArea の点・実行中タスク名をデバッグ線と文字で出す (知覚と同じ `AppendDebugLines`)。
- 文字列は全部 `LocalizationTable.inl` に en / ja の両方。

### 4.4 非機能

- 決定論: Debug / Release / Server.exe でビット一致。RNG は World::Rng のみ (FindRandomPoint / SearchArea / Wait の偏差)、BT が無い tick には引かない。ハッシュコンテナの走査順やポインタ値で順序を決めない (表はエンティティキー順、ノードは id 順)。
- 存在ゲート: BT を使わない既存の replay_verify の全ジョブがこの M85 の前後で PASS (golden の .rep を撮り直さない)。
- 性能: 100 体 × 30 ノードの木で BT フェーズが Release で 1 tick 0.5 ms 以下 (BehaviorTreeSelfTest で計測して ADR に書く)。
- `/fp:precise`、`rand()` 禁止、`_DEBUG` で状態を変えない。`/p:MyeWarnAsError=true` で 0 警告。

## 5. 受け入れ条件

1. `.bt.json` / `.bb.json` が Editor / Runtime / Server で起動時に読まれ、ReloadHub で読み直され、Asset Browser で作れて名前変更で拡張子が保たれる — `AssetDatabaseSelfTest` / `AssetOpsSelfTest` / `ReloadHubSelfTest` の追加項目 + `BehaviorTreeSelfTest`。
2. Composite 3 種の結果が 4.1.1 の表どおり (空の子・1 子・Parallel の Immediate / Delayed・背景のやり直し) — `BehaviorTreeSelfTest`。
3. BT 節: tick 50 で保存 → 新しいシステムへ復元 → tick 100 で連続実行とハッシュ一致 (BB・実行中ノード・Wait の残り・イベントの配送待ち・C++ タスク状態を含む)。BT の無いシーンではハッシュも RNG も変わらない — `BehaviorTreeSelfTest` + replay_verify の既存ジョブ全 PASS。
4. Decorator 5 種と Abort 4 種が 4.1.2 の表どおり (Self / LowerPriority / Both、Sequence の下の LowerPriority は Self 扱い、Abort の後始末の順序) — `BehaviorTreeSelfTest`。
5. `kBtMaxStepsPerTick` で無限ループの木が止まり、次の tick に続く — `BehaviorTreeSelfTest`。
6. Task 5 種が 4.1.3 どおり (MoveTo の Arrived / NoPath / Abort で停止 / 同じ地点へ 2 回 / observeTarget / Stuck で failOnStuck = false なら Running・true なら Failure で止まる、RotateTo の updateRotation の戻し) — `BehaviorTreeSelfTest` (NavMesh の固定ジオメトリ)。
7. AI ノード 4 種が 4.1.4 どおり (同距離は entity キー、名乗らない音、予測位置の吸着、FindTarget の陣営とタグ) — `BehaviorTreeSelfTest`。
8. イベントキュー: tick N 送信 → tick N+1 配送、順序、上限、BB への反映で Abort が起きる、tick N で保存 → 復元で配送待ちが残る — `BehaviorTreeSelfTest`。
9. PlayAnimation と `AnimatorPlay` (遷移と即切り替え、waitForEnd) と SubTree (BB 共有・不一致で Failure・入れ子 8 段) — `BehaviorTreeSelfTest` / `AnimatorControllerSelfTest`。
10. `PatrolRouteComponent` と Patrol (Loop / PingPong / Once、Abort 後に一番近い点から) — `BehaviorTreeSelfTest`。SceneView で点をドラッグでき 1 ドラッグ 1 Undo — 一時プローブの `--screenshot` と手順の記録。
11. BT 窓: ノードの追加・接続・並べ替え・削除・パラメータ・保存・再読込で位置が戻る、Undo / Redo、BB 編集、検査エラーの表示 — `BehaviorTreeEditorSelfTest` (窓のモデル層を ImGui 抜きで試す) + `--screenshot` の一時プローブ画像。
12. ライブ表示: Play 中と、タイムラインで巻き戻した tick で、実行中ノードの強調が正しい — 一時プローブの画像 2 枚 (Play 中・巻き戻し後)。
13. ABI v27: C++ タスク (状態がスナップショットに入る・ホットリロードで layoutHash 変更時にやり直し)、BB の読み書き、イベントの送受信、AnimatorPlay が GameLogic から動く — `BehaviorTreeSelfTest` の C++ タスク項目 + `--bt-demo` の replay 被覆。`check_rules.ps1` 0 件 (版表・C# ミラー・スロットの割り当て)。
14. C#: Interop のミラーと糖衣、C# タスクが Play で動き、記録中は Failure になる — 一時プローブで実走 (恒久テストは無し、C# は replay 被覆外)。
15. `--bt-demo`: 敵が巡回 → プレイヤーを発見して追う → 見失うと予測位置を捜索 → 諦めて巡回へ戻る、を 1 回の実行で通る (ログで各段階の tick を出す) — replay_verify の `bt` ジョブ (Debug / Release / Server.exe / snapshot stress) PASS、shot_verify の golden `bt` (既知の `nav` の FAIL は f6c7bef 起因として別扱い)。
16. Editor `--selftest` (Debug / Release)・Server `--selftest` で新しい FAIL 0。`/p:MyeWarnAsError=true` で 0 警告。`tools\check_rules.ps1` 0。
17. 文書: ADR-025 (実行モデル・状態の置き場所・イベントキュー・C# の保証外・性能の計測値・却下案)、engine_spec、engine-feature-guide、test_checklists、ロードマップの進捗表。

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | BT の核: アセット・コンポーネント・実行器・Composite 3 種・Wait・BT 節とハッシュ | なし | 1, 2, 3, 16 | `M85a: ビヘイビアツリーの核 — .bt/.bb アセット、BehaviorTree コンポーネント、Composite と BT 節 (snapshot v34)` |
| sub-02 | Decorator 5 種と Abort、手数の上限 | sub-01 | 4, 5, 16 | `M85b: BT の Decorator 5 種と Abort (Self / LowerPriority / Both)` |
| sub-03 | Task 4 種 (MoveTo / RotateTo / SetBlackboard / ClearBlackboard) | sub-02 | 6, 16 | `M85c: BT の Task — MoveTo / RotateTo / Set・ClearBlackboard` |
| sub-04 | AI ノード 4 種 | sub-03 | 7, 16 | `M85d: BT の AI ノード — FindRandomPoint / FindNearestTarget / SearchArea / FindTarget` |
| sub-05 | 汎用イベントキューと SendEvent | sub-04 (同じ BehaviorTreeSystem を触るので直列) | 8, 3, 16 | `M85e: sim の汎用イベントキューと BT の SendEvent` |
| sub-06 | AnimatorPlay と PlayAnimation、SubTree | sub-05 | 9, 16 | `M85f: アニメのステートを強制する AnimatorPlay、BT の PlayAnimation と SubTree` |
| sub-07 | 巡回ルート: PatrolRoute コンポーネント・ギズモ編集・Patrol タスク | sub-06 (直列) | 10, 16 | `M85g: 巡回ルート — PatrolRoute コンポーネントとギズモ編集、BT の Patrol` |
| sub-08 | BT 窓 (1): キャンバス・配置・接続・パラメータ・保存・Asset Browser から開く | sub-06, sub-07 | 11 (Undo と BB 以外), 16 | `M85h: BT エディタ — ノードの配置・接続・パラメータ編集と保存` |
| sub-09 | BT 窓 (2): Undo / Redo、Blackboard 編集、検査エラー | sub-08 | 11, 16 | `M85i: BT エディタの Undo と Blackboard の編集、検査エラーの表示` |
| sub-10 | BT 窓 (3): ライブ表示とタイムライン操作中の表示、SceneView のデバッグ線 | sub-09 | 12, 16 | `M85j: BT エディタのライブ表示 (Play 中と巻き戻し中)` |
| sub-11 | ABI v27: C++ タスク・BB・イベント・AnimatorPlay、C# ミラー | sub-10 | 13, 16 | `M85k: BT のスクリプト API と C++ タスク (ABI v27 = 151+n)` |
| sub-12 | C# タスクと C# の糖衣 | sub-11 | 14, 16 | `M85l: BT の C# タスクとイベント受信 (決定論の保証外)` |
| sub-13 | デモ `--bt-demo`、replay_verify `bt`、golden `bt` | sub-12 | 15, 16 | `M85m: BT のデモ (巡回・発見・追跡・捜索) と replay_verify / shot_verify` |
| sub-14 | 文書と全体検証 | sub-13 | 16, 17 | `M85n: ビヘイビアツリーの文書 (ADR-025) と全体検証` |

(ロードマップの 15 サブから: Wait を sub-01 へ (Composite を試す葉が要る)、グラフエディタ 4 本を 3 本へ、C++ Task とイベント受信を ABI の 1 本へまとめた)

## 7. 未決事項・リスク

- **最大のリスク = sub-01 の BT 節の復元一致** (実行中ノードの列とノードごとのインスタンス状態を、新しいシステムへ戻して連続実行と一致させる)。後続の全サブがこの表の形に乗るので、sub-01 で表の形 (ノードのインスタンス状態を「ノード id → 固定長の小さな POD + C++ タスク用の可変バイト列」で持つ等) を決め切る。
- UE の規則は記憶から書いた (2. #3)。
- `NavSystem` の到着済み Agent が同じ destination で再探索しない件 (MoveTo を同じ地点へ 2 回) — sub-03 で `hasDestination` の倒し → 立てで再探索されるか確かめ、されなければ NavSystem 側で「hasDestination が偽→真になったら要求し直す」を足す (Nav の挙動変更になるので `nav` / `perception` の replay ジョブで既存の不変を確認)。
- `ControllerAsset::defaultState` が実行時に効いていない (`AnimatorController.cpp:300-302`) — M85 では触らない。PlayAnimation のテストは state index を明示して書く。
- shot_verify の golden `nav` は f6c7bef 起因の既知の FAIL (ユーザー判断待ち)。sub-13 は `bt` だけを判定対象にし、`nav` は既知として報告する。
- C# タスクは決定論の被覆外 (2. #8)。
- ロードマップの「M85 の C++ Task のイベント受信」は ABI の 1 本 (sub-11) にまとめたので、sub-05 時点ではイベントの受け手は BT だけ。

## 8. 変更履歴

(確定後の変更のみ)

- 2026-10-06 (sub-11 VERDICT): ABI v27 = 158 (7 スロット)。CppTask の task 名は params へ、状態は 112 バイト上限の固定長、FIELDS 以外は 0 に戻す。OnStart が無ければ入った tick に OnTick、OnTick が無ければ Running、範囲外の戻り値は Failure。BT 節のバイト形式は不変なので snapshot は v39 のまま。BtRestart を自分の木のタスクから呼ぶと返った後に Abort
- 2026-10-06 (sub-10 VERDICT): Abort の矢印は Decorator 起点 (Self / LowerPriority / Timeout) だけ記録し、SimpleParallel の Immediate の停止は矢印にしない。記録は表示専用で BT 節・ハッシュに入れない (巻き戻し直後は矢印が空)。EngineContext に読み取り専用の BehaviorTreeSystem を追加。実行中タスク名は SceneView の ImGui 文字 (ギズモ表示が on のとき)。ライブの id は展開後の id を DisplayedIdOf で窓の木の id へ戻す
- 2026-10-06 (sub-09 VERDICT): Undo は「アセット全体の JSON」ではなく操作前後の BT / BB のコピー (保存できない途中の状態へも戻すため)、未保存の印は保存時の JSON との比較。BB のキー削除で BlackboardCondition のキー名は残して KeyMissing の検査にかける。必須のキー欄の定義 (KeyRequired) と検査の重さ (SubTree の未指定・未登録・根なしは警告、BB 違いはエラー、検査は保存を止めない) を coder の定義で確定。BB のキー改名・削除は編集中の木にだけ追従し、同じ BB を使うほかの木と BehaviorTreeComponent の Entity キー初期値は追従しない (既知の限界、ADR-025)。ReloadHub は内容が同じなら置き換えない
- 2026-10-06 (sub-08 VERDICT): 2. #19 / #20 はユーザー回答で確定。BT 窓にフォーカスがある間は Delete (と sub-09 の Ctrl+Z / Ctrl+Y) を窓が握る。「子を残す」削除は子を親なしの根として残す (Delete = 子を残す、Shift+Delete = 子ごと)。ノードのドラッグは子孫ごと、兄弟の順序は動かしたときだけ x で並べ直す (読み込み時は触らない)。保存は CheckSavable を通った木だけ。Git ゲートに BehaviorTreeDirty を追加。保存の直接登録と ReloadHub の再読込で木が 2 回やり直す件は sub-09 で「内容が登録済みと同じなら置き換えない」にする
- 2026-10-06 (sub-07 VERDICT): Patrol は入るたびに一番近い点から (2. #19)。親付きルートは前 tick の WorldMatrix。`BehaviorTreeComponent` に Entity キーの初期値 4 組を足す (2. #20、sub-08)。点のクリックは変形ギズモより優先、Inspector の点は専用 UI (1 操作 1 Undo)
- 2026-10-06 (sub-06 VERDICT): SubTree は「実行時の平らな展開」(部分木のノードを呼び出し側へ写し、実行木は 1 本、id は連番で振り直し) を採用。展開後 1024 ノードを超えたら SubTree を全部 Failure + 警告。部分木の根の Decorator の LowerPriority は親が SubTree ノードなので Self 扱い (UE も根に Decorator を置けない)。snapshot は v37 のまま。`AnimatorPlay` は ControllerLibrary を受け取り bool を返す (範囲外を拒否)。PlayAnimation の waitForEnd は ceil(クリップ長 / speed) tick (speed <= 0 は 1、クリップ無し・長さ 0 は即 Success)。遷移中の AnimatorPlay は遷移先と経過だけやり直す (ブレンド途中のポーズから飛ぶ、既知の限界)。ライブ表示 (sub-10) は展開後の id を元の木の (GUID, id) へ戻して表示する
- 2026-10-05 (sub-05 VERDICT): イベントの配達を「BT フェーズの冒頭」から「tick の頭 (スクリプトの Update より前)」へ変更 (4.1.6)。BtEvent に sentTick、BtParamType::String (63 バイト) を追加。上限 256 は積む時点の先着で数える。溢れても SendEvent は Success。送信は BehaviorTreeSystem のメンバ関数 (所有者経由、ABI は sub-11)
- 2026-10-05 (sub-04 VERDICT): SearchArea と Patrol に `failOnStuck` (既定 false) を追加 (2. #18)。FindNearestTarget で名乗らない音を選んだら Entity キーは未設定に戻し Vector だけ書く。BtParamType::Mask (64 ビット、16 進) を追加。BehaviorTreeSystem::Update は NavSystem を受け取る。FindNearestTarget の既定は 4 感覚とも true・currentlySensedOnly false、FindTarget の既定は radius 15・敵のみ・tagMask 0。SearchArea の吸着は水平 2 m / 垂直 4 m、着いた判定は水平 0.5 m、点の生成失敗はその点を消費して次の tick、「見えた」は同じ tick の percepts の視覚ビットで判定
- 2026-10-05 (sub-03 VERDICT): ノード JSON に `keys` (BB キー名の欄、種類表の keyNames) と GUID 型のパラメータ (16 進文字列) を追加。MoveTo の距離は水平 (XZ)、最初から acceptanceRadius 内なら Agent に書かず Success、目的地を書いた tick は status を読まない (SearchArea / Patrol も同じ規則)、observeTarget の既定 true、acceptanceRadius の既定 0.5。RotateTo は角速度 0 かつ Agent も 0 以下なら 360。SetBlackboard の Copy は同型・設定済みのみ。BehaviorTreeComponent が外れたら表を落とす前に Abort。種類ごとの追加状態 (BtNodeTypeInfo::extraStateBytes) を導入し snapshot v35。spec 7. の「同じ地点への MoveTo」のリスクは NavSystem 無変更で解消
- 2026-10-05 (sub-02 VERDICT): Decorator の細部を確定 — 条件は入るとき 1 回だけ評価 (Repeat の周回では再評価しない)、Timeout は「入った tick + ticks」で切れ同じ tick に子が終われば終わりが優先、Repeat の内側の Timeout は周回ごと、Cooldown は終了 / Abort の tick + ticks から入れる、未設定・型違いの大小比較は偽、LowerPriority は偽→真に変わった tick だけ働く (OnResultChange)、条件偽で入らなかったノードは Abort の記録なし、根のやり直しで Decorator の状態 (Cooldown の計時) は残す。監視 (4.1.1 の (2)) は手数の上限に数えない。Decorator の JSON は `{"type","key","params"}`、1 ノード 8 個まで
- 2026-10-05 (sub-01 VERDICT): SearchArea の点は事前に列で持たず、向かい始める時に 1 つずつ生成する (状態を固定長にするため、pointCount 上限 32)。ノードの追加状態は種類ごとの固定長領域 (sub-03 で導入) に置く。sub-01 が足した上限 (ノード 1024・深さ 64・tick パラメータ 216000) を仕様として承認
- 2026-10-05 (ユーザー回答、司会経由): MoveTo の Stuck の扱いを「常に Running」から「パラメータ `failOnStuck` (既定 false) で選ぶ」へ変更。4.1.3・2. #17・受け入れ条件 6・sub-03 を更新。ほか 4 件 (2. #1 / #9 / #10、4.1.1 の根のやり直し) は裁定どおりで確定
