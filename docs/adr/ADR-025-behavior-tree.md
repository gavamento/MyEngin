# ADR-025: ビヘイビアツリー (BT + Blackboard) — 実行状態をシステムの表で持つ

- 状態: **下書き** (2026-10-05、M85a。後続サブ (b〜n) が決定を足し、M85n で確定)
- 出所: 依頼「M85 の実装」。計画は `plans\ai-roadmap-m83-m86.md` の M85 節、仕様は `plans\m85-behavior-tree\spec.md`。
- 実体: `src\Engine\Engine\AI\` の `BehaviorTreeLibrary.{h,cpp}` (.bt.json とノード種類の表)、`BlackboardLibrary.{h,cpp}`
  (.bb.json)、`BehaviorTreeSystem.{h,cpp}` (実行器)、`BehaviorTreeSelfTest.{h,cpp}`。`Components.h` の `BehaviorTreeComponent`

## 背景

NavMesh (M82 / M84) と知覚 (M83) で「歩く」「見つける」ができるが、「見張る → 見つけたら追う → 見失ったら探す → 巡回へ戻る」の
組み立ては、固定 5 状態の `AgentBrain` かスクリプトを毎回書くしかない。UE の Behavior Tree + Blackboard に相当する、
アセットとして組めて、決定論 (Debug / Release / Server のビット一致・リプレイ・What-if・ロールバック) を崩さない仕組みを作る。

## 決定 1: 実行状態は BehaviorTreeSystem の表に置き、SimSnapshot に BT 節を足す

ADR-024 の決定 2 (状態は全部コンポーネント) は「固定長に収まる」から成り立った。BT は木のノード数・ブラックボードのキー数・
(後続サブの) SubTree の入れ子・C++ タスクの状態の大きさがアセットごとに違い、固定長のコンポーネントにすると上限が木の設計を縛る
(64 ノード × 数十バイト + ブラックボード 64 キー × 数十バイトだけで 1 体 4 KB 近く、シーンファイルにも実行状態のゼロ列が並ぶ)。

- コンポーネント (`BehaviorTree`、TypeId 78) は設定 (`tree` / `enabled` / `drawDebug`) と、システムが毎 tick 写す表示用の値
  (`status` / `activeNodeId` / `lastAbortTick`) だけを持つ。表示用の値も sim 状態として World 節に入る。
- 実行状態は `BehaviorTreeSystem::instances_` (エンティティキー昇順の `BtInstance` の列)。SimSnapshot の `'BT01'` 節
  (NAV 節の後・World 節の前)、`SimSources::behaviorTree` (ハッシュ)、`SimRefs::behaviorTree` の 3 点セットで運ぶ。
- ハッシュは内容ゲート: 表が空 (BehaviorTree を使わないシーン) なら何も畳まない。`StateHash` は BT 節と同じバイト列の FNV-1a で、
  書式と畳み込みが食い違わない。
- 却下: 全部コンポーネント。上限が木の大きさを縛り、後続のノード (C++ タスクの可変長の状態) が入らない。

## 決定 2: 表の形

`BtInstance` = エンティティ + 木の GUID + 根の結果 (`rootStatus`) + ブラックボードの値の列 + ノードの状態の列。

- ノードの状態 `BtNodeState` は**全ノード共通の固定長 POD** (`active` / `phase` / `child` / `counter` の 10 バイト)。木の
  `stateSlotCount` 個の欄を持ち、ノードの添字がそのまま欄の添字。使い方は種類ごと (Selector / Sequence = 今の子、Wait = 残り tick、
  SimpleParallel = メインが終わったか・メインの結果)。欄が足りなくなった種類 (MoveTo の目的地、Patrol の次の点など) は
  `BtNodeState` に名前付きの欄を足し、BT 節の書式と `kSimSnapshotVersion` を上げる。
- ブラックボードは `BbValue` (`isSet` + int / float / vec3 / entity) の列。型は木が指す `.bb.json` のキーが持ち、値は型に依らず
  同じ POD にした (型ごとに別表にすると書式が増え、キー順の添字が崩れる)。
- 木と BB の定義 (`shared_ptr<const BehaviorTreeAsset>` / `shared_ptr<const BlackboardAsset>`) は実行時の導出値で BT 節に入れない。
  復元後に `ApplySnapshot` がライブラリから引き直し、ノード数・キー数が保存時と違えば (アセットが編集された) その体だけ初期状態から
  やり直して警告する。
- 却下: ノードの状態を種類ごとの型 (variant) にする案。節の書式が種類の数だけ増え、`ReadSnapshot` の検証が種類ごとに要る。
  欄の使い方が種類ごとに違うのは `BehaviorTreeSystem.cpp` の各 `Visit*` が正本とし、読み手の少ない状態は固定欄で足りる範囲に収めた。
- 却下: ノードの実行状態を「アクティブなノードの経路 (スタック)」で持つ案。Parallel の 2 本の枝が同時に生きるので経路が木になり、
  結局ノードごとの欄が要る。

## 決定 3: 実行モデルは UE 方式 (実行中のノードを持ち続ける)

- 毎 tick 根から評価し直さず、`active` なノードだけを根から辿って再訪する (`Visit`)。複合ノードは自分の今の子だけを訪ねる。
- 結果は Running / Success / Failure。根が終わった tick の結果が `status` に出て、**次の tick に**根からやり直す
  (UE は同じ tick で再開するが、即終了する木が 1 tick に何周もするのを避ける)。
- 1 体 1 tick のノード訪問の手数の上限 `kBtMaxStepsPerTick = 256`。超えたらその tick はそこで止め (入っていないノードは Running で返し、
  次の tick に親が入り直す)、1 体につき 1 回だけ警告する。時間で打ち切ると機種で結果が変わるので件数にした。
- Abort は `AbortNode`: 実行中のノードの子孫を先に (深い方から) 抜けてから自分を抜ける。ノード固有の後始末はこの関数に足す。
  SimpleParallel の Immediate が背景を止める、無効化、木の読み直しがこの経路を通る (Decorator の Abort は M85b)。
- 処理順はエンティティキー (index → generation) 昇順で 1 体ずつ最後まで。BT がある tick にだけ World の RNG を引く
  (Wait の偏差が 0 でなければ入った tick に 1 回)。

## 決定 4: アセットとノード種類の表

- `.bt.json` (木 + ノードの表示位置) と `.bb.json` (キーの名前・型・初期値・eventName) は独立したアセットで、木がブラックボードを GUID
  1 つで参照する。ライブラリの作りは `NavFilterLibrary` と同じ (GUID キー、起動走査・ReloadHub・Create メニュー・ステージ分類)。
  ノードの位置を `.bt.json` に持たせるので、見た目だけの変更でも provenance の contentHash は変わる。
- ノードの種類は `BtNodeTypeInfo` の表 (名前・分類・子の数の範囲・パラメータ記述子) を正本とし、読み書きの検査と後続のエディタが
  同じ表を引く。ファイルには種類の名前で保存するので、`BtNodeKind` の並びは保存形式に出ない。
- 読み込み (`BtLinkAsset`) は未知の type・id の重複・存在しない子・子の数の違反・親が 2 つ・循環・深すぎ (`kBtMaxDepth` 64)・
  根が親を持つ、を失敗にする。パラメータは型の違い・未知の列挙名を失敗、範囲外の数値を範囲へ丸める。失敗した木は未登録
  (BehaviorTree コンポーネントは `AssetMissing`)。
- 実行器は木を `shared_ptr` で持つ。ReloadHub が同じ GUID を読み直して登録を置き換えても、古い木のまま実行中のノードを Abort できる
  (新しい木の形で古い状態を Abort すると添字がずれる)。置き換えの検出は `shared_ptr` の同一性。

## 決定 5: ライフサイクル

- 有効なコンポーネントが付いた tick に表を作り (ブラックボードは初期値)、根から始める。`enabled` を落とす・親が無効になると
  実行中を Abort して Idle にし、ブラックボードは表に残す。コンポーネントが外れる・エンティティが消えると表から消す。
- 木 (または使うブラックボード) が引けない: 何もせず 1 回警告し `status = AssetMissing`。木が未設定なら Idle で警告しない。
- 木が読み直されたら Abort して根から (ブラックボードは保つ)。ブラックボードが読み直されたらブラックボードも初期値へ戻す。
- シーン遷移は TickRunner が `Reset` する。Play の開始・終了 (エディタがシーンを読み直す) は TickRunner を通らないが、
  `Scene::Clear` → `World::Clear` が使った index の世代を進めるので、同じ index に作り直したエンティティは前の表のキーと一致せず、
  表の走査で落ちる (専用の検出は要らない)。

## 決定 6: 性能

計測は `BehaviorTreeSelfTest` (100 体 × 30 ノード、Wait と Parallel と Sequence の木、200 tick の平均)。
受け入れ基準は Release で BT フェーズ 1 tick 0.5 ms 以下。

| | Release | Debug |
|---|---|---|
| `Update` (BT フェーズ) | 17 µs / tick | 794 µs / tick |
| `StateHash` (ハッシュを取る記録・照合・ネットの検査 tick だけ) | 109 µs / tick | 1705 µs / tick |

Release の `Update` は基準の 1/25 以下。この木は Wait が長く 1 体 1 tick に訪ねるノードが数個 (実行中の経路だけを再訪する)
なので、後続サブでノードが重くなったら (MoveTo のクエリ、FindRandomPoint) その分を足して再計測する。

## 後続のサブが決めること

Decorator と Abort (M85b)、Task / AI ノード (c / d)、イベントキュー (e)、AnimatorPlay と SubTree (f)、巡回ルート (g)、エディタ (h〜j)、
ABI v27 と C++ タスク (k)、C# タスク (l)。それぞれ決定を足し、M85n で確定にする。
