# ADR-023: NavMesh (Recast Navigation) と決定論・状態の復元

- 状態: **下書き** (2026-10-03、M82a)。方式と計測値は sub-01 の試作で確定。sub-09 で全体検証の結果と
  文書への反映を足して仕上げる。
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
128 体は dtCrowd の保存を差分化する余地がある (sub-03 で実機の計測値を見て判断)。

## 決定 4: 将来の実行時再ベイクの差し込み口 (spec 4.4 F1〜F5)

再ベイクは作らない。次の口を作ってある。

- F1. タイルの差し替えは `NavTileStore::ReplaceTileLayers(tx, ty, layers)` + `Commit()` の 1 本。
  重なる障害物は外して付け直す (残すと `touched` が古い層の ref を指し、新しい層に効かない)。
  再ベイクは「別の生産者が `ReplaceTileLayers` へ新しい層を渡す」で足せる。
- F2. 入力収集 (World + タイル AABB -> 三角形・エリア) とタイル 1 枚分のベイク (`NavBakeTileLayers`) は
  Editor に依存しない純関数。実行時から呼ぶ場合も同じ関数を tick 境界で呼ぶ (sub-02 で入力収集を足す)。
- F3. `dtNavMeshParams` の `maxTiles` / `maxPolys` は固定し、タイルを何度差し替えても ref のビット配分は変わらない。
- F4. 差し替えた層は状態表が持つ (`isBase = false` の層は SaveState に実体が入る)。1 タイル 2 層の差し替えで
  約 6.3 KB。差し替えを頻繁に行う場合は、層を内容ハッシュで引く別の保管庫に出してスナップショットにはハッシュだけを
  書く (将来)。
- F5. 層の所有者は `NavTileStore`。アセット由来の層 (`AddBaseLayer`) も差し替えた層も `std::vector<uint8_t>`
  のコピーを持つ。

再ベイクを足すときの呼び出し順 (tick 境界、`NavSystem::Update` の先頭):
(1) 変化したタイルの集合を決定的な順で列挙 (2) `NavBakeTileLayers` で新しい層を作る (3) `ReplaceTileLayers`
(4) `Commit`。(2) は sim 内で Recast を回すことになるので、時間予算と決定論の追加検証が要る (後回し)。

## salt の桁配分

`dtNavMesh` の ref は salt / タイル番号 / ポリゴン番号に 32 ビットを分ける。
`saltBits = 32 - ceil(log2(maxTiles)) - ceil(log2(maxPolys))`、巡回までの差し替え回数 = `2^saltBits - 1` (スロットごと)。

| maxTiles | maxPolys | salt ビット | 巡回までの差し替え |
|---|---|---|---|
| 64 (試験) | 1024 | 16 | 65535 |
| 1024 (推奨の上限) | 1024 | 12 | 4095 |
| 4096 | 1024 | 10 (下限) | 1023 |

`dtNavMesh::init` は salt が 10 ビット未満だと失敗する。`maxTiles` はベイクした層の数 + 再ベイクで層が増える余裕
(1.5 倍を目安)、`maxPolys` は 1 タイルのポリゴン数の最大の 2 倍を目安にする (sub-02 のベイクで決める)。
スロットごとの salt は `Commit` につき 1 回だけ進むので、巡回するのは 4095 回の Commit (約 68 秒、毎 tick 差し替えた場合)
の後。古い ref が生き残るのは (a) crowd の経路、(b) 呼び出し側が保持した ref で、どちらも毎 tick 検証される
(`dtCrowd::checkPathValidity`、`isValidPolyRef`)。取り違えが起きるのは、1 つの ref が検証されないまま同じスロットの salt が丸 1 周する場合だけで、Commit は 1 tick に 1 回なので起きない。
salt の値自体も SimSnapshot に入る (スロットごとに u32)。

## Detour の制約 (実装の注意)

- `dtTileCache::update` が積める tile 更新は 64 件 (`MAX_UPDATE`)、障害物 1 個が触れる層は 8 枚 (`DT_MAX_TOUCHED_TILES`)
  まで。上限を超えると更新が黙って落ちる。`NavTileStore` は障害物の要求を 8 件ごとに処理する。
- 要求キューは 64 件 (`MAX_REQUESTS`)。8 件ごとの処理で超えない。
- 障害物の `maxObstacles`・タイルの `maxTiles` は固定長。超えると追加が失敗する。
- `Off-Mesh Link` の渡りの状態 (`dtCrowd::m_agentAnims`) は保存していない。M82 は渡りをアプリ側で行う (sub-07)。
- `LoadState` / `NavLoadCrowd` は失敗を返したときに途中まで書き換えた状態を残す。失敗は致命として扱う (sub-03)。

## AcousticNav との役割分担

`AcousticNav` は「聞こえた所へ向かう」特殊解で、音の伝播と同じ占有配列から流れ場を作る (M65 の判断)。
`AcousticNav.h` の「NavMesh を作らないのが判断の前提」は音響 AI の局所判断で、エンジン全体の禁止ではない。
NavMesh は汎用の移動 (段差・坂・回避・Link・動く障害物)。`AgentBrainComponent` は従来どおり `AcousticNav` を使い、
同じエンティティに `NavMeshAgent` があれば後に走る NavSystem の moveInput が勝つ。`AcousticNav.h` のコメントの整理は sub-09。

## sub-09 で仕上げること

- 全体検証 (`replay_verify` の `nav` ジョブ、`--snapshot-stress`) の結果
- `engine_spec.md` の NavMesh 節、`docs\engine-feature-guide.md` 9.3 の書き換え
- TypeId / ABI 番号の確定と `plans\m75-ugui.md` の注記
