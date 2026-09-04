# M67: ReSTIR 反射 + ReflectionClass (反射に映る側の品質制御) — 仕様書

- slug: m67-restir-reflection
- 状態: 確定 (2026-09-04)。`AskUserQuestion` が無い環境で planner が裁定し、§7 の U1〜U6 を司会がユーザーに確認した (2026-09-04)。**U4 (temporal の Jacobian) だけ裁定と逆 = 厳密に計算する**。他 5 件は裁定どおり
- 依頼原文: "C:\Users\akita\.claude\plans\dynamic-object-adaptive-restir-reflectio-magical-sketch.md"これの計画をエンジンに実装
- 正本: `plans/m67-restir-reflection/plan-original.md` (ユーザー作成、以下「元計画」)。本書は元計画を
  取り込み、**コードと突き合わせて見つかった穴と、その埋め方だけ**を追記する。元計画の
  「元の提案からの意図的な変更点」4 点・ReflectionClass をヒット側 `RtInstance` に置く設計修正・
  スコープ外 5 点は再議論しない。
- 基点コミット: 30f4993 (M66 完了直後、master、clean)

## 1. 目的 (なぜ作るか)

RT 反射レーン (M46h) は GGX VNDF 1spp → SVGF (履歴 8 / A-Trous ×2) で、roughness 0.3〜0.6 帯の
分散を A-Trous で均すぶん**反射像のディテールが溶ける**。ReSTIR (時空間サンプル再利用) で
レイ数を増やさずに実効サンプル数を上げ、**反射に映る側の物体**に ReflectionClass (5 段) を持たせて
「主役は保守的に (にじませず・ゴーストさせず)、小物は積極的に再利用」を実現する。

達成したい状態:
- `--rt-refl --rt-restir` で、同一レイ数のまま反射像の時間フリッカーが減る (機械指標 + ユーザー目視)
- 反射に映る物体のクラスを変えると再利用の挙動が実際に変わる (機械で観測できる)
- **ReSTIR off = 既存の絵とビット一致** — これを **golden で機械証明する** (元計画の最大の穴、§2 S1)
- sim / ワールドハッシュ / `.rep` / ECS / G-Buffer には 1 バイトも触れない

## 2. 疑った点と結論

元計画の「実物確認済み」主張を基点 30f4993 で再検証した。**正確だったもの**: `RtInstance.pad0/pad1` が
空き (`RtTypes.h:138-139`、`static_assert == 80`)、`RtHitMaterial` が `gRtInstances[hit.inst]` を
既に引いている (`rt_common.hlsli:282`)、`RtTraceRadianceLod` の first-hit 分解が可能
(`rt_common.hlsli:517-556`、ループ 1 周目が hit / P / N / material を持つ)、`Material` 末尾 append の
前例 `emissiveIntensity` (`GpuResources.h:195`)、`ParseMaterialJson` (`GpuResources.cpp:1001`)、
デバッグモード 12/13 が空き (`RtPasses.cpp:779-803` は 4〜11)、`RtHistory` の viewKey 別
ping-pong (`RtPasses.h:116-125`)、UAV は u0 のみ使用。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| S1 | **「golden 19 枚無変更 = 後方互換の証明」は RT 反射については空振り** | `shot_verify.bat` の 19 本の `call :shot` に `--rt-*` が 1 つも無い (bat:112 「RT デモは WARP では重すぎるので CI 対象外」)。`rtReflEnabled` を立てる口は `--rt-refl` (`EditorMain.cpp:345` / `RuntimeMain.cpp:341`) とメニューだけ = **RT 反射 / GI の絵はどこにも固定されていない**。M65 でも「4 サブぶん golden に写っていなかった」を踏んでいる (bat:301) | 裁定 (§7 U1) → 確認済み (2026-09-04) | **sub-01 で RT golden を足す**。実測 (planner、Release 30f4993 相当、WARP 960x540 frames 6): `--render-demo --deferred --rt-refl` の撮影 **11 s** (RT 無し 7 s)、**2 回撮って maxDiff=0**、RT 無しとは 45114 画素差。= 撮れる・決定的・被覆がある。「重すぎる」は `--rt-demo` (コーネル箱) の話。SSR と同じ**ローカル限定 tol=0** (BVH の hit/miss 分岐は機種で反転しうる) |
| S2 | `RtScene` / `RtSceneBuild` のどちらで `pad0` を埋めるか (元計画「未読」) | `RtScene::Update` (`RtScene.cpp:163-185`) が `RtInstance` を組み `Material*` から `RtMaterial` を埋めている**同じループ**。`RtSceneBuild` は純粋な BVH 構築で Material を知らない | 事実 | `RtScene::Update` で `inst.reflectionClass = mat ? mat->reflectionClass : 4`。`pad0` は**改名する** (`reflectionClass`、C++/HLSL 両方。レイアウト不変 = `static_assert` 不変。「pad0 の意味付けコメント」より名前で縛る) |
| S3 | 元計画の 3 パス (refl / temporal / spatial) のうち temporal の別 Dispatch は要らない | temporal 統合は「自画素の初期 reservoir + 前フレーム reservoir」だけを読む = 近傍同期が要らない。別 Dispatch にすると reservoir を**同じテクスチャで読み書き** (typed UAV load) するか 3 組目を持つことになる。R16G16B16A16 / R32G32B32A32 の typed UAV load は FL 11_0 で保証されない (保証は R32 単チャンネルのみ) | 裁定 (§7 U5) → 確認済み (2026-09-04) | **2 パス**: `rt_refl.cs.hlsl` = トレース + 初期化 + temporal 統合 (前フレーム = SRV、今フレーム = UAV、別テクスチャ)、`rt_refl_restir_spatial.cs.hlsl` = 空間統合 + resolve + 次フレームへの書き戻し。spatial は近傍が揃ってから読むので別 Dispatch (元計画どおり) |
| S4 | temporal 再利用の Jacobian は「前フレームの受け側位置 P_prev」が要るが reservoir に無い | 元計画の J 式は受け側の位置差 (d_old / d_new、cosθ) を使う。P_prev を持つには R32G32B32A32 が 1 枚増える (+16 B/px) | 裁定 (§7 U4) は「J = 1 近似」→ **ユーザー: 厳密に計算する (2026-09-04、裁定と逆)** | reservoir 組に **`rpos` (受け側ワールド座標 P、R32G32B32A32) を持ち、temporal でも `RtRestirJacobian(xs', ns', P_prev, P)` を掛ける** (spatial と同じ関数・同じ範囲棄却)。近似案 (P_prev ≈ P) は却下 = カメラや受け側が動いたフレームで d² / cosθ の比が 1 から外れる分を正しく補正する。§4.2 の表と §4.3 に反映。以後蒸し返さない |
| S5 | temporal 統合の妥当性判定に「前フレームの受け側法線・距離」が要るが、`RtHistory.geom` は SVGF の Accumulate が書く = ReSTIR の後段 | `RtPasses.cpp:401-490`。flip タイミングを合わせれば読めるが `rtTemporal=0` で消える = ReSTIR が SVGF temporal に縛られる (A/B できない) | 事実 | reservoir 組に **`geom` (受け側 N + カメラ距離、`RtHistory.geom` と同レイアウト) を持たせる** (+8 B/px)。`RtReprojectValid` をそのまま流用。ReSTIR は `--rt-no-temporal` でも成立する = S5 (調整) で ReSTIR 単体の寄与を見られる |
| S6 | 「デバッグモード 13 でクラス色分けを反射像側に出す」は reservoir 前 (S1 相当) には成立しない | クラスはヒット点でしか分からない。reservoir が無い段階で「反射像側」に出すには反射バッファへ書く場所が要る (`reflRt_.a` は誰も読まないが、golden 不変の主張を弱める) | 裁定 | **13 = 一次ヒットのクラス** (`rt_debug.cs.hlsl` に mode 3 と同型で追加。配管の検証はこれで足りる)、**14 = 反射像側 (reservoir の cls)** は sub-04 で。12 = reservoir M |
| S7 | 元計画 S5 「パラメータ調整 = ユーザー判定ループ」が「定数を直して再ビルド」だと 1 周が長い | クラス表は `kRtRefl*` と同じ constexpr の予定 (元計画 §4)。1 周 = 編集 + Engine.lib 再ビルド + 起動 | 裁定 (§7 U2) → 確認済み (2026-09-04) | **RT Debug → ReSTIR サブメニューに実行中スライダ** (クラス表 5×3 / SVGF 履歴 / A-Trous 回数 / 可視レイ / クラス上書き / 既定に戻す)。非永続。既定値の出所は `RtTypes.h` の定数表のまま (C++ が唯一の出所 = 元計画の流儀を維持) |
| S8 | 「反射に映る箱アクタのクラスを変えると挙動が変わる」を coder が**ヘッドレスで**確かめる手段が無い | クラスは Material の値 = 変えるには JSON かコードの編集 + 再起動 | 裁定 | `--rt-class-override N` (全インスタンスのクラスを N に強制、-1 = off) を足す。`0` と `3` の撮影でフリッカー指標が変わることを機械で見る。S5 でも「全部 Hero だとどう見えるか」に使える |
| S9 | 元計画「既定クラス (4) の値は現行挙動と一致させる」は字義どおりには成立しない | 現行挙動 = 再利用なし。クラス 4 の表 (8 px / 4 タップ / M 16) は再利用ありなので一致しようがない | 事実 | 読み替え: **Material の既定 = 4** かつ **ReSTIR off = 現行とビット一致**。表の値は元計画のまま採用し S5 で追い込む |
| S10 | 共有ヘルパの分解 (`RtTraceRadianceLod` → first-hit 版 + ラッパ) が GI 経路を動かしても検出できない | `rt_gi.cs.hlsl` → `RtTraceRadiance` → `RtTraceRadianceLod` (`rt_common.hlsli:559`)。GI の golden も無い (S1 と同根) | 裁定 (§7 U1) → 確認済み (2026-09-04) | sub-01 の golden は **反射 + GI の 2 枚** (`demo_render_rtrefl` / `demo_render_rtgi`)。GI 側が run-to-run で割れたら GI 側だけ落として報告 |
| S11 | 音響デモの箱は反射に映るか | `adem_*` は `makeMat` (`DemoContent.cpp:2699-2705`) で shader/texture/baseColor しか設定しない = roughness 既定 0.5 → `RtReflWeight` で RT 50% / IBL 50% の混色帯。**まさに元計画が狙う 0.3〜0.6 帯**。プレイヤー・敵は箱 (`SkinnedMeshComponent` 無し) なので BVH に入る | 事実 | 成立する。加えて **`--render-demo` の `rdemo_mirror` (粗さ 0.10) + `rdemo_spin` (0.45、回転体)** を golden と機械検証の被写体にする (静的・frame 3・11 s) |
| S12 | reservoir の保存量は wSum か W か | 元計画は wSum。統合の重み (教科書形) は `p̂_q(y) · W · M · J` で W を使う。resolve は `Ls · wSum / (M · lum(Ls))` で D_vis が約分される | 裁定 | **保存は W = wSum / (M · p̂_q(y))** (統合が教科書形のまま)。resolve はパス内のレジスタで wSum を持って `Ls · wSum / (M · lum)` (M=1 なら `lum/lum = 1` で **Ls とビット一致**) |
| S13 | `DemoContent.cpp` の場所 | 元計画は `src/GameLogic/DemoContent.cpp:3075` と書くが実体は **`src/Engine/Engine/DemoContent.cpp`** (Engine 層) | 事実 | 触る場所を訂正。デモへ足すのは Material のフィールドだけ (エンティティは足さない = 粒子 RNG ストリーム不変) |
| S14 | 撮影時の自動シード凍結 (`EngineLoop.cpp:299`) で temporal の効果が golden に写らない | 凍結中は毎フレーム同じ 1spp → M は伸びるが推定値は不変。spatial は画素間で違うので写る | 事実 | golden は「経路が壊れていない」を固定する目的。品質の観測は `--rt-anim-seed` で (A6 / A7) |
| S15 | **`Material` は cooked blob へ memcpy される** — フィールド追加は「末尾 append」だけでは済まない (planner の見落とし、sub-02 で coder が発見) | `ModelCook.cpp:15-19`「struct を memcpy で書く」+ `static_assert(sizeof(Material) == 56)` の門番。`kCookVersion` は M51b 導入 (`git log -S`) で、前例 `emissiveIntensity` (M46i) は cook cache **より前**の追加 = M67b が cook 以後で初のフィールド追加。`AssetID` (uint64) のアラインメントで 60 → 64 に丸まるため、暗黙パディングのままだと同じ入力の cooked ファイルのバイト列が run ごとに違いうる (`CookedCacheSelfTest` の memcmp も不定に) | 事実 (coder SELF_EVAL sub-02) | `kCookVersion` 1 → 2、`static_assert` 56 → 64、`Material` 末尾に明示 `pad0`。影響は「初回起動で 1 回焼き直す」だけ (cache は gitignore)。配布パッケージ (M51j 封印) は新 exe で作り直す (exe と cache は常に一緒に配る)。sub-07 で engine_spec §10.2 と CLAUDE.md のチェックリストに固定 |

再検証で**そのまま使える**と確認したもの: `--rt-refl` / `--rt-gi` / `--rt-debug N` / `--rt-no-temporal` /
`--rt-no-svgf` / `--rt-freeze-seed` / `--rt-anim-seed` の CLI (両 main)、`RtHistoryUv` / `RtReprojectValid` /
`RtLuminance` (HLSL は `rt_temporal.cs.hlsl:66-121`、C++ は `RtMath.h:256-320`)、`Blit` の mode 1 (a = 履歴長の
ヒートマップ、`rt_blit.hlsl:33`)、`RtDebugCB` の mode 分岐 (`rt_debug.cs.hlsl:55-62`)、`[rt]` ログ行
(`EngineLoop.cpp:1826-1837`、ヘッドレスで GPU 時間を残す唯一の口) と ProfilerWindow の行
(`ProfilerWindow.cpp:136`)、`$constGroups` の書式 (`check_rules.ps1:74-`)、Inspector のマテリアル編集
(`InspectorWindow.cpp:1448-1571`、load / save / スライダ)、新規マテリアルの雛形 (`AssetOps.cpp:339`)、
`AssetOpsSelfTest.cpp:310-319` の JSON 往復テスト、`.mat.json` はリポジトリに 2 枚のみ、ADR-016 は空き番、
新規 `.hlsl` は vcxproj に載らない (gen_project_files 不要)、cs_5_0 の UAV 8 本 / SRV は t11 以降が空き。

## 3. スコープ

- やる: 元計画 S0〜S4・S6 (§6 の表)。加えて本書で足した穴埋め: S1/S10 の RT golden 2 枚 + 仕上げで
  ReSTIR on の 1 枚 (計 22 枚)、S5 の reservoir `geom`、S7 のチューニング UI、S8 の `--rt-class-override`、
  可視レイ (元計画「後から有効化できる形」を**実装して既定 off**にする — フラグだけ用意して中身が無いと
  S5 で A/B できない)、temporal の厳密 Jacobian (U4、`rpos` テクスチャ)。
- やらない: 元計画「スコープ外」の全項目 (スキンメッシュの BVH 投入 / sparse tracing / オブジェクト単位の
  クラス上書き (※ `--rt-class-override` は全体強制のデバッグ用で、これではない) / cubemap 代替 / GI レーンの
  ReSTIR)。G-Buffer / `MaterialCB` / `ForwardPath` / `deferred_gbuffer.hlsl` に触れない。Forward 経路は
  対象外 (RT 反射自体が Deferred のみ)。unbiased ReSTIR (MIS 重み) はやらない — v1 は biased (M 加算) で、
  ADR に明記。
- 後回し: **S5 (パラメータ調整) は harness の外**でユーザーが回す (§4.6)。確定値は後続コミット
  `M67h` で `RtTypes.h` の既定表へ焼き、golden `demo_render_rtrefl_restir` を撮り直す。

## 4. 仕様

### 4.1 ReflectionClass (反射に映る側)

- 5 段 (`RtTypes.h` に定数 `kRtReflClassHero=0 / Character=1 / Vehicle=2 / Prop=3 / Default=4`、
  `kRtReflClassCount=5`。HLSL `MYE_RT_REFL_CLASS_COUNT 5` と規則 9 で照合)。
- **値の出所 = `Material::reflectionClass` (`int32_t`、既定 4、`emissiveIntensity` の後ろに append)**。
  `.mat.json` のキーは `"reflectionClass"` (整数)。**欠損 = 4、範囲外 (負・5 以上・非整数) = 4**
  (クランプではなく中立クラスへ落とす。-1 を 0=Hero に丸めてはいけない)。既存 `.mat.json` 2 枚は無編集で挙動不変。
- 流れ: `ParseMaterialJson` → `Material` → `RtScene::Update` で `RtInstance.reflectionClass` (旧 `pad0`) →
  HLSL `RtHitReflectionClass(RtHit)` = `gRtInstances[hit.inst].reflectionClass`。`RtInstance` の
  `pad1` は残す。`sizeof == 80` 不変。
- Inspector のマテリアル編集に Combo (5 クラス名、`Tr()`)。`MaterialEditToJson` が書き、
  `LoadMaterialEdit` が読む (欠損 = 4)。新規マテリアル雛形 (`CreateMaterialAsset`) にもキーを書く。
  ライブプレビュー (M53) はハッシュ同一性なのでフィールド追加で取りこぼさない。
- `--rt-class-override N` (CLI、両 main) / チューニング UI の同項目: `RtScene::Update` の引数で
  全インスタンスのクラスを N に強制。-1 = off (既定)。**Material は書き換えない**。
- デモの割り当て (Material のフィールドだけ。エンティティは足さない。U3 確認済み 2026-09-04):
  `rdemo_spin` = 0 Hero / `rdemo_pillar_a,b,c` = 3 Prop / `adem_player` = 0 Hero /
  `adem_agent_ear`, `adem_agent_eye` = 1 Character。他は既定 4。
- 色分け (デバッグ 13 / 14 で共通、HLSL `RtReflClassColor(int)`): 0 = 赤 (1, 0.2, 0.2) / 1 = 橙 (1, 0.6, 0.1) /
  2 = 黄 (0.9, 0.9, 0.2) / 3 = 水色 (0.2, 0.8, 1) / 4 = 灰 (0.5, 0.5, 0.5) / スカイ・空 reservoir = 黒。

### 4.2 Reservoir と数学 (`rt_restir_common.hlsli` ⇄ `RtMath.h`、両方更新・selftest がミラーを検証)

推定対象は現行と同じ「VNDF 方向の入射放射輝度の期待値」 `∫ Ls(L) · D_vis(L) dL`。
現行 1spp = `Ls` (pdf = D_vis で約分)。出力の次元は変えないので **合成側 (`common.hlsli:573`) は不変**。

| 記号 | 内容 |
|---|---|
| `xs` | 反射レイ first hit のワールド座標。スカイヒットは**方向ベクトル**を入れる |
| `ns` | ヒット点の法線 (両面規約 = レイに向く側)。**ゼロ = スカイのセンチネル** |
| `Ls` | ヒット点から出た放射輝度 = 既存 `RtTraceRadianceLod` の戻り値 (バウンス込み) |
| `cls` | ヒットしたインスタンスの ReflectionClass。スカイ = 4 |
| `M` | 統合したサンプル数 (0 = 空 reservoir) |
| `W` | unbiased contribution weight = `wSum / (M · p̂_q(y))` |

- **Target function**: `p̂_q(y) = lum(Ls) · D_vis(L | V_q, N_q, α_q)`、`L = normalize(xs − P_q)` (スカイは `L = xs`)。
  `D_vis` = GGX VNDF の pdf (Heitz 2018: `G1(V) · D(H) / (4 · (N·V))`、`H = normalize(V + L)`)。
  **新規 `RtGgxVndfPdf`** を両言語に足す。α は `max(α, kRtRestirAlphaMin = 1e-3)` で評価 (α=0 の δ を避ける)。
  `dot(L, N_q) ≤ 0` なら p̂ = 0。
- **初期サンプル** (rt_refl): 現行どおり VNDF で L を 1 本、`RtTraceRadianceFirstHit` で Ls と first-hit を得る。
  ソース pdf = D_vis なので `w = p̂/p = lum(Ls)`。`wSum = w, M = 1`。`lum = 0` (真っ黒) なら w = 0。
  現行の「N·V ≤ 1e-4 は鏡面方向」「ローブが面の下なら鏡面方向」の分岐はそのまま (ソース pdf の近似も現行と同じ)。
- **統合 (temporal / spatial 共通、`RtReservoirMerge`)**: 候補 reservoir `r'` (サンプル y'、W'、M'、cls') を
  `w = p̂_q(y') · W' · min(M', mCap[cls']) · J` で streaming RIS に足す (`wSum += w; M += min(M', mCap[cls'])`、
  `rnd < w / wSum` で採用)。乱数は `RtNextRand2(seed)` (既存の PCG3D 系列)。
- **Jacobian** (temporal / spatial 共通、S4 = ユーザー判断で厳密化): 受け側 `P_from` (候補の受け側 =
  temporal なら reservoir の `rpos`、spatial なら近傍の G-Buffer 位置) → `P_to` (自画素の P)、
  `J = (cosθ_to / cosθ_from) · (d_from² / d_to²)`、`d = |xs − P|`、`cosθ = |dot(ns, (P − xs) / d)|`。
  スカイ (ns = 0) は J = 1、L はそのまま。`J` が `[1/kRtRestirJacobianMax, kRtRestirJacobianMax]`
  (= 10) の外なら候補を棄却 (w = 0)。静止カメラ・静止面では `P_from == P_to` で J = 1 ちょうど
  (fp でも同じ式に同じ値が入るので比は 1) = 静止画では近似案と同じ絵になる。
- **クラス別の M 上限 (`mCap`)**: 統合前に候補の M を `mCap[cls']` へ切り詰める (上の式)。
  **書き戻し前**に `M > mCap[cls_sel]` なら `wSum *= mCap/M; M = mCap` (W は変わらない = 出力不変)。
- **Resolve**: `out = Ls_sel · wSum / (M · lum(Ls_sel))`、`lum ≤ 0` または `M = 0` なら 0。
  M = 1 (再利用なし) のとき `wSum = lum` → `out = Ls` とビット一致 (A5 の根拠)。
- **保存 (5 テクスチャ × 2 組、内部解像度、viewKey 別スロット)**:

| テクスチャ | フォーマット | xyz | w |
|---|---|---|---|
| `pos` | R32G32B32A32_FLOAT | `xs` (スカイは方向) | `W` |
| `rad` | R16G16B16A16_FLOAT | `Ls` | `M` (≤ 32 なので半精度で正確) |
| `nrm` | R16G16B16A16_FLOAT | `ns` (0 = スカイ) | `cls` (float、0〜4) |
| `geom` | R16G16B16A16_FLOAT | 受け側 N (G-Buffer) | 受け側カメラ距離 (`RtHistory.geom` と同レイアウト) |
| `rpos` | R32G32B32A32_FLOAT | 受け側ワールド座標 P (G-Buffer の `gp` の値そのもの。temporal の Jacobian の `P_from`。半精度にすると遠景で d² の比が狂うので fp32) | 予備 (0) |

  48 B/px × 2 組。1600×900 × 0.5² で約 35 MB/viewKey、`RtHistory` と同じく**使ったスロットだけ遅延確保**。
  組 A = フレーム間で持ち越す側 (固定 index 0)、組 B = フレーム内のスクラッチ (index 1)。
  `rt_refl` は A を読み B へ書く、`spatial` は B を読み A へ書く (flip しない。読む側と書く側が常に別テクスチャ
  = typed UAV load 不要)。「ジオメトリ無し」「roughness 超過」の画素は M = 0 を書く。

### 4.3 パス構成と配管

```
rt_refl.cs.hlsl (改)            gRsOn == 0: 現行と同一経路 (uniform 分岐、u1-u5 は書かない・張らない)
                                gRsOn != 0: 1spp → 初期 reservoir → temporal 統合 (A を SRV で読む、
                                            J は A の rpos と現 P から) → B (u1-u5) へ。
                                            u0 (reflRt_) には現行どおり生の Ls を書く
rt_refl_restir_spatial.cs.hlsl (新)  B と G-Buffer を読み、k タップ統合 → resolve → reflRestirRt_ (u0)
                                            + 次フレーム用に A (u1-u5) へ書き戻し (rpos = 自画素の P)
rt_temporal / rt_variance / rt_atrous (無変更)  入力が reflRt_ から reflRestirRt_ に変わるだけ
```

- **`RtRestirCB : register(b3)`** (両パス共通、C++ `struct RtRestirCB` + `static_assert`):
  `gRsOn / gRsSpatialOn / gRsVisRay / gRsHistValid / gRsUseVelocity / gRsClassOverride` (int)、
  `gRsPrevViewProj` (転置)、`gRsPrevCameraPos`、`gRsFrameIndex`、`gRsDepthThreshold` /
  `gRsNormalThreshold` (`kRtTemporal*` を流用)、`gRsJacobianMax`、
  `gRsClass[MYE_RT_REFL_CLASS_COUNT]` (float4: 半径 px / タップ数 / M 上限 / 予備)。
  レイアウトは coder が決めるが**配列長は規則 9 に登録**。`RtReflCB` (64 B) は触らない。
- temporal の履歴 UV: `RtHistoryUv` / `RtClipToPrevUv` / `RtReprojectValid` / `RtLuminance` を
  `rt_temporal.cs.hlsl` から **`rt_reproject.hlsli` へ純移動** (rt_temporal / rt_refl / spatial が include)。
  移動だけなので GI / 反射の golden で不変を証明する。履歴の有効条件は `Accumulate` と同じ
  (`lastSerial + 1 == rtViewSerial && prevViewProjValid`、velocity は `gbVelocity != null && histValid`)。
  ReSTIR を off にしたフレームで `hasLast = false` に落とす (on/off を切り替えても混ざらない)。
- spatial: 中心 reservoir のクラス `c0` で `k = taps[c0]`、`r = radius[c0]` (**内部解像度の画素**)。
  タップ = 半径 r の円板上の Vogel 螺旋 k 点を画素・フレームのハッシュで回転 (乱数は `RtPcg3d` 系列)。
  候補ごとに: 受け側の幾何一致 (`RtReprojectValid` を近傍 `geom` に流用) → **候補のクラス `cn` で
  `|offset| ≤ radius[cn]`** (Hero のサンプルは 2 px より遠くへ運ばれない = クラス境界で急変させない仕組み) →
  `L' = normalize(xs_n − P)` が半球内 → J の範囲内 → (`gRsVisRay` なら `RtTraceAnyHit(P + N·eps, L', d − eps)` で
  遮蔽なら棄却) → 統合。ループは `[loop]` で `MYE_RT_RESTIR_MAX_TAPS` (= 8、規則 9) を上限に `i ≥ k` で抜ける。
- `RtPasses`: `RtReservoirSlot` × `kHistorySlots`、`restirCS_` / `restirCB_` / `restirTimer_` /
  `RestirGpuMs()`、`RtReflResult` に `reservoirM` / `reservoirCls` (組 A の `rad` / `nrm` SRV、off なら null)。
  `RenderReflection` は `view.rtReflRestir != 0` のとき「refl → spatial → Accumulate(reflRestirRt_) → Denoise」、
  SVGF の履歴長 / A-Trous 回数は `view.rtReflRestirParams.svgfHistory / atrousIterations`
  (既定 = `kRtReflMaxHistory` = 8 / `kRtReflAtrousIterations` = 2 = 現行と同値)。off は現行コードのまま。
- `RenderView` **末尾 append**: `int32_t rtReflRestir = 0;` と `RtReflRestirParams rtReflRestirParams;`
  (`RtTypes.h` の POD、既定 = 定数表)。`RenderSystem` に `bool rtReflRestir` と同 params を持ち、毎フレーム写す。
  `rtDebugMode ∈ {12, 14}` のときは `view.rtReflRestir = 1` に強制 (影の 9 と同じ流儀)。
- CLI (両 main、`EngineLoop::Config` 経由): `--rt-restir` / `--rt-restir-no-spatial` / `--rt-restir-visray` /
  `--rt-class-override N`。全て既定 off / -1。
- `[rt]` ログ行と ProfilerWindow の `rt refl` 行に `restir %.3f ms` を足す。

### 4.4 UI / ビジュアル

- View → RT Debug メニュー: `ReSTIR Reflection` トグル (Temporal / SVGF の並び) と、サブメニュー `ReSTIR` に
  `Spatial reuse` / `Visibility ray` / クラス上書き (Off + 5 クラス) / クラス表 5 行 × (半径 1〜32 px / タップ 0〜8 /
  M 上限 1〜32) のスライダ / `SVGF history` (1〜32) / `A-Trous iterations` (0〜4) / `Reset to defaults`。
  非永続 (rtBounces と同じ扱い)。ReSTIR off の間はサブメニューを `BeginDisabled`。
- RT Debug のモード: 12 `Reservoir M` (Blit mode 1、param = 32: 赤 = 1 → 緑 = 32) /
  13 `Reflection Class (primary)` (rt_debug CS) / 14 `Reflection Class (reflected)` (Blit 新 mode 4 = `nrm.w` を
  §4.1 の色へ。空 = 黒)。12 / 14 は `--rt-refl` が前提 (10 / 11 と同じ。反射パスの産物を読む)。
  **13 は `--rt-refl` 不要** (rt_debug の CS はカメラから一次レイを撃つだけで反射バッファを見ない — sub-02 で確認)。
  `RtPasses::RenderDebug` は 4〜11 が Blit で早期 return し、それ以外が CS 経路に落ちる構造なので、
  12 / 14 の if は**その連鎖の中**に置き、13 は「どの早期 return にも当たらない」ことで CS へ落とす。
- Inspector: `反射クラス` Combo + ツールチップ (「反射に映るときの再利用の厳しさ。主役ほど保守的」)。
- 文字列は `LocalizationTable.inl` に en/ja、`###` 右辺は両言語一致・一意、`Tr()` を printf の唯一の引数にしない。

### 4.5 非機能

- **決定論**: 描画専用。sim / `WorldHash` / `.rep` / ECS / `FieldDesc` / ABI / C# に触れない
  (`replay_verify.bat` が機械確認)。新シェーダの乱数は `RtPcg3d(pixel, frame)` 系列のみ。
  `--screenshot` の自動シード凍結でフレーム間の乱数・タップ回転が固定される = golden が run-to-run で一致する。
- **後方互換**: `rtReflRestir = 0` (既定) の絵は現行と**ビット一致** (golden 21 枚)。`.mat.json` は欠損キー既定。
  `RtInstance` レイアウト不変。`RenderView` は末尾 append。
- **性能**: レイ数据え置き (可視レイ off)。追加コストは reservoir 書き出し + spatial の k タップ。
  数値は `[rt]` ログ (WARP) と ProfilerWindow (実 GPU、ユーザー) で見る。
- **規則**: 9 (`kRtReflClassCount` / `kRtRestirMaxTaps` を `$constGroups` へ)、10 (書式指定子)、
  1/2/4/7/8/11/12 は非接触だが `check_rules.ps1` を毎サブ回す。
- **層**: 生の D3D は `RtPasses` (Renderer) に閉じる。`RtScene` (Engine) は既存の前例どおり。

### 4.6 S5 (パラメータ調整) の切り分け

| 誰が | 何を | どこで |
|---|---|---|
| coder (sub-06) | トグル / CB 配管 / 既定値 / 実行中スライダ / `--rt-class-override` / フリッカー指標の一時スクリプト | 本 harness |
| ユーザー | `Editor.exe --render-demo --deferred --rt-refl --rt-restir` (と `--acoustic-demo`) で `--rt-anim-seed` 相当 (エディタは凍結しない) の実機目視。クラス表・SVGF 履歴・A-Trous・可視レイの要否・クラス境界のアーティファクト・二重 temporal の引きずり | harness の外 |
| coder (後続 `M67h`) | ユーザーの確定値を `RtTypes.h` の既定表へ焼く + golden `demo_render_rtrefl_restir` を `--update` | 別セッション |

## 5. 受け入れ条件

| # | 条件 | 検証手段 |
|---|---|---|
| A1 | ReSTIR off (既定) で golden **全枚**がビット一致 (sub-01 以降 21 枚、sub-07 以降 22 枚) | `tools\shot_verify.bat` (Release、`MYE_SHOT_SKIP_*` 無し) 全緑 |
| A2 | 新 golden (`demo_render_rtrefl` / `demo_render_rtgi`) は同一バイナリで 2 回撮って maxDiff=0、1 枚 ≤ 60 s (WARP、SHOTBASE 条件) | sub-01 で 2 回撮影 + `Editor.exe --img-diff A B --tol 0` PASS、所要秒を実装メモに |
| A3 | ReflectionClass の配管: JSON 欠損 = 4 / 範囲外・非整数 = 4 / 値 1 → 1 / 両端 0・4、雛形 (`CreateMaterialAsset`) の往復、Inspector 保存 → 読み直しで往復、`--rt-debug 13` で一次ヒットがクラス色 | `Editor.exe --selftest` (AssetOps の JSON 往復。sub-02 で実装済み)、`Runtime.exe --render-demo --deferred --rt-debug 13 --screenshot` の画像 (spin 赤 / 柱 水色 / 床 灰。sub-02 で画素実測済み)。**Inspector の往復は reviewer の実機操作** (`LoadMaterialEdit` / `MaterialEditToJson` は private でヘッドレスから呼べない): `.mat.json` を選択 → Combo でクラス変更 → 保存 → 別アセットを選んで戻ると値が残り、ファイルに `"reflectionClass": N` が書かれている |
| A4 | ReSTIR 数学の CPU ミラー: VNDF pdf が半球で 1 に積分 (α = 0.36 / 0.04、±3%) / reservoir 更新の採用確率が重み比 (1:2:7、2 万回、±0.02) / M=1 で `Ls · wSum/(M·lum) == Ls` ビット一致、lum=0 で 0 / J(A→B)·J(B→A) = 1 (±1e-5)、同一点 = 1、スカイ = 1、受け側が 2 倍遠ざかると J = 1/4 (cosθ 同じ) / 統合の重みが J に比例 (J=2 で候補の重みが 2 倍、範囲外 J で 0) / M 上限: M'=100・cap 8 で統合後 M = 9 / 書き戻しクランプで W 不変 / 定数表 5 行・taps ≤ 8・mCap ≤ 32 | `Editor.exe --selftest` (`RtSelfTest.cpp` に `TestRestir`) |
| A5 | `--rt-restir` (再利用なし = **sub-04 時点**の状態) と off の絵が `--img-diff --tol 1` PASS。sub-05 以降は temporal が常に効くので再検証しない (チューニング UI で全クラスの M 上限を 1 にすれば同等の状態を作れる — `--rt-no-temporal` は SVGF 側で ReSTIR の temporal は止めない) | sub-04 で `Runtime.exe --render-demo --deferred --rt-refl [--rt-restir] --rt-no-temporal --rt-no-svgf --screenshot` 2 枚を比較 |
| A6 | temporal: `--rt-anim-seed --rt-restir` で debug 12 の M が伸びる (鏡面パッチ領域の平均 G が frame 3 → 40 で増加) / フリッカー指標 (frame 40 と 41 の同領域の平均絶対差、`--rt-no-temporal --rt-no-svgf` で SVGF を外して測る) が off より小さい / `rpos` の配線: 静止シーンでは `P_prev == P` で J = 1 ちょうどなので、配線が壊れていれば J が範囲外で temporal が棄却され **M が 1 から伸びない** — 「M が cap まで伸びる」が配線の検査を兼ねる (J ≠ 1 の経路はヘッドレスでは通らない。selftest A4 とユーザーの実機のみ) | sub-05 の一時 Python (scratch) で PNG を数値化。画像は reviewer 用に `tests\actual\` へ |
| A7 | spatial + class: `--rt-class-override 0` と `3` で絵が異なる (maxDiff > 0) かつ 3 (Prop) のフリッカー指標 ≤ 0 (Hero)。`--rt-debug 14` で反射像側にクラス色 | sub-06、A6 と同じ手順 |
| A8 | メニュー / Inspector / デバッグ表示の文字列が en/ja 両方にあり規則 10 を通る | `pwsh -File tools\check_rules.ps1` |
| A9 | 規則 9 に `kRtReflClassCount` / `kRtRestirMaxTaps` が登録され緑 | 同上 |
| A10 | sim 非接触 | `tools\replay_verify.bat` 全緑 (sub-07) |
| A11 | `[rt]` ログと ProfilerWindow に `restir` の GPU 時間が出る。S0 のベースライン (`--render-demo` / `--acoustic-demo` の `--rt-refl`、WARP) が sub-01 の実装メモに残る | ログ行の目視 (coder)。**GPU 時間の計測 run は `--frames 20`** で回す — `GpuTimer` は `kFrames = 6` のリングをスロット再利用時 (7 フレーム目以降) にしか回収しないので `--frames 6` では全項 0.000 ms になる (sub-01 で実測)。golden の撮影条件 (frames 6) は変えない。frames 6 と 20 のスクショはビット一致 (sub-01 で確認済み)。ログの出力先は標準出力 (`.log` は作られない) |
| A12 | ADR-016 / `engine_spec.md` §6.4 / README / CLAUDE.md (CLI 一覧・golden 枚数・SKIP 変数) 更新、golden `demo_render_rtrefl_restir` (`--rt-refl --rt-restir`) 追加 | sub-07、`shot_verify.bat` 22 枚緑 |
| A13 | チューニング UI が実行中に効き、Reset で既定に戻る。非永続 | reviewer が実機で操作 (`cmd /c bin\x64\Release\Editor.exe --render-demo --deferred --rt-refl --rt-restir`) |
| A14 | ReSTIR on の絵も run-to-run で決定的 (maxDiff=0) | sub-04 以降、各サブで `--rt-restir` の撮影を 2 回 |

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | RT 反射 / GI の golden 2 枚 (ローカル限定) + ベースライン計測 | なし | A1 (21 枚) / A2 / A11 (S0) | `M67a: RT 反射 / GI の golden を追加 — ReSTIR の後方互換をビット一致で固定する` |
| sub-02 | ReflectionClass の配管 (Material → RtInstance → HLSL) + デバッグ 13 | sub-01 | A1 / A3 / A8 / A9 | `M67b: ReflectionClass — Material から RtInstance へ、一次ヒットの色分け表示` |
| sub-03 | ReSTIR 数学 (HLSL ⇄ C++ ミラー + selftest) | sub-02 | A1 / A4 / A9 | `M67c: ReSTIR の数学 — reservoir / VNDF pdf / Jacobian の CPU ミラーと selftest` |
| sub-04 | reservoir の配管 (初期化 + resolve、再利用なし) + デバッグ 12 / 14 + `--rt-restir` | sub-02, sub-03 | A1 / A5 / A8 / A9 / A11 / A14 | `M67d: ReSTIR 反射 — reservoir の配管 (M=1 で現行と等価)` |
| sub-05 | temporal reuse | sub-04 | A1 / A6 / A14 | `M67e: ReSTIR 反射 — temporal reuse (クラス別 M 上限)` |
| sub-06 | spatial reuse (クラス駆動) + 可視レイ + `--rt-class-override` + チューニング UI | sub-05 | A1 / A7 / A8 / A13 / A14 | `M67f: ReSTIR 反射 — spatial reuse (ReflectionClass 駆動) とチューニング UI` |
| sub-07 | 仕上げ (ADR-016 / engine_spec / README / CLAUDE.md / ReSTIR golden / 全検証) | sub-06 | A1 (22 枚) / A10 / A12 | `M67g: 仕上げ — ADR-016、engine_spec §6.4、ReSTIR on の golden` |

## 7. 未決事項・リスク

**ユーザー確認済み (2026-09-04、司会が AskUserQuestion で確認。planner の裁定と結果)**

- U1 GI の golden も足すか / 裁定: 足す (`demo_render_rtgi`、S10) / **確認済み: 足す**。
- U2 実行中スライダのチューニング UI を入れるか / 裁定: 入れる (S7) / **確認済み: 入れる**。
- U3 デモの Material にクラスを割り当てるか / 裁定: `rdemo_spin`=Hero、`rdemo_pillar_*`=Prop、
  `adem_player`=Hero、`adem_agent_*`=Character / **確認済み: 割り当てる**。
- U4 temporal の Jacobian を J=1 近似にするか / 裁定: 近似 (S4) / **確認済み: 厳密に計算する (裁定と逆)** →
  reservoir に `rpos` (R32G32B32A32、+16 B/px) を追加、sub-04 (配管) と sub-05 (式) に反映。planner の反対意見
  (再投影妥当性を通った同一面点なら P_prev ≈ P で誤差は 5% 以内、テクスチャ 1 枚と帯域を節約できる) は
  §2 S4 に記録済み。以後蒸し返さない。
- U5 temporal を `rt_refl` に畳んで 2 パスにするか / 裁定: 2 パス (S3) / **確認済み: 2 パス**。
- U6 S5 を harness の外に置き、確定値は `M67h` で焼くか / 裁定: そうする (§4.6) / **確認済み: そうする**。

**リスク・実装中に判明する見込みのもの (coder が「不安・質問」で拾う)**

- ~~`--rt-gi` が WARP で run-to-run 決定的でない可能性~~ → **sub-01 で実測して否定** (3 回の独立撮影で
  maxDiff=0)。GI golden は残す。
- `GpuTimer` は 7 フレーム目から: 計測を主張するサブ (sub-04 / sub-06) は `--frames 20` の計測 run を撮影とは
  別に回す (A11)。GpuTimer 側は M67 で触らない。
- `rt_refl.cs.hlsl` に uniform 分岐と include を足しただけで fxc のスケジューリングが off 経路の丸めを変える
  可能性 (理論上)。golden が 1 でも動いたら**塗り潰さず**報告 (golden-diff-triage の 4 点計測)。
- W のオーバーフロー: `Ls` が半精度、W は fp32。極端に暗い `lum` の候補が大きな W を持つ → firefly。
  `J` 範囲棄却に加え `W ≤ kRtRestirWMax` (仮 64) でクランプするかは sub-05 の実測で決める。
- temporal の厳密 J (U4): カメラが大きく動いたフレームは J が範囲外になって temporal 候補が棄却されやすい
  (= 履歴が切れてノイズへ落ちる。安全側)。S5 で「動くと荒れる」が目立つなら `kRtRestirJacobianMax` を
  temporal だけ緩める (別定数) — 判断はユーザー。
- 可視レイ off のバイアス (光漏れ) は v1 の既知制限。ADR に明記、S5 で `--rt-restir-visray` と A/B。
- spatial ループの動的タップ数: `[loop]` + 上限定数。`[unroll]` に倒すと `gRsClass` 参照で fxc が展開に失敗しうる。
- reservoir スロットのリサイズ / viewKey 切替で `hasLast` を落とす経路は `RtHistory` と同型に。落とし忘れは
  「SceneView と GameView の reservoir が混線」として出る。
- `RenderDebug` の早期 return 連鎖 (4〜11 は Blit) に 12 / 14 を足し、13 は CS 経路へ落とす順序。
- 二重 temporal (ReSTIR + SVGF) の引きずりは S5 で実測 (元計画の未確定 2 点目)。クラス境界のアーティファクトも
  同じく (3 点目。§4.3 の「候補のクラスで半径を制限」が緩和策で、出るなら線形補間)。

## 8. 変更履歴

- 2026-09-04 (ユーザー、司会経由の U1〜U6 確認): U4 が裁定と逆 = temporal の Jacobian を厳密に計算する。
  §2 S4 / §3 / §4.2 (Jacobian の段落、保存の表に `rpos`、メモリ見積 29 → 35 MB) / §4.3 (u1-u5) / §5 A4・A6 /
  §7 U4 とリスク 1 件を更新。sub-04 (reservoir 5 枚、u1-u5、t11-t15) と sub-05 (J の式、velocity は t16) と
  sub-07 (ADR の決定項目) を修正。他 5 件は裁定どおりで「確認済み」の印のみ。
- 2026-09-05 (coder SELF_EVAL sub-01 round 1): (a) golden の置き場所は末尾 append (sub-01 の指示「froxel の直後」は
  番号ずれを招く planner の誤り、coder の指摘を採用)。(b) A11 に「計測 run は `--frames 20`」を追記 —
  `GpuTimer` のリング (kFrames=6) は 7 フレーム目からしか回収しない実装 (`GpuTimer.cpp:29-46`)。sub-04 / sub-06 の
  検証手順にも反映。(c) §7 の「`--rt-gi` の決定性」リスクを実測で否定に更新。(d) sub-07 に CLAUDE.md の CLI 一覧
  (`--rt-refl` / `--rt-gi` が元から未掲載)・環境の罠 (GpuTimer)・`engine_spec.md:1711` の古い枚数 (fifteen) と
  `:403` の文言更新を積む。
- 2026-09-05 (coder SELF_EVAL sub-02 round 1): (a) §2 に S15 = `Material` は cooked blob へ memcpy (planner の見落とし) →
  `kCookVersion` 1 → 2 / `static_assert` 64 / 明示パディングを承認。(b) §4.4 に「13 は `--rt-refl` 不要、12/14 は前提」と
  `RenderDebug` の早期 return の順序を追記 (sub-04 にも)。(c) A3 の検証手段を実態に合わせ、Inspector の往復は reviewer の
  実機操作に (private メンバでヘッドレス不可。静的ヘルパへの切り出しは M67 の外)。(d) sub-07 に engine_spec §10.2 の
  版記述 + CLAUDE.md チェックリスト「Material にフィールドを足す」の 3 項 (cook 版 / static_assert / 明示パディング) +
  `CookedCache.h:20` のコメント数値 (56 → 60 は 64 の誤り) の衛生を積む。
