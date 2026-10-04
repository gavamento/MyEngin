# M82 NavMesh (Recast Navigation) — 仕様書

- slug: m82-navmesh
- 状態: 確定 (2026-10-03。Q1〜Q5 は司会経由でユーザー回答済み、8. 変更履歴参照)
- 依頼原文: ナビメッシュの実装,.claude\plans\imperative-scribbling-shore.md,不明点やあいまいな点は質問をして
- 事前計画: `C:\Users\akita\.claude\plans\imperative-scribbling-shore.md` の「M-A: NavMesh」。M-B (知覚) / M-C (BT) は範囲外

## 1. 目的 (なぜ作るか)

ゲーム制作者が、レベルを置いて Bake するだけで「敵 / NPC が障害物を避けて目的地へ歩く」を作れる状態にする。
しかもそれが MyEngine の契約 (Debug / Release / Server.exe のシミュレーションのビット一致、リプレイ、
What-if 分岐、ロールバック) を壊さないこと。既存の AI 移動は `AcousticNav` (音響グリッド上の流れ場) だけで、
これは「音が届く所へ向かう」特殊解であり、段差・坂・敵同士の回避・ジャンプ地点・動く障害物を扱えない。
後続の BT (M-C) の MoveTo / FindRandomPoint は、この M82 の API の上に乗る。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | Recast をそのまま sim に入れてビット一致するか | Recast/Detour/DetourCrowd は float の塊。ビルドは `build\Common.props:5-6` で全構成 `/fp:precise` かつ `/arch` 同一 (SSE2) なので理屈上は一致するが未計測。replay_verify は Debug / Release / headless Server.exe の 3 本で照合 (`tools\replay_verify.bat :chain`) | 事前計画で「不一致ならパッチで直す、自前実装へは置き換えない」 | sub-01 で実測。不一致は `external\recastnavigation\PATCHES.md` に記録してパッチ |
| 2 | dtCrowd / dtTileCache / dtNavMesh の内部状態が ECS の外にある | SimSnapshot は「外部状態は blob で持つ (粒子 / XPBD / 衝突ペア)」か「導出値なので restore で無効化 (AcousticField::Invalidate)」の 2 流儀 (`SimSnapshot.cpp:504-617`)。replay_verify は **毎 tick capture→restore→recapture の blob 比較** (`--snapshot-stress 37`、`EngineLoop.cpp:2125`)、ロールバックは **毎 tick capture** (`NetRollback.cpp:100`、約 148 KB) | — (技術判断) | sub-01 の試作で決める。要件は 4.4 の N1〜N4。**polyRef の salt が tile の追加削除履歴に依存する**ので、「コンポーネントから作り直す」だけでは経路コリドーが復元できない。候補は 4.4 に列挙 |
| 3 | TypeId 71 は M75h (InputField) の予約と衝突しないか | TypeId は登録順 (`ComponentRegistry.h:49-51`)。シーンは型名で保存 (`SceneSerializer.cpp:127,144`)、TypeId が入るのは SimSnapshot blob と .rep (使い捨てで毎回録り直す、`Components.cpp:38-41`) だけ | — | **末尾 append。番号は登録コミット時点の末尾から** (M75h より先なら 71〜75、InputField は 76 にずれる)。`plans\m75-ugui.md` の「InputField は 71」に注記を足す。予約は不要 |
| 4 | `AcousticNav.h:22-24`「NavMesh を作らないのが判断の前提」と衝突 | この前提は「音響 AI の移動を音の伝播と同じ占有配列から出す」局所判断で、エンジン全体の禁止ではない (`plans\hushed-rippling-beacon.md:55` は M65 の判断) | 事前計画で ADR に整理と決定 | ADR-023 を新設し「音響ナビ = 聞こえた所へ行く特殊解、NavMesh = 汎用移動」と役割を分ける。AgentBrain は従来どおり AcousticNav を使う (共存) |
| 5 | CharacterController に段差 (step offset) が無い | `CharacterControllerComponent` (`Components.h:754-765`) は radius / height / slopeLimitDeg / skinWidth のみ。`SolveCharacters` (`PhysicsSystem.cpp:1247`) に step 処理なし。sub-03 の実測: 登れる段差が速度しだい (1.5 m/s で 0.15 m、3.5 m/s で 0.25 m) | 2026-10-03 ユーザー: 「あるける最大傾斜や階段の高さを変更できるように」(planner の当初の結論「CC は変えない」を覆した)。既定値は「Unity と同じ 0.3」(planner の裁定「既定 0 で既存不変」を覆した) | **CC に `stepOffset` を末尾追加し、速度に関係なくその高さまで登る** (sub-10)。**既定 0.3** (Unity と同じ)。フィールドの無い旧シーンの CC も 0.3 で読まれ、段差を登るようになる。既存デモと外部プロジェクト (三校 / HAL Collector) の挙動が変わることを受け入れ、受け入れ条件 11 を緩める (変化が stepOffset だけによることを sub-10 の手順で切り分けて示す)。planner の反対意見 (記録のみ): 既存ゲームの挙動が予告なく変わり、既存デモの tick ハッシュ列の連続性が切れる。Surface の `maxClimb` 既定は 0.3 (sub-03 で一度確定した 0.1 を変更) |
| 19 | 坂の上限は `maxSlopeDeg` で決まるか (sub-03 coder の不安 1) | Recast の `rcFilterLedgeSpans` は隣接セルの高低差の幅が `walkableClimb` を超えるスパンを捨て、`rcBuildCompactHeightfield` の隣接接続も高低差 ≤ `walkableClimb`。実効の坂上限 ≈ atan(maxClimb / (2·cellSize))、旧既定 (0.1 / 0.3) で約 9 度。15 度の坂で途切れることを sub-03 で再現 | 2026-10-03 ユーザー: 「あるける最大傾斜や階段の高さを変更できるように」(planner の裁定 (a)「制約として受け入れる」を覆した) | **`maxSlopeDeg` が設定どおり効くようにする** (sub-10): Surface に `autoCellSize` (既定 on) を足し、ベイクのセルサイズを cs = min(agentRadius/2, maxClimb / (2·tan(maxSlopeDeg))) で決める (下限あり、下限に当たったら警告)。既定値どうしで cs 0.15・実効上限 45 度。インスペクタに実際の cs と実効の坂上限を表示。却下: Recast のパッチ (ボクセル上で坂と段差を区別する情報が無く、意味論ごと変わる)、既定 cellSize を固定値で細かくするだけ (maxClimb / maxSlopeDeg を変えるたびに手で合わせる必要があり「変更できる」にならない) |
| 20 | NavMesh の段差判定と CC の段差判定の量子化のずれ (sub-10 coder の不安 1) | Recast は段差を `cellHeight` の整数セルで比べる (`walkableClimb = floor(maxClimb / ch)`)。地面の天面がボクセル境界にあると、maxClimb + 1 セル未満の段差も接続される。sub-10 の庭では cellHeight 0.1 で 0.35 m の台が接続され、CC (stepOffset 0.3) は登れずに Agent が縁で `Moving` のまま押し続けた | `[ユーザーに聞ける]` (planner 裁定) | **ずれを 1 セル未満に抑え、残りは詰まり検出で受ける**: (1) `autoCellSize` のときは cellHeight も自動で決める: ch = min(cs / 2, maxClimb / 6)。既定では 0.05 で、ずれは 5 cm 未満 (sub-10 round 2) (2) 前進できない Agent を一定 tick 後に止める詰まり検出 (sub-05) (3) ADR に「NavMesh の段差上限は maxClimb から最大 1 セル超えうる」と書く。却下: NavMesh 側を 1 セル控えめにする = 0.3 ちょうどの段差が繋がらなくなり、受け入れ条件 6 の『maxClimb の段差を越える』が成り立たない。Inspector への注記だけ = Agent が無言で止まり続ける (AGENTS.md 3.4 の『理由が分からない無応答』) |
| 21 | CC の `stepOffset` に Transform の scale を掛けるか (sub-10 coder の不安 2) | radius / height には scale が掛かる (`PhysicsSystem.cpp:1782-1783`: radius × max(|sx|, |sz|)、height × |sy|)。stepOffset に掛けると、acoustic デモの Agent Eye (scale.y 1.6) の実効段差が 0.48 m になり、衝撃板 (天面 0.45) に乗り上がる。tick 26 で着手前と割れることを実測した | 2026-10-04 ユーザー: 「する (Unity と同じ)」(planner の裁定『掛けない』を覆した。acoustic の replay 基準と golden が動くことを承知のうえ) | **掛ける。height と同じ規則で、実効の段差 = stepOffset × |scale.y| (ワールドの scale、親を含む)**。段差は縦の長さなので、縦の量である height と揃える (radius の水平 max 規則は使わない)。上限は実効の全高 (height × |scale.y|)。0 以下と NaN は登らない。Agent の警告 (`stepOffset < maxClimb`) も実効値で比べる。planner の反対意見 (記録のみ): 見た目の拡大で登れる高さが変わるのは予想しにくい。acoustic デモの挙動が変わる |
| 6 | 「エージェント半径・高さ・最大段差・最大傾斜は CharacterController と共有」(事前計画) | CC の radius は Transform scale が掛かる (`Components.h:749-753`)。NavMesh は 1 つのエージェント寸法でしかベイクできない (Recast の walkableRadius 等はベイク時定数) | — | Surface が自分のベイク寸法を持つ (既定は CC の既定 0.3 / 1.8 / 45°)。Agent と CC の寸法が Surface を超えるとインスペクタに警告。「同じ値を参照」はベイク時定数なので不可能、と記録 |
| 7 | 動的更新の範囲。「タイル再ベイク」は実行時にジオメトリを再ラスタライズすることまで含むか | DetourTileCache の再構築は「ベイク済みの層 + 障害物」からタイルを作り直す (ジオメトリは読まない)。ジオメトリの再ラスタライズは Recast 本体を sim で回すことになり、決定論の面積が倍になる。M80 の破壊は FracturePiece に分かれる (`FractureSystem.h`) | Q1: 「再ベイクなしでよいが、将来再ベイクを入れることを考慮して設計すること」 | **静的ジオメトリの実行時再ベイクは実装しない**。壊れる壁は Obstacle を付けて消す運用。ただし**将来差し込める設計を必須にする** (4.4 の「将来の実行時再ベイクへの備え」F1〜F5、受け入れ条件 18) |
| 8 | Agent は CC 必須か、Transform を直接動かしてもよいか | 事前計画は CC.moveInput 経由。Unity の NavMeshAgent は既定で Transform を直接動かす。Rigidbody があると CC は無効 (`Components.h:743`) | Q2: CC 必須 (裁定どおり) | **CC 必須**。CC が無い / Rigidbody で無効の Agent は動かさず、インスペクタとログに警告 (Add Component 時に CC が無ければ CC も足す)。重力・接地・衝突を CC に任せられ、経路と物理の二重管理を作らない |
| 9 | 経路追従を Crowd とは別に先に作る (事前計画の 3 と 7) と二度手間 | dtCrowd が経路コリドー・追従・回避を一体で持つ。単独追従を先に書くと Crowd 導入時に捨てる | — | **Agent の移動は最初から dtCrowd**。回避の on/off は Agent ごとの設定 (`obstacleAvoidance` 品質 / 分離) で切る。サブ 3 と 7 を統合 |
| 10 | エリアコストを project_settings.json に置くと sim 入力がシーン・リプレイの外に出る | `project_settings.json` はチーム設定 (`EditorSettings.h:22-28`)。.rep は SimSnapshot を先頭に埋め込むだけ (`Replay.h:16`) で、project_settings は含まない | — | **コストは NavMeshSurface コンポーネント (hash 対象) に持つ** (16 エリア)。エリアの**名前**だけ project_settings.json (表示用)。Agent ごとのコスト上書きは無し、Agent は `areaMask` だけ |
| 11 | 複数の Surface と Agent の対応 | Recast の NavMesh は別インスタンス同士で接続できない | — | Surface ごとに 1 つの dtNavMesh。Agent は `agentTypeId` が一致する Surface に乗る。同じ `agentTypeId` の Surface が複数あれば entity.index 最小が勝ち + 警告 (AcousticVolume の前例、`plans\hushed-rippling-beacon.md:531`) |
| 12 | Off-Mesh Link を渡る間の動き。CC は重力を掛け続ける | CC に一時停止のフラグは無い (`Components.h:754-765`)。dtCrowd は OFFMESH 状態を返すだけで、渡り方はアプリ側 (RecastDemo の CrowdTool も自前で線形補間) | Q3: 3 種 (裁定どおり) | 種別 `Linear` (一定速度で直線) / `Jump` (放物線、高さ指定) / `Manual` (止まって状態を公開、スクリプトや後の BT が完了を通知) の 3 つ。梯子・ドアは `Linear` に速度と種別ラベルを付けて表す。渡る間は CC.moveInput=0 で、物理の後に nav が位置と CC.velocity を上書きする (CC の型は変えない) |
| 13 | NavMesh の表示は線だけで足りるか | sim → 描画のデバッグ経路は `DebugLineCmd` (線分) だけ (`DebugDraw.h`、`TickRunner.cpp:459-471`)。DebugUtils の `DU_DRAW_TRIS` を塗るには半透明三角形のパスが要る | Q4: 「Unity のような半透明の塗りも付ける」(planner の裁定「線だけ」を覆した) | **半透明の塗り (エリア色) + 線** (ポリゴン輪郭、タイル境界、Link、Obstacle、Agent の経路)。塗りは sim → 描画に三角形のレーンと半透明パスを新設する sub-04。golden `nav` を shot_verify に追加 |
| 14 | スクリプト API (ABI) を M82 に入れるか、BT (M-C) まで待つか | ABI は v23 = 131 スロット (`EngineAPI.h:39`、`check_rules.ps1:676`)。目的地は既存の Get/SetComponentField で書けるが、経路クエリ・最近点・ランダム点は関数が要る。M75h が v24 を計画済み (`plans\m75-ugui.md` G 節) | Q5: M82 に入れる (裁定どおり) | **M82 で ABI を 1 回 bump** (C++ と C# ミラー)。版は着手時点の次番号 (M75h より先なら v24、M75h は v25 へ)。BT を待つと GameLogic から NavMesh を使えない期間が長い |
| 15 | ベイク結果をどこに置くか | 前例は `.mfrac` (`FractureAsset.h:23`、`assets\Fracture\<名前>_<入力ハッシュ>.mfrac`、GUID で参照、エディタで非同期ベイク `FractureBakeService.h`)。音響のベイクは永続化されない (`AcousticField.h:311`) | — | `.mnav` を新設し `.mfrac` と同じ流儀 (GUID 参照、入力ハッシュ名、非同期ベイク、1 Undo で参照を設定)。中身は TileCache の層 + ベイク設定。dtNavMesh のタイルはロード時に層から作る |
| 16 | NavMesh 部品が無いシーンの既存リプレイ・golden が変わらないか | AgentSystem は「Agent が居ないとき RNG を引かない」(`AgentSystem.h:43-45`)。ハッシュは XPBD / 音響を中身があるときだけ畳む (WorldHasher) | — | **Presence gate を必須**: NavMesh 系コンポーネントが 1 つも無いシーンでは、RNG・ハッシュ・SimSnapshot の Nav 節の中身がどれも変わらない (節自体は空で書く) |
| 17 | `.mnav` の読み込みを `PreloadFractureAssets` の 3 か所 (`StartScene.cpp:67` / `EditorApp` / `TickRunner.cpp` のシーン遷移) に足すべきか (sub-02 で coder が逸脱) | `NavSystem::Update` (`NavSystem.cpp`) は stepSim の tick ごとに Surface の (entity, navAsset GUID) をキー順に並べ、前回と違うときだけ読み直す。フェーズ 3.4b は物理 3.6 より前なので最初の物理 tick に間に合う。Server / HeadlessSim も同じ経路 (`HeadlessSim.cpp` が NavSystem を所有) | — (技術判断、planner が sub-02 VERDICT で採用) | **tick 内の遅延ロードを採用**。呼び出し箇所 3 つに散らすより、シーン遷移 / Play 開始 / 復元のどれでも「その tick の World」で決まる方が漏れが無い。条件: sub-03 の SimSnapshot restore は Nav 節を当てる**前に**同じ読み込み (公開した同期関数) を済ませる — 空の NavSystem へ restore した直後の Update が読み直して復元状態を上書きしないこと |
| 18 | 編集中 (非 Play) の SceneView に NavMesh を出すか | sub-02 の輪郭は TickRunner フェーズ 4 の `debugLines` 経由で、物理 / 音響のデバッグ線と同じく Play 中 (と Runtime / Server の tick) にしか出ない。Unity は編集中の Scene ビューに NavMesh を出す。Bake の結果を見るのに Play が要るのは制作の手間 | `[ユーザーに聞ける]` (planner 裁定) | **編集中にも出す** (塗り + 輪郭)。sim を回さない表示専用の読み込み経路を足す。描画レーンを新設する sub-04 に入れる。Surface の範囲箱ギズモ (4.3、どのサブにも割り当て漏れ) も sub-04 へ。却下: 「Play 中だけ」= Bake → 確認の往復ごとに Play が要る |

## 3. スコープ

- やる:
  - Recast Navigation の vendor (`external\recastnavigation\`、Recast / Detour / DetourTileCache / DetourCrowd / DebugUtils)、`external\VERSIONS.md` 記録、決定論の実測とパッチ
  - コンポーネント 5 種 (NavMeshSurface / NavMeshAgent / NavMeshObstacle / NavMeshModifier / NavMeshLink)
  - エディタでのベイク (非同期、進捗表示、Bake / Clear)、`.mnav` アセット、Runtime / Server でのロード
  - Agent の移動 (dtCrowd による経路追従 + 群衆回避、CC.moveInput 駆動)
  - Obstacle による動的切り抜き (tick 境界で同期確定)、Modifier によるエリア、Link による Off-Mesh 接続、エリアコスト
  - SimSnapshot / What-if / ロールバック / replay_verify との整合
  - デバッグ描画 (線 + 半透明の塗り)、golden `nav`、Create → 3D Object の 4 項目、インスペクタ
  - 将来の実行時再ベイクを差し込める設計 (タイル単位の差し替え口、Engine 層のベイク関数、再ベイクと両立する復元方式。再ベイク自体は作らない)
  - スクリプト API (ABI bump 1 回、C# ミラー)
  - CharacterController の `stepOffset` (段差を速度に関係なく登る) と、Surface のセルサイズ自動決定で `maxSlopeDeg` / `maxClimb` を設定どおりに効かせる (2. #5 / #19、sub-10)
  - SelfTest、replay_verify の `nav` ジョブ、ADR-023、engine_spec / feature guide の追記
- やらない (明示的に外したもの):
  - 自前 A* (事前計画で却下済み)
  - 静的ジオメトリの実行時再ラスタライズの**実装** (Q1。差し込み口だけ作る)
  - CC 無しで Transform を直接動かすモード (Q2)
  - Agent ごとのエリアコスト上書き、Height Mesh、NavMesh の複数タイプ自動合成
  - 知覚 (M-B)、ビヘイビアツリー (M-C)
- 後回し:
  - 実行時再ベイク (変化の検知・トリガー・sim 内での Recast 実行)、Agent ごとのコスト上書き (要望が出たら別マイルストーン)

## 4. 仕様

### 4.1 振る舞い

**コンポーネント** (名前は確定。フィールドは最小集合で、coder が足すなら「仕様との差分」に出す)

| コンポーネント | 主なフィールド | hash |
|---|---|---|
| `NavMeshSurfaceComponent` | `agentTypeId` int / ベイク範囲 `center`・`size` (ローカル AABB) / `agentRadius` 0.3・`agentHeight` 1.8・`maxClimb` 0.3 (sub-10)・`maxSlopeDeg` 45 / `cellSize`・`cellHeight`・`tileSize` / `autoCellSize` (既定 on、sub-10) / `collectLayerMask` / `areaCosts[16]` / `navAsset` AssetID / 表示フラグ (kFieldNoHash) | 対象 |
| `NavMeshAgentComponent` | `agentTypeId` / `speed`・`acceleration`・`angularSpeedDeg`・`stoppingDistance` / `radius`・`height` (回避用) / `areaMask` u32 (sub-06 で末尾追加) / `avoidanceQuality` 0..3 (0=回避なし) / `destination` Float3・`hasDestination` / 実行状態 (`status`: Idle / Moving / Arrived / NoPath / OnLink / Inactive / Stuck (sub-05、2. #20)、`remainingDistance`、`pathPartial`) | 対象 |
| `NavMeshObstacleComponent` | `shape` (Box / Cylinder) / `center`・`size` (Box)・`radius`・`height` (Cylinder) / `carve` bool | 対象 |
| `NavMeshModifierComponent` | ローカル AABB / `area` (0..15) / `affects` (ベイク時に反映、実行時は Obstacle と同じ TileCache の area 書き換え) | 対象 |
| `NavMeshLinkComponent` | `start`・`end` (ローカル) / `width` / `bidirectional` / `area` / `traversal` (Linear / Jump / Manual) / `traversalSpeed`・`jumpHeight` | 対象 |

- エリア: 0 = Walkable (コスト 1)、1 = NotWalkable、2 = Jump (Link の既定)、3〜15 = ユーザー定義。名前は `project_settings.json` (表示のみ)、コストは Surface の `areaCosts`。
- **Tick の位置**: TickRunner の フェーズ 3.4 (音響 + AgentSystem) の後、3.5 (アニメーション) の前に独立した `if (stepSim)` ブロックで `NavSystem::Update` (AgentSystem は `ts.acoustic` ゲートの中なので相乗りしない)。順序は (1) Obstacle / Modifier / Link のコンポーネント差分を TileCache へ反映し、`update` を**全部終わるまで**同期で回す (2) Agent をエンティティキー (entity.index、同値は generation) 順に dtCrowd と同期 (CC の実位置を crowd 側へ書き戻す) (3) `dtCrowd::update(1/60)` (4) 望む速度を CC.moveInput へ、状態を Agent へ書く。物理 (3.6) の後に Link 渡り中の Agent の位置を上書きする。
- **AgentBrain との共存**: 同じエンティティに AgentBrain と NavMeshAgent があれば、moveInput は後に走る NavSystem が勝つ (feature guide 9.3 の「後に走る AI が優先」と同じ規則) + インスペクタ警告。
- 目的地まで完全な経路が無ければ、dtCrowd の部分経路 (最寄りの到達可能点まで) で動き、着いたら `Arrived` + `pathPartial = true`。目的地も始点も NavMesh に乗らない (最寄り点が見つからない) ときだけ `NoPath`。
- 乱数: `findRandomPoint` 系の `frand` は `World::Rng()` (Pcg32) 経由。Agent が居ないときは引かない。
- エッジケース: Surface が無い / アセット未ベイク / `agentTypeId` に合う Surface が無い → Agent は `Inactive` で止まる (落ちない、ログは状態が変わった tick に 1 回)。アセット読み込み失敗 → その Surface だけ無効、他は動く。dtCrowd の容量 (既定 128、Surface ごと) を超えた Agent は entity キー順で後ろから `Inactive`。

### 4.2 データ・保存形式・互換性

- 新 TypeId 5 個を**末尾 append** (2. #3)。シーン (`kDocVersion=4`) は変えない (型名保存なので新型の追加は互換)。
- `.mnav`: 版付きバイナリ (`.mfrac` と同じ Serialize / Deserialize、境界検査、同じ入力から同じバイト)。中身 = ベイク設定 + 入力ハッシュ + TileCache の圧縮層 (圧縮器は**無圧縮の自作** `dtTileCacheCompressor`、FastLZ を入れない) + Link / Modifier のベイク時スナップ。保存先 `assets\NavMesh\<Surface名>_<入力ハッシュ16桁>.mnav`、`.meta` で GUID、`AssetType::NavMesh`。ビルド (配布物) へは `BuildSettingsWindow.cpp:208-210` の `assets\` 丸ごとコピーで入る (:253 の一覧はクック物専用で対象外。sub-02 VERDICT で訂正)。
- 読み込み: `NavSystem::Update` (stepSim の tick、フェーズ 3.4b) が Surface の (entity, navAsset) の変化を見て遅延ロードする (2. #17)。
- SimSnapshot: Nav 節を追加し `kSimSnapshotVersion` 24 → 25 (sub-01 の結論で節の中身を決める。節が要らない結論なら bump しない)。
- ABI: 1 回だけ bump (2. #14)。最小の関数: `NavSetDestination` / `NavStop` / `NavGetAgentState` / `NavFindPath` (コーナー列を呼び出し側バッファへ) / `NavSamplePosition` (最寄り点) / `NavRaycast` / `NavFindRandomPoint` (半径内、World RNG)。全部 POD + C ABI、`Interop.cs` 位置ミラー、`check_rules.ps1` の版表を更新。

### 4.3 UI / ビジュアル

- Create → 3D Object: 既存 6 項目の後に区切り線、`NavMesh Surface` / `NavMesh Obstacle` / `NavMesh Modifier` / `NavMesh Link`。Undo は既存の `CreateItem` / `RecordCreate`。Surface はシーン境界を覆う既定サイズ (20×10×20)。
- Add Component に 5 種。NavMeshAgent を足すと CC が無ければ CC も足す (1 Undo)。
- Surface のインスペクタ: ベイク設定、`Bake` / `Clear`、ベイク中の進捗とキャンセル、結果の要約 (タイル数・ポリゴン数・所要時間)、表示切り替え (NavMesh / タイル境界 / Link / Obstacle / 経路)。寸法の不整合 (Agent / CC が Surface より大きい) を警告。
- SceneView ギズモ: Surface の範囲箱 (sub-04)、Obstacle の形 (sub-05)、Modifier の箱 (sub-06)、Link の 2 点と矢印 (sub-07)。
- NavMesh の表示は**編集中 (非 Play) の SceneView にも出る** (2. #18、sub-04)。Bake 直後に Play せず結果を確認できること。
- デバッグ描画は Runtime でも出せる (`debugLines` 経路 + 新設の三角形レーン、resim 中は積まない)。**NavMesh は半透明の塗り (エリア色、Unity 風) + 輪郭線**。床と Z ファイトしない。NavMesh が変わらない tick では三角形を作り直さない。golden `nav` (`--nav-demo` の固定 tick、表示 on) を `shot_verify` に載せる。
- 文字列は全部 `LocalizationTable.inl` (en / ja)。

### 4.4 非機能

- **決定論 (最重要)**:
  - N1. 同じシーン・同じ入力で、Debug / Release / headless Server.exe (Release) の各 tick ハッシュが一致 (replay_verify の `nav` ジョブ)。
  - N2. `--snapshot-stress` (毎 tick capture → restore → recapture) で blob が一致し、その後のハッシュも連続実行と一致。
  - N3. tick T のスナップショットを新しいプロセス状態 (空の NavSystem) へ restore して N tick 進めた結果が、連続実行と一致 (What-if / ロールバック / join が通る経路)。
  - N4. ベイク (`.mnav` のバイト列) が Debug / Release で一致 (エディタのベイクは sim 外だが、ロード後のタイルは sim 状態になるため)。
  - **N2/N3 の実現方式 (sub-01 で確定、ADR-023)**: 方式 (b') = Nav 節に `NavTileStore::SaveState` (差し替えた層 + 障害物 + スロット表 + スロットごとの salt。差し替えなしで約 0.7 KB) と `NavSaveCrowd` (dtCrowd のエージェント配列 + `dtLocalBoundary`。8 体 約 3.9 KB / 128 体 約 66.5 KB) を保存する。`NavTileStore::Commit` は変更のたびに全タイルをキー順・スロット順に入れ直し、dtNavMesh のリンク順・検索連鎖順の**履歴依存を消す**。dtCrowd の経路要求は同じ update 内で完走させ (Recast パッチ 1)、tick 境界に `dtPathQueue` の途中状態を残さない。
    - 却下: (a) `addTile` は接続済みデータを受け取れず、`dtPathQueue` は private / (b) の素朴版 (salt の表だけ) は実測で割れた (同じ最終状態でリンク順ハッシュが別) / (c) 問題の本体は salt ではなくリンク順。
    - 計測 (Release): 状態表の保存 2.0 µs、dtCrowd の保存 8 体 5.3 µs・128 体 65 µs、復元 4 µs・68 µs、変更なしの Commit 12.5 µs、障害物 2 個の Commit 666 µs (13 層)。128 体はロールバックの毎 tick capture (約 148 KB) に +45% のバイト数 — 差分化するかは sub-03 で実機の数字を見て決める。
    - Off-Mesh を dtCrowd 自身に渡らせない (`m_agentAnims` は保存しない)。渡りは NavSystem 側 (sub-07) が持ち、その状態はスナップショット対象。
- **将来の実行時再ベイクへの備え (Q1)** — 再ベイクは作らないが、次を満たす:
  - F1. 実行時のタイル変更 (TileCache の再構築: Obstacle / Modifier / Link) は NavSystem の**タイル単位の差し替え口 1 本** (tick 境界の同期確定ステップ) を通る。将来の再ベイクは「その口へ新しい層を渡す別の生産者」として足せる形にする。
  - F2. ベイク入力の収集 (`World` + タイル AABB → 三角形・エリア) と、タイル 1 枚分のベイク (三角形 → TileCache の層) は **Engine 層の純関数** (Editor に依存しない、決定的な順序、Runtime / Server からも呼べる)。エディタのベイクはそれを非同期ワーカーで回してファイルに書くだけ。
  - F3. `dtNavMeshParams` の `maxTiles` / `maxPolys` は、タイルを何回差し替えても ref のビット配分が変わらない値で固定する。salt の桁が 1 セッションで巡回しないことを ADR に根拠付きで書く。
  - F4. 復元方式 (N2/N3) は「タイルの中身がアセットと違う」状態でも成り立つ。sub-01 の試作で、1 枚のタイルを**別ジオメトリからベイクした層で差し替えた後**に保存 → 復元 → 連続実行一致を確かめる。
  - F5. TileCache の層の所有者は NavSystem (アセットのバッファを借りない)。アセット由来と実行時生成の層が同じ寿命管理で共存できる。
- 反復順序: dtCrowd へのエージェント追加・TileCache への障害物追加はエンティティキー順。ポインタ・ハッシュコンテナの順序に依存しない。
- ベイクはエディタの非同期ワーカー (sim 外)。実行時 (sim 内) の TileCache 更新は同期で、時間を計測して SelfTest のログに出す。
- ビルド: vendor したソースは libtess2 と同じ扱い (`build\Engine.vcxproj` に手書き、`WarningLevel=Level3`・`TreatWarningAsError=false`、`/fp:precise` 継承)。`check_rules.ps1` は `src\` しか見ないので vendor 側の `rand()` 等は対象外だが、Recast 本体に `rand()` があれば呼ばれない経路であることを確かめる。
- レイヤー: Recast の型は `src\Engine\Engine\Navigation\` の中に閉じ、Shared (ABI) へは POD だけ。Editor はベイクの呼び出しと表示のみ。

## 5. 受け入れ条件

1. Recast が `external\recastnavigation\` にあり、`external\VERSIONS.md` にタグ / コミットとライセンス (Zlib) が載る。Debug / Release のビルドで、vendor したファイルと M82 の新規・変更ファイルに警告が 0。`/p:MyeWarnAsError=true` の全体成功は、HEAD 由来の `src\Engine\Renderer\Compute\ProjectComputeRunnerSelfTest.cpp` の C4127 (M75g の記録でも既知、M82 と無関係) が直るまで条件にしない。— msbuild の警告一覧
2. 固定の試作シーン (手続き生成のジオメトリ) で、ベイク → 経路クエリ → 障害物の追加・削除 → Crowd 100 tick の結果ハッシュが Debug と Release で一致。パッチを当てたら `PATCHES.md` に記録。— `Editor.exe --selftest` の NavDeterminism 項目を両構成で実行し、ログのハッシュを照合
3. N2・N3 を満たす復元方式が決まり、ADR-023 に「採用方式・却下案と理由・capture に足した時間 (µs / tick)」が書かれている。— SelfTest (連続実行 vs restore 再開のハッシュ一致) + ADR
4. Create → 3D Object の 4 項目で、コンポーネント付きエンティティが作られ Undo / Redo できる。— SelfTest または `--screenshot` で目視
5. Surface の Bake で `.mnav` が作られて GUID で参照され、Clear で外れる。同じ入力のベイクはバイト一致 (Debug / Release)。Runtime.exe でシーンを開くと NavMesh がロードされる。— SelfTest + Runtime 実行ログ
6. Agent が目的地へ、段差 (`maxClimb` 以下、CC の `stepOffset` ≥ `maxClimb`)・坂 (`maxSlopeDeg` 以下。既定設定で 30 度以上を含む、2. #19) を越えて到達し、`Arrived` になる。届かない目的地は `NoPath` か部分経路で止まる。CC が無い Agent は `Inactive`。— SelfTest
7. 2 体以上の Agent がすれ違いで重ならない (回避あり) / 回避なしでは重なりうる。— SelfTest (距離の最小値)
8. Obstacle (carve) を経路上に置くと同じ tick 内に TileCache が確定し、その tick の Agent が迂回する。消すと元に戻る。— SelfTest
9. Modifier のエリアとコストで経路が変わる (高コスト域を避ける)。`areaMask` で通れないエリアを避ける。— SelfTest
10. Link の Linear / Jump / Manual がそれぞれ渡れる (Manual は完了通知まで止まる)。片方向の Link は逆向きに使われない。— SelfTest
11. NavMesh 系コンポーネントが無いシーンでは、既存の replay_verify 全ジョブが PASS し、golden (`shot_verify`) が変わらない。**例外 (sub-10、2. #5 / #21)**: CharacterController を含むシーン (`--acoustic-demo` / `--nav-demo`、外部プロジェクト) は、CC の既定 `stepOffset` 0.3 (scale.y 倍) による挙動の変化だけを認める。`--acoustic-demo` は、拡大した Agent Eye (scale.y 1.6、実効 0.48 m) が衝撃板 (0.45 m) に乗り上がることで変わる。これはユーザーが承知のうえで受け入れた (2026-10-04)。条件は sub-10 の「既存への影響の切り分け」: (i) 全 CC の `stepOffset` を 0 に強制した実行が、着手前 HEAD と CC の状態 (位置・速度・接地) で全 tick 一致する (ii) 0.3 での差は CC を持つエンティティから始まる (最初に割れる tick とエンティティを記録)。golden の `--update` を認めるのはこの条件を満たした画像だけ。着手前から FAIL している 5 枚 (parts / joints / acoustic_forward / acoustic_deferred / fracture_after) は sub-10 では `--update` しない (別の原因の FAIL を塗り潰さない)。— `tools\replay_verify.bat` / `tools\shot_verify.bat`
12. replay_verify に `nav` ジョブ (`--nav-demo`: Agent 数体・Obstacle の出し入れ・Link・Modifier を含む) が追加され、Debug / Release / Server.exe と snapshot stress で PASS。— `tools\replay_verify.bat`
13. デバッグ描画 (NavMesh 輪郭・Link・Obstacle・Agent 経路) が Editor と Runtime で出る。— `--nav-demo --screenshot` の画像
14. ABI の新関数が C++ (GameLogic) と C# から呼べて、版と slot 数が `check_rules.ps1` と一致。— `check_rules.ps1` + C# は temp プローブで実走 (C# レーンは replay 被覆外)
15. 全 UI 文字列が en / ja で埋まり、`check_rules.ps1` が 0。— `tools\check_rules.ps1`
16. ADR-023 (将来の再ベイクの差し込み方を含む)、`engine_spec.md` の NavMesh 節、`docs\engine-feature-guide.md` 9.3 の書き換え、`AcousticNav.h` のコメントの整理、`plans\m75-ugui.md` の TypeId / ABI 番号の注記。— 差分レビュー
17. NavMesh が半透明のエリア色で塗られ、輪郭線と一緒に Editor と Runtime で出る。床と Z ファイトしない。NavMesh 不変の tick で三角形を作り直さない。golden `nav` が `shot_verify` で PASS。— `Runtime.exe --nav-demo --screenshot` + ログ + `tools\shot_verify.bat`
18. 将来の実行時再ベイクの差し込み口 (4.4 F1〜F5) がある: (a) タイル差し替え後の保存 → 復元 → 連続実行一致の SelfTest (b) Engine 層の入力収集 + タイルベイク関数を Editor 無しの SelfTest から呼んで、エディタのベイクと同じバイトが出る (c) ADR-023 に再ベイクの足し方 (どの関数を、どのタイミングで呼ぶか) が書かれている。— SelfTest + ADR
19. CC の `stepOffset` = h で、高さ h の段差を速度 (0.5 m/s 以上) に関係なく登り、h + 0.05 m は登らない。既定は 0.3 (Unity と同じ)。`stepOffset` を 0 にした CC は着手前と同じ挙動 (CC の状態列が一致)。Surface のインスペクタに実際のセルサイズと実効の坂上限が出て、設定がそれを超えると警告。— PhysicsSelfTest / NavAgentSelfTest / NavEditorSelfTest / replay_verify

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | Recast の vendor と決定論・復元方式の試作 (タイル差し替え込み) | なし | 1, 2, 3, 18 (a) | `M82a: Recast Navigation を vendor し、ベイク・経路・TileCache・Crowd のビット一致と復元方式を実測する` |
| sub-02 | NavMeshSurface とベイク (.mnav) + 輪郭のデバッグ描画 | sub-01 | 4 (Surface), 5, 13 (輪郭), 15, 18 (b) | `M82b: NavMeshSurface と .mnav ベイク — エディタで Bake / Clear、Runtime でロード` |
| sub-03 | NavMeshAgent と dtCrowd の移動 + SimSnapshot の Nav 節 + `nav` ジョブ | sub-02 | 3 (実装側), 6, 7, 11, 12 (Agent のみ), 13 (経路) | `M82c: NavMeshAgent — dtCrowd で経路追従と回避、CC.moveInput 駆動、スナップショット対応` |
| sub-10 | 歩ける最大傾斜と段差の高さを設定どおりに効かせる (CC `stepOffset` + セルサイズ自動決定) | sub-03 | 6 (坂・段差), 11, 19 | `M82d: CharacterController に stepOffset を足し、NavMesh の最大傾斜と段差を設定どおりに効かせる` |
| sub-04 | NavMesh の半透明の塗り (エリア色) と golden `nav` | sub-10 | 13, 17 | `M82e: NavMesh を半透明のエリア色で塗る — 三角形のデバッグ描画レーンと golden nav` |
| sub-05 | NavMeshObstacle (TileCache の切り抜き) | sub-03 (sub-04 の後に直列) | 4 (Obstacle), 8, 12 (Obstacle 追加) | `M82f: NavMeshObstacle — TileCache の切り抜きを tick 境界で同期確定` |
| sub-06 | NavMeshModifier とエリアコスト | sub-03 (sub-05 の後に直列) | 4 (Modifier), 9 | `M82g: NavMeshModifier とエリアコスト — ベイク時と実行時のエリア書き換え` |
| sub-07 | NavMeshLink (Off-Mesh Link の渡り) | sub-06 | 4 (Link), 10, 12 (Link 追加) | `M82h: NavMeshLink — Off-Mesh Link を Linear / Jump / Manual で渡る` |
| sub-08 | スクリプト API (ABI bump + C# ミラー) | sub-07 | 14 | `M82i: NavMesh のスクリプト API (ABI vNN)` |
| sub-09 | 文書と全体検証 | sub-08 | 11, 12, 15, 16, 17, 18 (c) | `M82j: NavMesh の ADR-023 と仕様書・機能ガイドを更新し、全体検証を通す` |

**実行順は sub-03 → sub-10 → sub-04 → … → sub-09** (sub-10 はユーザー回答で後から足したので番号が飛ぶ。コミット接頭辞は実行順に M82d〜M82j)。sub-04〜sub-06 は依存上どれも sub-03 の後だが、`--nav-demo` / golden `nav` / TileCache の更新経路を共有するので**直列で回す**。sub-07 は sub-06 のエリア (Jump) を使う。

## 7. 未決事項・リスク

- Q1〜Q5 は回答済み (8. 変更履歴)。
- R7. 塗りの半透明混合が WARP (CI) と実 GPU で画素差を出す可能性。sub-04 で既存 golden の前例 (許容差 / `MYE_SHOT_SKIP_*`) に合わせて決める。
- R8. F3 の `maxTiles` / `maxPolys` を大きく取ると ref の salt 桁が減る。sub-01 で桁配分と巡回までの差し替え回数を数値で出す。
- R1. ビット一致が取れない箇所が Recast 本体の深部 (例: `rcRasterizeTriangles` のクリップ、`dtObstacleAvoidanceQuery` のサンプリング) にあるとパッチ範囲が広がる (+1〜3 サブ)。sub-01 で判明させる。
- R2. (解消、sub-01) 復元方式は (b')。残るのは Commit が O(全タイル) であること — sub-05 で数百タイル・障害物を毎 tick 動かす条件で計測し、必要なら最適化 (履歴依存を消す性質は保つ)。
- R3. (解消、sub-03 実測 → sub-10) CC は `stepOffset` で段差を登る。`maxClimb` 既定 0.3。
- R10. セルサイズ自動決定で既定の cs が 0.3 → 0.15 になり、ベイク量が約 4 倍・タイルの実寸が半分になる。sub-10 で既定範囲のベイク時間とタイル数を測り、`tileSize` の既定を決める。
- R4. (解消、sub-01) 経路要求は同じ update で完走させるパッチで途中状態を無くした。代わりに重い経路が 1 tick に集中しうる — sub-03 で最悪 tick の時間を計測し、問題なら**要求数 (件数) で**絞る (時間で絞ると決定論が崩れる)。
- R9. `/p:MyeWarnAsError=true` が HEAD (`ProjectComputeRunnerSelfTest.cpp` の C4127) で失敗する = CI の設定 (`ci.yml` の `MYE_MSBUILD_ARGS`) では現状ビルドが落ちる。M82 の範囲外。
- R5. ABI 番号は M75h と先着順。sub-07 着手時に `EngineAPI.h` の現在版を確認する。
- R6. replay_verify のジョブ数が増え実行時間が延びる (現在 14 ジョブ、`MYE_REPLAY_JOBS=3` で 239 s)。

## 8. 変更履歴

(確定後の変更のみ)

- 2026-10-04 / 出所: coder SELF_EVAL sub-10 round 2 (planner VERDICT OK)
  - 2. #20 の補足: cellHeight の自動値は、目標 max(0.02, min(cs/2, maxClimb/6)) を『maxClimb がちょうど整数セルになる分割数』に丸め、×0.9999 で floor の落とし穴を避ける。単純な maxClimb/6 では急な坂の設定で 1 セル欠ける (60 度が 56 度) のを SelfTest で検出したため。
  - 既知の限界: 段差を登る tick に、位置が 1 tick で 0.13〜0.16 m 前へ出る (velocity は moveInput の速さで頭打ち)。acoustic_forward / acoustic_deferred の golden は、Agent Eye の乗り上がりを画面に写さない (着手前とバイト一致)。どちらも ADR-023 に記録する (sub-09)。

- 2026-10-04 / 出所: ユーザー (司会経由、sub-10 VERDICT の `[ユーザーに聞ける]` 2 件への回答)
  - 2. #20: 裁定どおり (cellHeight を自動で細かくし、残りは Stuck で受ける)。
  - 2. #21: 「stepOffset を拡大縮小する (Unity と同じ)」。planner の裁定を撤回し、height と同じ |scale.y| 倍に変更。受け入れ条件 11 の例外に acoustic の変化を明記。sub-10 の round 2 の指摘に追加。

- 2026-10-04 / 出所: coder SELF_EVAL sub-10 round 1 (planner VERDICT)
  - 2. #20 (新設): 段差の量子化のずれ。`autoCellSize` で cellHeight も自動にする (sub-10 round 2)。詰まり検出は sub-05 に追加。`[ユーザーに聞ける]`
  - 2. #21 (新設): stepOffset はワールド m で、scale を掛けない (coder の判断を採用)。`[ユーザーに聞ける]`
  - 4.1: Surface の `tileSize` 既定を 32 → 48 に変えた (sub-10 の計測: cs 0.15 で 3×3 タイル・5.0 ms・63 KB が最速かつ最小)。

- 2026-10-03 / 出所: ユーザー (司会経由。CC の `stepOffset` 既定値の `[ユーザーに聞ける]` への回答「Unity と同じ 0.3」)
  - 2. #5: 既定を 0 → 0.3 に変更 (planner の裁定を撤回、反対意見は 2. #5 に記録)。フィールドの無い旧シーンの CC も 0.3 で読まれる。
  - 受け入れ条件 11: CC を含むシーンの変化は、stepOffset だけが原因と切り分けた場合に限って認める形に緩めた。19 の「既定 0 は不変」を「0 にした CC は不変」に変更。
  - sub-10: NavMeshAgent 用の 0.3 の特別扱いを削除 (既定が 0.3 になったため)。「既存への影響の切り分け」「外部プロジェクトの確認」「.rep の扱い」の節を追加。
  - 外部プロジェクトのデータ (シーン・golden) は、MyEngine の作業では書き換えない (更新はユーザーの判断)。

- 2026-10-03 / 出所: ユーザー (司会経由、spec 2. #19 の `[ユーザーに聞ける]` への回答「あるける最大傾斜や階段の高さを変更できるように」)
  - 解釈: 「階段の高さ」= CC が実際に越えられる段差を設定で変えられること、「最大傾斜」= `maxSlopeDeg` が ledge 判定に食われず設定どおりに効くこと。どちらも「値は既に Surface にあるが効いていない」ので、効かせる側を直す。
  - 2. #5 / #19 の結論を差し替え (CC は変えない → `stepOffset` を足す、制約として受け入れる → セルサイズ自動決定)。3. スコープの「やらない」から step offset を外し「やる」へ。受け入れ条件 6 を元の趣旨 (maxSlopeDeg 以下) に戻し、19 を新設。
  - **sub-10 を新設**し、sub-03 と sub-04 の間で実行する (sub-04 の依存を sub-10 へ)。コミット接頭辞を実行順に振り直し (sub-10 = M82d、sub-04〜09 = M82e〜M82j)。sub-04 の 3d (実効の坂上限の表示) は sub-10 へ移した。R3 を解消、R10 を追加。

- 2026-10-03 / 出所: coder SELF_EVAL sub-03 round 1 (planner VERDICT)
  - 2. #5: CC の登れる段差の実測値を記録し、maxClimb 既定 0.1 m を確定。
  - 2. #19 (新設) / 受け入れ条件 6: 坂の実効上限は atan(maxClimb / (2·cellSize)) (Recast の ledge 判定と隣接接続)。既定値は変えず、インスペクタで実効上限の表示と警告 (sub-04)。`[ユーザーに聞ける]`。
  - 4.1: NavMeshAgent に `pathPartial` (本文にあり表に無かった) を追加。`areaMask` は sub-06 で末尾追加 (エリアに意味が付くのが Modifier から)。Surface に `drawAgentPaths` (NoHash)。`avoidanceQuality 0` = 自分は近傍を見ない (他の Agent からは回避される。Unity の NoObstacleAvoidance と同じ)。経路の描画はコーナーの折れ線 + 目的地の縦線 (コリドーのポリゴン列は描かない)。

- 2026-10-03 / 出所: coder SELF_EVAL sub-02 round 1 (planner VERDICT)
  - 4.2: ビルド設定のコピー対象への `.mnav` 追加を削除。`BuildSettingsWindow.cpp:208-210` が `assets\` を再帰コピーするので不要 (spec の見立て違い)。
  - 2. #17 / 4.2: `.mnav` の読み込みを Preload 3 か所ではなく `NavSystem::Update` の遅延ロードに変更 (coder の逸脱を採用)。sub-03 の restore は Nav 節を当てる前に読み込みを済ませる条件を追加。
  - 2. #18 / 4.3: 編集中の SceneView にも NavMesh を出す (planner 裁定、`[ユーザーに聞ける]`)。Surface の範囲箱ギズモがどのサブにも割り当てられていなかったので sub-04 へ。

- 2026-10-03 / 出所: coder SELF_EVAL sub-01 round 1
  - 4.4: 復元方式を (b') で確定 (候補 a / b / c はどれも不成立、実測と根拠は ADR-023)。計測値と Off-Mesh を dtCrowd に渡らせない方針を追記。
  - 受け入れ条件 1: `/p:MyeWarnAsError=true` 全体成功を「M82 が触るファイルの警告 0」へ緩めた。理由は HEAD 由来の別ファイル (`ProjectComputeRunnerSelfTest.cpp` C4127) で、M82 の変更と無関係 (M75g の記録 `plans\m75-ugui.md` でも既知)。R9 に記録。
  - 7.: R2 / R4 を解消扱いにし、残る計測 (Commit の O(全タイル)、最悪 tick の経路探索) を sub-05 / sub-03 へ割り当てた。

- 2026-10-03 / 出所: ユーザー (司会経由の Q1〜Q5 回答)
  - Q1: 「再ベイクなしでよいが、将来再ベイクを入れることを考慮して設計すること」。再ベイクの実装なしは維持し、4.4 に F1〜F5 (タイル単位の差し替え口 / Engine 層の純関数ベイク / ref のビット配分固定 / 差し替えと両立する復元方式 / 層の所有) を追加。受け入れ条件 18 を新設し sub-01 / sub-02 / sub-09 に割り当て。3. スコープを更新。
  - Q4: 「Unity のような半透明の塗りも付ける」。planner の裁定 (線だけ) を撤回。4.3 に塗りと golden `nav` を追加、受け入れ条件 17 を新設、**sub-04 (塗り) を挿入**し、旧 sub-04〜08 を sub-05〜09 へ繰り下げ (コミット接頭辞も M82e〜M82i へ)。sub-05〜07 の golden 条件を「`nav` だけ更新可」に変更。
  - Q2 / Q3 / Q5: 裁定どおり。2. の該当行の「ユーザーの判断」欄を回答で埋めた。
  - 別件: エンジンのバージョン変更 `b1920a7` (kEngineVersion 0.6.8.22) が単独コミットされた。ABI (`MYE_API_VERSION` 23) と `kSimSnapshotVersion` (24) は不変を確認、M82 の版計画に影響なし。
