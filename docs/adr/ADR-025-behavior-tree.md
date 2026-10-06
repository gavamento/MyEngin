# ADR-025: ビヘイビアツリー (BT + Blackboard) — 実行状態をシステムの表で持つ

- 状態: **確定** (2026-10-06、M85n)
- 出所: 依頼「M85 の実装」。計画は `plans\ai-roadmap-m83-m86.md` の M85 節、仕様は `plans\m85-behavior-tree\spec.md`。
- 実体: `src\Engine\Engine\AI\` の `BehaviorTreeLibrary.{h,cpp}` (.bt.json とノード種類の表)、`BlackboardLibrary.{h,cpp}`
  (.bb.json)、`BehaviorTreeSystem.{h,cpp}` (実行器)、`BtTaskRegistry.{h,cpp}` (C++ タスク)、`BtManagedTaskLane.h` (C# タスク)、
  `BehaviorTreeSelfTest.{h,cpp}`。`Components.h` の `BehaviorTreeComponent` (TypeId 78) と `PatrolRouteComponent` (TypeId 79)。
  エディタは `src\Editor\Windows\AI\BehaviorTreeWindow.*`。
- 番号: ABI v27 = 158 スロット、`kSimSnapshotVersion` 39 (v34 BT 節とコンポーネント → v35 ノードの追加状態 → v36 SearchArea →
  v37 イベントキュー → v38 PatrolRoute → v39 Entity キーの初期値)。

## 背景

NavMesh (M82 / M84) と知覚 (M83) で「歩く」「見つける」ができるが、「見張る → 見つけたら追う → 見失ったら探す → 巡回へ戻る」の
組み立ては、固定 5 状態の `AgentBrain` かスクリプトを毎回書くしかない。UE の Behavior Tree + Blackboard に相当する、
アセットとして組めて、決定論 (Debug / Release / Server のビット一致・リプレイ・What-if・ロールバック) を崩さない仕組みを作る。
`AgentBrain` は変更せず共存させる (Inspector が同居に警告を出す)。

## 決定 1: 実行状態は BehaviorTreeSystem の表に置き、SimSnapshot に BT 節を足す

ADR-024 の決定 2 (状態は全部コンポーネント) は「固定長に収まる」から成り立った。BT は木のノード数・ブラックボードのキー数・
SubTree の入れ子・C++ タスクの状態の大きさがアセットごとに違い、固定長のコンポーネントにすると上限が木の設計を縛る
(64 ノード × 数十バイト + ブラックボード 64 キー × 数十バイトだけで 1 体 4 KB 近く、シーンファイルにも実行状態のゼロ列が並ぶ)。
ADR-024 と結論が違うのは前提 (固定長に収まるか) が違うためで、ADR-024 の決定が誤りだったわけではない。

- コンポーネント (`BehaviorTree`、TypeId 78) は設定 (`tree` / `enabled` / `drawDebug` / Entity キーの初期値) と、システムが毎 tick 写す
  表示用の値 (`status` / `activeNodeId` / `lastAbortTick`) だけを持つ。表示用の値も sim 状態として World 節に入る。
- 実行状態は `BehaviorTreeSystem::instances_` (エンティティキー昇順の `BtInstance` の列)。SimSnapshot の `'BT01'` 節
  (NAV 節の後・World 節の前)、`SimSources::behaviorTree` (ハッシュ)、`SimRefs::behaviorTree` の 3 点セットで運ぶ。
- ハッシュは内容ゲート: 表もイベントの配送待ちも空 (BehaviorTree を使わないシーン) なら何も畳まない。`StateHash` は BT 節と同じ
  バイト列の FNV-1a で、書式と畳み込みが食い違わない。BT を使わない既存の replay_verify ジョブのハッシュは M85 の前後で変わらない。
- **`activeNodeId` は SubTree を展開した後の実行木のノード id**。アセット上の id と一致するのは SubTree を含まない木だけで、
  部分木のノードは展開時に連番へ振り直される (決定 9)。BT 窓は (GUID, 元の id) の導出値へ戻して表示する。ABI (`BtGetBlackboard`
  などはノード id を返さない) と文書の側では展開後の id と読むこと。
- 却下: 全部コンポーネント。上限が木の大きさを縛り、C++ タスクの可変長の状態が入らない。

## 決定 2: 表の形

`BtInstance` = エンティティ + 木の GUID + 根の結果 (`rootStatus`) + ブラックボードの値の列 + ノードの状態の列 + 種類別の追加状態。

- ノードの状態 `BtNodeState` は**全ノード共通の固定長 POD**。木の `stateSlotCount` 個の欄を持ち、欄の並びは
  [ノード 0..N-1][Decorator (ノード順)]。使い方は種類ごと (Selector / Sequence = 今の子、Wait = 残り tick、SimpleParallel = メインが
  終わったか・メインの結果、Cooldown = 入れるようになる tick、Repeat = 終えた回数、Timeout = 打ち切る tick)。
- 欄に収まらない状態 (MoveTo の目的地、SearchArea の点、Patrol の次の点、C++ タスクの状態) は `BtNodeTypeInfo::extraStateBytes` が
  決める種類別の追加状態 (`BtNodeDef::extraOffset`、1 ノード 128 バイトまで)。パディングを持たない 4 / 8 バイト揃えの構造体で、
  足したら BT 節の書式と `kSimSnapshotVersion` を上げる。後始末は `ReleaseBody` の switch に足す。
- ブラックボードは `BbValue` (`isSet` + int / float / vec3 / entity) の列。型は木が指す `.bb.json` のキーが持ち、値は型に依らず
  同じ POD にした (型ごとに別表にするとキー順の添字が崩れる)。
- 木と BB の定義 (`shared_ptr<const BehaviorTreeAsset>` / `shared_ptr<const BlackboardAsset>`) は実行時の導出値で BT 節に入れない。
  復元後に `ApplySnapshot` がライブラリから引き直し、ノード数・キー数が保存時と違えば (アセットが編集された) その体だけ初期状態から
  やり直して警告する。
- 却下: ノードの状態を種類ごとの型 (variant) にする案。節の書式が種類の数だけ増え、`ReadSnapshot` の検証が種類ごとに要る。
- 却下: ノードの実行状態を「アクティブなノードの経路 (スタック)」で持つ案。Parallel の 2 本の枝が同時に生きるので経路が木になり、
  結局ノードごとの欄が要る。

## 決定 3: 実行モデルは UE 方式 (実行中のノードを持ち続ける) + ポーリングの監視

- 毎 tick 根から評価し直さず、`active` なノードだけを根から辿って再訪する (`Visit`)。複合ノードは自分の今の子だけを訪ねる。
- 結果は Running / Success / Failure。根が終わった tick の結果が `status` に出て、**次の tick に**根からやり直す
  (UE は同じ tick で再開するが、即終了する木が 1 tick に何周もするのを避ける)。
- Decorator の Abort は UE の Observer Aborts の OnResultChange 相当 (None / Self / LowerPriority / Both)。UE のコールバックでなく、
  **毎 tick の BT フェーズ冒頭で、監視中の Decorator を木の優先順 (深さ優先・前順) に全部評価する (ポーリング)**。
  BB の書き換え元 (スクリプト / ABI / イベント / 他ノード) と書いたタイミングに結果を依存させないため。LowerPriority / Both は
  親が Selector のときだけ有効 (エディタが検査エラー、実行時は Self 扱い)。Task が BB に書いた値は次の tick の監視で反映される。
- 却下: BB 書き込み時の即時コールバック。同じ tick の中で書いた順に Abort が走り、スクリプトの実行順で結果が変わる。
- 却下: 毎 tick 根から評価し直す方式。Observer Aborts は実行中ノードを持つモデルでしか意味を持たない。
- 1 体 1 tick のノード訪問の手数の上限 `kBtMaxStepsPerTick = 256`。超えたらその tick はそこで止め (入っていないノードは Running で返し、
  次の tick に親が入り直す)、1 体につき 1 回だけ警告する。時間で打ち切ると機種で結果が変わるので件数にした。
- Abort は `AbortNode`: 実行中のノードの子孫を先に (深い方から) 抜けてから自分を抜ける。ノード固有の後始末 (MoveTo の停止、
  C++ タスクの `onAbort`、Cooldown の計時開始) はこの関数に足す。SimpleParallel の Immediate が背景を止める、無効化、木の読み直し、
  Decorator の Abort がこの経路を通る。
- 処理順はエンティティキー (index → generation) 昇順で 1 体ずつ最後まで。BT がある tick にだけ World の RNG を引く
  (Wait の偏差が 0 でなければ入った tick に 1 回、FindRandomPoint / SearchArea)。
- `BtRestart` (ABI) を自分の木のタスクから呼ぶと、そのタスクが返った後に Abort する (実行中のノードの内側から木を壊さない)。

## 決定 4: アセットとノード種類の表

- `.bt.json` (木 + ノードの表示位置) と `.bb.json` (キーの名前・型・初期値・eventName) は独立したアセットで、木がブラックボードを GUID
  1 つで参照する。ライブラリの作りは `NavFilterLibrary` と同じ (GUID キー、起動走査・ReloadHub・Create メニュー・ステージ分類)。
- **ノードの位置を `.bt.json` に持たせるので、見た目だけの変更でも provenance の contentHash は変わる** (Net の接続・リプレイの
  ヘッダ差分に出る)。別ファイルに分けても `assets\` 以下の全ファイルを畳むので contentHash は動き、利点が無い。UE もグラフは
  アセットに入る。ユーザー判断 (2026-10-05) で確定。
- ノードの種類は `BtNodeTypeInfo` の表 (名前・分類・子の数の範囲・パラメータ記述子・キー欄名) を正本とし、読み書きの検査と
  エディタが同じ表を引く。ファイルには種類の名前で保存するので、`BtNodeKind` の並びは保存形式に出ない。種類は 18
  (Composite 3 / Task 5 / AI 4 / Gameplay 2 (PlayAnimation・SendEvent) / Tree 1 / Patrol / CppTask / CsTask) と Decorator 5
  (BlackboardCondition / Invert / Cooldown / Repeat / Timeout)。
- 読み込み (`BtLinkAsset`) は未知の type・id の重複・存在しない子・子の数の違反・親が 2 つ・循環・深すぎ (`kBtMaxDepth` 64)・
  根が親を持つ、を失敗にする。パラメータは型の違い・未知の列挙名を失敗、範囲外の数値を範囲へ丸める。失敗した木は未登録
  (BehaviorTree コンポーネントは `AssetMissing`)。
- 実行器は木を `shared_ptr` で持つ。ReloadHub が同じ GUID を読み直して登録を置き換えても、古い木のまま実行中のノードを Abort できる
  (新しい木の形で古い状態を Abort すると添字がずれる)。置き換えの検出は `shared_ptr` の同一性。ReloadHub は読み直した内容が
  同じなら置き換えない (保存直後に木が 2 回やり直すのを避ける)。

## 決定 5: ライフサイクル

- 有効なコンポーネントが付いた tick に表を作り (ブラックボードは初期値、Entity キーの初期値 4 組を書く)、根から始める。
  `enabled` を落とす・親が無効になると実行中を Abort して Idle にし、ブラックボードは表に残す。コンポーネントが外れる・
  エンティティが消えると表から消す。
- 木 (または使うブラックボード) が引けない: 何もせず 1 回警告し `status = AssetMissing`。木が未設定なら Idle で警告しない。
- 木が読み直されたら Abort して根から (ブラックボードは保つ)。ブラックボードが読み直されたらブラックボードも初期値へ戻す。
  「実行中の位置を保ったまま差し替える」ことはしない。部分木アセットを読み直したときも、取り込んでいる親の木のエンティティは
  Abort → 根からやり直しになる。
- シーン遷移は TickRunner が `Reset` する。Play の開始・終了 (エディタがシーンを読み直す) は TickRunner を通らないが、
  `Scene::Clear` → `World::Clear` が使った index の世代を進めるので、同じ index に作り直したエンティティは前の表のキーと一致せず、
  表の走査で落ちる (専用の検出は要らない)。
- BB の Entity キーをシーンの物で埋める手段として、`BehaviorTreeComponent` に「Entity キーの初期値」を 4 組持たせた
  (UE で Pawn ごとに BB を初期化するのに相当、ユーザー判断 2026-10-06、snapshot v39)。

## 決定 6: 性能

計測は `BehaviorTreeSelfTest` (100 体 × 30 ノード、Wait と Parallel と Sequence の木、200 tick の平均)。
受け入れ基準は Release で BT フェーズ 1 tick 0.5 ms 以下。

| | Release | Debug |
|---|---|---|
| `Update` (BT フェーズ) | 37.5 µs / tick | 1502 µs / tick |
| `StateHash` (ハッシュを取る記録・照合・ネットの検査 tick だけ) | 121 µs / tick | 1932 µs / tick |

2026-10-06 (M85 完了時、`Editor.exe --selftest` の `[perf] 100 trees x 30 nodes` の 1 回分。ほかの計測と同じ実行の中なので
ばらつく)。sub-01 の時点は Release 17 µs / 109 µs、Debug 794 µs / 1705 µs で、`Update` が約 2 倍になった (ノードの機能が
増えた後の値。1 回の計測で原因の切り分けはしていない)。Release の `Update` は基準 (0.5 ms) の 1/13 以下。この木は Wait が長く 1 体 1 tick に
訪ねるノードが数個 (実行中の経路だけを再訪する) なので、MoveTo のクエリ・FindRandomPoint・SearchArea のように重いノードが
並ぶ木の計測はしていない (未検証)。

## 決定 7: 汎用イベントキュー

- イベント = POD `{ nameHash, sender, target (null = 全体), vec3, float, int32, seq }`。送る手段は BT の SendEvent ノードと
  ABI `BtSendEvent` (C++ / C#)。tick N のどこで送っても **tick N+1 の頭に配る** (`TickRunner` のフェーズ 3 直前、`stepSim` の中で
  `DeliverPending`)。BT の `Update` は配達済みの分を BB へ反映するだけ。BT より前に走るスクリプトの Update が配達分を読めるように
  するため。一時停止中の tick は配達しない (配送待ちは残る)。
- 順序は送信元のエンティティキー → 送信順 (seq)。1 tick の上限 `kBtMaxEventsPerTick = 256`、溢れた分は捨てて 1 回だけ警告
  (件数で決めるので決定論)。溢れて捨てた場合も SendEvent ノードは Success (送りっぱなし)。
- 受け手: `.bb.json` のキーに `eventName` を書くと、その名前のイベントが自分宛て・全体宛てに届いたらそのキーへ書く
  (同じ tick に複数届いたら配送順の最後が勝つ)。C++ / C# は `BtEventCount` / `BtGetEvent` で読む (読めるのは配達された tick の中だけ)。
- 配送待ちは tick 境界をまたぐので BT 節に入れる。配達済みの分は入れず、復元後は空 (次の配達で入れ替わるので連続実行と一致)。
- 却下: 配達を BT の `Update` の中に置く案。BT より前のスクリプトが読めず、tick N+1 のはずの配達が tick N+2 に見える。

## 決定 8: C++ タスクの状態の扱い、C# タスクは決定論の保証外

- C++: `REGISTER_BT_TASK(T, FIELDS(...))`。T は POD (trivially copyable、align ≤ 16)。状態はノードのインスタンスごとに BT 表が持ち
  BT 節に入る (被覆内)。**状態は 112 バイトまで** (追加状態は固定 128 バイト = ヘッダ 16 + 状態)。`FIELDS` に書いたメンバは
  `.bt.json` の `"fields"` から BT 窓で編集でき、**書かないメンバとパディングはコールバックの前後で 0 に戻す**
  (`BtTaskCanonicalizeState`。ハッシュのバイト一致のため)。ホットリロードは名前で引き直し、`layoutHash` が変わったら実行中の
  その木を最初からやり直す。DLL に無い名前は Failure + 1 回警告。
- C#: `[BtTask]` を付けたクラス。C# レーンは状態が World の外にあり、記録・検証・Net・再シムでは走らない。レーンが止まっている
  場面は即 Failure + 1 回警告、BT 窓と Inspector に「決定論の保証外」と出す。ノードごとの `fields` は C++ と同じ形で、
  入るたびに JSON で渡す。リロード中に動いていた C# タスクは次の tick に `OnStart` から (fields 付きで) やり直す。
  C# タスクを含む木は replay_verify の対象に入れない。
- ABI v27 = 158 スロット: `BtGetBlackboard` / `BtSetBlackboard` / `BtSendEvent` / `BtEventCount` / `BtGetEvent` / `AnimatorPlay` /
  `BtRestart` の 7 本 + `MyeScriptModule` 末尾の `btTaskCount` / `btTasks`。`MyeManagedVTable` の `BtTask` / `BtTaskCatalog` は
  エンジンと `MyeScripting.dll` の内部契約で、EngineAPI の版は動かさない (外部プロジェクトの `MyeScripting.dll` は exe と
  同時ビルドが前提)。外部プロジェクトの `GameLogic.dll` は v27 で再ビルドが要る。

## 決定 9: SubTree は平らな展開

- SubTree は実行器が木を登録から引くとき (`BtExpandSubTrees`) に、部分木のノードを親の木の実行用の複製へ連番で振り直して取り込む
  (ファイル上の SubTree は子を持たない。入れ子は `kBtMaxSubTreeDepth` 8 段、
  全体は `kBtMaxNodes` 1024 ノードまで。超えたら Failure + 警告、自分自身を含む循環もこれで止まる)。実行器は SubTree を
  特別扱いせず、`BtNodeState` の欄も同じ表で足りる (入れ子の状態を BT 節に持たなくてよい)。
- 部分木は親と**同じ BB アセット**でなければ検査エラー・実行時 Failure (UE の親 BB の派生は許さない。BB の継承は作らない)。
- 部分木の根に付けた LowerPriority は Self 扱い。
- 却下: 部分木ごとに実行状態を入れ子で持つ案。BT 節の書式が再帰になり、`ReadSnapshot` の検証が重い。

## 決定 10: 巡回ルートと Patrol

- `PatrolRouteComponent` (TypeId 79) は最大 32 点 (ローカル座標) + 点ごとの `waitTicks` + `mode` (Loop / PingPong / Once)。
  回るのは Patrol ノード (BT 無しの巡回は作らない = 目的地を書く主体を Nav と BT の 2 つにしないため)。
- 次の点はルート側でなく Patrol ノードのインスタンス状態に持つので、ルートを複数の敵が共有できる。**入るたびに一番近い点から**
  (初回も同じ、同距離は index 小。ユーザー判断 2026-10-06)。親付きルートは前 tick の WorldMatrix で世界座標へ。

## UE との違い

| 項目 | UE | M85 | 理由 |
|---|---|---|---|
| Abort の監視 | BB 変更のコールバック | 毎 tick 冒頭のポーリング | 決定論 (書き込み元の順序に依らない) |
| 根が終わった後 | 同じ tick で再開 | 次の tick に根から | 即終了する木が 1 tick に何周もするのを避ける |
| Service | あり | なし | スコープ外 (ユーザー選択) |
| Decorator の通知 | OnResultChange / OnValueChange | OnResultChange のみ | スコープ外 |
| Decorator の Inverse | 条件 Decorator のフラグ | 独立の Invert ノード | Unity Behavior の Inverter に揃えた |
| BB | 継承・Instance Synced あり、型が多い | 継承なし、Bool / Int / Float / Vector / Entity | 平らな展開と BT 節の単純さ |
| SubTree | Run Behavior (BB 派生可) | 同じ BB 必須・平らな展開 | 決定 9 |
| MoveTo の Stuck | 詰まっても失敗しない | 既定は同じ (Running)、`failOnStuck` で Failure | ユーザー判断 (2026-10-05) |
| 無限ループ | 起こり得る | 手数の上限 256 で止める | 決定論 |
| 巡回 | なし (標準では自作) | Patrol ノード + PatrolRoute | M85 のスコープ |
| C# / C++ のカスタムタスク | Blueprint / C++ | `REGISTER_BT_TASK` (被覆内) / `[BtTask]` (保証外) | 決定 8 |

## 既知の限界・未検証

仕様どおりにせず・確かめていないものを隠さないために一覧にする。

**UE との対応は未照合**
- UE の規則 (Observer Aborts、Loop、Cooldown、Simple Parallel、TimeLimit) は UE 5 の公式ドキュメントの**記憶**に基づく。
  Web で照合していない。特に Loop が子の Failure で抜ける点、Cooldown が Abort された後も計時を始める点、Simple Parallel の
  背景が先に終わったときのやり直しは未検証 (本 ADR の「UE との違い」は記憶ベースの対応表)。

**テストの検出力**
- FindNearestTarget の同距離タイブレークのテストの検出力は薄い (実装を壊しても落ちない可能性)。
- 親付き Agent への RotateTo は未検証。
- AnimatorPlay の true 経路 (遷移が実際に始まる) を C# から確認していない。
- 遷移中の AnimatorPlay はブレンド途中のポーズから飛ぶ (今のポーズから `durationTicks` で移るため、見た目に段差が出る)。

**実装の限界**
- SubTree は平らな展開 (上限 1024 ノード・8 段)、部分木の根の LowerPriority は Self 扱い、BB の継承なし。
- Patrol は入るたびに最近傍点から (巡回の続きから再開はしない)。
- ReloadHub が登録を置き換えると、登録のパス・名前が小文字になる。
- BB のキーの改名・削除は編集中の木だけ追従する。ほかの木・Entity キーの初期値は追従しない。
- C++ タスクの状態は 112 バイトまで。`FIELDS` に書かないメンバは保持されない (コールバックの前後で 0 へ戻る)。
- BB の ABI 書き込み (`BtSetBlackboard`) は BT のインスタンスが無い tick には効かない (0 を返す)。
- AnimatorPlay の名前引きは先頭の同名ステート。
- `BtRestart` を自分の木のタスクから呼ぶと、タスクが返った後に Abort する。
- 死んだエンティティの C# タスクのインスタンスの掃除は enter のときだけ。
- 性能は Wait 主体の木でしか測っていない (決定 6)。

**デバッグ表示**
- 巻き戻し直後は Abort の矢印が空。SimpleParallel の Immediate の停止は矢印にしない。デバッグ線は再シム中は積まない。
- 未保存編集中の窓では、増えたノードがライブ強調されない (木が窓と実行器で食い違う)。
- What-if の分岐 (非ライブ側) の BT 表示はしない。

**C# タスク (決定論の保証外)**
- C# タスクのインスタンスは巻き戻しで戻らず、リロード・シーン遷移で捨てられる。
- C# レーン停止の警告は GameLogic のホットリロードで再び 1 回出る。
- C# タスクの fields は enter / tick の毎回 JSON で渡す。リロード中に動いていたタスクは次の tick に `OnStart` から (fields 付きで)
  やり直す。
- `MyeManagedVTable` の `BtTask` / `BtTaskCatalog` は内部契約で、外部プロジェクトの `MyeScripting.dll` は exe と同時ビルドが前提。

**検証環境**
- `replay_verify` の並列 cold cook で起動中に落ちる / 無反応が 1 回ずつ出たことがある (M85 起因の根拠はないが原因は未特定)。
- 画面の実操作 (BT 窓の編集・SceneView のギズモ) の目視確認はユーザー作業。`docs\test_checklists.md` に項目を置いた。
