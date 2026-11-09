# ADR-036: かき乱しフィールド — 場の状態は sim が持ち、霧・草・水・粒子・跡はその読み手として描く

- 状態: **確定** (2026-11-09、M96a〜M96q)
- 出所: M96。計画と判断の経緯は `plans/m96-disturbance-field/spec.md` (§2 が疑った点と結論、§8 が変更履歴)。
- 実体:
  - sim: `src/Engine/Engine/Disturbance/` (`DisturbanceState` = Scene が持つ状態、`DisturbanceStep` = 1 tick の計算、`DisturbanceParams`、`DisturbanceRender` = 描画への詰め替え、`DisturbanceDebugDraw`)。呼び出しは `src/Engine/Engine/Loop/TickRunner.cpp` (Profiler のスコープ `disturbance`)。
  - コンポーネント: `DisturbanceFieldComponent` (TypeId 96)、`DisturbanceSourceComponent` (TypeId 97)。`ParticleEmitterComponent::fieldInfluence`、`TerrainComponent` の跡の見た目の欄 (末尾)。
  - 描画 (Renderer 層は POD だけ受け取る): `src/Engine/Renderer/Passes/DisturbanceFieldPass.{h,cpp}` (2D = Texture2DArray)、`DisturbanceVolumePass` (3D = Texture3D 最大 4 本)、`SkyboxPass` の背景だけの全画面パス (`forward_background_fog.hlsl`)。
  - シェーダ: `assets/shaders/ground_fog.hlsli` (地面の霧)、`disturbance_common.hlsli` / `disturbance_volume.hlsli` (場の読み取り)、`froxel_inject.cs.hlsl` (Volume の注入)、`particle_sim.cs.hlsl`、`water_surface.hlsl`、`forward_terrain.hlsl` / `deferred_terrain.hlsl` (跡)、`GrassField.surface.hlsl` / `WaterGerstner.surface.hlsl` (サーフェス)。
  - API: `src/Shared/EngineAPI.h` (ABI v35、`FieldAddImpulse` / `FieldSample`)、`src/Shared/ScriptAPI.h`、`src/Scripting/Interop.cs` (`FieldSample` だけ)。アニメイベント `"kind": "disturb"` (`AnimatorController.h`)。
  - デモ・検証: `--field-demo` (`DemoContentField.cpp`、`src/GameLogic/Scripts/ZZFieldDemoDriver.cpp`)、`--field-bench`、`replay_verify` のジョブ `field` と jobs A/B、`DisturbanceFieldSelfTest` (スイート `disturbance` / `disturbancelong`)、`tools/check_rules.ps1` 規則 18 と `tools/check_rules_selftest.ps1`。
- 番号: ABI **v34 → v35** (200 → **202** スロット)、SimSnapshot **v69 → v76** (v70 = DFLD 節 + 2 型、v71 = 流れの解法の欄、v72 = Volume、v73 = 追従、v74 = 粒子、v75 = 起伏に沿う層、v76 = 地形の跡)、TypeId **96** / **97**、check_rules **規則 18**。

## 背景

ゼルダの伝説 時のオカリナの「歩くと割れ、斬ると渦を巻いて散り、時間で戻る霧」を、プレイヤーの動き・攻撃・爆発・風で本当に動く場として入れる。
霧だけの機能にせず、同じ場を草の倒れ・水面の波紋・粒子の流れ・雪や砂の跡・ゲームロジックの読み取り (`FieldSample`) が共有する。

制約は 3 つ。

1. 場の状態は固定 60 Hz の sim 状態で、WorldHash・SimSnapshot・リプレイ・タイムトラベル・ロールバックに乗る。Debug / Release / WARP でビット一致する。
2. 場を置かないシーン (三校・HAL Collector・既存デモ) のハッシュ列を 1 ビットも動かさない。
3. 見た目は描画レーン (ハッシュ対象外) で、sim の結果を読むだけ。物理 (剛体・布・ロープ・空力・浮力) は場に押されない。

## 決定

### D1. 場は Scene が持ち、計算は状態を持たない関数群

`Scene::Disturbance()` が状態 (`DisturbanceState`) を持つ。却下: EngineLoop 所有 (TilemapSystem 型) = `SimSourcesOf` / `SimRefs` / `TickServices` / `HeadlessSim` / `SimInit` の 5 か所の配線とシーン切り替えの Reset が要り、1 か所の渡し忘れが静かに壊れる型 (M70c)。
FeelState 型なら `SimSourcesOf` が Scene から引くので配線が要らず、Play / Stop とシーン切り替えの Clear の 3 か所で済み、Server.exe (HeadlessSim) も自動で回る。

### D2. 内容ゲートと DFLD 節

スロットが 0 かつキューが空ならハッシュに何も畳まない。SimSnapshot の `'DFLD'` 節は常に書く (件数 0 でもレイアウト固定)。
列ごとの基準高さ・Volume の流れ y は、**非空のときだけ**畳む (場を置かない・2D だけのシーンのハッシュ列を動かさない)。

### D3. 浮動小数点: + − × ÷ sqrt floor min max だけ

sim の計算に CRT の超越関数 (`exp / pow / log / sin / cos / tan` 等) を使わない。減衰は UE Niagara Fluids の「Dissipation (大きいほど速く消える)」と逆向きの**残し率 (keep / tick)** で持つ (音響の `glowKeepPerTick` と同じ流儀)。
check_rules 規則 18 が `src/Engine/Engine/Disturbance/*.cpp` (selftest と描画への詰め替えを除く) を機械検査し、`tools/check_rules_selftest.ps1` が「違反を仕込むと実際に error になる」ことを `replay_verify` の rules ジョブで毎回確かめる。
例外: インパルスを組む境界 (ABI の `FieldAddImpulse` とアニメイベント) の角度 → `coneCos` の `std::cos` は許す (物理の `PhysicsCharacters.cpp` に前例がある。spec §2 #28)。規則 18 の検査範囲 (`Disturbance/*.cpp`) の外にある。

### D4. 計算方式 — 安定流体 (Stam) の簡約

源の注入 → 速度の半ラグランジュ移流 → 目標速度 (一様風 × `windScale`) への減衰 → 圧力投影 (Jacobi、**毎 tick 0 から・固定回数**) → 濃さの移流 → 静止状態への復元。波紋は別の減衰つき波動方程式。
圧力は作業領域で状態ではない (warm start を却下: ハッシュとスナップショットが 1 チャネル太る)。
投影は発散・勾配・Jacobi が一貫した作用素になるようにする。中心差分の発散と勾配に隣接 5 点の Jacobi を組むと 64 反復でも発散が 19% 残り、前進 / 後退差分の組は 20 反復で 22% 残ったので、幅 2 の 5 点 (3D は 7 点) を採った (R14)。**Radial のインパルスは発散源**として投影の右辺から引く (爆発の外向きの流れがその step の投影で消えない)。
公式資料: UE Niagara Fluids ([リファレンス](https://dev.epicgames.com/documentation/unreal-engine/niagara-fluids-reference-in-unreal-engine)、[概要](https://dev.epicgames.com/documentation/unreal-engine/fluid-simulation-in-unreal-engine---overview))。Niagara は GPU で回すが、ここは sim 状態なので CPU。

### D5. 並列は「出力次元だけを割る」(ADR-028)

移流・投影・復元・波紋は前の配列を読んで自分のマスだけを書くダブルバッファにし、2D は z 行、3D は (y, z) 行で `jobs::ParallelRanges` に割る。源とインパルスの注入は直列 (重なる源の加算順が結果に効く)。
jobs あり / なしのハッシュ一致は `SimParallelSelfTest` 相当と `replay_verify` の jobs A/B (`field`) が見る。

### D6. 源は付けた物だけ、インパルスのキューも sim 状態

- 自動の源は `DisturbanceSourceComponent` を付けた物だけ。却下: 全 CC・全動的剛体を暗黙に源にする = 暗黙の挙動 (AGENTS §3.2)、半径の推定が要る。速度の取得は自動 (CC → 非 kinematic Rigidbody → 位置の差分)。
- 明示のインパルス (C++ の `FieldAddImpulse`、アニメイベント `disturb`) は長さ 256 の配送待ちキューに積み、積んだ後の最初の場の step で必ず消費する。キューは sim 状態 (ハッシュ・DFLD に入る)。却下: LateUpdate から積んだものを捨てる = 呼んだ場所で効いたり効かなかったりする。
- アニメイベントの向きは「イベント定義の向き × 速さ」、位置は既存の `joint` 規則。却下: 刀身の前 tick 位置を状態に持つ = snapshot に欄が増え、ジョイントごとの表が要る。
- C# レーンは書き込めない (`FieldAddImpulse` は 0)。読み取り (`FieldSample`) は使える。

### D7. 格子の原点は整数セル

`originCell × cellSize`。固定は最初の step で丸めて 1 回だけ決め、追従 (`followTarget`) は 1 セル以上のずれ (`followMargin`) で整数セル単位にずらす (重なる部分を写し、入ってきたマスは静止状態)。同じ原点表現の上に乗るので、追従あり 600 tick も 2 ワールドで一致する。

### D8. 描画への受け渡し

Renderer 層は POD (`DisturbanceFieldUpload`) だけを受け取る (`AcousticVolumeUpload` と同じ)。2D = Texture2DArray `RGBA16F` (vx, vz, ρ, τ) + `RG16F` (R = 波紋 h、G = 列の基準高さ − baseY)、3D = 最大 4 本の Texture3D `RGBA16F`。通番が前回と同じなら転送しない。共有する定数は規則 9 (C++ ⇔ HLSL) が縛る。
編集中 (Play 前・状態が無い場) は静止状態の定数で描く (描画レーンは sim の状態を作らない)。

### D9. 地面の霧は専用描画 (板のレイマーチ)、フロクセルは Volume 専用

最初は 2D 層をフロクセルへ注入した (sub-02)。霧として見えたが、浅い角度で深度スライスの横縞が出た。ユーザー判断で、GroundLayer の霧は**各ピクセルで層の板をレイマーチする**専用描画 (`GroundFogComposite`、既定 24 段、段の始まりはピクセル位置からの固定の雑音) に替えた。合成は 10 か所のシェーダで「地面の霧 → 既存の解析フォグ・フロクセル」の順。層が無い / 交わらないピクセルは何もしない。
却下した案: フロクセルのスライス数を増やす (全シーンのコストと約 7 MB が増え、縞は薄くなるだけ)。霧を半透明の重ね板で描く (時のオカリナの作り方。濃さの場を板の枚数でしか表せず、カメラが層に入ると破綻する)。全画面の後処理 1 回 (後から描く透明物の水・粒子が霧の手前に出る)。
副作用: GroundLayer の霧はフロクセル無しで見える。霧の後ろに透明物が重なると透明物のピクセルでも積分する (R12、コストは面の数に比例)。

### D10. Forward のスカイ無しの背景にも霧を掛ける

Forward には背景を描くシェーダが無く clear 色のまま残るため、Deferred (光パスの背景分岐で地面の霧とフロクセルを掛ける) と絵が食い違った。ユーザー指示で取り消し (旧 R13)、Forward の不透明の後・透明の前に**背景だけを塗る全画面の三角形**を 1 回描く。深度は far、深度テストは LESS_EQUAL・書き込みなし = clear のままの画素にだけ乗る。色は clear 色に、Deferred の背景分岐と同じ処理を掛けたもの。
描く条件は「Forward のビュー かつ スカイを描いていない かつ (霧のある GroundLayer が 1 枚以上 または そのビューでフロクセルが走る)」。条件が欠ければパスそのものが走らず、絵はビット不変。解析フォグ (`FogComponent`) の背景の扱いは変えていない。
**変わる絵**: Forward かつスカイ無しかつフロクセルが走るシーンの背景の画素だけ。golden は `fog` (`--fog-demo --froxel --particle-backend gpu`、Forward) の 1 枚だけが対象で、撮り直した (差は最大 11、背景の全画素に縦の帯)。Deferred の全部、スカイのある Forward、霧もフロクセルも無い Forward は差 0 (パスを一時的に止めたビルドが着手前とビット一致 = 変化はこのパスだけが原因)。

### D11. 太陽光は場ごとの bool、既定 false

自己照明 (`fogColor × fogBrightness`、等方・影なし) は常に持つ。加えて場ごとの `fogSunLight` が true の場の媒質にだけ、平行光の散乱 `σ_s × 放射輝度 × HG(g)` を足す。
既定 false の理由: 既存のフロクセルの設計 (平行光を注入しない) と同じ絵が既定になり、時のオカリナ風の無陰影が既定になる。場ごとにした理由: 屋内の煙と屋外の霧が同じシーンに並ぶ。解析フォグの太陽寄せはグローバルの媒質にだけ掛かり、場の媒質は別の媒質なので二重計上にならない。
**影なし** (木陰でも照る)。カスケード影をフロクセルの注入へ束縛するのは別の仕事として後回し。

### D12. 地面の霧の局所ライトは ON / OFF、最大 8 本、影なし

場ごとの `fogLocalLights` (既定 false、GroundLayer だけ)。CPU が毎フレーム「影響球が ON の層の板の AABB と交わる局所ライトを、ライト一覧の順に最大 8 本」選び、縮約した写し (528 B の CB) を渡す。各シェーダの既存のライト一覧には依存しない (ライト一覧を持たない合成箇所がある)。超えた分は落として WARN 1 回。減衰とスポットは `common.hlsli::LightSample` と同じ式 (`ground_fog.hlsli::GfogLocalLightRadiance` に二重管理している。common.hlsli は全シェーダが include するので共有ヘッダ化は見送った)。
却下: 本数無制限 (段数 × 本数のコストが読めない)、影つき (10 か所にシャドウアトラスを束縛するコストと複雑さに見合わない。影つきのビームは Volume で作る)。

### D13. Volume のためのフロクセルの自動有効 (場だけのモード)

フロクセルが off のビューでも、`autoFroxel` (既定 true) かつ霧を出す有効な Volume があれば**場だけのモード**で回す: グローバルの媒質 0、解析フォグは全区間 (受け渡ししない)、ゴッドレイは降ろさない → 場の外は従来の off と同じ値。カメラが `froxelOn` のとき (フル) は従来どおりに場の媒質を足すだけ。モードの出入りで INFO ログ 1 行。
却下: カメラの `froxelOn` を自動で立てる (グローバルの媒質・解析フォグの縮み・ゴッドレイの降下が黙って起きる)。シーン全体の設定にする (場を置いた所で止められない)。

### D14. 3D は 4 個まで全部描く

有効な場は entity index 昇順で先頭 4 個 (sim も描画も同じ数)。3D テクスチャは配列にできないので、Volume は最大 4 本の Texture3D (SRV 4 本、寸法は各自)、フロクセルの注入は 4 個を順に足す (格子 AABB の外では読まない)。

### D15. 起伏に沿う 2D 層は列ごとの基準高さを状態に持つ

`followTerrain` の場は列 (x, z) ごとの `columnBaseY` を、作り直しの step で全列、追従のずらしでは入ってきた列だけ `SampleTerrainHeightWorld` で焼く。**導出値だが状態に入れる**: 地形のコライダーが実行時に動いても「焼いた時点の値」で決定的になり、復元の後に焼き直す順序の誤り (M65a の音響の距離場と同じ型) を作らない。移流・投影は平らな格子のまま (斜面に沿う流れはしない)。
却下: 毎 tick 地形を引く (128² 本のレイ)、描画だけで起伏に合わせる (sim の注入の板・`FieldSample` と絵がずれる)。

### D16. 粒子は CPU が sim の状態を直接引き、GPU は 1 フレーム遅れのテクスチャを読む

`fieldInfluence × (場の流れ − 粒子の速度)` を加速度に足す。y 成分は Volume の中だけ寄せる (2D 層の流れの y は常に 0 で、寄せると上昇する煙の vy を 0 へ潰す)。ローカル空間のエミッタは場を読まない。場が重なったときの選び方は CPU (entity index 順) と GPU (2D 層 → Volume の順) で違ってよい。

### D17. 跡の見た目の欄は TerrainComponent の末尾

跡の色・強さは「その地面が雪か砂か」で決まるので、場ではなく地面の持ち物。1 つの場が雪と砂の両方の地形にまたがっても、それぞれの色で描ける。跡は移流しない (復元だけ。濃さだけが流れに乗る)。

### D18. 霧のノイズは描画だけで掛け、設定は DisturbanceFieldComponent の末尾に置く (M96 の後の追補)

霧の濃さ σ に `1 + 強さ × (2n − 1)` を掛けてムラを付ける (n = ノイズ [0,1]。平均の濃さは変えない)。ノイズは 2 種類で、重ねて使える:
**数値のノイズ** (3D の値ノイズの fBm。大きさ・重ね数・流れる速度・形の変わる速さ) と **マップのノイズ** (テクスチャの R をワールドの xz に真上から繰り返して敷く。大きさ・流れる速度)。
2D 層 (`DfldLayerSigma`、地面の霧) と Volume (`DfldVolumeAccumulate`、フロクセル注入) の両方に同じ式で効く。式は `disturbance_noise.hlsli` に 1 本だけ置く。

- **sim と `FieldSample` は読まない** (ユーザー判断)。ゲームの判定は今までどおり決定的で、マップを CPU で読む仕組みも要らない。霧の「見た目の濃さ」と「判定の濃さ」はノイズの分だけずれる。
- **設定はコンポーネントの末尾** (ユーザー判断)。DisturbanceFieldComponent は 104 → 160 B (hash 対象なので World のカラムの生バイトが変わり、SimSnapshot v77、`--field-bench` の焼き込み値も採り直した)。sim の式は変えていない。
- **時計は sim の tick** (水面と同じ。`(simTick − 1 + 補間) / 60`)。描画フレーム数では進めないので、60 FPS 以外でも同じ速さで流れ、決定的撮影で同じ絵になる。編集中 (tick 0) は模様が止まる。
- **流れ × 時刻は CPU が double で計算し、周期で折り返してから CB に入れる** (数値のノイズは格子 256 周期、マップは UV 1 周期)。長く回しても float の桁が落ちない。シェーダは時刻を読まない。
- **強さ 0 は CB も 0 で、シェーダは読まない** = 既定の絵は 1 画素も動かない (`fog.png` などの golden はそのまま)。

却下: 描画専用の別コンポーネント (TypeId 98、NoHash) — snapshot と焼き込み値に触れずに済むが、ユーザーが「場に欄を足す」を選んだ。
`FieldSample` にも数値のノイズを反映する — C++ 側に同じ式が要り、規則 18 (四則と sqrt など) の範囲で fBm を書く必要がある。マップは CPU で読めないので 2 種類の扱いが割れる。
ノイズを sim の濃さそのものに焼く — 霧が流れに乗って模様が崩れ、決定論の対象 (ハッシュ) が増える。

### D19. 霧の嵩は保存して運ぶ別チャネルにし、戻すのは重力だけにする (M96 の後の追補)

「蹴ると穴が開き、押し出した分が縁に盛り上がり、重力でゆっくり埋まる」をシミュレーションで出すために、2D 層に**嵩チャネル** (`channels` のビット 8、嵩 m、静止状態 1、[0, 4]) を足した。列の層の厚さは `thickness × m`。

- **濃さでは出せない**: 濃さは [0,1] で頭打ち (静止 1)、移流は非保存の半ラグランジュ、戻りは `densityKeep` の一様な指数回復。押しのけた分が消えるので縁が盛り上がらず、穴は周りから流れ込むのではなく一様に湧いて戻る。濃さの式を変えると既存の場面のハッシュと絵が全部動くので、別チャネルにした。
- **保存形の移流 (donor-cell)**: 投影の後の流れで、面の速度 (両側の流れの平均) × 風上の嵩を運ぶ。同じ面の量は両側のマスが同じ式から求めるのでビット一致し、総量は面で打ち消し合う。出るクーラン数の和が 1 を超えるマスは比で縮めて嵩を負にしない。投影は Radial インパルスの発散を残すので、蹴り (Radial を数 tick) の中の嵩が外へ運ばれ、流れが弱まる半径の外で詰まって縁になる。
- **重力の広がり**: 重い霧の薄膜 (多孔質媒体型の拡散)。面の量 = `massGravity × 平均の嵩 × 嵩の差`。薄い所ほど流れにくく、穴の縁から内へゆっくり埋まる。`massGravity × 嵩の上限 <= 0.25` (`kMaxMassGravity` = 0.0625) で単調 = 負にならない。
- **戻す項を持たない**: 重力 0 なら穴は開いたまま。格子の外は嵩 1 の開いた境界 (流れが縁まで届けば出入りする)。
- **欄は旧 `pad3` の 4 バイト** (`massGravity`、既定 0.03)。コンポーネントの大きさは 160 B のまま。DFLD 節のスロットに `mass` 配列 (嵩チャネルの場だけ)、SimSnapshot v78。ハッシュは嵩のある場だけ配列を畳む (他の場面のハッシュ列は増えない)。Volume では嵩のビットを落とす。
- **描画**: aux テクスチャを RG16F → RGBA16F にして B に嵩 (嵩の無い層は 1)。`DfldLayerSigma` と C++ の `DisturbanceSampleAt` の p(y) は列の厚さで測る (下限 `kMinColumnThickness`)。板の上端 (`texScale.w`) は `ずれ + thickness × (嵩 − 1)` の最大。草・水・粒子・跡が使う層の帯 (`extent.z`) は嵩で変えない。
- **ABI は変えない**: 蹴りは既存の `MyeFieldRadial` を数 tick 続けて呼ぶ (1 tick で嵩が動けるのは 1 マスまで)。デモは `--fog-kick-demo`。

- **コライダーが嵩を押しのける** (追補 2): 層に入り込んだコライダー (sphere / box / capsule / convex のトリガー以外と、CharacterController のカプセル) の中のマスを固体とし、その嵩を形の中心から外向きへ 1 マス/tick で押し出す (保存形の donor-cell)。固体に接する面は空気の流れと重力の流束を閉じるので、霧は固体へ入らず、重力も固体の中へは埋め戻さない。固体の判定は列の 3 点 (厚さの 0.25 / 0.5 / 0.75) を物理の `shapes::DistanceToShape` で調べる (床の箱やメッシュ・地形は押しのけない)。固体の目印は毎 step コライダーから作り直す作業領域で、状態ではない。場ごとに `ignoreColliders` (旧 pad2 の 1 バイト、既定 false) で切れる。嵩の場だけが対象なので、他の場面の挙動とハッシュは変わらない。押しのけ方は場の既定値 (`pushMargin` = 形からさらに外側の余白 [m]、`pushStrength` = 押しのける割合。固体のマスに 1 − 強さを残す) と、物ごとの上書き `DisturbancePusherComponent` (TypeId 98、margin / strength) で変えられる (ユーザー判断: 場 + 物ごとの上書き)。跡が残る時間は場の `massGravity` (埋まる速さ) で決まる。SimSnapshot v79。歩くと溝ができる理由: 源 (DisturbanceSource) の流れは投影で発散を消されるので、嵩を押しのけるには形そのものを固体として扱う必要がある。

却下: 浅水方程式 (霧の層自身の速度を持つ) — 空気の流れ (投影する非圧縮の流れ) と別の速度場が要り、状態が倍になる。蹴りで嵩を注入側で直接書き換える (穴を掘って縁へ足す) — 押しのけ方が流れの結果にならず、ボールなど他の源と一貫しない。

## 計測

2026-11-09、RTX 3060・Release。
`--field-bench` は 10 tick の窓を 20 個以上測った**最良の窓** (並列の coder が同じ機械でビルドしていたため。平均と最悪は別に書く)。

| 項目 | 値 | 出所 |
|---|---|---|
| 2D 128×128・全チャネル・20 反復、sim + ハッシュ、jobs なし | 最良の窓 1.17 ms (平均 1.30、最悪 1.91 = 並列ビルドの重なり) | sub-04 round 2 |
| 同、jobs あり | 約 0.9〜1.0 ms | sub-04 |
| 同、反復 0 | step 約 1.25 ms (移流・復元・波紋が残る) | sub-04 |
| ハッシュ (`HashStateBytes`) | 0.43 ms → 0.032 ms (ADR-030 決定 13 と同じ方式へ) | sub-04 round 2 |
| Volume 32×16×32・20 反復、jobs なし | 最良の窓 1.99〜2.04 ms (平均 2.06〜2.10) | sub-07 |
| 同、jobs あり | 0.59 ms | sub-07 |
| スナップショット 1 枚 | 2D 128×128 全チャネル 408,645 B、Volume 277,277 B、`followTerrain` の場は列の基準高さ (cellsX × cellsZ の float) が増える | sub-04 / sub-07 / sub-10 |
| 列の基準高さの焼き込み 128² 列 (作り直しの step で 1 回だけ。追従では入る列だけ) | 6〜10 ms | sub-10 |
| 地面の霧、フレーム全体の GPU 時間の差 (Deferred 1920×1080、層 4 枚が全部カメラ線分と交わる最悪形) | 層 0 = 1.00 ms、層 1 = 1.57 ms、層 4 = 3.21 ms | sub-03 |
| 同、Forward 1920×1080 | 層 0 = 0.56 ms、層 1 = 1.18 ms、層 4 = 2.74 ms | sub-03 |
| Forward の背景パス (1920×1080、パス本体の Draw だけ) | 地面の霧だけ 0.050 ms、地平線カメラ (背景が画面の約 57%) 0.155 ms、フロクセルだけ 0.027 ms | sub-16 |
| 地面の霧の局所ライト (`fogLocalLights`) off → on の 1080p のフレーム全体の GPU 時間の差 (`--render-stats-dump` の `gpuMs.frame`、同じシーンで on / off を交互に 9 回ずつ、最小値の差。本数 = 層の中の点光源の数) | Forward: 1 本 +0.15 ms、4 本 +0.37 ms、8 本 +0.58 ms。Deferred: 1 本 ±0 (雑音)、4 本 +0.21 ms、8 本 +0.30 ms。0 本 (off) は従来と同じ経路でコスト増なし。ほぼ本数に比例し、既定の上限 8 本のままで 1 ms 未満。雑音は ±0.1 ms 程度 (GPU のクロックで最悪値が 2 倍になる回があり、中央値は信用できない) | sub-17 (機械が空いた状態の再測定) |
| フロクセルの注入 (160×90×64)、Volume 0 / 1 / 4 個 (GpuTimer、各 2 回の測定) | 0 個 0.092 / 0.093 ms、1 個 0.108 / 0.277 ms (後者は外れ値)、4 個 0.118 / 0.118 ms。注入パスは Volume の個数にほぼ依らず約 0.1 ms (4 個で +0.03 ms 以内) | sub-07 (`--froxel` のダンプ) |

- 性能の線 (spec §5 の 15 / 20): 1 tick 予算の 10% (1.67 ms、ADR-030 と同じ線)。2D は jobs なしで合格。**Volume は jobs なしで線を超える** (速度が 3 成分、Jacobi が 7 点なので、同じマス数でも 2D の約 1.8 倍が本質的に掛かる) ので、実行時の既定である jobs ありで 1.67 ms 以下を合格の線にした。既定の寸法・反復回数は変えていない。却下: Volume の既定の反復を 10 に下げる (渦が弱くなり、jobs なしでも約 1.6 ms で線ぎりぎり)、既定の寸法を 32×8×32 に下げる (煙の柱の高さ方向の解像度が半分)、三線形の SIMD 化 (決定論の確認が要る別の仕事)。
- 地面の霧の段数 24 は既定のまま (4 層同時が最悪形で、通常の 1 層は 1080p で +0.1〜0.6 ms)。
- タイムトラベルで遡れる秒数 (推定。`TimeTravelConfig` は 30 tick ごとに 1 枚・120 枚・64 MiB): 1 枚に場だけで 2D 1 個 約 0.4 MB、2D 4 個 約 1.6 MB、Volume 4 個 約 1.1 MB が加わる。64 MiB に収まる枚数は場が 2D 1 個なら 120 枚の上限 (60 秒) が先に効き、2D 4 個では約 40 枚 (約 20 秒)、Volume 4 個では約 58 枚 (約 29 秒) に減る。予算は変えていない (spec §7 R3)。ワールドのほかの状態が 1 枚に足されるので、実際はこれより少し短い。

## 採らなかった案 (全体)

- **GPU で sim する**: sim 状態は CPU に置く (決定論・ハッシュ・スナップショットのため)。GPU は読むだけ。
- **物理を押す**: 剛体・布・ロープ・空力・浮力・当たり判定は場に押されない。`SampleWind` と空力 3 か所は M96 で変えていない (`git diff 2961ba3` で該当関数に差分が無いことを確認)。
- **圧力の warm start**: 状態が 1 チャネル太る。
- **場をアセットに焼く**: 状態は実行時だけ。シーンにはコンポーネントの設定だけが保存される。

## 制限

- 跡のマス (既定 0.25 m) では足跡の形までは出ず、通った筋になる (R5)。
- 場の寸法 128² × 4 個や Volume 4 個を置くとタイムトラベルで遡れる秒数が 60 秒より短くなる (上記の推定)。
- アニメイベントの位置は前 tick の WorldMatrix から作るので、斬撃の位置が 1 tick 分後ろにずれる。既存のイベント (sound / effect / noise) と同じ制限 (R7)。
- 列の基準高さは焼いた時点の地形で決まり、地形を実行時に動かしても追わない (R8)。
- 太陽光は影なし (木陰でも照る)。局所ライトは最大 8 本・影なし。
- 投影の Jacobi は幅 2 の 5 点なので、格子が 4 つの部分格子に分かれ、市松模様の圧力の成分が減衰しない (R14)。絵にムラが出たらスタガード格子を検討する。
- 物理を押さない (場の流れは剛体や浮力に効かない)。
- `FieldSample` は呼んだ時点の状態を返す。Update (フェーズ 3) からは前 tick の結果、LateUpdate からはこの tick の結果。
- ヒットストップ中は霧も止まる (場の step は `stepSim` のゲートの中)。キューは止まっている間も積め、再開後の最初の step で消費する。
- C# レーンは場へ書き込めない。
- 霧のノイズは見た目だけ。`FieldSample` の濃さにはノイズが入らない (D18)。マップは mip 0 を読む (遠くの細かい模様はちらつきうる)。編集中は時計が 0 なので模様が流れない。
- 突風・霧が視界を遮る (知覚) は後回し。
- `water_surface.hlsl` の `ApplyFog` の引数の並びが M77b (746775c) 以来宣言と違っていた (fogColor に視距離が入る) 潜在バグを、M96 の池のデモで露見したので直した (W4 統合後の追補)。Fog コンポーネントを持つ水面のシーンの見た目が「正しい霧」へ変わる。
