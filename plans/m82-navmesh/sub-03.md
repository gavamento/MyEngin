# sub-03: NavMeshAgent と dtCrowd の移動 + SimSnapshot の Nav 節 + `nav` ジョブ

- 依存: sub-02
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1 (Agent、Tick の位置、AgentBrain 共存、エッジケース)、4.4 (N1〜N3)。

1. `NavMeshAgentComponent` を末尾 append (hash 対象)。Add Component で CC が無ければ CC も足す (1 Undo)。
2. `NavSystem` (sim 側、`src\Engine\Engine\Navigation\`): Surface ごとに dtNavMesh / dtTileCache / dtNavMeshQuery / dtCrowd (容量 既定 128) を持つ。TickRunner のフェーズ 3.4 の後・3.5 の前に独立の `if (stepSim)` ブロック。順序は spec 4.1 のとおり (Agent はエンティティキー順で同期、CC の実位置を crowd へ、`dtCrowd::update(1/60)`、望む速度 → CC.moveInput、状態 → Agent)。
2b. (sub-02 VERDICT、spec 2. #17) `.mnav` の読み込みは `NavSystem::Update` 内の遅延ロード (Surface の (entity, navAsset) が変わったときだけ)。restore はこの読み込みを**Nav 節を当てる前に**済ませる (同期関数を公開して restore 側から呼ぶ等)。空の NavSystem へ restore → 次の Update が「構成が変わった」と読み直して復元状態を捨てる、を起こさないこと。受け入れ 3 の SelfTest がこの経路を通る。
3. **sub-01 で決めた復元方式**で (タイル差し替え済みの状態でも成り立つ形のまま。spec 4.4 F4) SimSnapshot に Nav 節を足す (`kSimSnapshotVersion` 24 → 25)。World hash にも Nav の外部状態を畳む (中身があるときだけ、XPBD / 音響と同じ content-gated)。Presence gate: NavMesh 系が無ければ RNG もハッシュも変わらない。
4. **計測 (sub-01 の申し送り)**: `--nav-demo` 実機で (i) Nav 節の capture バイト数と時間 (ロールバックの毎 tick capture 約 148 KB への上乗せ) (ii) 最悪 tick の `dtCrowd::update` 時間 (経路要求を同じ update で完走させるパッチの影響)。(i) は 128 体換算で +45% の見込み — 差分化するかの判断材料として SELF_EVAL に数字を書く。(ii) が重ければ 1 tick の経路要求を**件数で**絞る (時間で絞らない)。
4b. CC が越えられる段差の実測 → Surface の `maxClimb` 既定を確定 (spec 2. #5、R3)。結果を SELF_EVAL に書く。
5. `--nav-demo` (ShowcaseScenes の `kShowcases` に 1 行、`DemoContent.cpp`、editorOnly でない) と replay_verify の `nav` ジョブ (`:job_nav`、`-Jobs` 一覧、`:diagnose`、件数表示)。デモには Agent 数体と固定の目的地切り替え (tick で決まる) を入れ、`.mnav` はデモ用に用意 (assets にコミットするかデモ構築時にベイクするかは coder が決めて理由を書く。後者なら Debug / Release 一致は sub-02 で担保済み)。
6. デバッグ描画に Agent の経路 (コリドー / コーナー) を足す。
7. インスペクタ: 状態表示、寸法不整合・AgentBrain 併用・CC 無しの警告。

## やらないこと (このサブでは)
- Obstacle / Modifier / Link、ABI

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`src\Engine\Engine\Navigation\NavSystem.{h,cpp}`、`src\Engine\Engine\Loop\TickRunner.cpp` (340-387 付近)、`TickServices` (NavSystem の所有)、`src\Engine\Engine\Replay\SimSnapshot.{h,cpp}` / `WorldHasher.cpp`、`src\Engine\Engine\Demo\ShowcaseScenes.cpp` / `DemoContent.cpp`、`tools\replay_verify.bat`、`.github\workflows\ci.yml` のコメント (ジョブ数)、InspectorWindow、Localization
- 罠: AgentSystem は `ts.acoustic != nullptr` ゲートの中 (`TickRunner.cpp:340-386`) なので相乗りしない。位置は前 tick の WorldMatrix (`TickRunner.cpp:345-346`)。

## 受け入れ条件 (このサブ)
1. (spec 6) 段差・坂を越えて `Arrived`。届かない目的地は部分経路 + `pathPartial`、NavMesh 外は `NoPath`。CC 無しは `Inactive`。— SelfTest
2. (spec 7) すれ違いの最小距離: 回避ありで 2 体の半径和以上、回避なしで下回りうる。— SelfTest
3. (spec 3 実装) SelfTest: tick T で SimSnapshot を撮って新しい NavSystem へ restore → N tick 後のハッシュが連続実行と一致。
4. (spec 11, 12) replay_verify 全ジョブ PASS (新 `nav` を含む、snapshot stress と Server.exe 含む)。既存 golden 不変。
5. 0 警告、check_rules 0、selftest に新 FAIL なし。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`
- `Runtime.exe --nav-demo --screenshot` で経路描画を目視

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

実装:
- Components.h/.cpp: NavMeshAgentComponent (TypeId 72、末尾 append。status/remainingDistance/pathPartial は kFieldReadOnly の sim 状態 = hash 対象)。Surface に drawAgentPaths (NoHash)
- NavSystem.h/.cpp: Update(World&, dt) = Surface の遅延ロード -> Agent をキー順で dtCrowd と同期 (CC の実位置を corridor へ書き戻し、瞬間移動 > 2 m は置き直し) -> 目的地の要求 (変わったときだけ) -> dtCrowd::update -> status/remaining/pathPartial と CC.moveInput (crowd が今 tick に進めた変位 / dt) と yaw を書く。容量 128/Surface、超過はキー後ろから Inactive、動かせない理由は初回だけ WARN
- NavSystem: SaveSnapshot / ValidateSnapshot / ApplySnapshot (Nav 節。World 差し替え後に Surface の .mnav を先に読み込んでから store の差し替え分・dtCrowd・スロット表を当てる。同じ構成で読み込み済みなら再利用)、HasHashableState / StateHash (crowd に載った Agent が居るときだけ畳む = 内容ゲート)
- SimSnapshot.h/.cpp: kSimSnapshotVersion 24 -> 25、SimRefs.nav、NAV1 節 (SES 節の後・World 節の前)。SimSourcesOf に nav (末尾引数、既定 nullptr)。WorldHasher: SimSources.nav を末尾 append、"Nav" の 2 行 (agents / stateHash) + #total
- EngineLoop / HeadlessSim: simRefs.nav、TickRunner: Update に fixedDt、SimSourcesOf に ts.navSystem
- NavMeshAsset: RegisterInMemory / LoadByGuid (メモリ登録の資産。デモと SelfTest がファイルを作らない)
- NavAgentSelfTest (新規): CC の登れる段差の実測、庭 (段差 + 10 度の坂 + 台 + 登れない孤島) で Arrived / 部分経路 / NoPath / Inactive / Idle、すれ違い、Nav 節の復元、presence gate、容量 + 計測
- デモ: --nav-demo (DemoContent.cpp BuildNavShowcaseScene、ShowcaseScenes.cpp)、GameLogic の NavDemoDriver (300 tick で目的地を出発点へ。Get/SetComponentField)、replay_verify.bat の :job_nav (-Jobs / :diagnose / 件数表示)、ci.yml の job 名
- Editor: Add Component が NavMeshAgent に CC を同じ 1 Undo で足す (Editor/Scene/ComponentDependencies.{h,cpp} の AddComponentWithRequirements)、カタログ、インスペクタ (状態 + CC 無し / Rigidbody / AgentBrain 併用 / Surface 無し / 寸法超過の警告)、Localization 12 件 (en/ja)、NavEditorSelfTest に Add Component の Undo/Redo
- その他: AcousticAudioSelfTest の snapshot 版期待値 24 -> 25、EngineCliSelfTest に --nav-demo、NavSurfaceSelfTest の Update 呼び出しに dt

仕様との差分:
- [未実装] Agent の areaMask (spec 4.1 のフィールド表) — エリアに意味が付くのは sub-06 (Modifier) から。今入れると使わない欄になる。sub-06 でコンポーネント末尾に追加する
- [未実装] デバッグ描画の「コリドー」(ポリゴン列) — 経路は角 (corner) の折れ線 + 目的地の縦線のみ。色は状態別 (移動 緑 / 到着 青 / NoPath 赤)
- [追加] NavMeshAgent.pathPartial (spec 4.1 の本文にあるがフィールド表に無かった)、Surface.drawAgentPaths (4.3 の「経路」表示切り替え)
- [追加] NavSystem::Update のシグネチャに dt (spec は 1/60 固定。TickRunner の ctx.fixedDt を渡す)
- [追加] デモのナビメッシュは assets にコミットせず、シーン構築時にメモリ上で焼いて `nav://demo` として登録 (NavMeshAsset::RegisterInMemory)。理由: kNavBakeVersion を上げても古くならない / Debug・Release・Server が独立に焼くので replay 一致がベイクの構成間一致の実行経路での証明になる
- [追加] dtCrowd の tick 末に全 Agent の targetPathqRef を DT_PATHQ_INVALID へ正規化 (NavSystem.cpp)。完了した要求の控え番号は dtPathQueue の連番で WAITING_FOR_PATH の間しか読まれない死んだ値だが、復元した crowd は連番が 1 から数え直しになり、連続実行と復元後で blob / ハッシュが割れた (SelfTest で再現 -> 修正)。Recast 本体は無改造 (PATCHES.md の追記なし)
- [追加] avoidanceQuality 0 は params.collisionQueryRange を 0.01 にして近傍探索ごと止める。dtCrowd の衝突解決 (押し戻し) は DT_CROWD_SEPARATION と無関係に近傍全員へ働くので、これをしないと「回避なしですり抜ける」(spec 7) にならない (確認: 回避なしの最小距離 0.076 m)。他の Agent からは見える
- [追加] Nav 節のスロット表は載っているスロットだけを番号つきで書く (128 スロット全部を書くと 6 体でも +2.9 KB 余分)
- [追加] Agent の yaw: 親が無いエンティティだけ angularSpeedDeg で進行方向へ回し、回転を pure yaw で上書き (傾きは捨てる)。親付きは触らない
- [追加] Editor/Scene/ComponentDependencies (AddComponentWithRequirements)。Inspector の Add Component から切り出して SelfTest で Undo/Redo を検査できるようにした

検証:
- 両構成ビルド (Debug/Release、MyEngine.sln) -> 警告 0 / エラー 0
- Editor --selftest 両構成 -> NavAgent / NavSurface / NavEditor / NavDeterminism ALL PASS。FAIL は Source control self test の 2 件のみ (着手前 HEAD と同じ既知)。Debug の 1 回目は Fracture editor self test が 3 件 (weight cache) FAIL、2 回目以降は再現せず。Debug の 2 回目は DllReloader の自己テストで 18 分止まり、kill して再実行したら通った (どちらも nav と無関係に見えるが原因は未調査)
- Server --selftest 両構成 -> NavAgent ALL PASS、庭のワールドハッシュ D97085D12C7C561C が Debug / Release / Server で一致
- tools
eplay_verify.bat -> 15 ジョブ全部 PASS (新 nav ジョブ込み、snapshot stress 37 と Server.exe の verify 含む) 126.5 s
- tools\shot_verify.bat -> 5 枚 FAIL (parts / joints / acoustic_forward / acoustic_deferred / fracture_after)。着手前 HEAD と同じ集合 (既知)。golden は触っていない
- tools\check_rules.ps1 -> 0 error / 0 warning
- Release Runtime.exe --nav-demo --screenshot -> plans\m82-navmesh\screenshots\sub-03_nav_demo_f60.png / f200.png (NavMesh 輪郭 + Agent の経路線 + 目的地の縦線が出る。Agent は目的地へ移動する)
- 計測 (Release、NavAgentSelfTest):
  - (i) Nav 節: 6 体 2,894 B / 128 体 38,782 B。128 体で capture 69.5 us (Nav 節なしの 5.5 us に +64 us)、restore 58 us。Nav 節のバイト数は 128 体で 1 tick の capture (約 148 KB) の +26%、数体なら +2% 前後 -> 差分化は不要と判断
  - (ii) dtCrowd::update の最悪 tick: 128 体全員が tick 0 に経路要求しても 0.76-0.87 ms (Debug 4.1 ms)。dtPathQueue の容量 (8 件) が 1 tick の長い探索を自然に絞るので、件数で絞る追加実装はしなかった。NavSystem::Update 全体は avg 0.55-0.60 ms / 最悪 0.84-0.92 ms
- 4b (R3): CC の登れる段差: 3.5 m/s で 0.25 m、1.5 m/s で 0.15 m (0.20 m は低速で止まる)。Surface.maxClimb の既定は 0.1 のまま確定 (低速でも登れる 0.15 の内側)
- 未実行: Editor GUI の目視 (Inspector の状態表示・警告・Add Component の見た目)。--nav-demo を Editor で開いての確認

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1/2/3/4/5 の検証済み。areaMask とコリドー描画が未実装 (理由付き)、Recast の ledge 判定という仕様に無かった制約が残る (下の質問)
  正しさ: 4 — SelfTest + replay_verify (stress と Server 含む) + 3 構成のハッシュ一致で検証。GUI は未観測
  コード品質: 4 — NavSystem に UpdateSurface が長い関数として残る。Nav 節の書式は NavSystem に閉じ SimSnapshot は不透明バイト列だけを運ぶ
  テスト: 4 — 受け入れ 1/2/3 を網羅。Obstacle 等の後続サブの状態はこの節に載らない (sub-05 で拡張が要る)

不安・質問 (planner へ):
1. Recast の ledge 判定: 隣接セルの高低差が walkableClimb を超えるスパンを歩けない面として捨てるので、傾斜の実効上限は maxSlopeDeg ではなく atan(maxClimb / (2 * cellSize)) になる。maxClimb 0.1 / cellSize 0.3 の既定だと約 9 度で、maxSlopeDeg 45 (既定) の坂は穴だらけになる (15 度の坂で再現した。ポリゴンが途切れ Agent が部分経路で止まった)。CC は 0.15 m までしか登れない (step offset なし = 変えない方針) ので maxClimb を上げられない。対応案: (a) 仕様どおり (Surface のインスペクタに制約を書く) (b) Recast の rcFilterLedgeSpans を傾斜を見るパッチに (c) Surface の既定 cellSize を 0.2 にして約 14 度まで。今は (a) + SelfTest/デモは 10 度 / cellSize 0.2 で成立
2. 回避なし (quality 0) の実装は collisionQueryRange=0.01 で近傍探索を止める方式でよいか (他の Agent の回避からは見える)
3. sub-05 以降の Obstacle で store が変わる状態は Nav 節の store ブロック (SaveState) に既に載るが、Obstacle コンポーネントとの対応 (key) は未検証 — sub-05 の SelfTest で復元後の Commit を確認してほしい

触ったファイル: (最終報告の一覧を参照)

申し送り:
- 次サブ (sub-04 塗り / golden nav): `--nav-demo` は Agent が動くので golden の frame を固定 (tick 固定の撮影) に注意。経路線は Agent の状態で毎 tick 変わる (AppendDebugLines 内で毎回作る)
- sub-05: NavSystem::Update の先頭 (SyncSurfaces の後) に Obstacle 同期を足す。store の変更は Commit まで。crowd の経路は dtCrowd が checkPathValidity で張り直す
- sub-06: Agent に areaMask を追加 (末尾 append)、エリアのフラグ化 (NavMeshProcess::process が今は walkable=0x01 のみ) を変えると NavDeterminismSelfTest の期待ハッシュが動く
- sub-07: Link の渡りは NavSystem 側で持ち、その状態を Nav 節 (SaveSnapshot) に足す。status の kOnLink は定義済み
- テスト用: Nav 節の書式変更時は NAV1 の magic を上げずに壊れた blob を ValidateSnapshot が拒否する (境界検査あり)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-03)。不安 1 (坂の実効上限) は spec 2. #19 で (a) を裁定 (`[ユーザーに聞ける]`)、表示と警告は sub-04 へ。不安 2 (回避なし = 近傍探索を止める) は採用。不安 3 は sub-05 の 3b に割り当て。areaMask の後回し (sub-06 1b) とコリドー描画なしを承認。should: (1) Editor GUI (Agent インスペクタの状態・警告、Add Component で CC も付く見た目、Editor で --nav-demo) は reviewer が観測 (2) selftest の非再現 FAIL 2 種 (Fracture editor weight cache 3 件 / DllReloader の 18 分停止) を reviewer が再実行で確認。DllReloader は GameLogic に NavDemoDriver.cpp が増えたことと関係しうる (3) `SimSnapshot.cpp:651` で ApplySnapshot の戻り値を捨てている。失敗は NavSystem 側でログが出るので致命ではないが、次に SimSnapshot を触るサブで戻り値を扱うこと。
