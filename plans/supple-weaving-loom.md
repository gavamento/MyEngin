# M60′: XPBD 変形体 — ロープ / 布 / ソフトボディ / 塑性・粘弾性

**再開手順**: `git log --oneline -5` で最後に完了したサブを確認 → 本ファイルの進捗表と
突き合わせ → 次のサブの節を読む → 着手前にそのサブの「冒頭確認」があれば先に潰す。
1 サブ = 1 コミット (`M60'a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルの進捗表には**計画外の事実・罠・申し送りのみ**書く。

**スコープ (ユーザー決定 2026-08-27)**: ロードマップ (`greedy-cooking-wave.md` 全体
ロードマップ節) のフルセット — ステートフルバックエンド様式の確立 → 粒子+拘束基盤 →
ロープ/鎖 → 布 (空力カーネル共有) → ソフトボディ → 塑性/粘弾性/クリープ/応力緩和。
**カップリングは衝突まで双方向 (ユーザー決定)** — アタッチ経由で剛体に力が返るだけでなく、
変形体の接触が剛体を押す (布の上に箱が載る) ところまでやる。ただし実装は
片方向 → アタッチ双方向 → 接触双方向の 3 段で積み、各段で安定性を確定させてから進む
(リスクと縮退ラインの節を参照)。

## 出口の姿

Inspector で `Rope` / `Cloth` / `SoftBody` を 1 つ付けるだけで、吊り橋のロープ・旗・
テント幕・ゼリー状の塊が材料 (`.physmat`) の剛性・弾性・圧縮性・粘性・塑性に従って
動き、剛体と双方向に絡む。全状態がワールドハッシュ + SimSnapshot に載り、
replay / タイムトラベル / ネットロールバックが従来どおりビット一致で通る。

## 調査済みの土台 (2026-08-27 調査。計画の前提)

**追い風**:
- `AeroSampling.h:20-21` がヘッダ自身で「第 2 の呼び手 = M60' の布 (予約事項 3)」を明記。
  `AccumulateSurfaceElement` は純関数で形状を知らない — 布は自分で `SurfaceElement` を
  組んで流せばよい。
- `PhysMatLibrary.h:33-34` が「ヤング率そのものは置かない — M60' の XPBD compliance
  (= 1/k) 側で扱う」と場所取り済み。キーは末尾 append + `Sanitize` で前方互換に足せる。
- 「ECS 外の sim 状態」の前例は CpuParticleBackend が完備 — ハッシュ節
  (`WorldHasher.cpp` `HashCpuParticles`) / SimSnapshot 節 (`WriteParticles`/`ReadParticles`) /
  エミッタ別 PCG32 / owner.index 昇順の池。XPBD バックエンドは全面的にこの範型を踏む。
- SimSnapshot は節ごとに magic を持ち「小さい節を全部読み切ってから World を最後に差し替え」
  る構造 (`SimSnapshot.cpp:244-247`)。節追加は World 節の前に挿して版を bump するだけ。
- TypeId は 43 (Vehicle) まで消費済み → **44=Rope / 45=Cloth / 46=SoftBody** (登録順 =
  実装順 c → h → k を厳守)。
- ABI は M60 と同じく追加ゼロ見込み (M59k の汎用スロットで届く。v14 のまま)。

**逆風 (完全新規になるもの)**:
- **XPBD の骨格はコード上に 1 行も無い**。M60 の位置補正パス (`PhysicsSystem.cpp:3780-3962`)
  は compliance も λ 蓄積も持たない純射影で、位置変化を速度へ戻さない。流用不可、新設。
- **粒子と剛体・コライダーの衝突は現状ゼロ** (CpuParticleBackend は場のみで動く)。
  欲しい低レベルルーチン (`SphereTriContact` `Shapes.cpp:1257` / `ClosestPtPointTri` `:1181` /
  `SoupGatherTris` `:1162`) は**全部 `Shapes.cpp` の無名 namespace 内** → ヘッダ露出の
  小リファクタが必須 (e)。
- **「毎フレーム CPU から頂点を更新してライティングに乗せる」描画経路が無い**。
  メッシュ VB/IB は `D3D11_USAGE_IMMUTABLE` 固定 (`GpuResources.cpp:163,169`)。動的 VB を
  持つのは UIRenderer / VfxRenderer / EditorLinePass の 3 つで、全部ライティング外。
  布/ソフトボディの PBR 描画は新経路 (j)。ロープは既存 MeshInstancing で逃がす (g)。

## アーキテクチャ

```
EngineLoop (所有)
  └ XpbdBackend  … 粒子池 (owner.index 昇順)。sim 状態の本体
      ├ PhysicsSystem::Update(world, dt, contacts, xpbd) … substep 内で剛体と交互に解く
      ├ WorldHasher   … SimSources 経由で内容ゲート畳み込み (b)
      ├ SimSnapshot   … SimRefs に member 追加 + 専用節 (b, v4)
      └ RenderSystem  … 池を読んで描画 (g/j。非ハッシュ = 決定論の面積を広げない)
```

- **状態は全部バックエンドの池に置く** (粒子数が可変なので ECS のカラムに置けない —
  M59h「コンポーネント常駐」の家風から意図的に離れる最初の例。これが予約事項 1 の
  「箱を開ける」の意味)。コンポーネント (Rope/Cloth/SoftBody) は**オーサリングデータのみ**
  を持ち、池は `Sync(world)` がコンポーネントの有無から生成/破棄する
  (CpuParticleBackend::SyncEmitters と同じ形)。
- **substep 統合**: XPBD は substep が小さいほど収束が良い (Small Steps)。剛体の
  substep ループ (`PhysicsSystem.cpp:1971-4245`) の**内側**に粒子の
  「速度積分 → 位置予測 → 拘束射影 (固定反復) → 速度更新 v=(x−xPrev)/h」を配置する。
  接触双方向 (f) は剛体の速度ソルバ/位置補正の 2 パスに粒子行を混ぜる形を第 1 候補
  とする (詳細は f 冒頭で確定)。反復数・順序は固定 (早期終了なし)。
- **決定論**: PhysicsSystem の家風に従い**スカラー float のみ** (`XMVECTOR` 禁止、
  `PhysicsSystem.cpp:24`)。CpuParticleBackend の SSE は真似ない (あちらは参照実装込みで
  検証済みの独立系)。粒子順 = 池順 = owner.index 昇順。接触候補は明示キーで整列。
  乱数は使わない (XPBD に乱数は不要)。`std::cos/sin` 禁止 (`AeroSampling.h:16-19` と同じ理由)。
- **コードの置き場**: `PhysicsSystem::Update` は既に 3400 行の単一関数。XPBD は
  `src\Engine\Engine\Physics\XpbdBackend.h/.cpp` (池と Sync) +
  `XpbdSolver.h/.cpp` (拘束射影の純関数群) の新規ファイルに置き、Update からは
  段ごとの呼び出しだけを書く。

## 決定台帳

| # | 論点 | 決定 |
|---|---|---|
| 1 | スコープ | **フルセット** (ユーザー決定)。塑性・粘弾性・クリープ・応力緩和まで本計画。M61 (破壊) が塑性量を入力に使う順序も自然 |
| 2 | カップリング | **衝突も双方向** (ユーザー決定)。実装は 3 段 (片方向 e → アタッチ双方向 d → 接触双方向 f) で積む。※ d と e の順は実装順 — アタッチが先なのは衝突より安定して作れて、吊り下げの絵が早く出るため |
| 3 | 状態の置き場 | **ステートフルバックエンド** (予約事項 1 の箱を開ける)。粒子数が可変な時点でコンポーネント常駐は物理的に不可能。様式 = CpuParticleBackend 範型 (池 + ハッシュ節 + snapshot 節 + Sync) |
| 4 | ハッシュの畳み方 | **内容ゲート** — 池が 1 つも無ければ節ごと畳まない。★CPU 粒子節は「ポインタ非 null なら空でも定数を 1 個畳む」形 (`WorldHasher.cpp:329-332`) なので**同じ形にしてはいけない** — 無条件に畳むと全既存シーンのハッシュが動き、.rep 版 bump が要る。内容ゲートなら既存シーンはビット不変 = bump 不要 |
| 5 | ソルバの数値方針 | スカラー float・固定反復・λ 蓄積つき XPBD (compliance α̃ = α/h²)。SSE 化は将来 (参照実装が要るので今はやらない) |
| 6 | ソフトボディの体積 | **閉表面メッシュ + 全体積拘束 (圧力モデル)**。四面体化はしない — 四面体メッシャは 1 マイルストーン級の別仕事で、ゲーム的な「ゼリー/風船」の絵は表面メッシュで足りる。局所体積が要る用途は将来 |
| 7 | 布の面 | グリッド生成 (N×M)。任意メッシュからの布化はしない (エッジ抽出・非多様体の沼)。UV はグリッドで自明に張る |
| 8 | 描画の家風 | 描画は池を**読むだけ** (M60 決定台帳 8/15 の継続 = 決定論の面積を広げない)。法線・セグメント行列は描画側で毎フレーム再計算。**ただし布空力 (i) が sim 内で法線を要る**のは別物 — 粒子位置からの純関数導出で、状態には持たない |
| 9 | ロープの描画 | セグメントごとの円柱を **MeshInstancing** で描く (動的 VB 不要、ライト付き、コスト最小)。布/ソフトボディだけが動的 VB (j) を要る |
| 10 | 生成メッシュの ID | 布/ソフトボディの描画メッシュを MeshLibrary へ登録する場合、その**生成 ID をシーンへシリアライズしない** (機種依存 ID のコミット禁止則)。Cloth/SoftBody は MeshRenderer を持たず、material の AssetRef だけをコンポーネントに持つ |
| 11 | 材料との接続 | M59a2 の値選択則を踏襲 — 材料割当あり → .physmat 値 / 未割当 → コンポーネントの既存フィールド。compliance 系キーは l で末尾 append |
| 12 | スリープ | **v1 では入れない** (対象外の節)。粒子の静けさ計数は将来。ショーケース規模 (数百〜千粒子) では常時シムで問題ない |
| 13 | 自己衝突 | **やらない** (布‐布、ロープ‐ロープ、布内の自己交差)。空間ハッシュの決定論設計は M62 の粒子流体スパイクと同じ箱で検討 |
| 14 | ABI | 追加ゼロ見込み = v14 のまま (M60 決定台帳 17 の継続)。粒子状態へのスクリプトアクセスは対象外に明記 |
| 15 | 命名 | バックエンド = `XpbdBackend`、集約 struct = `SimSources` (a で確定。実装時に改名するならこの表を更新) |

## 予約事項の消化 (greedy-cooking-wave.md「マイルストーンを跨ぐ予約事項」)

- **予約事項 1** (ステートフルバックエンドの導入様式) → **a で準備コミット、b で本体**。
  a = WorldHasher 3 出口 + `HashWorldImpl` の
  `(const CpuParticleBackend*, const TimeControl*, const PersistStore*)` を
  `const SimSources&` へ置換 (**挙動不変 = ハッシュ値ビット同一**)。
  参照 19 ファイル / `HashWorld(` 呼び出し ~107 箇所の機械的更新。
  b 以降、バックエンド追加 = SimSources に member 1 個 + ハッシュ節 1 個 + snapshot 節 1 個。
- **予約事項 3** (面サンプリングカーネル) → **i が第 2 の呼び手**。ただし調査で判明した
  3 つの穴に対処する: ①片面モデル (`un > 0` のみ圧を積む) — 布側で要素ごとに法線を
  風上へ反転して渡す。②力の畳み先が 1 個の `AeroAccum` — 要素 1 枚ごとに accum を
  作って 3 頂点へ分配 (トルクは捨てる)。③加算順序は固定 (並列化禁止) — 三角形 index
  昇順で回す。カーネル本体は**変更しない** (唯一の実運用呼び手 Aero.surfaceModel を
  護る。両面フラグを足したくなったら別コミットで検討)。
- 予約事項 4 (substeps の置き場) → そのまま使う。XPBD も `PhysicsEnvironment.substeps` に従う。
- 予約事項 6 (TypeId 末尾 append) → 44/45/46。

## サブ分割 (14 サブ、a → n)

### M60'a: 予約事項 1 — WorldHasher の SimSources 化 (挙動不変)
- `WorldHasher.h` に `struct SimSources { const CpuParticleBackend* particles; const
  TimeControl* time; const PersistStore* persist; }` (全 member 既定 null)。
  `HashWorld` / `HashWorldDetailed` / `HashWorldDump` / `HashWorldImpl` の引数を
  `(World&, const SimSources&, ...)` へ置換。**畳み込み順は 1 ミリも動かさない**。
- 呼び出し側 19 ファイルを機械的に更新 (最大は PhysicsSelfTest.cpp の 41 行)。
  一時変数で `SimSources s; s.particles = ...;` と組む定型で置換し、diff を読める形に保つ。
- 検証: ①WorldHasherSelfTest の「3 出口 total 一致」PASS。②`--selftest` 全 PASS。
  ③`replay_verify.bat` PASS。④**2 段階ビルド照合** — 置換前後で同一シーンの
  `[phys] hash @240` ログが一致 (M59a 流の機械的証明。ハッシュ値がビット同一である
  ことの直接証拠)。⑤`check_rules.ps1` 0 error。
- .rep 版 bump **なし** (値が変わらないから)。ソース追加なし (ヘッダ内 struct のみ) なら
  gen_project_files 不要。

### M60'b: XpbdBackend 骨格 — 池 + ハッシュ節 + snapshot 節 + 配線
- `XpbdBackend.h/.cpp` 新規。池 = `struct Pool { EntityID owner; 種別; 粒子 SoA
  (px,py,pz / vx,vy,vz / xPrevX..Z / invMass) + 拘束 (rest 値は塑性で変わるので状態) }`、
  owner.index 昇順。`Sync(World&)` / `Reset()` / `PoolsForSnapshot()` を
  CpuParticleBackend と同じ顔で。まだ何もシミュしない (Sync と器だけ)。
- WorldHasher: `SimSources` に `const XpbdBackend* xpbd` を追加し、`HashXpbdPools` を
  CPU 粒子節の**後**に内容ゲートで畳む (決定台帳 4)。ダンプ列名は "Xpbd"。
- SimSnapshot: `SimRefs` に `XpbdBackend* xpbd` を追加、`WriteXpbd`/`ReadXpbd` 節を
  Loop 節の後・**World 節の前**に挿入、`kSimSnapshotVersion` 3 → **4**。
  EngineLoop の SimRefs 組み立て箇所 (EngineLoop.cpp:444 付近ほか) を全部更新。
- TickRunner/EngineLoop: EngineLoop が backend を所有し `TickServices` 経由で
  `PhysicsSystem::Update(world, dt, contacts, xpbd)` へ渡す (まだ中身は素通し)。
- 検証: ①空 backend で全既存セルフテスト・replay_verify がビット不変で PASS
  (内容ゲートの証明)。②新規 `XpbdSelfTest.cpp` (連鎖末尾へ append) — 手で池を組んで
  snapshot 往復ビット一致 + ハッシュが池の 1 float 変異で変わること。
  ③gen_project_files 実行。

### M60'c: XPBD ソルバ核 — 距離拘束 + Rope (TypeId 44)
- `XpbdSolver.h/.cpp`: 距離拘束の射影 (λ 蓄積、compliance α̃ = α/h²、減衰)。
  substep 内配置: 剛体の速度積分の後に「粒子: 重力 → 位置予測」、剛体ソルバの後に
  「粒子: 拘束射影 (固定 N 反復) → v=(x−xPrev)/h」。反復数は定数 (kXpbdIterations、
  初期値 8。早期終了なし)。
- `RopeComponent` (TypeId 44): `segmentCount` / `length` / `radius` / `mass` /
  `compliance` / `damping` / `attachStart` (bool、自エンティティ位置へピン) /
  `attachEnd` (bool) / `connectedEntity` (EntityRef、終端の接続先。d で使う。c では
  ワールド固定ピンのみ)。FieldDesc + `MYE_JP` + Inspector + LocalizationTable en/ja。
- 生成: `Sync` がコンポーネントから池を組む。初期配置はエンティティの姿勢から直線に
  垂らす。物理は tick 3.6 で `WorldMatrixComponent` を読めない (車輪と同じ罠、
  `Components.h:1036-1038`) — 剛体収集と同じ親フレーム合成で書く。
- 可視化: EditorLinePass へ粒子と拘束のデバッグ線 (M60a の関節可視化の範型)。
- ★ついで消化 (M60b 申し送り 8): Inspector を触るこのサブで、Joint の type コンボと
  フィールド出し分けを**絵で確認**する (一時プローブ)。崩れていたら別コミットで直す。
- 検証: XpbdSelfTest — ①2 粒子 1 拘束の解析解 (吊り下げ静止長 = rest + mgα/h² 系)、
  ②compliance=0 で伸び ~0、③100 tick 後の replay 往復ビット一致、④エネルギーが
  発散しない (上限監視)。replay_verify PASS (既存シーンはビット不変のまま)。

### M60'd: アタッチ双方向 — ロープで剛体を吊る
- 粒子↔剛体のアタッチ拘束: 剛体側は「アンカー点の逆質量 + 逆慣性の実効質量」で
  XPBD の重みに入れ、補正を pose (並進+回転) と速度の両方へ返す。M60a の
  `ConstraintBlock` の K の組み方 (`PhysicsSystem.cpp:483-524`) を粒子行で流用できるか
  冒頭で判定 — 使えなければ XPBD 側の剛体重み式 (Macklin 系) で新設。
- `RopeComponent.connectedEntity` が生きる。眠っている剛体はアタッチ経由で起こす
  (関節の起床 `:3074-3091` と同じ扱い)。
- 検証: ①ロープで吊った箱の静止張力 = mg (解析値)、②吊った箱を外すと自由落下、
  ③振り子の周期がオーダー一致、④眠った剛体がロープを引くと起きる、
  ⑤replay 往復ビット一致。

### M60'e: 粒子 vs 世界の衝突 (片方向)
- Shapes リファクタ: `SphereTriContact` / `ClosestPtPointTri` / `SoupGatherTris` /
  `SoupWorldTri` / `TriFaceNormal` を `shapes::` 公開へ (挙動 1 ビット不変。
  M60g2 の `DecomposeRowMajorTRS` 公開と同じ流儀の準備リファクタ)。
- 粒子を半径 r の球として静的コライダー + 剛体 (sphere/box/capsule/convex/mesh/terrain)
  に対し射影。摩擦 (静/動) は接触面内の位置差で。候補収集は粒子 AABB (半径ぶん) で
  小さく保つ — `kMeshMaxCandidates=256` は粒子単位なら溢れない見込み (溢れ検知ログは残す)。
- この段では**剛体は不動として扱う** (片方向)。ペア順は (pool, 粒子 index, 形状 index)
  の明示キーで固定。
- 検証: ①床の上のロープが貫通せず静止 (めり込み < slop)、②斜面で滑って落ちる、
  ③terrain / mesh / convex 各形状で貫通なし、④replay 往復ビット一致。

### M60'f: 接触双方向 — 変形体が剛体を押す ★本計画の最深部
- 冒頭確定: 方式 A =「粒子↔剛体接触を剛体側の速度ソルバ + 位置補正の 2 パスに
  粒子行として混ぜる」(剛体‐剛体接触と同型、ソルバが 1 本に閉じる) vs
  方式 B =「XPBD 射影が剛体 pose を直接補正」(実装は軽いが 2 ソルバが押し合う)。
  **A を第 1 候補**として 10 段スタック + 布上の箱で安定性を測ってから確定する。
- 質量比クランプ (重い箱 vs 軽い布粒子で発散しないための実効質量下限) を定数で。
- 検証: ①布の上に箱を落として静止 (箱が沈み込み、布が張る)、②ハンモック (両端
  アタッチ + 中央に剛体)、③10 tick 順序を入れ替えても決定論が割れない
  (= ペア順キーの検証)、④substeps 4/8/16 で発散しない、⑤replay 往復ビット一致。

### M60'g: ロープ描画 — 円柱チェーンの MeshInstancing
- 描画側が池を読み、セグメントごとのワールド行列 (位置 + 方向 + radius スケール) を
  組んで単位円柱を MeshInstancing で流す。非ハッシュ (決定台帳 8)。
- material は `RopeComponent` の AssetRef (決定台帳 10 — MeshRenderer は持たない)。
- 検証: --replay-record しながら絵を目視 + スクショ 1 枚を一時プローブで撮る
  (golden 登録は m まで待つ)。既存 golden 14 枚が maxDiff=0 のまま。

### M60'h: 布 — Cloth (TypeId 45) + 曲げ拘束
- `ClothComponent` (TypeId 45): `countX` / `countY` / `width` / `height` / `mass` /
  `stretchCompliance` / `bendCompliance` / `damping` / `thickness` (衝突半径) /
  ピン指定 (上辺固定等のプリセット int) / `connectedEntity` / material AssetRef。
- 生成: N×M グリッド + 構造/せん断エッジ (距離拘束) + 曲げ拘束 (隣接三角形の
  対角距離方式 — 二面角方式より安く決定論的に単純)。
- 衝突は e の粒子球で足りる (thickness = 半径)。デバッグ線描画。
- 検証: ①旗 (上辺ピン) が重力で垂れて静止、②曲げ compliance の大小で折れ皺が変わる
  (定量: 静止時ポテンシャルエネルギー比較)、③粒子数上限のクランプ (面が大きくても
  溢れない)、④replay 往復ビット一致。

### M60'i: 布空力 — AccumulateSurfaceElement の第 2 の呼び手 (予約事項 3)
- 三角形 index 昇順に `SurfaceElement` (重心・面積・法線・3 頂点平均速度) を組み、
  要素ごとに法線を風上へ反転して単発 `AeroAccum` で呼び、力を 3 頂点へ 1/3 分配。
  カーネル本体は無変更 (Aero.surfaceModel の挙動をビットで護る)。
- 風は `PhysicsEnvironment` の既存フィールドを読む。
- 検証: ①一様風で旗がなびく (定性) + 定常状態の傾き角が風速に単調 (定量)、
  ②Aero.surfaceModel の既存セルフテストが全 PASS のまま (カーネル不変の証明)、
  ③replay 往復ビット一致。CPU 予算: 三角形数 × substeps の逐次呼びを実測して
  進捗表に記録 (溢れるならショーケースの布サイズで調整。カーネ限並列化はしない)。

### M60'j: 動的メッシュ描画経路 — 布/ソフトボディを PBR へ
- `GpuResources` に DYNAMIC VB のメッシュ登録 + `UpdateMeshVertices()` を追加
  (`Mesh` 構造体は不変レイアウトのまま `usage` 分岐)。IMMUTABLE 経路は 1 バイトも
  変えない。★`Mesh.aabbMin/aabbMax` を毎フレーム更新する — 忘れるとフラスタム
  カリングで布が消える (調査で判明済みの罠)。法線は頂点共有で面法線平均 (描画側)。
- RenderSystem に Cloth/SoftBody の収集経路 (MeshRenderer 非依存、決定台帳 10)。
  影 / deferred / forward の両パスで描けること。
- 検証: ①布が PBR + 影つきで描ける、②カメラを引いてもカリングで消えない、
  ③--render-demo / 既存 golden 14 枚 maxDiff=0、④ヘッドレス (--selftest) で
  D3D 無しでも sim 側が落ちない。

### M60'k: ソフトボディ — SoftBody (TypeId 46) + 体積拘束
- `SoftBodyComponent` (TypeId 46): 閉表面メッシュ (球/箱のプリセット生成。任意メッシュは
  対象外)、`volumeCompliance` (圧縮性) / `stretchCompliance` (剛性・弾性) / `damping`
  (粘性の一次) / `pressure` (過圧係数、風船)。
- 全体積拘束: C = V − pressure·V0、勾配は面法線の 1/3 和 (標準 XPBD 体積拘束)。
  表面エッジの距離拘束と併用。
- 検証: ①静止体積が pressure に単調、②床に落として潰れて戻る (体積保存誤差 < 1%)、
  ③剛体を上に載せて双方向 (f の再検証)、④replay 往復ビット一致。

### M60'l: 塑性・粘弾性・クリープ・応力緩和 + PhysMat 接続
- 距離拘束に塑性: |strain| > yield で rest 長を creep 率で現在長へ寄せる (rest は
  b から池の状態 = ハッシュ/snapshot 対象なので追加コストなし)。
  応力緩和/クリープ = Maxwell 要素 (rest が時定数 τ で緩和)。全て tick 内の純関数
  (実時間・乱数を混ぜない)。
- `.physmat.json` へ末尾 append: `stretchCompliance` / `bendCompliance` /
  `volumeCompliance` / `plasticYield` / `plasticCreep` / `relaxationTime` (名称は
  実装時に確定してこの行を更新)。`Sanitize` に NaN/負値の防波堤。値選択は M59a2 則
  (材料あり → physmat / 無し → コンポーネントフィールド)。プリセット 4+1 種への
  値追記は任意 (省略時は既定値 = 弾性のみ)。
- 検証: ①yield 未満で完全弾性 (rest 不変)、②yield 超で曲がり癖が残る (rest 変化)、
  ③τ で応力が指数緩和 (対数プロット直線性)、④materialOverride の優先順位、
  ⑤replay 往復ビット一致 (rest が状態に入っている証明)。

### M60'm: ショーケース --deform-demo + replay 7 ペア目 + golden 15 枚目
- `--deform-demo`: 旗 (風) / 吊り橋ロープ + 箱 / ハンモック / ゼリー落下 / 塑性で
  曲がる棒 を 1 シーンに。DemoContent 正本方式 (生成物は gitignore)。
- `tools\replay_verify.bat` に 7 ペア目を追加。`shot_verify.bat` に `deform.png`
  (15 枚目) — **物理系なので frame 120 で撮る** (joints.png の前例)。CI 判定に入るか
  (機種差の有無) はローカル → CI の順で確認して tol を決める。
- 検証: replay_verify 7 ペア PASS / shot_verify 15 枚 PASS / 既存 14 枚 maxDiff=0。

### M60'n: 仕上げ — spec / README / 規則
- `engine_spec.md`: 7.x に変形体の節 (バックエンド様式・決定論契約への追記 —
  「ECS 外 sim 状態はバックエンド + ハッシュ節 + snapshot 節の 3 点セット」を明文化)。
  11.2 規則群に抵触が無いか通読。README に機能概要 (日本語)。
- ABI 最終確認 (追加ゼロなら v14 のまま = check_rules の表も不変)。
- `check_rules.ps1` / `crash_verify` / `net_verify` を一巡 (ネットロールバックが
  XpbdBackend の snapshot 節で正しく巻き戻ることを net_verify で確認)。

## 壊れるもの台帳

| 対象 | いつ | 内容 |
|---|---|---|
| WorldHasher の署名 | a | 3 出口 + Impl を `SimSources` へ (**ハッシュ値はビット同一** — 証明は 2 段階ビルドの `[phys] hash` 照合) |
| TypeId | c, h, k | **44=Rope / 45=Cloth / 46=SoftBody** を末尾 append |
| SimSnapshot 版 | b | v3 → **v4** (Xpbd 節追加。World 節の前)。旧 blob は版不一致で拒否 = 仕様どおり |
| ReplayFile 版 | **bump なし見込み** | ハッシュは a で値不変・b で内容ゲート。変形体入りシーンは新規コンテンツなので過去 .rep と衝突しない |
| `.physmat.json` スキーマ | l | compliance 系 6 キーを末尾 append (未指定 = 既定値 = 従来) |
| Shapes.h の公開面 | e | `SphereTriContact` 等 5 関数の公開 (**挙動 1 ビット不変**の準備リファクタ) |
| `PhysicsSystem::Update` 署名 | b | `XpbdBackend*` 引数追加 (TickRunner/TickServices 経由) |
| GpuResources | j | DYNAMIC VB 経路追加 (IMMUTABLE 経路は不変) |
| 新規セルフテスト | b | `XpbdSelfTest.cpp` を連鎖へ append (PhysicsSelfTest は 6109 行で満杯) |
| golden | m のみ | `deform.png` 1 枚追加。既存 14 枚は maxDiff=0 を各サブで確認 |
| replay_verify | m | 6 → 7 ペア |
| ABI | — | 追加ゼロ見込み = v14 のまま |

## 対象外 (明示)

- 自己衝突 (布‐布・ロープ‐ロープ・布内交差) — M62 の粒子流体スパイクと同じ箱
- 変形体のスリープ — 将来。常時シムの CPU コストで受ける
- 四面体ソフトボディ / 任意メッシュの布化・ソフトボディ化 — 将来
- 引き裂き (tearing) — M61 (破壊) の接続グラフ側で扱う方が筋が良い
- 粒子状態へのスクリプト (C++/C#) アクセス — ABI 追加ゼロの帰結。需要が出たら別途
- SSE 化 — スカラー参照実装が固まってから将来検討
- GPU シム — 決定論契約 (Debug/Release/WARP ビット一致) と両立しないので恒久対象外

## リスクと縮退ライン

1. **f (接触双方向) が安定しない** — 最深部。縮退の階段: 質量比クランプ強化 →
   kXpbdIterations/substeps 増 (showcase 側で払う) → 方式 B へ切替 → それでも駄目なら
   「布上の剛体」をショーケースから外し接触は片方向へ縮退 (**ユーザーへ相談してから**)。
   d (アタッチ双方向) までは縮退しても死守する。
2. **布空力の CPU 予算** — 逐次固定順が譲れないので、溢れたら布の解像度で調整。
   i で実測値を進捗表に残す。
3. **動的 VB の性能** (j) — Map/Discard × 布枚数。数枚 × 千頂点なら余裕の見込みだが、
   D3D11_USAGE_DYNAMIC の書き方 (全書き換え) を守る。
4. **e の摩擦の決定論** — 接触面内の位置差方式は履歴を持たない純関数に保つ
   (アンカー履歴を持つと snapshot 対象が増える。持つ場合は池に入れて b の 3 点セットに従う)。

## 検証の柱 (全サブ共通)

- Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS →
  `tools\replay_verify.bat` PASS → `pwsh -File tools\check_rules.ps1` 0 error。
  ソース追加時は `pwsh -File tools\gen_project_files.ps1`。m は `shot_verify.bat` も。
- 既存シーンのビット不変が主張できるサブ (a/b/e) は 2 段階ビルドの `[phys] hash @240`
  照合で機械的に証明する。
- 新機能サブは「解析解 or 単調性」の定量テストを XpbdSelfTest へ必ず 1 本以上足す。
- UI 文字列は LocalizationTable.inl に en/ja 両方 (規則 10)。コメントは日本語で
  「なぜ・罠」を書く。

## 進捗

| サブ | 状態 | コミット | 備考 |
|---|---|---|---|
| M60'a 予約事項 1 (SimSources) | 完了 | (本コミット) | 下記「M60'a の申し送り」参照 |
| M60'b XpbdBackend 骨格 | 未着手 | — | |
| M60'c ソルバ核 + Rope | 未着手 | — | M60b 申し送り 8 (Joint Inspector 絵確認) をついで消化 |
| M60'd アタッチ双方向 | 未着手 | — | |
| M60'e 粒子 vs 世界 (片方向) | 未着手 | — | |
| M60'f 接触双方向 | 未着手 | — | 最深部。冒頭で方式 A/B を確定 |
| M60'g ロープ描画 | 未着手 | — | |
| M60'h 布 + 曲げ | 未着手 | — | |
| M60'i 布空力 (予約事項 3) | 未着手 | — | |
| M60'j 動的メッシュ描画 | 未着手 | — | |
| M60'k ソフトボディ | 未着手 | — | |
| M60'l 塑性・粘弾性 + PhysMat | 未着手 | — | |
| M60'm ショーケース + 回帰 | 未着手 | — | |
| M60'n 仕上げ | 未着手 | — | |

### M60'a の申し送り (計画外の事実・罠)

1. **取りこぼしの安全網はコンパイラだった** — `nullptr` は `const SimSources&` へ暗黙変換
   されないので、旧署名のままの呼び出しは全部コンパイルエラーで露見する。b で member を
   足すときも「positional 波括弧の既存呼び出しは末尾 append なら壊れない」が成立する。
2. 実測の置換規模: 19 ファイル ±150 行 (計画の「~107 箇所」と整合)。最多は
   PhysicsSelfTest.cpp の 41 呼び出しで、全部 `HashWorld(w, nullptr)` →
   `HashWorld(w)` の機械置換に落ちた。
3. 挙動不変の証明は selftest の `hash @` 行 14 本 (物理 9 + 関節系ほか) の置換前後
   ビット一致で取った。`[phys]` に限らず `hash @` で拾うと関節シーン (@300) も入る。
