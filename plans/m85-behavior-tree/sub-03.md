# sub-03: Task 4 種 (MoveTo / RotateTo / SetBlackboard / ClearBlackboard)

- 依存: sub-02
- 状態: OK (M85c としてコミット、ハッシュは台帳)
- 往復: 0

## やること
- spec 4.1.3 の 4 種 (Wait は sub-01 で済み)。
- MoveTo: NavMeshAgent の `destination` / `hasDestination` を書く (`EngineApiTable.cpp:1360-1376` と同じ書き方)。acceptanceRadius / Arrived で Success、NoPath / Inactive で Failure、Stuck はパラメータ `failOnStuck` (bool、既定 false、spec 2. #17) が false なら Running・true なら Failure (`hasDestination = false` にして止める)、observeTarget、終了と Abort で `hasDestination = false`、navFilter の一時差し替えと戻し、NavMeshAgent 無しで Failure。
  **確認すること**: 到着済みの Agent に同じ地点の MoveTo を 2 回目に出したとき (hasDestination を倒して立て直す) に再探索され Success で終わるか (`NavSystem.cpp:1460-1476` の `SameBits` 判定)。されないなら NavSystem に「hasDestination が偽 → 真で要求し直す」を足し、`nav` / `perception` の replay_verify ジョブが撮り直しなしで PASS することを確かめる (spec 7.)。足したなら仕様との差分に書く。
- RotateTo: LocalTransform の Y 回転、angularSpeedDeg (0 = Agent の値、Agent も無ければ 360)、toleranceDeg、実行中は `updateRotation = false`、終了・Abort で元の値に戻す。
- SetBlackboard / ClearBlackboard: spec 4.1.3。
- MoveTo / RotateTo の状態 (書き換え前の navFilter・updateRotation、最後に書いた目標) は BT 節に入る。

- (sub-01 VERDICT より) ノードの状態: sub-01 の `BtNodeState` は全ノード共通の固定 10 バイト。MoveTo / RotateTo の追加の状態 (目標の前回位置 float3、書き換え前の navFilter・updateRotation) を**全ノード共通の欄に足して全ノードを太らせない**。ノードの種類表 (`BtNodeTypeInfo`) に「その種類が持つ追加状態のバイト数」を持たせ、表がノードごとに固定長の追加領域を割り当てる形を推奨 (BT 節の書式は種類ごとの追加領域を生バイトで書く)。別の形にするなら理由を SELF_EVAL に。BT 節の書式が変わるので snapshot の版を +1。

## やらないこと (このサブでは)
- AI ノード、Patrol、デバッグ線 (sub-10)

## 触る場所 (planner の見立て)
- `BehaviorTreeSystem.cpp` / ノード表 / `BehaviorTreeSelfTest.cpp`、`NavSystem.cpp` (必要時のみ)
- SelfTest の NavMesh は `NavDeterminismSelfTest` / NavSystem の SelfTest が使う固定ジオメトリの作り方を流用

## 受け入れ条件 (このサブ)
1. (spec 6) MoveTo: Arrived で Success / 届かない目的地で Failure / Abort で Agent が止まる / Stuck を作った状況で failOnStuck = false は Running のまま・true は Failure で Agent が止まる / 同じ地点へ 2 回 / observeTarget で目標を追う / navFilter が戻る。RotateTo: 角速度どおり回り tolerance で Success、updateRotation が戻る。Set / Clear の 5 型 — `BehaviorTreeSelfTest`。
2. (spec 3) MoveTo 中に保存 → 復元 → 連続実行一致 (Nav 節と BT 節の両方)。
3. NavSystem を変えた場合: replay_verify の既存ジョブ全 PASS (撮り直しなし)。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`、NavSystem を触ったら `tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
実装:
  - BehaviorTreeLibrary.h/.cpp — BtNodeKind に MoveTo / RotateTo / SetBlackboard / ClearBlackboard。BtNodeTypeInfo に keyNames / keyCount (ブラックボードのキー名の欄) と extraStateBytes (種類別の追加状態のバイト数) を追加。BtParamType::Guid (BtParamValue::u、JSON は 16 進文字列)。BtNodeDef に keys / extraOffset、BehaviorTreeAsset に extraStateBytes。BtLinkAsset が追加領域をノードの並びに割り当てる。JSON は node の "keys" オブジェクト (名前は種類表)。追加状態の構造体 BtMoveToState (24 B) / BtRotateToState (4 B) と param / flag の名前空間。
  - BehaviorTreeSystem.h/.cpp — BtInstance::extra (生バイト)。MoveTo / RotateTo / SetBlackboard / ClearBlackboard の訪問。ReleaseBody (終了と Abort の両方が通る後始末: 目的地を倒す・navFilter / updateRotation を戻す・追加状態を 0 へ) を AbortNode (子孫の後・Decorator の後始末の前) / AbortBody / EndBody から呼ぶ。BT 節に extra を Blob で書き、読みで上限 (kBtMaxExtraBytes) を検証、ApplySnapshot は長さが木と合わなければ初期状態からやり直す。
  - BehaviorTreeSystem.cpp:Update — BehaviorTreeComponent が外れても生きているエンティティの表は、落とす前に Abort する。
  - SimSnapshot.h — kSimSnapshotVersion 34 -> 35 (BT 節の追加状態)。AcousticAudioSelfTest.cpp の版の検査も 35 へ。
  - BehaviorTreeSelfTest.cpp — Task 4 種のアセット / SetBlackboard / RotateTo / MoveTo (NavMesh の固定ジオメトリ = 24 x 24 m の床、壁はベイク後に置く) / 保存 → 復元 (MoveTo と RotateTo の途中、同じ system と新しい system) / 追加状態の長さ不一致の各テスト。BT 節の craft に追加状態の欄を足した。
  - NavSystem.cpp は触っていない。

仕様との差分:
  - [追加] ノードの JSON に "keys" (ブラックボードのキー名。MoveTo / RotateTo は "target"、SetBlackboard は "key" / "sourceKey"、ClearBlackboard は "key")。spec 4.2 のノードの JSON は params だけだったが、キー名は数値ではないため params に入らない。空文字 = 未指定 (実行時 Failure、読み込みは通す)。
  - [追加] BtParamType::Guid (MoveTo の navFilter)。
  - [追加] MoveTo の細部: 距離は水平 (XZ) で測る (Agent の中心は足元より 0.9 m 上のため 3D だと acceptanceRadius が効かない)。最初から acceptanceRadius の内側なら Agent に何も書かず Success。目的地を書いた tick は Nav がまだ走っておらず status が前の目的地のものなので読まない (観測で書き直した tick も同じ)。既定 acceptanceRadius = 0.5 m、observeTarget = true (UE と同じ)。
  - [追加] RotateTo の細部: angularSpeedDeg = 0 のとき Agent の角速度が 0 以下でも 360 (終わらない RotateTo を作らない)。入った tick から 1 tick 分回す。toleranceDeg 以内になった tick に Success (次の tick を待たない)。水平距離がほぼ 0 の目標は向きが定まらないので Success。
  - [追加] SetBlackboard の細部: source = Constant の Entity は Failure (定数のハンドルは無い。空にするのは ClearBlackboard)、Self は Entity / Vector だけ (Bool / Int / Float は Failure)、Copy は同じ型で設定済みのキーだけ (未設定・型違いは Failure)。パラメータは source / boolValue / intValue / floatValue / vectorX / vectorY / vectorZ。
  - [追加] BehaviorTreeComponent が外れた (エンティティは生きている) とき、表を落とす前に木を Abort する。しないと MoveTo の目的地・navFilter / RotateTo の updateRotation が Agent に残るため。
  - [追加] kBtValueLimit (1e9)・kBtMaxExtraBytesPerNode (64)・kBtMaxExtraBytes を名前付き定数にした (kBbCompareLimit は kBtValueLimit の別名にした)。

検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true → 警告 0・エラー 0 (どちらも)。
  - bin\x64\Release\Editor.exe --selftest → exit 0、BehaviorTree ALL PASS。[perf] 100 体 x 30 ノード Update 21.7 us / tick。
  - bin\x64\Debug\Editor.exe --selftest 1 回目 (実装直後) → exit 1。既知 flake だけ: Fracture weight cache 3 件 + net V1 LoadPersist / LoadGame の 1 件 (記録のとおり 1 回目)。BehaviorTree は ALL PASS。同じビルドの再実行 (2 回目) → exit 0、新規 FAIL 0。
  - bin\x64\Release\Server.exe --selftest → exit 0、BehaviorTree ALL PASS。Debug の Server は未実行。
  - tools\check_rules.ps1 → 0 error / 0 warning。
  - (sub-02 should#1) 変異試験 1 回: MonitorNode の LowerPriority の変化検出 `now && !was` を `now` へ、AbortNode の子孫が先を親が先へ、を同時に壊して Server --selftest → BehaviorTree が 6 件 FAIL (子が先: Abort Self の順序・RotateTo の Abort・MoveTo の Abort・LowerPriority で MoveTo を止める順序 / 変化検出: sub-02 の「条件が真のまま」・MoveTo の「真のままなら Abort せず最後まで歩く」)。元に戻し、再ビルド後の上記 3 回の selftest は全 PASS。
  - 確認すること (到着済みの Agent へ同じ地点の MoveTo): NavSystem を変えずに足りる。根のやり直し (Nav が hasDestination の偽を 1 tick 見る) は 1 回目の Success から 2 tick 後に 2 回目も Success (tick 220 → 222)。同じ tick の中で続けて出した場合 (Nav は偽を見ない) は、Nav の arrived が残るので次の tick に Arrived で Success (tick 221)。どちらもテスト化。replay_verify は NavSystem を触っていないので未実行。
  - Stuck: ベイク後に置いた壁 (NavAgentSelfTest の Pusher と同じ配置) で Stuck を作る。failOnStuck = false は 700 tick Running のまま (nav status 6 = Stuck、hasDestination 真)、true は Failure で次の枝へ移り hasDestination 偽・nav status Idle。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1・2・4 を満たす。差分は全て [追加] として列挙 (keys の JSON 形式は planner の確認が欲しい)。replay_verify は条件外 (NavSystem 無変更) として未実行
  正しさ: 4 — 変異試験で順序と変化検出の退行を検出できることを確認、保存 → 復元はハッシュで毎 tick 一致。親付き Agent の RotateTo・実機の Runtime での MoveTo は未検証
  コード品質: 4 — 後始末の入口を ReleaseBody 1 つに集約。YawOf / WrapPi は NavSystem の無名名前空間にあるため複製した
  テスト: 4 — 5 型の Set / Clear、RotateTo の角速度・updateRotation の戻し・Abort、MoveTo の Arrived / NoPath / Abort / 2 回 / observeTarget / navFilter / Stuck 両方 / 保存 → 復元 (同じ・新しい system)。親付きと、複数 Agent が同じ木を使う場合は無い
不安・質問:
  - ノード JSON の "keys" を足した形 (spec 4.2 に無い) でよいか。sub-08 以降のエディタもこの形 (種類表の keyNames) で欄を出す想定。
  - RotateTo / MoveTo の三角関数 (std::sin / cos / atan2) は NavSystem の TurnToward と同じ関数。Debug / Release のビット一致は sub-13 の replay_verify `bt` で RotateTo を通して確かめる必要がある (この sub では BT の replay 被覆が無い)。
触ったファイル:
  - src\Engine\Engine\AI\BehaviorTreeLibrary.h
  - src\Engine\Engine\AI\BehaviorTreeLibrary.cpp
  - src\Engine\Engine\AI\BehaviorTreeSystem.h
  - src\Engine\Engine\AI\BehaviorTreeSystem.cpp
  - src\Engine\Engine\AI\BehaviorTreeSelfTest.cpp
  - src\Engine\Engine\Replay\SimSnapshot.h
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp
  - plans\m85-behavior-tree\sub-03.md (実装メモ)
申し送り:
  - 種類別の追加状態は BtNodeTypeInfo::extraStateBytes + BtNodeDef::extraOffset + BtInstance::extra。新しい種類 (sub-04 の SearchArea / sub-07 の Patrol) はこの欄に構造体を置き、終了 / Abort の後始末は ReleaseBody の switch へ足す (extra は ReleaseBody が 0 へ戻す)。構造体はパディングなし・全フィールド 4 / 8 バイトで揃える (BT 節とハッシュは生バイト)。追加状態を足したら kSimSnapshotVersion を上げる。
  - ノードのキー名の欄は BtNodeTypeInfo::keyNames (btnodekey::kTarget / kSourceKey)。エディタ (sub-08) の検査エラー「未設定のキー参照」は keys の空文字と BB に無い名前を見る。
  - 木が Agent へ書くもの (目的地・navFilter・updateRotation) は ReleaseBody が戻す。BehaviorTreeComponent が外れても Update の dropInstance が Abort する。
  - MoveTo は目的地を書いた tick に status を読まない (Nav 前の古い値)。AI ノード (SearchArea / Patrol) が MoveTo を内部で使うときも同じ規則。
  - BT 節の BbValue は SaveSnapshot が isSet / i / f / v / entity を全部書くので、Copy は BbValue 丸ごとのコピーで足りる。

## フィードバック履歴
- round 1: VERDICT OK (planner)。ノード JSON の "keys" (種類表の keyNames) を承認し spec 4.2 に反映。MoveTo / RotateTo / SetBlackboard の細部 (水平距離、書いた tick は status を読まない、observeTarget 既定 true、Constant の Entity は Failure 等) を承認。sub-02 should#1 の変異試験で 6 件 FAIL を確認済み = 消込。同じ地点への MoveTo は NavSystem 無変更で通ることを確認 (spec 7. のリスク解消)。RotateTo の三角関数のビット一致は NavSystem と同じ関数の前例があり、sub-13 の replay_verify bt で RotateTo を通して確かめる
