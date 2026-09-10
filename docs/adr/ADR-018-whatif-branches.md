# ADR-018: タイムトラベルの分岐 (What-if リプレイ) — 未来は捨てずに分岐へ、ゴーストは 1 回の再シムで焼く

- 状態: 採用 (2026-09-10、M72a〜M72i)
- 出所: 2026-09-06 の「コンテストで評価される追加案」3 番 (「過去へ戻って入力を変え、そこから
  再シムし、元の未来をゴーストで重ね描画」)。計画は `~/.claude/plans/cheeky-zooming-rocket.md`
  (ユーザー決定 3 点: 変えるのは入力 / チューニング値 / シーン状態の 3 種、見せ方は Timeline の
  分岐レーン + SceneView のゴースト)。本 ADR は**決定と却下理由と実測値**だけを残す。
- 関連: **ADR-004** (リプレイ一貫性 — 分岐も再シムも `RunOneTick` 1 本を通る)、
  M52d/M52e (SimSnapshot と TimeTravel の土台。`engine_spec.md` の "Snapshots and time travel" /
  "Branches" / "Ghosts" / "Field-level divergence" / "Input overrides")、
  M52a (`HashWorldDump` / `DiffHashDumps` — M72g はこれを 2 レーンに向けただけ)。

## 決定

### 決定 1: 未来は捨てない。分岐は「suffix だけを所有する」ツリー

M52e はシークで戻った後に走った tick が記録済みの未来を `entries_.resize` で捨てていた。
M72a から未来は `TimeTravelBranch` に移る。分岐は `[forkTick, End)` の entry と `tick >= forkTick` の
スナップショットだけを所有し、それより前は `parent` へ委譲する (ライブは id 0)。
`EntryOn / HashAtTickOn / SnapshotAtOrBeforeOn(lane, t)` が親を歩く。

却下: **分岐ごとのフルコピー** (148 KB × 120 枚を分岐数ぶん複製 = 64 MiB の予算が 1 本で尽きる) /
**フラットなリスト** (「既存分岐より前でもう一度分岐」で共有 prefix の所有者が変わり、
最寄りスナップショットの探索が別レーンの枚を拾って嘘の世界を復元する)。

### 決定 2: 分岐点のスナップショットはレーン固有。編集後の状態を pinned で撮り直す (欠陥の修正)

計画時に見つけた **M52e の欠陥**: 「シークで T へ戻る → ポーズ中に Inspector で編集 → 再開」で、
`DropSnapshotsAfter(T)` が tick==T のスナップショット (編集前) を残したままだったので、後で T へ戻ると
編集前の世界が復元され、`HashAtTick(T)` (= `entries_[T-1].hashAfter`、これも編集前) と一致して
**OK と言いながら編集が消えた** (T+k へ戻ると HASH MISMATCH)。原因は「tick T が走る前の状態は
レーン内で一意」という前提を Inspector 編集が破っていること。

`Fork` はライブのハッシュを撮り、記録上の「T 前の状態」と違えば (= 編集された) 編集後の状態を
**pinned** スナップショットで撮り直す。`HashAtTick` は「その tick のスナップショットがあればその
stateHash」を優先する (編集点以外では entry の hashAfter と一致するのが不変条件 = selftest で固定)。
ハッシュを余分に撮るのは `NeedsBoundaryCheck` が真の tick だけ (未来がある / シーク直後 /
直前がポーズ tick) — 通常の tick には 1 回も足していない。

### 決定 3: 同一分岐は畳む。予算は全レーン合計。切替の途中では上限を適用しない

Step / 再開の繰り返しで分岐が量産されるのを防ぐため、ライブが分岐の終端へ追いついた tick で
1 回だけ `FirstDivergence` を比べ、乖離が無ければ消す。予算 (`maxBytes`) は全レーン合計で、超過は
まず分岐側の pinned でないスナップショット (古い分岐から)、次にライブ最古。fork tick がリングの外へ
出た分岐は部分木ごと消す。本数上限 8 は最古の葉から (いま作った分岐は守る)。
**`SwitchToBranch` の途中では上限を適用しない** — 継ぎ足し待ちの要素が葉として消える。

### 決定 4: ゴーストは第 2 の World ではなく、分岐した瞬間の 1 回の再シムで採取する

却下: **第 2 の World / TickServices を同時に回す**。`TickServices` の sim 側シングルトン
(ScriptHost / ManagedHost / CollisionSystem / AcousticField / XPBD / ParticleSystem / …) は Init で
`Scene*` を掴んでいて、複製 = EngineLoop の初期化を丸ごと二重化する規模。しかも毎フレーム tick
コストが 2 倍になる。`ActorEdit` の第 2 Scene は「tick を回さない描画専用」だから成立している例で、
前例にはならない。

採用: 分岐が生まれた次のフレーム頭で、その分岐のスナップショットへ戻し、分岐の記録入力で終端
(または `ghostMaxTicks`) まで **同じ `RunOneTick`** を回しながら、WorldMatrix + MeshRenderer を持つ物の
tick ごとの行列を `GhostTrack` に採取する (疎: 前 key と 1 バイトも違わなければ積まない、消えた物は
墓標 1 個)。焼き終わりは記録ハッシュと照合し、ライブへは強制復元のシークで戻る。
**`RunOneTick` にフックは足していない** — 採取は再シムループが `RunOneTick` の後に World を読むだけ
(その時点の状態は tick 末ハッシュと同一)。シーク / 焼き / 乖離ダンプの 3 者は `RunResim` 1 本を通る
(出力抑止の置き場所を 1 箇所に閉じ込める)。

却下: **`TickServices` に観測者フック** (計画の案)。tick 本体を触らずに同じ情報が取れるので、
`replay_verify` の被覆を増やしてまで入れる理由が無い。

### 決定 5: 入力の上書きは置換チェーンの後ろで OR し、entry に記録させる

`InputOverride` はライブ / 合成入力の**後**に OR / 置換するので、`OnTickEnd` がそのままリングに載せる =
後からシークしても同じビットが再現する (合成入力と同じ扱い)。verify / net では適用しない。
意味論は「ライブレーンの to-do」: レーンを切り替えて同じ tick を再び走らせるとまた効く。

### 決定 6: 乖離のドリルダウンは既存の `HashWorldDump` + `DiffHashDumps` をインプロセスで撃つ

2 レーンをその tick まで再シムしてダンプし、`DiffHashDumps(…, outReport)` の報告行を Timeline に出す。
新しい診断機構は作らない — M52a の道具は「1 フィールドの変異は必ず valueDiffs==1」まで含めて
既に検証済みで、それをファイルではなく 2 レーンに向けただけ。

## 版バンプ

無し。`kSimSnapshotVersion` (blob 形式不変 — 分岐点のスナップショットも同じ形式) /
`kReplayFileVersion` (.rep 無関係、記録中はリングが起きない) / `MYE_API_VERSION` (スクリプト向け
API 追加なし) / `InputSnapshot` / `TimeTravelEntry` のいずれも不変。`GhostTrack` はメモリ内専用。

## 実測 (既定デモ 528 体、527 体が動く)

| 項目 | Debug | Release |
|---|---|---|
| 再シム | 約 4 ms/tick | 約 0.12 ms/tick |
| ゴースト焼き 100 tick | 470 ms (52598 keys / 2.9 MB) | 25 ms |
| フィールド差分 (2 レーン × 再シム + ダンプ) | 326 ms | 19 ms |
| Jump を 20 tick 押した分岐の乖離 | tick+1 で `PlayerController.jumpCount` / `prevSpace` の 2 つ (両構成で同値) | 同左 |

三校ステージ (動く物は敵 3 体 + プレイヤー + ビーコン程度) ではゴーストは桁違いに小さい。

## 検証の口

- `Editor.exe --whatif-selftest [N]` (Debug / Release、`replay_verify.bat` の whatifdebug / whatifrelease):
  戻って再開 → 分岐が残る → ゴーストが焼けて verified → 同じ入力で畳まれる → 編集して分岐 →
  分岐点で乖離 / 戻っても編集が残る → 元の分岐へ切替 → Jump を押して分岐 → 上書きが entry に
  記録され、押した範囲内で乖離 → 最初の乖離 tick で 2 レーンをダンプして葉のフィールドを名指し。
- `TimeTravelSelfTest` (ライブ World 無し): ツリーの move / 親付け替え / 親歩き / pinned 優先 /
  切替の継ぎ足し / 畳み込み / 刈り込み / 本数上限 / GhostTrack の疎化と墓標 / 上書きの経路選択 /
  差分の報告行。
- 手動: `docs/test_checklists.md` の M72。

## 踏んだ罠 (プローブが捕まえたもの)

- 焼いた後の復元先に焼き終わりの `ctx.tickIndex` (= 分岐の終端) を渡していた。ライブの tick は
  焼く前に控える。
- プローブの段番号 20 を `wiStage < 9` のゲートが弾き、以後の段が走らずゲームが回り続けた。
  終了は番号の大小ではなく `kWiDone` との一致で見る。
- 先頭アクション (WatcherRun) を押しても既定シーンでは誰も読まず、ハッシュが 1 ビットも動かずに
  分岐が「同一」として畳まれた。押すのは誰かが読むアクションでないと検査にならない。

## 申し送り

- **M72i (2026-09-11 に実装)**: 半透明の実メッシュのゴースト。計画の「第 2 の World を
  `viewKey=0` で描き、RenderSystem に半透明ティント経路を足す」は採らず、**エディタ専用の
  `GhostMeshPass`** (`Renderer\GhostMeshPass.*` + `assets\shaders\ghost_mesh.hlsl`) にした —
  PickingPass と同じ「MeshVertex の VB を POSITION/NORMAL だけ宣言して描く」型で、深度テストあり・
  深度書き込みなし・アルファブレンド、法線で軽く陰影。RenderSystem もマテリアルも触らない。
  メッシュが引けない物だけワイヤ箱に落ちる。
- ゴーストの予算 8 MiB は既定デモ (527 体が動く) では約 280 tick で打ち切る。動く物が少ない
  シーンでは 1800 tick まで届く。UI に打ち切りが出るので、必要なら `TimeTravelConfig` で上げる。
- C# レーンは従来どおり巻き戻らない (分岐にも同じ注記)。
