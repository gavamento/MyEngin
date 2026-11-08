# M96 かき乱しフィールド — 設計の下書き (司会の事前調査、2026-11-08)

planner の出発点。裁定で変えてよい。ユーザー回答: 2D 層と 3D の両方 / ゲームロジックが読む (sim・hash・snapshot 対象) / 用途 = 草・水面の波紋・煙と粒子・跡 / 源 = プレイヤー移動・攻撃と爆発・敵と動く物体・風 / 格子は固定と追従の両方 (固定が先) / 物理は押さない。

### 現状（調査済みの事実）
- 霧: `FogComponent`（解析式、全体で一様＋高さ）と、M57 のフロクセル（`src\Engine\Renderer\Passes\FroxelPass.*`、`assets\shaders\froxel_inject.cs.hlsl:92-93` の 1 行で密度が決まる。既定 off、テンポラル feedback 0.9、太陽光は注入しない）。場所ごとに変わる密度を入れる口はない
- 型紙: 音響の残光（CPU 格子 → `AcousticVolumePass` で 3D テクスチャ → `acoustic_common.hlsli::AcousticSample`）。ただし音響のセル配列は導出値で、sim 状態ではない。大きなバイト列を sim 状態に持つ前例は M91 Compute State（`WorldHasher.cpp:438-466 HashComputeState`、ADR-030 決定 13: 8 MiB で Release 0.94ms/tick）
- 並列: ADR-028（出力次元だけを割る、jobs あり／なしのハッシュ一致で証明。`SimParallelSelfTest.cpp`）
- 草: 描画も揺れも存在しない。差し込み口は M79 サーフェスシェーダの VS（`MyEngineSurface.hlsli`。予約テクスチャ追加が必要）
- 水: `WaterWaveComponent` は Gerstner 4 本の解析式。波紋の口はない。`water_surface.hlsl` の VS には SRV がない
- 粒子: CPU 池はハッシュ対象（`CpuParticleBackend.cpp:437-503`）。GPU は t3 が空き（`particle_sim.cs.hlsl`）
- 跡: 実行時の地形変形・蓄積マスクはない。`terrain_common.hlsli:60 SampleTerrainSurface` にマスク（t29 以降）を足せる
- 風: `PhysicsEnvironmentComponent.windVelocity`（一様）。`SampleWind` は point を無視している
- 源: CC の速度は `PhysicsCharacters.cpp:500-513` で確定。アニメイベント `AnimEventFired` はジョイントのワールド位置を持つ（`AnimatorController.h:254-267`、配送 `TickRunner.cpp:509-545`）。爆発 API はない。足音の型は `AcousticField.cpp:867-895`
- 現在の番号: ABI v34 = 200、SimSnapshot v69、TypeId 95、ADR-035、check_rules 規則 17

### 方式
- **状態**: ECS 外の池 `DisturbanceFieldPool`（Scene か EngineLoop が所有）。3 点セット契約（状態・ハッシュ節・snapshot 節）を守り、`SimSourcesOf` に 1 本で入れる。内容ゲートにより、フィールドが無いシーンのハッシュは 1 ビットも動かない
- **ソルバ**: 安定流体（Stam）の簡約版。毎 Tick 次の順で回す: 源の注入 → 速度の半ラグランジュ移流 → 減衰 → 圧力投影（Jacobi・固定反復回数）→ 濃さ・跡の移流 → 静止状態への復元 → 環境風。すべて「前の配列を読んで自分のマスだけ書く」ダブルバッファにして、行（3D はスライス）単位で `jobs::ParallelRanges` に割る。float と `/fp:precise` で計算する
- **チャネル**: 流れ (vx, vz[, vy])、濃さ（霧・煙）、跡（溜まって、ゆっくり消える。回復時間 0 = 永続）、波紋（高さ h と hPrev。2D 層だけ、減衰つき波動方程式）
- **コンポーネント**（TypeId 96〜）
  - `DisturbanceFieldComponent`: 形（GroundLayer 2D / Volume 3D）、セル数（整数）、セルの大きさ、層の厚さ、粘性・復元・減衰、追従対象（0 = 固定）。entity.index 昇順で最大 4 個まで有効
  - `DisturbanceSourceComponent`: 半径・強さ・濃さを押しのける量・跡の深さ。速度は CC → Rigidbody → 位置の差分の順で自動に取る。プレイヤーも敵も同じ
- **攻撃・爆発**: `ClipEventKind` に `Disturb` を追加し、スイングの瞬間に刀身ジョイントの位置と速度から注入する（スクリプト不要）。ABI に `FieldAddImpulse(pos, dir, radius, strength, shape = 点/放射/円錐)` を追加する。C# からの書き込みは拒否する（既存の規則に合わせる）。破壊の `onBreak` からの放射注入は任意
- **読み取り**: ABI `FieldSample(pos, out density, out velocity, out trail)`。C# からも読める。Update が読むのは前 Tick の値（知覚と同じ規約）
- **Tick 位置**: フェーズ 4 の Fracture の後・パーティクルの前（`TickRunner.cpp:613-616`）。この Tick の CC・剛体の速度、確定した行列、アニメイベント、破断を読み、粒子には同じ Tick で効かせる
- **追従モード**: 格子の原点を整数セル単位でずらす（はみ出したマスは捨て、入ってきたマスは静止状態で初期化）。原点もセル値も sim 状態に入るので決定的
- **描画**（描画レーン、ハッシュ対象外）: Tick 後の配列を serial 付きで Texture2D / Texture3D に上げる（`AcousticVolumePass` 型。Renderer 層は POD だけを受け取る）
  - 霧: `froxel_inject.cs.hlsl` の sigmaT にフィールドの濃さを掛ける（2D 層は高さ方向のプロファイルを掛ける）。CB の `static_assert(2720)` と HLSL を同時に更新する
  - 草: `MyEngineSurface.hlsli` に予約テクスチャと `MyeFieldSample` を追加し、デモの草束（インスタンス化したエンティティ）を VS で倒す
  - 水: `water_surface.hlsl` の PS 法線（可能なら VS 変位も）に波紋を足す。浮力には効かせない（制限として明記）
  - 跡: forward_terrain / deferred_terrain の色と法線をマスクで変える（凹みは任意）。当たり判定は変えない
  - 粒子: CPU のスカラー経路と GPU の t3 で流れをサンプルする
- **物理は押さない**: `SampleWind` と空力の 3 か所は変えない

### サブ分割の案（1 サブ = 1 コミット）
| サブ | 内容 |
|---|---|
| a | 2D ソルバの核: コンポーネント、池、3 点セット、`FieldAddImpulse` / `FieldSample` の ABI（v35）、SelfTest、デバッグ描画 |
| b | 源: `DisturbanceSource`（CC / Rigidbody / 位置差分）、アニメイベントの `Disturb`、環境風、onBreak（任意） |
| c | 霧の描画: アップロード、フロクセルへの注入、デモシーン「霧の広場」、スクショ |
| d | 3D ボリューム: 3D ソルバとフロクセルへの注入 |
| e | 追従モード |
| f | 粒子（CPU / GPU） |
| g | 水面の波紋 |
| h | 跡（地形） |
| i | 草（サーフェスシェーダの予約テクスチャ + デモの草束） |
| j | 仕上げ: エディタ（カタログ、Profiler の数値）、ADR-036、engine_spec、feature-guide、test_checklists、replay_verify のジョブ `field`（`tools\mcp\src\verify.rs` の JOBS も） |

### リスクと検証で困る所
- フロクセルはテンポラル feedback 0.9 のため、速い一振りの霧が尾を引く。指数スライスなので、遠くの薄い層は粗くなる → c で実際の絵を撮って判断し、駄目なら地面の層専用の描画を追加する
- sim のコスト: 2D 128×128 × 6ch ≈ 384KB、3D 64×16×64 × 5ch ≈ 1.3MB。毎 Tick のハッシュとタイムトラベルのリングに効く → a と d で ms を計測し、ADR-030 の表と比べる
- 跡のマス 0.25m では足跡の形までは出ない（通った溝になる）
- snapshot を上げると焼き込み値の表明 2 件（`AcousticAudioSelfTest.cpp`、`TilemapSelfTest.cpp`）が落ちる（`2961ba3` の前例）

### 検証
- 新規 `DisturbanceFieldSelfTest.cpp`: フィールドが無ければハッシュ不変／jobs あり・なしでハッシュ一致／snapshot 復元後に memcmp 一致／注入が無ければ静止状態へ単調に戻る／境界でマスがはみ出さない／追従でずらしても決定的／C# からの書き込みは拒否
- `Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`（新ジョブ `field`、`jobsab`）
- 実際の絵: デモシーンを `Runtime.exe` でスクショし、歩く・振る・爆発のあとに霧が晴れて戻ることを撮る。最後の目視はユーザーが行う
