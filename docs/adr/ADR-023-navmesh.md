# ADR-023: NavMesh (Recast Navigation) と決定論・状態の復元

- 状態: **確定** (2026-10-04、M82a〜M82j)。方式と計測値は M82a の試作で確定し、M82b〜M82i の実装で
  各決定を足した。全体検証の結果は末尾の「検証結果」。
- 改訂: 2026-10-05 (M84a〜M84e、NavMesh の拡張)。決定 6 の「複数の Surface」を決定 14 で置き換え、決定 14 を足した。
  計画は `plans\ai-roadmap-m83-m86.md` の「M84 で決めたこと」。
- 出所: 依頼「ナビメッシュの実装」。仕様は `plans\m82-navmesh\spec.md`、事前計画は
  `C:\Users\akita\.claude\plans\imperative-scribbling-shore.md` の M-A。
- 試作の実体: `src\Engine\Engine\Navigation\NavTileCacheSupport.{h,cpp}` と
  `NavDeterminismSelfTest.{h,cpp}`。パッチは `external\recastnavigation\PATCHES.md`。

## 背景

エンジン唯一の AI 移動は `AcousticNav` (音響グリッド上の流れ場) で、「音が届いた所へ向かう」特殊解である。
段差・坂・敵同士の回避・ジャンプ地点・動く障害物は扱えない。汎用の経路探索として Recast Navigation
(Recast = ベイク、Detour = 経路クエリ、DetourTileCache = 動的な障害物、DetourCrowd = 群衆) を vendor して使う。

最大の制約は MyEngine の契約 (Debug / Release / Server.exe のシミュレーションがビット一致し、
`SimSnapshot` から復元しても連続実行と同じ結果になる) を壊さないこと。Recast は浮動小数の塊で、
`dtNavMesh` / `dtCrowd` / `dtTileCache` の内部状態は ECS の外にある。

## 決定 1: Recast v1.6.0 を TileCache 方式で使う

- `external\recastnavigation\` に v1.6.0 の Recast / Detour / DetourTileCache / DetourCrowd / DebugUtils を入れる
  (`external\VERSIONS.md`)。自前 A* へは置き換えない (事前計画で却下済み)。
- ベイクは `rcBuildHeightfieldLayers` で層を作り `dtBuildTileCacheLayer` で TileCache の層にする。
  `dtNavMesh` のタイルは層 + 障害物から `dtTileCache::buildNavMeshTile` が作る。
- 層の圧縮は**無圧縮の自作** `NavRawCompressor` (FastLZ を入れない)。`.mnav` のバイト列が圧縮器に依存しない。
- 層の所有者は `NavTileStore`。呼び出し側のバッファを借りず、コピーして持つ (アセット由来の層も、
  将来の実行時再ベイクで作った層も同じ寿命)。
- TileCache のエリア ID は 0 = 通行不可 (障害物の切り抜き)、63 = 歩行可。`NavMeshProcess` が 63 を
  `kNavAreaWalkable = 0` へ写す。仕様のエリア 0..15 を Modifier が層へ書くときは、0 を避けた ID を使う (sub-06)。

## 決定 2: ビット一致は実測で確認した。パッチは要らなかった

`NavDeterminismSelfTest` (固定ジオメトリ: 床・段差・登れない壁・坂・上の足場・柱) のハッシュ 16 個が
Debug Editor / Release Editor / Debug Server.exe / Release Server.exe で一致した (2026-10-03)。
対象: ベイクした層のバイト列、`dtNavMesh` の内容、`findPath` / `findStraightPath` / `raycast` /
`moveAlongSurface` / `findDistanceToWall` / `findRandomPoint` (Pcg32 の frand) の結果、障害物の出し入れ後の
内容、`dtCrowd` 8 体 100 tick の毎 tick の位置・速度・経路。`/fp:precise`・`/arch` 共通の効果と考えられるが、
`Common.props` の設定を変えた構成では再確認が要る。

`rand()` / `time` 系は Recast に無い。乱数は `findRandomPoint` 系の `frand` 引数だけで、引数なしの関数ポインタ
(`float (*)()`) なのでコンテキストを渡せない。呼び出し中だけ有効な静的ポインタ (`World::Rng()`) 経由にする。

## 決定 3: 復元方式は「部品から作り直す + 全タイルの正規化 + 明示的な表」

### 候補と結論

| 候補 | 内容 | 結論 |
|---|---|---|
| (a) 内部状態をそのまま保存 | dtNavMesh の free list・リンクプール・ハッシュ連鎖、dtPathQueue の途中状態まで書く | 却下。`addTile` は接続済みのデータを受け取れず (毎回リンクを張り直す)、`dtPathQueue` は private。パッチが広すぎる |
| (b) salt の表だけで作り直す (spec 4.4 の素朴版) | 層 + 障害物 + タイル salt から `dtNavMesh` を作り直す | **却下 (実測)**。下記の履歴依存で、復元後の `dtNavMesh` が連続実行とリンク順で割れる |
| (c) Recast の salt 設計を変える | salt を履歴に依存させない | 却下。salt は参照の失効検知で、問題の本体はリンク順 |
| **採用 (b')** | (b) + `Commit()` が変更のたびに全タイルをキー順に入れ直して履歴を消す + スロット表と salt を明示保存 + dtCrowd をエージェントごとに明示保存 | 全シナリオで連続実行と一致 |

### 履歴依存の実証

`dtNavMesh` のポリゴンのリンク順 (A* の展開順 = 同点のときの経路を決める) と、タイルの検索連鎖の順
(`getTilesAt` の返す順) は、`addTile` / `removeTile` の履歴で決まる。隣のタイルを入れ替えると、その隣接ポリゴンの
リンクは「入れ替えたタイルへのリンクが先頭」になる (新しいリンクは先頭へ挿す)。同じタイル集合でも、後から
入れ替えたかどうかでリンク順が変わる。

試験: 障害物の出し入れとタイル差し替えを実行し、`Commit()` の正規化を切ると、同じ最終状態のリンク順ハッシュが
割れた (`0x270640B3D9D88ECA` 対 `0xE2BAE9CBE3175BDA`)。salt だけを保存しても復元できない根拠。

### 採用した状態の定義 (NavTileStore)

状態 = 層 (差し替えたものだけ) + 障害物 (キー順) + スロット表 (層 -> `dtNavMesh` のタイル番号) +
スロットごとの salt。`Commit()` は (1) `dtTileCache::update` を upToDate まで回す、(2) 再構築した層 /
有無が変わった層のスロットの salt を 1 回だけ進める、(3) 全タイルのデータを複製し、全 `removeTile` の後、
スロット番号順に `addTile(data, ..., lastRef)` で入れ直す (`lastRef` に表の salt を入れる)。リンクプール・
ハッシュ連鎖・free list は入れ直しで決まるので、履歴が消える。`LoadState` は層・障害物・表を書き戻して
同じ `Commit` 相当の確定をする。

- 層のキーは (ty, tx, layer) の辞書順。障害物のキーは呼び出し側の決定的なキー (エンティティキー)。
  TileCache のスロット番号・障害物の ref・`dtPathQueue` のハンドルは状態に含めない (結果に影響しない)。
- `dtCrowd`: エージェントごとに、位置・速度・経路 (corridor)・近傍・衝突境界 (`dtLocalBoundary`、friend パッチ)・
  経路コーナー・目標状態を保存する。フィルタと回避パラメータは設定から再現する (保存しない)。
  **経路要求は同じ update 内で完走させる** (パッチ `MAX_ITERS_PER_UPDATE`)。tick 境界に途中状態が残らない。
  途中のエージェントがいると `NavSaveCrowd` は失敗を返す。

### 検証した項目 (全て Debug / Release / Server.exe で PASS)

- 連続実行 100 tick と、tick 50 を新しい世界へ復元して 50 tick 進めた結果が毎 tick 一致 (障害物のみ / タイル差し替え込み)。
- 毎 tick の 保存 -> 同じ世界へ復元 -> 再保存 でバイト列一致。結果も連続実行と一致 (`--snapshot-stress` 相当)。
- 変異試験: salt の表を戻さない / 差し替え後の障害物を付け直さない / 境界を捨てる、をそれぞれ入れると FAIL する。
- 障害物を足して外すと、リンク順を含む `dtNavMesh` の内容が最初と一致する。
- 障害物 70 個 (要求キュー 64 を超える) を 1 回の Commit で入れた結果が、1 個ずつ Commit した結果と一致する。

### 計測値 (`NavDeterminismSelfTest`、2026-10-03)

| 項目 | Release | Debug |
|---|---|---|
| 状態表の保存 (層差し替え 1 タイル + 障害物 2 個、6957 B) | 2.0 us | 86 us |
| dtCrowd の保存 8 体 | 5.3 us / 3.9 KB | 334〜362 us |
| dtCrowd の保存 128 体 | 65 us / 66.5 KB | 6.3 ms |
| 状態表の復元 (変更なし) | 5〜6 us | 約 90 us |
| dtCrowd の復元 8 体 / 128 体 | 4 us / 68 us | 40 us / 634 us |
| `Commit` (障害物 2 個の追加、層 13 枚、TileCache の再構築を含む) | 666 us | 1.1 ms |
| `Commit` (変更なし、正規化のみ) | 12.5 us | 54 us |

ロールバックは毎 tick capture する (約 148 KB)。Nav 節は 8 体で約 +4.5 KB (+3%)、128 体で +67 KB (+45%)。
試作時の見積もり。M82c の実機の計測で 128 体は +26% (38,782 B) に収まったため、差分化しないと決めた (決定 7)。

## 決定 4: 将来の実行時再ベイクの差し込み口 (spec 4.4 F1〜F5)

再ベイクは作らない。次の口を作ってある。

- F1. タイルの差し替えは `NavTileStore::ReplaceTileLayers(tx, ty, layers)` + `Commit()` の 1 本。
  重なる障害物は外して付け直す (残すと `touched` が古い層の ref を指し、新しい層に効かない)。
  再ベイクは「別の生産者が `ReplaceTileLayers` へ新しい層を渡す」で足せる。
- F2. 入力収集 (`NavPrepareBakeInputs` = World + Surface から設定と三角形、`NavCollectTriangles` は範囲にタイルの AABB も取れる) と
  タイル 1 枚分のベイク (`NavBakeTile`) は Editor に依存しない関数。エディタの Bake は `NavBakeAsset` (全タイルを
  `NavBakeTile` で回す) をワーカースレッドで呼んで `.mnav` に書くだけ。`NavBakeTile` の結果は `NavBakeAsset` と同じバイトになる
  ことを Editor 無しの SelfTest で確認している (`NavSurfaceSelfTest`)。
- F3. `dtNavMeshParams` の `maxTiles` / `maxPolys` は固定し、タイルを何度差し替えても ref のビット配分は変わらない。
- F4. 差し替えた層は状態表が持つ (`isBase = false` の層は SaveState に実体が入る)。1 タイル 2 層の差し替えで
  約 6.3 KB。差し替えを頻繁に行う場合は、層を内容ハッシュで引く別の保管庫に出してスナップショットにはハッシュだけを
  書く (将来)。
- F5. 層の所有者は `NavTileStore`。アセット由来の層 (`AddBaseLayer`) も差し替えた層も `std::vector<uint8_t>`
  のコピーを持つ。

### 将来の実行時再ベイクの足し方 (今は未実装)

ユーザー判断 (2026-10-03): 実行時の再ベイクは今回入れない。ただし将来入れることを考慮した設計にする
(planner の当初の裁定「差し込み口なし」から変更)。上の F1〜F5 がその備えで、足すときの手順は次のとおり。

| 順 | いつ | 何を呼ぶか |
|---|---|---|
| 1 | tick 境界。`NavSystem::Update` の先頭、`SyncSurfaces` の後・`SyncObstacles` の前 (TickRunner のフェーズ 3.4b) | 変化したタイルの集合を、(ty, tx) の辞書順で列挙する。変化の検知 (どのコライダーが動いたか) は新しい生産者の仕事 |
| 2 | 同じ tick の同じステップ | `NavCollectTriangles(world, タイルのAABB, collectLayerMask, soup)` で三角形を集め、`NavBakeTile(config, soup, tx, ty, layers)` で層を作る。`config` は Surface から `NavMakeBakeConfig` で作る (ベイク時と同じ値) |
| 3 | 同じステップ | `NavTileStore::ReplaceTileLayers(tx, ty, layers)`。重なる障害物は外して付け直される |
| 4 | 同じステップの最後 | `NavTileStore::Commit()` を 1 回。ここで全タイルがキー順に入れ直され、履歴依存が消える。`Obstacle` / `Modifier` / `Link` の同期はこの後ろ (今と同じ) |

未実装のもの (足すときの宿題):

- 変化の検知と、どの tick に何枚まで焼くか。Recast を sim の中で回すので、時間予算を **件数** で絞る
  (時間で絞ると決定論が崩れる)。1 tick の上限を超えた分は翌 tick に持ち越し、持ち越しの状態もスナップショットに入れる。
- 焼いた層の保管。差し替えた層は状態表がバイト列ごと持つ (F4)。頻繁に差し替えるなら、内容ハッシュで引く保管庫に出して
  スナップショットにはハッシュだけを書く。
- 追加の決定論の検証 (sim 内の Recast が Debug / Release / Server で一致すること。M82a の `NavDeterminismSelfTest` は
  ベイクを sim 外で行った場合の検証で、sim 内の呼び出しでは未確認)。
- `kSimSnapshotVersion` と `NavTileStore` の状態版の bump (保存形式が変わるとき)。

差し替えを 1 回でも行ったシーンの保存 -> 復元 -> 連続実行の一致は M82a の SelfTest で確認済み (受け入れ条件 18 (a))。

## salt の桁配分

`dtNavMesh` の ref は salt / タイル番号 / ポリゴン番号に 32 ビットを分ける。
`saltBits = 32 - ceil(log2(maxTiles)) - ceil(log2(maxPolys))`、巡回までの差し替え回数 = `2^saltBits - 1` (スロットごと)。

| maxTiles | maxPolys | salt ビット | 巡回までの差し替え |
|---|---|---|---|
| 64 (試験) | 1024 | 16 | 65535 |
| 1024 (推奨の上限) | 1024 | 12 | 4095 |
| 4096 | 1024 | 10 (下限) | 1023 |

`dtNavMesh::init` は salt が 10 ビット未満だと失敗する。`maxTiles` = max(ceil(1.5 × 層数), 層数 + 4) (再ベイクで層が増える余裕)、
`maxPolys` = NextPow2(2 × 実測の 1 タイルの最大ポリゴン数) で下限 64 (`NavBake.cpp`)。ref のビット配分で salt が 10 ビット未満になるなら
ベイクは Failed になり、メッセージでタイルまたはセルを大きくするよう求める。
スロットごとの salt は `Commit` につき 1 回だけ進むので、巡回するのは 4095 回の Commit (約 68 秒、毎 tick 差し替えた場合)
の後。古い ref が生き残るのは (a) crowd の経路、(b) 呼び出し側が保持した ref で、どちらも毎 tick 検証される
(`dtCrowd::checkPathValidity`、`isValidPolyRef`)。取り違えが起きるのは、1 つの ref が検証されないまま同じスロットの salt が丸 1 周する場合だけで、Commit は 1 tick に 1 回なので起きない。
salt の値自体も SimSnapshot に入る (スロットごとに u32)。

## Detour の制約 (実装の注意)

- `dtTileCache::update` が積める tile 更新は 64 件 (`MAX_UPDATE`)、障害物 1 個が触れる層は 8 枚 (`DT_MAX_TOUCHED_TILES`)
  まで。上限を超えると更新が黙って落ちる。`NavTileStore` は障害物の要求を 8 件ごとに処理する。
- 要求キューは 64 件 (`MAX_REQUESTS`)。8 件ごとの処理で超えない。
- 障害物の `maxObstacles`・タイルの `maxTiles` は固定長。超えると追加が失敗する。
- `Off-Mesh Link` の渡りの状態 (`dtCrowd::m_agentAnims`) は保存していない。渡りは `NavSystem` が持ち、Nav 節に保存する (決定 11)。
- `LoadState` / `NavLoadCrowd` は失敗を返したときに途中まで書き換えた状態を残す。失敗は致命として扱う (sub-03)。

## AcousticNav との役割分担

`AcousticNav` は「聞こえた所へ向かう」特殊解で、音の伝播と同じ占有配列から流れ場を作る (M65 の判断)。
`AcousticNav.h` の「NavMesh を作らないのが判断の前提」は音響 AI の局所判断で、エンジン全体の禁止ではない。
NavMesh は汎用の移動 (段差・坂・回避・Link・動く障害物)。`AgentBrainComponent` は従来どおり `AcousticNav` を使い、
同じエンティティに `NavMeshAgent` があれば後に走る NavSystem の moveInput が勝つ (Inspector に警告)。
`AcousticNav.h` のコメントは「音響ナビは NavMesh を使わない。汎用の移動は ADR-023」の意味に整理した。

## 決定 5: ユーザーが planner の裁定を覆した点

planner はユーザーに聞けない環境で裁定し、ユーザーが後から変えたものがある。経緯ごと残す。

| 論点 | planner の裁定 | ユーザーの判断 (採用) | planner の反対意見 (記録) |
|---|---|---|---|
| 実行時の再ベイク | 差し込み口を作らない | 実装はしないが、将来入れることを考慮した設計にする (決定 4) | — |
| NavMesh の表示 | 線だけ | Unity のような半透明の塗りを付ける (決定 9) | 半透明が WARP と実 GPU で画素差を出しうる |
| 歩ける最大傾斜と段差 | 既定設定の実効上限 (約 9 度) を制約として受け入れ、Inspector に表示する | 「あるける最大傾斜や階段の高さを変更できるように」 (決定 8) | — |
| `stepOffset` の既定 | 0 (既存の CC は不変) | Unity と同じ 0.3 | 既存ゲームの挙動が予告なく変わる。既存デモの tick ハッシュ列の連続性が切れる |
| `stepOffset` と scale | 掛けない (ワールド m) | Unity と同じく `\|scale.y\|` を掛ける | 見た目の拡大で登れる高さが変わるのは予想しにくい。acoustic デモの Agent Eye (scale.y 1.6) が衝撃板に乗り上がり、acoustic の replay 基準と golden が動く |

裁定どおりだったもの: Agent は CharacterController 必須、Link は Linear / Jump / Manual の 3 種、スクリプト API は M82 に入れる、
編集中の SceneView にも NavMesh を出す、cellHeight も自動で決め残りは Stuck で受ける。

## 決定 6: データと読み込み

- **TypeId**: 末尾 append で 71 NavMeshSurface / 72 NavMeshAgent / 73 NavMeshObstacle / 74 NavMeshModifier / 75 NavMeshLink。
  予約はしない (シーンは型名で保存され、TypeId が入るのは SimSnapshot と使い捨ての `.rep` だけ)。UI の M75h (InputField) は
  76 以降へずれる (`plans\m75-ugui.md` に注記)。
- **`.mnav`** (`assets\NavMesh\<Surface名>_<入力ハッシュ16桁>.mnav`、GUID 参照): ベイク設定 + 入力ハッシュ + TileCache の層。
  `.mfrac` と同じ流儀 (版付き、境界検査、同じ入力から同じバイト、非同期ベイク、1 Undo で参照を設定)。
  ビルド (配布物) へは `assets\` の丸ごとコピーで入る。Link と Modifier は入れない (どちらも World から差す実行時のオーバーレイ)。
- **読み込み**: `NavSystem::Update` が、stepSim の tick ごとに Surface の (entity, navAsset GUID) をキー順に並べ、前回と違うときだけ
  読み直す (遅延ロード)。`PreloadFractureAssets` のように呼び出し箇所を 3 つに散らさず、シーン遷移 / Play 開始 / 復元のどれでも
  「その tick の World」で決まる。SimSnapshot の復元は Nav 節を当てる前に同じ読み込みを済ませる (空の NavSystem へ復元した直後の
  Update が読み直して復元状態を上書きしない)。
- **Presence gate**: NavMesh 系コンポーネントが無いシーンでは、RNG・ワールドハッシュ・SimSnapshot の Nav 節の中身が
  どれも変わらない (節自体は空で書く)。NavMesh が World の RNG を引くのは `NavFindRandomPoint` だけ (決定 12)。
- **複数の Surface と容量**: ~~同じ `agentTypeId` の Surface が複数あればエンティティキーの小さい方が勝つ (警告つき)。~~
  M84b で、同じ `agentTypeId` の Surface はまとめて 1 つのナビメッシュに焼くように改めた (決定 14 の B)。
  dtCrowd の容量はナビメッシュ (グループ) ごとに 128 体で、超えた Agent はキー順の後ろから `Inactive` になる。

## 決定 7: Agent は最初から dtCrowd、CharacterController 必須

- 経路追従と回避を別々に作ると Crowd 導入時に捨てるので、事前計画の「単独追従」と「Crowd」を統合した。回避の強さは
  Agent の `avoidanceQuality` (0 = 自分は近傍を見ない。他の Agent からは回避される。Unity の NoObstacleAvoidance と同じ)。
- **CharacterController 必須**: 重力・接地・衝突を CC に任せ、経路と物理の二重管理を作らない。NavSystem は dtCrowd の望む速度を
  `CC.moveInput` に書く。CC が無い / Rigidbody で無効な Agent は動かさず `Inactive` (警告)。Transform を直接動かすモードは作らない。
- **Tick の位置**: TickRunner のフェーズ 3.4 (音響 + AgentSystem) の後、3.5 の前に独立した `if (stepSim)` で `NavSystem::Update`。
  順序は (1) Surface の読み込み (2) Obstacle / Modifier / Link の差分を TileCache に反映して `Commit` (3) Agent をエンティティキー順に
  dtCrowd と同期 (CC の実位置を crowd 側へ書き戻す) (4) `dtCrowd::update(1/60)` (5) 望む速度を `CC.moveInput` へ、状態を Agent へ。
  物理 (3.6) の後に `NavSystem::PostPhysics` が Link を渡っている Agent の位置を上書きする。
- **クエリは前の tick の状態を見る**: ABI のクエリ (決定 13) と Agent の状態は、前の tick の `Update` で確定した値。スクリプトは
  `Update` より前に走るので、シーンを読んだ最初の tick は未ベイクと同じ 0 を返す。
- **経路が完全にないとき**: 目的地まで届かなければ dtCrowd の部分経路で最寄りの到達可能点まで歩き、`Arrived` + `pathPartial`。
  目的地も始点も NavMesh に乗らない (最寄り点が無い) ときだけ `NoPath`。
  部分経路の `Arrived` は、終点まで max(stoppingDistance, radius) 以内で前進が 60 tick 止まったときに確定する。完全な経路より
  1 秒遅れる (dtCrowd の位置が終点の数 cm〜0.12 m 手前で止まるため。原因は dtPathCorridor の位置の置き方と見ているが追跡していない)。
  既知の限界として受け入れた。即時に確定したければ「終点の近くで速度 0」を見る形で足せる。
- **Stuck** (`status = 6`): `Moving` の残り距離が **60 tick** の間に基準から max(radius/4, 1 cm) 以上縮まなければ `Stuck` と表示し、
  WARN を 1 回出す。**止めない**: crowd の目標と移動入力は保ち、押し続ける。前進が戻れば (基準から 1 回でも縮む・遠ざかる) `Moving` へ戻る。
  前進が止まっている Agent は、次のどちらかなら `Arrived` にする (レビュー round 1 #1: 同じ目的地へ向かう複数の Agent が目的地の手前で
  渋滞し、回避ありだと全員が恒久停止していた)。(a) 残り距離が max(stoppingDistance, 2 × radius) 以内。(b) 同じ目的地
  (差 ≤ stoppingDistance) で既に `Arrived` の Agent に、中心の水平距離が半径の和の 2 倍以内 (渋滞では dtCrowd の分離が接触より広く離す) で接している。(b) は連鎖するので N 体が
  全員いずれ `Arrived` になる。判定はキー順で、同じ tick に先に着いた Agent も後続の判定に使う (連鎖は 1 tick で伸びる)。
  渋滞の後続は Stuck にしない (レビュー round 2 #9): 前進が止まっていても、同じ目的地 (差 ≤ stoppingDistance) へ向かい、
  自分より残り距離が厳密に短い `Moving` / `Stuck` の Agent に水平の中心距離が半径の和の 3 倍以内で接している間は `Moving` のままで WARN も出さない
  (到着の (a)(b) より後に判定する。回避が強いと列の間隔が開くので、到着の (b) の 2 倍より広い 3 倍にした)。厳密に短いものに限るので互いに待つ循環は起きない。
  Stuck になるのは前に接した仲間のいない先頭だけ。
  部分経路の終点の近くで止まるのも到着として扱う。Stuck のまま残るのは、完全な経路の途中を塞がれて押し合う Agent (塞ぎを消せば
  `Moving` → `Arrived`)。理由: Recast の段差判定は cellHeight の整数セルで、NavMesh の登れる高さと
  CC の `stepOffset` に 1 セル未満のずれが残る (決定 8)。縁で押し続ける Agent を「理由が分からない無応答」にしない (AGENTS.md 3.4)。
  定数の妥当性を実ゲームの渋滞で確かめてはいない (三校は AgentBrain を使うので当面影響しない)。
- `targetPathqRef` は tick の末に `DT_PATHQ_INVALID` に正規化する。`dtPathQueue` の連番は復元すると 1 から始まるので、そのままでは
  連続実行と復元後でバイト列が割れる。経路要求は同じ update で完走する (パッチ 1) ので、tick 境界に生きた要求は残らない。
- 回避なし (`avoidanceQuality` 0) の Agent は `collisionQueryRange` を 0.01 にして近傍探索ごと止める。dtCrowd の衝突解決 (押し戻し) は回避の設定と無関係に近傍全員へ働くので、止めないと「回避なしですり抜ける」にならない。他の Agent からはこの Agent が見える。
- Nav 節の実測 (Release): 6 体 2,894 B / 128 体 38,782 B、capture +64 µs、restore 58 µs。ロールバックの毎 tick capture は
  約 148 KB で、128 体のとき +26%。**差分化しない** (ロールバックの帯域に効く規模ではないと判断)。最悪の tick の `dtCrowd::update`
  は 128 体の一斉要求で 0.76〜0.87 ms。`dtPathQueue` の容量 (8 件) が自然に絞るので、件数の上限は足さない。
- 坂の実効上限は Recast の ledge 判定と隣接接続のため atan(maxClimb / (2·cellSize))。CC が登れる段差も実測した
  (速度しだいで 1.5 m/s のとき 0.15 m、3.5 m/s のとき 0.25 m)。これが決定 8 の動機になった。

## 決定 8: 歩ける傾斜と段差を設定どおりに効かせる

ユーザー判断 (決定 5) を受けて、CC と Surface の両側を直した。

- **CC の `stepOffset`** (既定 0.3、範囲 0..5、ハッシュ対象): 前の tick に接地していてジャンプしておらず、水平移動の進みが歩幅の半分未満の
  キャラが、「stepOffset だけ持ち上げる -> 前へ出す (歩幅から歩幅 + 半径まで 5 段階) -> 真下へ着地点を二分探索 (12 回固定)」を試す。
  速度に関係なく設定の高さまで登る。上限は実効の全高。0 以下と NaN は登らない。回数は固定で決定論を保つ。
  着地面の法線が `slopeLimitDeg` より急なら登らない。
- **`stepOffset` は `|scale.y|` 倍** (height と同じ規則)。段差は縦の長さなので、縦の量である height に揃えた (radius の水平 max 規則は使わない)。
  影響: acoustic デモの Agent Eye (scale.y 1.6) の実効段差が 0.48 m になり、衝撃板 (天面 0.45) に乗り上がる。
  `--acoustic-demo` の replay 基準が動くのはこのためで、ユーザーが承知のうえで受け入れた。**acoustic_forward / acoustic_deferred の
  golden は Agent Eye の乗り上がりを画面に写さない** (撮影の範囲に入らず、着手前とバイト一致)。つまり golden はこの変化を守らない。
  両 golden は着手前から FAIL していて M82 では撮り直さない。将来撮り直すときは、この挙動の変化が含まれることを確かめること。
- **登る tick の跳び**: 位置は 1 tick で 0.13〜0.16 m 前へ出る (0.15 m の段で測定)。velocity は moveInput の速さで頭打ちにする
  (登りのための前進を移動速度に数えない)。見た目が気になるなら定数 `kStepAdvanceTries` と上限で調整できる。
- **セルサイズの自動決定** (Surface の `autoCellSize`、既定 on): `cellSize = min(agentRadius/2, climb / (2·tan(maxSlopeDeg)))`
  (下限 `kNavMinCellSize` = 0.05。下限に当たったら警告)。既定値どうし (半径 0.3、climb 0.3、45 度) で 0.15 m、実効の坂の上限は 45 度。
  climb は生の `maxClimb` ではなく Recast に渡る `floor(maxClimb / cellHeight) × cellHeight`。Inspector は実際のセルと実効の坂上限を表示し、
  設定が超えると警告する。`tileSize` の既定は 32 -> 48 (計測: 既定範囲 20×10×20 で cs 0.15 のとき 48 が最速かつ最小、3×3 タイル 5.0 ms・63 KB)。
- **`cellHeight` も自動**: 目標 max(0.02, min(cs/2, maxClimb/6)) を、maxClimb がちょうど整数セルになる分割数 N に丸め、×0.9999
  (floor の落とし穴を避ける)。既定で 0.05 m。単純な maxClimb/6 では、急な坂の設定 (60 度) で実効の climb が 1 セル欠けて 56 度になった。
- **段差判定の量子化のずれ**: Recast は段差を cellHeight の整数セルで比べるため、NavMesh の段差上限は設定の `maxClimb` から最大 1 セル未満
  (既定で 5 cm 未満) 超えうる。CC は `stepOffset` ちょうどまでしか登らないので、ずれた段差の縁で Agent が押し続ける。残りは Stuck (決定 7) で受ける。
  却下: NavMesh 側を 1 セル控えめにする = `maxClimb` ちょうどの段差が繋がらず、「`maxClimb` の段差を越える」が成り立たない。
  Inspector への注記だけ = Agent が無言で止まり続ける。
- 却下: 坂の制約として受け入れる (planner 当初)、Recast 本体のパッチ (ボクセル上で坂と段差を区別する情報が無く、意味論ごと変わる)、
  既定の cellSize を固定値で細かくするだけ (`maxClimb` / `maxSlopeDeg` を変えるたびに手で合わせることになり「変更できる」にならない)。
- 既存への影響: `stepOffset` 0 の CC は着手前と CC の状態列が一致する。既定 0.3 では acoustic の Agent Eye 以外のサンプル
  (旧 nav デモ・三校 verify・HAL Collector) で差が出なかった。`kSimSnapshotVersion` 25 -> 26 (World 節が伸びる)。旧 blob は明示的に拒否する。

## 決定 9: 表示は描画フレーム側の NavDebugView、半透明の塗りを付ける

- ユーザー判断 (決定 5): Unity のような半透明の塗りも付ける。
- NavMesh 本体 (塗り・輪郭・タイル境界) は `NavDebugView` が作り、`RenderSystem::navView` 経由で `NavFillPass` (塗り) -> `EditorLinePass` (線) の順に描く。
  sim の tick に頼らないので、編集中 (非 Play) の SceneView にも Play 中にも同じ経路で出て、resim の影響を受けない。
  編集中は `.mnav` から、Play 中は NavSystem の dtNavMesh から作る。
- 作り直しは (Surface の entity, navAsset, 表示フラグ) と `NavTileStore::Generation` (プロセス内で一意) が前回と違うときだけ。不変の tick では
  三角形を作らない (`[nav] debug view rebuilt (#N)` のログ)。Obstacle などで実行時に変わった NavMesh も Generation で拾う。
- 床との Z ファイトは持ち上げ (塗り 2 cm / 線 3 cm) と傾斜つき深度バイアスで避ける。`editor_line.hlsl` を流用していて新シェーダは無い。
- 却下: 「sim -> 描画の三角形レーン」(`DebugLineCmd` と同じ経路で塗りの三角形を積む)。編集中の表示と Play 中の表示が 2 経路に割れ、
  tick レーンは resim のたびに積み直しになる。Agent の経路線だけは従来どおり tick の `debugLines` (resim 中は積まない)。
- golden `nav` (`--nav-demo` frame 120、tol=3) は `MYE_SHOT_SKIP_NAV` の囲い付き。半透明の混合が WARP (CI) と実 GPU で画素差を出す
  可能性があり、開発機の WARP で撮った。**CI の WARP での一致は未確認** (赤くなったら `ci.yml` に 1 行足す)。

## 決定 10: Obstacle は tick 境界で同期確定、carve=false は何もしない

- `NavSystem::SyncObstacles` が、carve の立った Obstacle のワールド形を store の障害物 (キー順、復元済みの状態そのもの) と突き合わせ、
  外す -> 付ける -> `Commit` を同じ tick の内に済ませる。NavSystem 自身は前回の記録を持たないので、復元の直後でも二重追加・消し忘れが起きない。
- **Box の切り抜き**: y 回転だけの Box は `DT_OBSTACLE_ORIENTED_BOX` で実形のまま切り抜く (状態の保存形式 v2 に yaw)。x / z に傾いた Box は
  回転後の外接 AABB。Cylinder は y 回転を無視。動きは **5 cm** (`kObstacleMoveThreshold`) 以内なら付け直さない。
- Obstacle は、ワールド AABB が重なる Surface にだけ付ける (範囲外のものは TileCache に触れない)。
- **`carve = false` は何もしない**。Unity の「carve しない = 動く障害物を Agent が回避だけする」は、dtCrowd に任意形状の動く障害物を
  回避する口が無く再現できない。Inspector に注意書きを出す。
- 作り直されたタイルが無い tick では dtNavMesh の入れ直し (正規化) を省く (中身が同じ形になるため)。履歴依存を消す性質は復元一致の SelfTest で確認。
- 計測 (324 タイル、障害物 8 個を毎 tick 動かす、Release): 平均 857 µs / 最悪 1,258 µs、うち入れ直し 約 266 µs。Debug は平均 4,239 µs。これ以上の最適化は不要と判断した。
- `HasPolyAt` (試験用) は `findNearestPoly` が穴の縁のポリゴンも拾うので、最寄り点が 5 cm 以内のときだけ「歩ける」と判定する。
- 版: `kSimSnapshotVersion` 26 -> 27、NavTileStore の状態 v2。

## 決定 11: Modifier とエリアコスト、Link の渡り

### エリアとコスト

- **エリアコストは Surface コンポーネントに置く** (16 種)。`project_settings.json` に置くと、sim の入力がシーンとリプレイの外に出る
  (`.rep` は SimSnapshot を埋め込むだけで project_settings は含まない)。エリアの**名前**だけ project_settings (表示用)。
  Agent は `areaMask` だけ持ち、Agent ごとのコスト上書きは作らない。
- ポリゴンのフラグは `1 << area`。filter は `areaMask` の種類ごとに 16 種まで割り当て、超えたら最後の filter を共有して警告を 1 回出す。
- **Modifier はベイクに焼き込まない**。焼き込むと、実行時に動かす・消すときに元のエリアへ戻せない。TileCache の層の上のオーバーレイとして、
  Obstacle と同じ tick 境界の同期更新で塗る。編集中の表示も同じ関数で重ねる。将来の再ベイク (決定 4) とも衝突しない。
- **Recast パッチ 3**: 障害物に `paint` / `priority` / `areaId` を足した。paint は切り抜かず範囲内のエリアを塗り替える。塗り順は `priority`
  (エンティティキーが大きい方が勝つ) の昇順、そのあとに切り抜き。**`areaId` が 0 でないとき、通行不可のセルを復活させない**。
- 高コストの帯を避けさせるために、コストを 1 より上げたエリアがある Surface では `DT_CROWD_OPTIMIZE_VIS` を外す (視線の近道がエリアコストを
  見ずに帯を突っ切る。実測あり)。既定 (全コスト 1) の Surface は従来どおり。エリア 1 (NotWalkable) にした範囲は NavMesh の外と同じ扱い。
- 版: `kSimSnapshotVersion` 28、NavTileStore の状態 v3 (障害物に area)。TypeId 74。

### Off-Mesh Link

- 渡り方は Linear / Jump / Manual の 3 種 (梯子・ドアは Linear に速度と種別ラベルを付けて表す)。Manual は止まって状態を公開し、
  スクリプトが `NavCompleteLink` で完了を通知する。
- **Link は `.mnav` に入れない** (手置きの NavMeshLink の話。ベイクで自動生成した Link は焼く、決定 14 の E)。Modifier と同じ理由で持ち主をコンポーネントだけにし、World から差分で TileCache のタイルへ
  Off-Mesh Connection として差し込む (入口を持つタイル列だけ作り直し、Obstacle / Modifier と同じ `Commit` にまとめる)。
- **制約**: 出口は入口のタイルと同じか隣まで (Detour の制約)。つながらない Link は WARN + Inspector の警告。入口の y を床へ寄せる処理がある。
- **Recast パッチ 4**: `dtCrowd::update` の Off-Mesh の補間 (`m_agentAnims`) を止め、OFFMESH の間は速度 0 にするだけにした。渡りは NavSystem が
  持つ。渡りの状態 (フェーズ・経過 tick・入口・出口) は Nav 節の `NavAgentSlot` に入りスナップショット対象。dtCrowd のアニメは使わない。
- 渡る間は `CC.moveInput = 0` で、物理の後に `PostPhysics` が位置と CC.velocity を上書きする (CC の型は変えない)。
- **渡っている途中で Link が消えた・動いた**: 渡り始めに保存した出口まで渡り切ってから NavMesh へ戻す。戻ったら出口の最寄り点で経路を引き直し、
  最寄り点も無ければ「NavMesh の外」の扱い (目的地ありなら `NoPath`、無ければ `Idle`)。`Inactive` は動かせない理由 (CC 無し・Surface 無し・容量超過)
  専用のまま残す。
- 親を持つ Agent の渡りは、親の WorldMatrix (前 tick) の逆行列でローカルへ戻して位置を書く (SelfTest: 親が平行移動 / 平行移動 + y 軸 90 度回転)。
- 版: `kSimSnapshotVersion` 29、NavTileStore の状態 v4、TypeId 75。

## 決定 12: スクリプト API (ABI v24 = 139 スロット)

- M82 で ABI を 1 回 bump した (v23 = 131 -> v24 = 139)。BT を待つと GameLogic から NavMesh を使えない期間が長いため。
  8 本: `NavSetDestination` / `NavStop` / `NavGetAgentState` / `NavFindPath` / `NavSamplePosition` / `NavRaycast` / `NavFindRandomPoint` /
  `NavCompleteLink`。C ABI + POD、`Interop.cs` は位置ミラー、`check_rules.ps1` の版表は `24 = 139`。
- `NavSamplePosition` / `NavFindPath` / `NavRaycast` / `NavFindRandomPoint` は `areaMask` を取る。`NavFindPath` は `outPartial` (null 可) で部分経路を返す。
- `NavFindRandomPoint` は Detour の `findRandomPointAroundCircle` を使わない (円に触れるポリゴンの点を返すので、半径外の点が出る)。
  World の Pcg32 で円の中の点を一様に最大 16 回選び、最寄りのポリゴンへ吸着して半径内なら採用する。**RNG の消費規則**: Surface 無し・
  近傍に NavMesh 無し・radius ≤ 0 のときは引かない。center と**つながっている**とは限らない (孤島の点も返る)。
- クエリは前の tick の状態を見る (決定 7)。書く 3 本 (`NavSetDestination` / `NavStop` / `NavCompleteLink`) は `NavMeshAgent` のフィールドを書き、
  tick の頭の NavSystem が拾う。
- **外部プロジェクト (三校 / HAL Collector) の `GameLogic.dll` は版が合わず読み込みを拒否される**。再ビルドが要る。
- その後の版: v25 = 144 (M83、知覚、ADR-024)、v26 = 151 (M84d2、決定 14 の D)。M75h (InputField) は v27 以降になる。

## 決定 13: 歩行面の高さは層のセルの高さで補う (詳細メッシュは作らない)

- 問題: TileCache のポリゴンは頂点の高さの平面しか持たず、詳細メッシュ (detail mesh) が無い。段差 (天面 0.30 m) をまたぐ床のポリゴンは
  1 枚の平面になり、天面の上の点はすべて 0.05 m を返した。30 度の坂は x=4.5 で +0.42 m、x=7.0 で -0.33 m ずれた。結果として塗りが天面で
  床下に埋もれ、`NavSamplePosition` などの y が歩行面から最大 0.42 m 外れた (レビュー round 1 の指摘 2・3)。
- 採用 (a): 層 (`dtTileCacheLayer`) のセルの高さを歩行面の出どころにする。`NavTileStore::SampleSurfaceHeight(x, z, yHint)` が、`entries_` の層の
  バイト列だけから高さを返す (sim の状態を読み書きしない純関数)。柱に層が複数あれば、ポリゴン上の高さ (yHint) に最も近い層を採る。
  - ベイクは面の上端を切り上げて最低 1 セルの厚みを持たせるので、セルの値は真の面より 0〜1 セル上にある。半セルを引く。
  - 斜面ではセルの値が範囲内の最大高さになる。隣のセルとの勾配 (差が `1.2 × cs` 以内のときだけ。超えれば段差として使わない) から、
    隅の分 `cs/2 × (|gx| + |gz|)` を引き、セル内の位置へ勾配で寄せる。
  - 層の高さは Obstacle / Modifier で変わらない (どちらもエリアの値だけを変える)。タイルの作り直しに追従する必要が無い。
- 適用先 (1 か所ずつ): `NavSystem` の `NavSamplePosition` / `NavFindPath` の角 / `NavRaycast` の当たり点 / `NavFindRandomPoint` が返す点 (関数 `SnapToSurface`)、
  `NavDebugView` の塗りと輪郭。Raycast が壁に当たらず終点まで着いたときは、呼び出し側の `to` をそのまま返す。
- 表示: DebugUtils が出す三角形・線分を、平面の高さと歩行面の差が 0.04 m を超える間だけ最長辺で二等分し (辺は `3 × cs` 以下まで)、割った点ごとに
  高さを合わせる。平らな床は割らない。段差の縁には幅 `3 × cs` ほどの斜めの帯が出る。
  - 実測 (庭 13 × 13 m、cellSize 0.15): 全面を一律に割ると 16,184 三角形 (作り直し 5 ms)。適応分割は 1,182 三角形 (約 1.2 ms)。
    一律に割ると面積に比例して増え、100 × 100 m では約 100 万三角形 (外挿) になるので採らなかった。
- 精度 (庭、`NavAgentSelfTest` の「(高さ)」): 段差の天面・30 度の坂・台・床で、`NavSamplePosition` 最大 0.032 m、`NavRaycast` 0.028 m、
  `NavFindRandomPoint` 0.035 m、塗り 0.025 m。ポリゴンの平面だけだと `NavSamplePosition` は 0.418 m。
- 却下 (b): 詳細メッシュを作って Detour に渡す (`dtNavMeshCreateParams::detailMeshes` ほか)。Detour の `getPolyHeight` が正しい値を返し、
  クエリ側の補正が要らなくなるのが利点。ただし (1) TileCache の層から詳細メッシュを作る処理はライブラリに無い (`rcBuildPolyMeshDetail` は
  `rcCompactHeightfield` を要る)。ポリゴン内の標本点の三角形分割を自前で書き、`DetourTileCache` のビルドへ層を渡すパッチも要る。
  (2) dtCrowd の Agent の y と経路が変わりうるので、NavDeterminism のハッシュ、`--nav-demo` と `--acoustic-demo` の replay 基準、
  golden を全部焼き直すことになる。(a) は sim の状態に一切触れないので、ハッシュは 1 つも動かなかった。
  試作は (a) だけ行い、(b) は構築していない。(b) の実測は無く、上の 2 点は実装前の見積もり。
- 却下: ポリゴンの頂点の高さだけを補正する。段差の天面は 1 枚のポリゴンの内側にあり、頂点では天面の高さが分からない。

## 決定 14: NavMesh の拡張 (M84、2026-10-05)

範囲はユーザーが UE / Unity の機能を見比べて選んだ。入れなかったもの (実行時の再ベイク、Obstacle の強化、Smart Link、
ベイク入力の選び方、AI デバッガ / EQS / 経路テスト / Nav Invoker) は 2 回聞いて選ばれなかった。

### A. Agent Type の表 (M84a)

- `project_settings.json` の `navAgentTypes` (名前・半径・高さ・段差・傾斜・飛び降りの高さ・飛び越えの距離、最大 16、id 0 は固定) を
  Project Settings で編集する (`src\Editor\Project\NavAgentTypes.h`、エディタ専用)。Unity の Agent Types / UE の Supported Agents に当たる。
- **sim は Surface に写した値だけを見る**。project_settings はシーンとリプレイの外にある (決定 11 のエリアコストと同じ理由)。
  型を選んだときと Bake のときに Surface へ写し、表にある型なら Surface 側の寸法は Inspector で読み取り専用にする。
  表を変えても開いているシーンは変わらず、Inspector が「型との食い違い」「焼いた後に寸法が変わった」を警告する。
- Agent の radius / height は Agent 自身の値のまま (Unity と同じ)。

### B. Surface の接続 (M84b、UE 式)

- **同じ `agentTypeId` の有効な Surface をまとめて 1 つのナビメッシュに焼く**。UE は Agent ごとにナビメッシュを 1 つ持ち、
  複数の NavMeshBoundsVolume の範囲を合わせて焼く。計画の「Unity と同じ」は誤りで、Unity は別の Surface を自動ではつながず
  NavMesh Link を要求する。ユーザーは UE 式を選んだ。
- グループ = 同じ型の有効な Surface (`NavCollectSurfaceGroups`)。leader (エンティティキー最小) のセル・タイル・エリアコストと
  `navAsset` をグループ全体に使い、実行時に読み込むのも leader だけ。Nav 節の書式は変えていない。
- ベイクの範囲は全 Surface を合わせた AABB で、歩行面は Surface の箱の中だけに切り詰める (`kNavBakeVersion` 2。単独の Surface でも
  端のタイルが範囲の外へはみ出さなくなった)。三角形は Surface ごとの `collectLayerMask` で集め、1 コライダーは 1 回。
- Obstacle / Modifier / Link は、グループのどの Surface の範囲に入っても効く。Bake / Clear はグループの全 Surface に 1 Undo で効き、
  `.mnav` の名前は Agent Type の名前。

### C. エリアのコストを Agent・クエリごとに (M84c)

- `.navfilter.json` (UE の NavigationQueryFilter): エリアごとの「コストの上書き (0 = Surface のまま、1..1000)」と「通らない」のビット。
  計画の「コスト倍率」ではなく Unity / UE と同じ**上書き**にした。
- 資産の扱いは `.physmat.json` と同じ (`NavFilterLibrary` を GUID で引く、起動時の走査・ReloadHub・Inspector で編集)。
  中身はワールドハッシュに入れず、provenance の contentHash が守る。見つからないフィルタは「無し」として扱い 1 回警告する。
- Agent に `navFilter` (AssetRef) を末尾追加 (`kSimSnapshotVersion` 31)。dtCrowd のフィルタは (areaMask, navFilter) の組ごとに割り当て、
  16 種 (`DT_CROWD_MAX_QUERY_FILTER_TYPE`) を超えたら最後を共有する (決定 11 の溢れ処理を組へ広げた)。

### D. Agent の細かい制御と ABI v26 (M84d1 / M84d2)

- Agent に `isStopped` / `autoBraking` / `avoidancePriority` / `separationWeight` / `updatePosition` / `updateRotation` と、
  読み取り専用の `desiredVelocity` / `nextPosition` を末尾追加 (`kSimSnapshotVersion` 32)。
- `isStopped` は経路を保ったまま最高速度 0 で減速し、Stuck に数えない。最高速度 0 では回避の速度サンプリングが `1 / vmax` で破綻するので、
  止まっている間はサンプリングを外す。
- `updatePosition = false` は `moveInput` を書かず、crowd は毎 tick 実位置から取り直す (ユーザー選択)。Unity のように `nextPosition` が
  実位置から離れたまま進むことはしない。ルートモーションやスクリプトで動かす用途。
- `avoidancePriority` は Unity の「高い側は低い側を無視する」ではなく、分離・回避・押し戻しの分担の重み付けにした (ユーザー選択、
  **Recast パッチ 6**。同じ優先度どうしは元の Recast とビット一致)。`autoBraking = false` は **Recast パッチ 5** (`DT_CROWD_NO_AUTO_BRAKING`)。
- Warp は一度きりのフィールドではなく `NavSystem::Warp` (その場で Transform と crowd を置き直し、目的地を保って引き直す)。
  `CalculatePath` は Agent の今の位置から経路を引いて `NavAgentPath` (ポリゴン 256 + 角 256) に返し、`SetPath` はそれを回廊へ直接入れる
  (引き直さない)。ポリゴンが古い (タイルの作り直し)・種別違い・渡りの途中の経路は拒否する。
- **ABI v26 = 151** (v25 = 144 から 7 本): `NavWarp` / `NavCalculatePath` / `NavSetPath` / `NavFindPathFiltered` / `NavSamplePositionFiltered` /
  `NavRaycastFiltered` / `NavFindRandomPointFiltered`。既存スロットのシグネチャを変えない規則 (EngineAPI.h 冒頭) に従い、navFilter 付きの
  クエリは別スロットにした。Warp / SetPath は ABI からその場で crowd を書き換える (要求の列にすると LateUpdate から呼んだ分が tick を
  またいで残り、スナップショットに入れる必要が出るため)。POD は `MyeNavPath` (4112 バイト)。細かい制御のフィールドは汎用の
  フィールド ABI で読み書きし、専用スロットは作らない。`--nav-demo` の `NavDemoDriver` が tick 150〜420 で v26 を使う (replay_verify の被覆)。

### E. Link の自動生成 (M84e、Unity の Generate Links)

- ベイクの最後に、層から組んだナビメッシュの外周の辺 (隣のポリゴンもタイルをまたぐ接続も無い辺) を 0.5 m 以上の間隔で調べる (`NavLinkGen.cpp`)。
  - 飛び降り: 辺の外側の下に、`maxClimb` より低く `dropHeight` 以内の歩行面がある。一方通行。
  - 飛び越え: 辺の外側の水平 `jumpDistance` (縁から縁の隙間) 以内に、高さの差が `maxClimb` 以内の歩行面がある (いちばん近い面だけ)。双方向。
  - 捨てる候補: 途中がベイク入力の三角形に当たる (腰の高さの線分。手すり・壁・柱の向こう)、ナビメッシュ上を歩いて Link の長さの 2 倍以内で着く。
  - 入口・出口とも 1 m (または半径 x 4) 以内の候補は 1 本にまとめる。1 回のベイクで最大 4096 本。
- **`dropHeight` / `jumpDistance` は Agent Type の表に持ち Surface へ写す** (跳べる距離はキャラの能力なので型ごと)。生成の on/off と、生成した Link の
  渡り方 (Linear / Jump / Manual・速さ・弧の高さ) は Surface で指定する (ユーザー選択)。渡り方はベイクに入らず、渡り始めに leader の Surface から読む。
  物ごとの除外 (Unity の OffMeshLink Generation フラグ) は入れていない (ユーザー選択)。エリアは 2 (Jump) 固定。
- **生成した Link は `.mnav` に焼く** (形式 2、形式 1 も読める)。決定 11 の「Link は焼かない」は手置きの Link の話で、こちらはベイクの導出値なので
  別物。`key` / `userId` の最上位ビットで手置きと区別し、NavSystem が毎 tick の Link の一覧で手置きの後ろへ足す。store の状態とハッシュは
  手置きと同じ経路を通る。ポリゴンの上限 (`maxPolysPerTile`) は Link を入れた後の数で決める。
- 生成を切った Surface は、生成の値を入力ハッシュに混ぜない (既存の `.mnav` を焼き直さずに使える)。`kNavBakeVersion` は上げていない。
- Surface に 6 フィールドを末尾追加 (`kSimSnapshotVersion` 33)。ABI は変えていない。生成した `.mnav` のバイト列は Debug / Release で一致する
  (NavAgentSelfTest 14 節の期待ハッシュ)。

## 除外した案

| 案 | 理由 |
|---|---|
| 自前 A* | 事前計画で却下。段差・坂・回避・Link・動く障害物を一から作る価値が無い |
| Recast を使い、実行時にジオメトリを再ラスタライズ | 決定論の面積が倍になる。壊れる壁は Obstacle で表す運用にし、差し込み口だけ残した (決定 4) |
| 経路追従を Crowd と別に先に作る | Crowd 導入時に捨てる。最初から dtCrowd |
| FastLZ で層を圧縮 | `.mnav` のバイト列が圧縮器に依存する。無圧縮の自作で十分小さい (3×3 タイルで 63 KB) |
| Agent が Transform を直接動かす | 重力・接地・衝突の二重管理になる。CC 必須 |
| `carve = false` で Agent 回避のみ | dtCrowd に口が無い (決定 10) |
| `project_settings.json` にエリアコスト | sim 入力がシーン・リプレイの外に出る (決定 11) |
| Modifier / Link をベイクに焼き込む | 実行時に動かす・消すときに戻せない (決定 11) |
| tick レーンに塗りを積む | 編集中と Play 中で表示経路が割れる (決定 9) |
| Surface の接続を Unity 式 (Link を要求する) | 同じ型の範囲を合わせて焼く UE 式をユーザーが選んだ (決定 14 の B) |
| `.navfilter.json` をコストの倍率にする | Unity / UE と同じ上書きにした (決定 14 の C) |
| `avoidancePriority` を Unity 式 (高い側は無視) にする | 重み付けをユーザーが選んだ (決定 14 の D) |
| navFilter を既存のクエリのスロットへ引数で足す | 既存スロットのシグネチャを変えない ABI の規則に反する。別スロットにした (決定 14 の D) |
| Agent Type の表を sim から直接読む | project_settings はシーンとリプレイの外。Surface へ写す (決定 14 の A) |

## 既知の限界

- 静的ジオメトリの実行時再ベイクは未実装 (決定 4)。
- 部分経路の `Arrived` は完全な経路より 60 tick 遅れる (決定 7)。
- ポリゴンは平面のままなので、Detour 自身の関数 (`dtCrowd` の Agent の `npos.y` など) は段差の上で歩行面からずれる。
  スクリプト API の返す y と塗りだけを歩行面に合わせた (決定 13)。段差・坂の縁から約 1 セル以内の点は、どちらの面の高さが返るかが縁で決まる。
- NavMesh の段差上限は `maxClimb` から最大 1 セル未満超えうる。残りは Stuck で受ける (決定 8)。登る tick に位置が 0.13〜0.16 m 前へ出る。
- acoustic の golden は Agent Eye の乗り上がりを写さない (決定 8)。
- 半透明の塗りの CI (WARP) での一致は未確認 (決定 9)。
- `NavFindRandomPoint` の点は center とつながっているとは限らない (決定 12)。
- Stuck / 渋滞到着の定数 (60 tick、radius/4、2 × radius、半径の和の 2 倍、再基準の radius) は、2 / 4 / 8 体の SelfTest 以外の実ゲームの渋滞で検証していない。
- Link の出口がタイルを 2 つ以上離れる場合はつながらない (Detour の制約)。
- 自動生成の Link は、縁の真下の飛び降りと同じ高さへの飛び越えだけを作る。隙間の先の低い床へ飛ぶ (飛び越え + 飛び降り) は作らない。
  物ごとに生成を外す指定は無い (決定 14 の E)。生成した Link を渡っている途中のスナップショット復元は、手置きの Link の試験 (11c) と同じ経路だが専用の試験は無い。
- `CalculatePath` / `SetPath` の経路は固定長 (ポリゴン 256・角 256) で、超える経路は切れる (決定 14 の D)。

## 検証結果

M82j の時点 (2026-10-04、HEAD `d8ff284` + 文書のみの変更) で全体検証を行った。

| 検証 | 結果 |
|---|---|
| `tools\replay_verify.bat` | 全 15 ジョブ PASS (144.7 s)。`nav` ジョブ (`--nav-demo`: Agent 数体・Obstacle の出し入れ・Link・Modifier) は Debug の snapshot stress 付き verify、Release、`Server.exe` (Release) の 3 本で毎 tick のハッシュが一致。time travel / What-if / 静的規則 (0 error / 0 warning) も PASS |
| `tools\shot_verify.bat` | golden `nav` PASS。FAIL は着手前から同じ 5 枚だけ (parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192。maxDiff/画素数が着手前と同一) |
| `Editor.exe --selftest` (Debug / Release、直列) | NavDeterminism / NavSurface / NavEditor / NavAgent を含め ALL PASS。FAIL は着手前から同じ Source control self test の 2 件 (`external cherry-pick state closes the normal write gate`、`external revert state survives status refresh`) だけ |
| `Server.exe --selftest` (Debug / Release) | ALL PASS |
| `tools\check_rules.ps1` | 0 error / 0 warning |
| `/p:MyeWarnAsError=true` のビルド | 下の「ビルド警告」 |

- 受け入れ条件 18 (c): このドキュメントの「将来の実行時再ベイクの足し方」。
- 画面での確認 (エディタで Create -> 4 項目 -> Bake -> 再生 -> Agent が歩く) は、各サブで `--screenshot` と SelfTest (`NavEditorSelfTest`) で
  確認した分のみ。M82j では撮り直していない。手で操作する項目は `docs\test_checklists.md` の「M82」にまとめた。

### M84 の全体検証 (2026-10-05、HEAD `d9e08a6` + 文書のみの変更)

| 検証 | 結果 |
|---|---|
| `Editor.exe --selftest` (Debug / Release) | 全件 PASS (終了コード 0)。NavSurface / NavAgent の期待ハッシュ 3 件 (`.mnav` 形式 2・庭のワールドハッシュ・生成した Link の `.mnav`) は Debug で採り Release で一致 |
| `Server.exe --selftest` (Debug / Release) | 全件 PASS (終了コード 0) |
| `toolseplay_verify.bat` | 全 16 ジョブ PASS (140 s)。`nav` ジョブは ABI v26 の `NavDemoDriver` を含む。静的規則 0 error / 0 warning |
| `tools\shot_verify.bat` | FAIL 6 枚。5 枚は M82j と同じ (parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192)。**`nav` が 214/21664 で新たに FAIL**。差は線 (輪郭・経路・Link) の上だけで、M83 より前のコミット `f6c7bef` (EditorLinePass の線を AntialiasedLine + 面の手前へずらす) と合う。M84b の切り詰めの寄与は切り分けていない。golden は更新していない |

### ビルド警告

`replay_verify.bat` 内の通常ビルド (Debug / Release) で、M82 のファイルから出る警告は 0 (既存の `ProjectComputeRunnerSelfTest.cpp` の C4127 は通常ビルドでも 14 件出る)。`/p:MyeWarnAsError=true` を付けたビルドは **Debug / Release とも**
`src\Engine\Renderer\Compute\ProjectComputeRunnerSelfTest.cpp` の C4127 (7 件、条件式が定数) で `Engine.vcxproj` が落ち、後続のプロジェクトまで進まない。
このファイルは M82 の範囲外 (着手前の HEAD から同じ、`plans\m75-ugui.md` の M75g の記録でも既知)。
CI は `MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true` を使うので、このファイルが直るまで現状では CI のビルドが落ちる。
M82 が足したファイルと vendor したソースの警告は 0 (上のエラー一覧に他のファイルが出ない)。ただし Engine が落ちるため、
Engine より後ろでビルドされる Editor / GameLogic / Server の M82 変更分をこのフラグ付きでは確認できていない。