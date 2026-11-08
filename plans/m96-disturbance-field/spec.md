# M96 かき乱しフィールド (動く霧と、その汎用化) — 仕様書

- slug: m96-disturbance-field
- 状態: 確定 (2026-11-08。§7 の `[ユーザーに聞ける]` 1〜7 は司会が聞いた。推奨と違う回答 3 / 4 / 5 / 6 を §2 #12 / #14 / #15 / #16 と §3〜§6 に反映し、サブを 12 → 14 本にした。経緯は §8。ユーザー判断の確定事項は台帳「ユーザー判断」節)
- 依頼原文:
  このエンジンに
  ゼルダの伝説時のオカリナにある動く霧を実装したい
  プレイヤーの動きや攻撃などで実際に動く霧
  霧意外にも使えるようにしたい

  わからなかったら質問して。
  あっているかの確認したいからどんなふうの物だと思っているか先に言って

## 1. 目的 (なぜ作るか)

ゲームの作り手が、**プレイヤーの歩き・攻撃・爆発・敵の動き・風で実際に動く場**をシーンに置けるようにする。
1 つの場が「流れ (速度)」「濃さ」「跡」「波紋」を持ち、それを次の 5 つの見た目と、ゲームロジックの読み取りが共有する:

- 霧 (地面を這う薄い層 = 時のオカリナの迷いの森の霧、と立体の煙) — 歩くと割れ、斬ると渦を巻いて散り、時間で戻る
- 草の揺れ (通ると倒れて戻る) / 水面の波紋 / 粒子 (煙・砂ぼこりが流れに乗る) / 跡 (雪・砂に通った筋が残る)
- ゲームロジック: 「ここは霧が濃いか」「足跡が残っているか」をスクリプトが読める (C++ も C# も)

達成したい状態:

1. 場の状態は**固定 60 Hz の sim 状態**で、WorldHash・SimSnapshot・リプレイ・タイムトラベル・ロールバックに乗る。Debug / Release / WARP でビット一致する
2. 場を置かないシーン (三校・HAL Collector・既存デモ) は**ハッシュ列が 1 ビットも動かない**
3. 見た目は描画レーン (ハッシュ対象外) で、sim の結果を読むだけ。物理 (剛体・布・ロープ・空力) は場に押されない

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | 二重実装ではないか (既存の霧・風・水・粒子で足りないか) | `FogComponent` は解析式 (`Components.h:970-982`)。フロクセルは一様密度 × 高さの指数だけ (`froxel_inject.cs.hlsl:92-93`)。風は一様 (`PhysicsEnvironmentComponent.windVelocity`、`Components.h:1300-1302`)。水は Gerstner 4 本の解析式。場所ごとに変わり、かき乱されて戻る状態はどこにも無い | — | 新規。既存の霧 (フロクセル) は**描画先として再利用**し、密度を足す口を足す (二重の霧描画は作らない) |
| 2 | 状態の持ち主は EngineLoop (TilemapSystem 型) か Scene (FeelState 型) か (design-draft は両案併記) | TilemapSystem は EngineLoop が持つので、SimSourcesOf / SimRefs / TickServices / HeadlessSim / SimInit の 5 か所の配線とシーン切り替えの Reset (`TickRunner.cpp:1003-1004`) が要った (M94k `1eaee36` で 26 ファイル)。FeelState は Scene が持ち (`Scene.h:98-102, 258-259`)、SimSourcesOf が scene から引くので配線が要らず、Play/Stop とシーン切り替えの Clear が 3 か所 (`PlayModeController.cpp:26,40`、`TickRunner.cpp:992`) で済む。場はシーンのコンポーネントから作られるので寿命もシーンと同じ。Server.exe (HeadlessSim) も Scene 経由で自動的に回る | planner 裁定 | **Scene が持つ** (`Scene::Disturbance()`)。計算は状態を持たない関数群 (`src\Engine\Engine\Disturbance\`)。却下: EngineLoop 所有 = 配線 5 か所 + Reset 漏れの危険 (M70c の「1 か所だけ渡し忘れ」と同じ型) |
| 3 | 計算方式 | Unity / UE の同種機能 = UE Niagara Fluids (Grid 2D / 3D Gas)。セルが密度・速度を持ち、移流・圧力解 (**Pressure Solve Iterations** = 反復回数で精度と速さを交換)・**Density / Velocity Dissipation** (大きいほど速く消える) で回す ([Niagara Fluids リファレンス](https://dev.epicgames.com/documentation/unreal-engine/niagara-fluids-reference-in-unreal-engine)、[流体シミュレーションの概要](https://dev.epicgames.com/documentation/unreal-engine/fluid-simulation-in-unreal-engine---overview))。Niagara は GPU で回すがここは sim 状態なので CPU | — | 安定流体 (Stam) の簡約: 源の注入 → 速度の半ラグランジュ移流 → 減衰 → 圧力投影 (Jacobi・**固定回数**) → 濃さの移流 → 静止状態への復元。波紋は別の減衰つき波動方程式。減衰は UE と逆向きの「残し率 (keep / tick)」で持つ (音響の `glowKeepPerTick` と同じ流儀。§4.4 の決定論のため指数関数を使わない) |
| 4 | 圧力の初期値を前 tick から引き継ぐ (warm start) か | 引き継ぐと圧力も sim 状態になり、ハッシュとスナップショットが 1 チャネル太る | planner 裁定 | **毎 tick 0 から**。圧力は作業領域で状態ではない。反復回数は固定 (既定 20) なので結果は決定的 |
| 5 | 「敵・動く物体を自動で」= 全ての動く物体を暗黙に源にするか、源のコンポーネントを付けた物か | AGENTS §3.2「暗黙的な登録で挙動を予測しにくくしない」。UE Niagara Fluids も **Collide Against** で相手を選ぶ (上記リファレンス)。コンポーネントを付ければスクリプト無しで速度を自動で拾う (CC → Rigidbody → 位置の差分) ので「呼び出し不要」は満たす | planner 裁定 `[ユーザーに聞ける]` | **`DisturbanceSourceComponent` を付けた物だけ**。速度の取得は自動。敵や破片はプレハブに付ける。却下: 場に「全 CC・全動的剛体を自動で源にする」旗 = 暗黙の挙動、半径の推定 (形の外接球) が要る |
| 6 | 攻撃 (スイング) の速度をどこから取るか (design-draft は「刀身ジョイントの位置と速度」) | `AnimEventFired` は位置 (`pos[3]`) しか持たず、しかも前 tick の WorldMatrix から作る (`AnimatorController.h:254-267`)。速度を出すには刀身の前 tick 位置を sim 状態に持つ必要がある | planner 裁定 `[ユーザーに聞ける]` | **イベント定義の向き** (コントローラのエンティティの空間) × 速さで注入する。振りの軌跡はイベントを数個置いて描く。却下: 刀身の前 tick 位置を状態に持つ = snapshot に欄、ジョイントごとの表が要る |
| 7 | 1 つのキューに積んだインパルスの寿命 | スクリプトの Update (フェーズ 3)・アニメイベント (3.5)・onBreak (4、場の前) から積んだものは同じ tick の場の step で消費できるが、LateUpdate (5) から積んだものは次の tick まで残る。BT のイベント待ち (`SimSnapshot.h` v37) は配送待ちを状態に入れた前例 | planner 裁定 | **配送待ちのキューは sim 状態** (ハッシュ・DFLD 節に入る)。規則は「積んだ後の最初の場の step で必ず消費する」の 1 本。却下: LateUpdate からの注入を捨てる = 呼んだ場所で効いたり効かなかったりする |
| 8 | 浮動小数点の決定論 (Debug / Release / WARP) | `/fp:precise` 全構成 (AGENTS §4.3)。CRT の超越関数は CRT 依存として避けた前例 (`engine_spec.md:1660` の capsule の定数表) | — | sim の計算は `+ − × ÷ sqrt floor min max` だけ (§4.4)。`std::exp / pow / sin / cos` 等を Disturbance の sim ファイルで使わないことを check_rules 規則 18 で機械検査する |
| 9 | 並列化で結果が変わらないか | ADR-028「出力次元だけを割る」と jobs あり / なしのハッシュ一致による証明 (`SimParallelSelfTest.cpp`、replay_verify `:job_jobsab`) | — | 移流・投影・復元・波紋は「前の配列を読んで自分のマスだけ書く」ダブルバッファにし、z 行 (3D は y スライス) で `jobs::ParallelRanges` に割る。源とインパルスの注入は**直列** (重なる源は加算順が結果に効く) |
| 10 | 状態のサイズと、ハッシュ・タイムトラベルのコスト | M91 の実測: ハッシュは約 9 GB/s、State 8 MiB で Release 0.94 ms/tick (ADR-030 決定 13)。タイムトラベルの予算は 64 MiB / 120 枚 (`TimeTravel.h:96-98`) | planner 裁定 | 有効にしたチャネルの配列だけを持つ。既定 2D = 128×128 (0.25 m = 32 m 四方)、流れ 2 + 濃さ + 跡 = 4 float × 16384 = 256 KiB。3D 既定 = 32×16×32 (0.5 m)、4 float × 16384 = 256 KiB。1 フィールドの上限 262,144 マス、有効なフィールドは 4 個まで。性能は §5 の 15 / 20 で計測し、1 tick 予算の 10% (1.67 ms、ADR-030 と同じ線) を超えたら既定値を下げる |
| 11 | 場の位置 — 固定と追従 (追従は後) | 台帳: 両方、固定が先 | 確定済み | 格子の原点は**最初から整数セル** (`originCell × cellSize`)。固定は場のエンティティの位置をセルに丸めて 1 回だけ決める (最大半セルずれる)。追従 (sub-07) は追う相手が 1 セル以上離れたら整数セル単位でずらす — 同じ原点表現の上に乗る |
| 12 | 2D 層と地形の起伏 | 2D 層の基準の高さは場のエンティティの y の 1 値。丘の上では霧が地面に潜る / 浮く。当たる地形の高さは `SampleTerrainHeightWorld` (`PhysicsSystem.h:218-225`、地形コライダーへのレイ、重なりは最も高い面・同値は entity index 小 = 決定的) で sim から引ける | **今回入れる** (2026-11-08。planner の裁定「後回し」の逆) | GroundLayer に `followTerrain` (既定 false)。列ごとの基準高さ `columnBaseY` を作り直しの step で焼き (追従のずらしでは入ってきた列だけ)、**状態に入れる** (ハッシュ・DFLD)。導出値だが、地形のコライダーが実行時に動いても「焼いた時点の値」で決定的になり、復元の後に焼き直す順序の誤り (M65a の音響の距離場と同じ型) を作らないため。移流・投影は平らな格子のまま (斜面に沿う流れはしない)。sub-09 を新設。却下: 毎 tick 地形を引く = 128² 本のレイが毎 tick / 描画だけで起伏に合わせる = sim (注入の板・`FieldSample`) と絵がずれる |
| 13 | 霧の描画先はフロクセルで足りるか (薄い層・速い一振りの尾) | フロクセルは指数スライス 64 枚 / 最大 64 m、テンポラル feedback 0.9 (`FroxelPass.h:20-35`)。sub-02 の実測 (`2687227`): 霧として見え、尾は理論 22 フレーム。ただし浅い角度で見ると深度スライスの横縞が出る (sub-02 のスクショ 02 / 03)。縦は距離 10 m で約 0.13 m / セル | **地面の霧専用の描画を足す** (2026-11-08。planner の裁定「横縞は許容」の逆) | **GroundLayer の霧はフロクセルから外し、各ピクセルで層の板をレイマーチする** (`GroundFogComposite`、sub-03)。カメラ → 面の線分と板 (格子の矩形 × 基準高さの範囲) の交差区間を解析的に求め、固定の段数 (既定 24) で積分する。段の始まりはピクセル位置からの固定の雑音 (フレーム番号に依らない)。合成はフロクセルと同じ 10 か所で「地面の霧 → 既存の解析フォグ・フロクセル」の順。フロクセルは Volume 専用になる。副作用として、GroundLayer の霧はフロクセル無しで見える (§2 #15 の自動有効は Volume だけが対象になる)。照らすのは環境光・自己照明・(bool で) 太陽光で、局所ライトは層ごとの ON/OFF (§2 #24、sub-15)。却下: フロクセルのスライス数を増やす = 全シーンのコストと 7 MB が増え、縞は薄くなるだけ / 霧の板をメッシュ (半透明の重ね板、時のオカリナの作り方) で描く = 層の厚みと濃さの場を板の枚数でしか表せず、カメラが層に入ると破綻する / 全画面の後処理 1 回で描く = 後から描く透明物 (水・粒子) が霧の手前に出る |
| 14 | 霧の明るさと太陽光 | フロクセルは平行光 (太陽) を注入しない (`froxel_inject.cs.hlsl:15-16`。太陽まわりの散乱は解析フォグ `deferred_light.hlsl:249-275` とゴッドレイの受け持ち)。日中の霧が環境光だけで暗い灰色になりうる。時のオカリナの霧はほぼ無陰影の白。フロクセルの注入にはカスケード影が束縛されていない | **当てる / 当てないを bool で選べるようにする** (2026-11-08。planner の裁定「自己照明で代用」に選択肢を足す形) | 自己照明 `fogColor × fogBrightness` (等方・影なし) は常に持つ。加えて**場ごとの** `fogSunLight` (既定 **false**) が true の場の媒質にだけ、平行光の散乱 `σ_s × 放射輝度 × HG(g)` を足す (GroundLayer は地面の霧の描画 = sub-03、Volume はフロクセルの注入 = sub-08)。既定 false の理由: 既存のフロクセルの設計 (平行光を注入しない) と同じ絵が既定になる、時のオカリナ風の無陰影が既定になる、影なしで木陰まで照るのを作り手が選んで受け入れる形にする。場ごとにした理由: 屋内の煙と屋外の霧が同じシーンに並ぶ。二重計上: 解析フォグの太陽寄せはグローバルの媒質にだけ掛かり、場の媒質は解析フォグが知らない別の媒質なので重ならない。グローバルの媒質には今までどおり足さない。制限 (影なし) は Inspector の説明と ADR-036 に書く。却下: シーン全体の 1 つの bool = 屋内外の混在を表せない / 影つき = カスケード影をフロクセルの注入へ束縛する別の仕事 (後回し) |
| 15 | フロクセルが off のとき (既定 off、シーンカメラは `CameraPostFx.froxelOn`、エディタの Scene View はグローバル設定 `RenderSystem.cpp:935-949`) | 場を置いてもフロクセルを入れないと霧は見えない。ただしフロクセルを素直に on にすると (1) グローバルの媒質 (`froxelDensity` 既定 0.02 と高さの霧) が足される (2) 解析フォグがフロクセルの奥行きの外だけに縮む (`deferred_light.hlsl:250-265` の受け渡し) (3) ゴッドレイが自動で降りる (`RenderSystem.cpp:2364-2374`) = 場と関係ない見た目が黙って変わる | **場を置いたら自動で有効にし、OFF にもできる** (2026-11-08。planner の裁定「必須のまま」の逆) | **対象は Volume (3D) の霧だけ** (GroundLayer は §2 #13 の専用描画でフロクセルが要らなくなった)。場ごとの `autoFroxel` (既定 true)。ビューのフロクセルが off で、`autoFroxel` かつ霧を出す有効な Volume があれば、**「場だけのモード」**で回す: グローバルの媒質 0、解析フォグは全区間 (受け渡ししない)、ゴッドレイは降ろさない → 場の外は従来の off と同じ値 (受け入れ 31 (a) で差 0 を断言)。カメラが `froxelOn` のとき (フル) は従来どおりに場の媒質を足すだけ。明示: Inspector の `autoFroxel` の説明 (自動で回ること・GPU の約 7 MB と注入パスの時間・止め方)、ビューがモードに入った / 出たときの INFO ログ 1 行ずつ、Profiler の既存のフロクセルの時間。sub-08。却下: カメラの `froxelOn` を自動で立てる = 上の (1)〜(3) が黙って起きる / シーン全体の設定にする = 場を置いた所で止められない |
| 16 | 描画できる場の数 | sim は 4 個。描画側は霧・草・水・地形・粒子のシェーダが場を読む。3D テクスチャは配列にできない | **3D も 4 個まで描く** (2026-11-08。planner の裁定「1 個」の逆) | 2D 層は最大 4 枚を Texture2DArray (寸法は最大値、層ごとの UV 範囲は CB)。3D は**最大 4 個をそれぞれの Texture3D** (SRV 4 本、寸法は各自) で全部描く。有効な場は合わせて 4 個なので、描かれない場は無い。フロクセルの注入は 4 個を順に足し、格子の AABB の外では読まない。注入パスの GPU 時間を 0 / 1 / 4 個で測る (受け入れ 20)。sub-07 |
| 17 | 跡は流れで流されるか | 雪の足跡は風で動かない | — | 跡は移流しない (復元だけ)。濃さだけが流れに乗る |
| 18 | 波紋は浮力・当たり判定に効かせるか | 台帳: 物理は押さない | 確定済み | 効かせない (見た目と読み取りだけ)。跡も地形の当たり判定を変えない。`SampleWind` と空力 3 か所は変えない |
| 19 | 場の中の見た目専用の欄 (霧の色など) をハッシュ対象のコンポーネントに置いてよいか | WorldHash はカラムの生バイトを畳むので、色を変えるとハッシュ値は変わる (sim の結果は変わらない)。NavMeshSurface の `drawObstacles` 等、hash 対象の型に表示用の欄を置いた前例あり | planner 裁定 | 1 つのコンポーネントにまとめる (sim は見た目の欄を読まない、と注記)。別の NoHash コンポーネントに分けるのは型が増えるだけ |
| 20 | ゲームロジックが読む時点 | スクリプトの Update はフェーズ 3、場の step はフェーズ 4 | — | `FieldSample` は**呼んだ時点の状態**を返す。Update からは前 tick の結果、LateUpdate からはこの tick の結果 (知覚と同じ規約)。文書化する |
| 21 | C# から書けるか | C# は NoHash レーン。hash 対象の状態を C# の判断で動かす API は拒否する規則 (`EngineAPI.h` `SendScriptMessage` / `PlayFeel` の注記) | — | `FieldAddImpulse` は C# から 0 (拒否)。`FieldSample` は C# から読める |
| 22 | 時間を止める手触り (ヒットストップ・ポーズ) | 場の step は `stepSim` のゲートの中 (粒子と同じ。`TickRunner.cpp:613-616` の直後) | — | ヒットストップ中は霧も止まる (粒子と同じ拍)。キューは止まっている間も積めて、再開後の最初の step で消費 |
| 23 | 突風 (時間で変わる風)・霧が視界を遮る (知覚) | 依頼の 4 用途 4 源には含まれるが、最小の形は一様風 + 局所の風源 (Constant の源) で満たせる | **後回しでよい** (2026-11-08、裁定どおり。起伏に沿う霧は #12 で今回入れる) | 後回し (§3) |
| 24 | 地面の霧を局所ライト (点・スポット) で照らすか | sub-03 の専用描画はライト一覧を持たない合成箇所 (粒子・VFX・スカイ) を含む 10 か所で走る。段数 24 × ライト本数 × (影なら) 影サンプルのコスト。フロクセル (Volume) は局所ライトのビームと影を既に持つ (`froxel_inject.cs.hlsl` の LightSample + SampleShadowAtlas) | **照らす / 照らさないを ON/OFF で切り替える** (2026-11-08。planner の推奨「照らさない」に選択肢を足す形。太陽光の bool と同じ扱い) | 場ごとの `fogLocalLights` (既定 **false**。太陽光と同じ理由で、既定は従来の絵とコスト)。GroundLayer だけ (Volume では無視)。ライトは CPU が毎フレーム「影響球が ON の層の板の AABB と交わる局所ライトを、ライト一覧の順に最大 8 本」選び、縮約した写しを場の CB に載せる (各シェーダの既存のライト一覧の並びに依存しない。10 か所に同じ口)。超えた分は落として WARN 1 回。減衰とスポットは `common.hlsli::LightSample` と同じ式。**影なし** (10 か所にシャドウアトラスを束縛するコストと複雑さに見合わない。影つきのビームは Volume で作る)。sub-15 を新設 (sub-03 は 10 か所の合成を作るだけで十分大きいので分けた)。却下: 各シェーダの既存のライト一覧を読む = 一覧を持たない合成箇所がある / 本数無制限 = 段数 × 本数のコストが読めない / 影つき = 上記 |
| 25 | スカイボックスの無い背景 (clear 色) に Forward でも地面の霧・フロクセルの霧を掛けるか | Forward には背景を描くシェーダが無く、clear 色のまま残る (sub-03 round 2 の実測 sub03r2_03)。Deferred は光パスの背景分岐 (albedo.a < 0.5) で地面の霧とフロクセルを掛けている。golden には Forward + `--froxel` の `fog` (`tools\shot_verify.bat:257`) がある | **Forward でも掛ける** (2026-11-08。R13 の取り消し)。**フロクセルの背景も Deferred に揃える** (2026-11-08。planner の推奨「揃えない」の逆) | Forward の不透明の後・透明の前に、**背景だけを塗る全画面の三角形**を 1 回描く。深度は far で、深度テストは LESS_EQUAL・書き込みなし = clear のままの画素にだけ乗る。色は clear 色に、Deferred の背景分岐と**同じ処理** (地面の霧 `GroundFogCompositeBackground` → フロクセルの合成。フロクセルの部分は Deferred の背景分岐の式を写す) を掛けたもの。解析フォグ (`FogComponent`) の背景の扱いは今の Forward のまま変えない (依頼の外)。**描く条件 = Forward のビュー かつ スカイボックスを描いていない かつ (霧のある GroundLayer が 1 枚以上 または そのビューでフロクセルが走る)**。条件が欠けたらパスそのものが走らず、絵はビット不変。既存の「Forward + フロクセル + スカイ無し」のシーンは**背景の画素だけ**変わる (受け入れ 36 で、変わってよいシーンと差 0 のシーンを分け、golden の更新手順を決める)。sub-15 の後に置き、局所ライトも同じ関数で揃う。sub-16。却下: clear 色を霧の色へ寄せる = 視線ごとの通過長が出ない / 単色のスカイボックスを自動で描く = スカイの設定と見分けが付かない |
| 26 | 跡 (雪・砂) の見た目の欄 (色・強さ) をどこに置くか | 場の側 (DisturbanceFieldComponent) に置くと、並列の波で同じ struct の末尾を触るサブ (sub-07 / 09 / 10) と重なる。跡の色は「その地面が雪か砂か」で決まるので、意味としても地面の持ち物 | planner 裁定 (並列化のとき、2026-11-08) `[ユーザーに聞ける]` | **TerrainComponent の末尾** (見た目の欄。sim は読まない。snapshot は v76)。1 つの場が雪と砂の両方の地形にまたがっても、それぞれの色で描ける。却下: 場の側 = 1 つの場に色が 1 つしかなく、並列の波でも衝突する |
| 27 | Volume の性能の線 (受け入れ 20) | sub-07 の実測: 32×16×32・20 反復で Release・jobs なし 1.99〜2.04 ms (最良の窓)、jobs あり 0.59 ms、反復 0 で 1.25 ms。2D の同条件は 1.11 ms。3D は速度が 3 成分、Jacobi が 7 点なので、同じマス数でも約 1.8 倍が本質的に掛かる。実行時の既定は jobs あり | planner 裁定 (ユーザー不在、2026-11-09) `[ユーザーに聞ける]` | **jobs ありで 1.67 ms 以下を合格の線にし、jobs なしの値は ADR-036 に書く**。既定の寸法・反復回数は変えない。却下: Volume の既定の反復を 10 に下げる = 渦が弱くなり、jobs なしでも約 1.6 ms で線ぎりぎり / 既定の寸法を 32×8×32 に下げる = 煙の柱の高さ方向の解像度が半分になる / 三線形の SIMD 化 = 決定論の確認が要る別の仕事 |

## 3. スコープ

- やる:
  - 2D 層 (GroundLayer) と 3D 体積 (Volume) の場。チャネル = 流れ (常時) / 濃さ / 跡 (2D のみ) / 波紋 (2D のみ)
  - 源: `DisturbanceSourceComponent` (動きから自動 = CC / Rigidbody / 位置の差分、定常 = 扇風機・煙の噴き出し口)、明示のインパルス (C++ スクリプト / アニメイベント `disturb`)、環境の一様風 (`PhysicsEnvironmentComponent.windVelocity` × 場ごとの倍率)
  - 読み取り ABI `FieldSample` (C++ / C#)、書き込み ABI `FieldAddImpulse` (C++ のみ) = ABI v35 = 202 スロット
  - 位置: 固定 → 追従 (プレイヤーに付いて動く格子)
  - 地形の起伏に沿う 2D 層 (`followTerrain`、列ごとの基準高さ)
  - 見た目: 霧 (2D 層 = 板のレイマーチによる専用描画 4 枚、3D = フロクセル 4 個。自己照明 + 場ごとの太陽光の bool + 地面の霧の局所ライトの ON/OFF、Volume のためのフロクセルの自動有効 = 場だけのモード)、粒子 (CPU / GPU)、水面の波紋、地形の跡、草 (サーフェスシェーダ + デモの草束)
  - デモ `--field-demo` (霧の広場) と replay_verify のジョブ `field`、jobs A/B への追加
  - デバッグ描画 (速度の矢印、濃さ)、Profiler のスコープ、Inspector の日本語名
  - ADR-036、engine_spec、engine-feature-guide、test_checklists
- やらない (明示的に外したもの):
  - 物理を押す (剛体・布・ロープ・空力・浮力・当たり判定・`SampleWind`)
  - 全物体を暗黙に源にする (§2 #5)
  - GPU での sim (sim 状態は CPU。GPU は読むだけ)
  - 温度・燃焼・液体 (Niagara の 2D / 3D Liquid 相当)
  - 場をアセットに焼いて保存する (状態は実行時だけ。シーンにはコンポーネントの設定だけが保存される)
- 後回し:
  - 突風 / 霧が知覚 (視線) を遮る (§2 #23)
  - 太陽光の影 (カスケード影をフロクセルの注入へ束縛する。§2 #14) / 斜面に沿って霧が流れ落ちる / 地形を実行時に変えたときの基準高さの焼き直し

## 4. 仕様

### 4.1 振る舞い

#### A. 場 (`DisturbanceFieldComponent`)

- 有効な場 = `DisturbanceFieldComponent` を持つ生きたエンティティのうち **entity index の昇順で先頭 4 個**。5 個目以降は sim しない (Inspector に注記、ログに WARN を場ごとに 1 回)
- 形: `GroundLayer` (2D、`cellsY` は 1 に固定) / `Volume` (3D)
- 原点: 最初の step で `originCell = round(場のエンティティのワールド位置 / cellSize) − (cellsX/2, 0, cellsZ/2)` (Volume は y も `− cellsY/2`)。GroundLayer の基準面の高さ `baseY` = 場のエンティティのワールド y (丸めない)。格子は軸に平行 (エンティティの回転は無視)。固定モードは以後動かない (エンティティを動かしても格子は動かない)。追従は sub-07 (§4.1 F)
- 初期化: その場の状態が無い / 寸法・`cellSize`・形・チャネルが状態と違うときに作り直す (`cellSize` はスロットのハッシュ・DFLD にも入る)。全マス = 静止状態 (流れ 0、濃さ `restDensity`、跡 0、波紋 0)。他の欄 (keep 率など) の変更は作り直さず次の step から効く
- 消滅: エンティティが破棄された / コンポーネントが外れた / 有効な 4 個から外れた場の状態は、その tick の step の冒頭で捨てる
- セルの並び: `index = x + cellsX * (z + cellsZ * y)` (x が最内、次に z、最後に y)。2D は y = 0 だけ
- セル中心: `world = (originCell + (x, y, z) + 0.5) × cellSize`。GroundLayer の y は使わず、層は `[基準高さ, 基準高さ + thickness]` の板。基準高さ = `baseY`、ただし `followTerrain` の場 (sub-09) は列 (x, z) ごとの `columnBaseY` (作り直しの step で全列、追従のずらしでは入ってきた列だけ `SampleTerrainHeightWorld` で焼く。地形が無い列は `baseY`)。以下の式の `baseY` は全て「その列の基準高さ」と読む

#### B. 1 tick の step (フェーズ 4、Fracture の後・粒子の前、`stepSim` のときだけ)

場ごとに entity index 昇順で、次の順に回す。番号の後ろの括弧は入れるサブ。

1. 掃除と初期化 (A) (sub-01)
2. 追従のずらし (sub-08)
3. 源の注入 — `DisturbanceSourceComponent` を entity index 昇順に、直列 (sub-01)
4. 配送待ちインパルスの注入 — 積んだ順に、直列。全ての場の注入が終わってからキューを空にする (sub-01)
5. 速度: 半ラグランジュ移流 (sub-04) → 目標速度への減衰 (sub-01、目標は sub-01 では 0、sub-04 から風) → 速さの上限
6. 圧力投影 (Jacobi、`pressureIterations` 回、初期値 0) (sub-04)
7. 濃さの移流 (sub-04) → 濃さ・跡の復元 (sub-01)
8. 波紋の 1 段 (sub-01)
9. 値の範囲の強制 (濃さ・跡 [0,1]、波紋 ±`kMaxRippleHeight`、非有限は静止状態に戻す — 起きたら WARN) (sub-01)
10. 描画用の通番を 1 進める (状態ではない。ハッシュに入れない) (sub-01)

#### C. 注入の式 (源・インパルス共通)

- 距離 `d`: 3D = 点とセル中心の距離。2D = 水平距離と板からの縦のはみ出し `dy = max(0, baseY − py, py − (baseY + thickness))` を合わせた距離。`d >= radius` のマスは触らない (格子外は書かない)
- 重み `w = (1 − d / radius)²`
- 速さ係数 `s`: Moving の源 = `min(|v_src| / fullSpeed, 1)` (`fullSpeed <= 0` なら 1)、Constant の源とインパルス = 1
- 流れ: 目標速度 `u`、混ぜ率 `a = clamp(w × strength, 0, 1)` (インパルスは `a = w`)、`v += a × (u − v)`。2D は xz 成分だけ
  - Moving: `u` = 源の速度 (D)。Constant: `u` = 源の回転 × `constantVelocity`
  - インパルス Point: `u = dir × speed` / Radial: `u = normalize(セル中心 − 点) × speed` (`d` が 1e-6 未満のマスは `u = 0`) / Cone: Point に `w × saturate((cosθ − coneCos) / (1 − coneCos))` を掛ける (θ = `dir` とセル方向の角)
- 濃さ (濃さチャネルがあるとき): `Δ = w × s × densityDelta` (`densityDelta` ∈ [−1, 1])。`Δ < 0` なら `ρ += Δ × ρ` (晴らす)、`Δ > 0` なら `ρ += Δ × (1 − ρ)` (足す = 煙)
- 跡 (跡チャネル、2D のみ): `τ += w × trailDepth × (1 − τ)` (速さに依らない。立ち止まると深くなる)
- 波紋 (波紋チャネル、2D のみ): `ripple != 0` のときだけ、`a = clamp(w × s, 0, 1)` で `h += a × (ripple − h)`、**`hPrev` も同じ a で `ripple` へ寄せる** (メートル。h を目標の高さ `ripple` へ寄せる = 何 tick 続けても `|ripple|` を超えて伸びない。h だけを寄せると `h − hPrev` が毎 tick 速度として積もり、目標の約 3 倍まで行き過ぎた = sub-07 round 3 の実測)。**sub-07 round 2 で変更**: 元の `h += w × s × ripple` は、同じ場所へ毎 tick 足すと波動方程式に加速度を与えることになり、h が 2 乗で伸びて 8 tick で上限 `kMaxRippleHeight` に張り付いた (歩いて水を渡る源は、半径の中に数十 tick とどまる)。`ripple == 0` の源は波紋に触れない (他の源の波を消さないため)

#### D. 源 (`DisturbanceSourceComponent`)

- 位置 = エンティティの WorldMatrix × `offset` (この tick の Transform 確定後の値)
- Moving の速度: CC があれば `CharacterControllerComponent.velocity` → 非 kinematic の Rigidbody があればその線速度 → どちらも無ければ `(今の位置 − prevPos) × 60` (`prevValid` が偽の最初の tick は 0)。毎 tick (`stepSim` の tick) の最後に `prevPos` / `prevValid` を書く (コンポーネント内 = ハッシュ対象)。**有効な場が 1 つも無い tick でも書く** (後から場が現れたときに古い位置との差で速度が跳ねないため)
- 全ての有効な場に効く (場を選ぶマスクは v1 では持たない)

#### E. 明示のインパルス

- C++ (エンジン内部 `DisturbanceQueueImpulse` / ABI `FieldAddImpulse`) とアニメイベント `disturb` が配送待ちキューに積む。上限 1 tick あたりではなく**キューの長さ 256**。超えたら積まず 0 を返し WARN (tick ごとに 1 回)
- キューは「積んだ後の最初の step」で消費する (§2 #7)。場が 1 つも無いシーンで積まれたインパルスはその step で捨てる (キューを空にする)
- アニメイベント `disturb` (sub-06): `.controller.json` の `clipEvents` の 1 件に `"kind":"disturb"` と `radius` / `speed` / `shape` (`"point"|"radial"|"cone"`) / `dir` (コントローラのエンティティの空間、既定 `[0,0,1]`) / `coneDeg` / `density` / `trail` / `ripple` を書く。位置は既存の `joint` 規則 (`AnimEventFired.pos`)。向きはコントローラのエンティティの WorldMatrix (前 tick の確定値、位置と同じ) で回す

#### F. 追従 (sub-08)

- `followTarget` (EntityRef) が生きていれば、その位置をセルに丸めた中心と今の中心の差 (整数セル) が `followMargin` セル以上になった step で、原点をその差だけずらす
- ずらしは全チャネルで「重なる部分を写し、入ってきたマスは静止状態、出たマスは捨てる」。キュー・源の注入より前 (B の 2)
- 状態 = `originCell` (整数) だけが増える情報。決定的

#### G. 読み取り (`FieldSample`)

- 引数の場 (null なら位置を含む有効な場のうち entity index 最小) の、位置での値をセル中心の双線形 (3D は三線形) で返す。格子外のマスは静止状態として補間する (静止状態の流れ = その場の目標速度 = 風 × windScale。sub-04 以降)
- 2D の「含む」= xz が格子内、かつ y ∈ `[baseY − thickness, baseY + 2 × thickness]`
- 2D の濃さは高さの形 `p(y) = y <= baseY ? 1 : saturate(1 − (y − baseY) / thickness)` を掛けた値 (描画と同じ式)。`followTerrain` の場の `baseY` は列の基準高さの双線形
- 返す値: 流れ (vec3、2D は y = 0)・濃さ・跡・波紋の高さ・場のエンティティ。無効なチャネルの値は 0 (濃さチャネルが無い場の濃さも 0)
- 読むのは呼んだ時点の状態 (§2 #20)

#### H. 波紋の 1 段

- `C = min(rippleSpeed / (cellSize × 60), kMaxRippleCourant = 0.7)` (2D の安定条件 1/√2 未満)
- `h' = (2h − hPrev + C² × (hE + hW + hN + hS − 4h)) × rippleKeep`、格子外の隣は 0。`hPrev ← h`、`h ← h'`

#### I. 復元と減衰

- 速度: `v = v_target + (v − v_target) × velocityKeep`。`v_target` = sub-01 では 0、sub-04 から `PhysicsEnvironmentComponent.windVelocity × windScale` (2D は xz、Volume は xyz = 上昇気流も効く。sub-07 で決定)。速さの上限 `cellSize × 60` (1 tick 1 セル) で長さを切る
- 濃さ: `ρ = restDensity + (ρ − restDensity) × densityKeep`
- 跡: `τ = τ × trailKeep` (`trailKeep = 1` で永続)

#### J. 移流・投影・風 (sub-04)

- 半ラグランジュ: セル中心から `−v × (1/60)` 戻った点を双線形 (3D 三線形) で引く。格子外は静止状態 (流れは `v_target`、濃さは `restDensity`) = **開いた境界**。風上の縁から静止濃度の霧が入ってくる
- 投影: 発散 → Jacobi (圧力の格子外は 0 = ディリクレ) → 勾配を引く。`pressureIterations = 0` で投影しない。離散の発散・勾配・Jacobi の組は合成が一貫した作用素にする (sub-04 round 1: 中心差分の発散と勾配に、隣接 5 点の Jacobi を組むと 64 反復でも発散が 19% 残った)
- **Radial のインパルスは発散源** (sub-04 round 1 で決定): その step で Radial が注入した速度の差分の発散を、投影の右辺から引く (`∇²p = div(v) − div(Δv_radial)`)。爆発の外向きの流れはその step の投影で消されず、霧を押し出してから、次の step 以降の投影と減衰で消える。作業配列だけで、状態は増えない。Point / Cone / 動く源は従来どおり投影する (渦になる)
- 3D の浮力 (sub-07): `vy += buoyancy × (ρ − restDensity) / 60` (濃い煙が昇る)

### 4.2 データ・保存形式・互換性

#### コンポーネント (反射登録の末尾 = TypeId は登録順)

- `DisturbanceFieldComponent` (TypeId 96、hash 対象、sub-01)
  - sim が読む: `shape` (int32、0 = GroundLayer / 1 = Volume)、`cellsX` / `cellsY` / `cellsZ` (int32、既定 128 / 1 / 128。shape を変えても寸法は自動で変えない。Volume の推奨 32×16×32 は Inspector の説明に書く)、`cellSize` (既定 0.25 m)、`thickness` (GroundLayer の層の厚さ、既定 1.0 m)、`channels` (uint32 ビット: 1 = 濃さ / 2 = 跡 / 4 = 波紋。既定 1)、`restDensity` (既定 1.0)、`densityKeep` (既定 0.99)、`velocityKeep` (既定 0.95)、`trailKeep` (既定 0.999)、`rippleSpeed` (m/s、既定 1.5)、`rippleKeep` (既定 0.995)
  - 見た目 (sim は読まない。sub-01 でまとめて入れる): `fogDensity` (濃さ 1 の消散係数 1/m、既定 2.0)、`fogColor` (float3)、`fogBrightness` (自己照明、既定 1.0)、`fogSunLight` (bool、既定 false。§2 #14、sub-03)、`autoFroxel` (bool、既定 true。§2 #15、sub-08)、`drawDebug` (bool、sub-04)。`fogLocalLights` (bool、既定 false。§2 #24) は sub-15 で足す (見た目の bool の並びの明示の詰め物を使えればサイズ不変)
  - 後のサブで末尾に足す: `pressureIterations` (int32、既定 20) / `windScale` (既定 1.0) (sub-04)、`buoyancy` (sub-07)、`followTarget` (EntityRef) / `followMargin` (int32、既定 4) (sub-08)、`followTerrain` (bool、既定 false、変えたら作り直す。sub-09)
  - 範囲: 1 フィールドのマス数 ≤ 262,144、各辺 ≥ 2、`cellSize` ∈ [0.05, 8]、keep 率 ∈ [0, 1]。範囲外は Inspector で切り、sim は切った値で動く (切った値で状態を作る)
- `DisturbanceSourceComponent` (TypeId 97、hash 対象、sub-01)
  - `mode` (int32、0 = Moving / 1 = Constant)、`offset` (float3、ローカル)、`radius` (既定 0.75 m)、`strength` (流れの混ぜ率、既定 0.5)、`fullSpeed` (既定 3 m/s)、`densityDelta` (既定 −0.3)、`trailDepth` (既定 0)、`ripple` (既定 0)、`constantVelocity` (float3、ローカル)、`prevPos` (float3、ReadOnly)、`prevValid` (uint32、ReadOnly)
- 既存型への追加: `ParticleEmitterComponent` に `fieldInfluence` (既定 0 = 影響なし、sub-10)。ほかの既存型は変えない
- どちらも `static_assert(sizeof == N)` で暗黙の詰め物なし (既存の登録と同じ)

#### Scene が持つ状態 (`DisturbanceState`、`src\Engine\Engine\Disturbance\`)

- スロット (最大 4、entity index 昇順): `entity` (index + 世代)、`shape`、寸法、`channels`、`originCell` (int32 × 3)、`baseY`、配列 (流れ 2 or 3 本・濃さ・跡・波紋 h / hPrev。有効なチャネルだけ確保)、`followTerrain` の場だけ `columnBaseY` (cellsX × cellsZ、sub-09)
- 配送待ちキュー (最大 256 件、`FieldImpulse` = 位置・向き・半径・速さ・形・coneCos・濃さ・跡・波紋)
- 描画用の通番 (状態ではない)
- Play の開始 / 終了 (`PlayModeController.cpp:26,40`) とシーン切り替え (`TickRunner.cpp:992`) で Clear (Feel と同じ 3 か所)

#### ハッシュ (WorldHasher)

- `SimSources` の末尾に `disturbance` (Scene から引く。`SimSourcesOf` の引数は増やさない)
- **内容ゲート**: スロットが 0 かつキューが空なら節ごと畳まない (既存シーンのハッシュ列不変)
- 畳む順: スロット順に (entity、shape、寸法、channels、originCell、baseY のビット、各配列の生バイト) → キュー (件数 + 生バイト)。`--hash-dump` には場ごとの小計を出す

#### スナップショット

- 新しい節 `'DFLD'` を TM2D 節の後・World 節の前に置く。参照が無い / 空でも件数 0 の節を書く (レイアウト固定)
- 版: sub-01 = v70 (DFLD 節 + 2 型)。以後レイアウトを変えるサブ (sub-04 / sub-07 / sub-08 / sub-09 / sub-10) は**着手時の `kSimSnapshotVersion` + 1** を取り、版コメントを足す。`AcousticAudioSelfTest.cpp:129` の版の表明を同時に直す (`2961ba3` の前例)
- 復元は World を差し替えた後に状態を丸ごと差し替える。検証に失敗したら何も書き換えない (既存の規約)

#### ABI (sub-05)

- v35 = 202 スロット: `FieldAddImpulse` / `FieldSample` を `SetFeelOutputScale` の次に append
- `int32_t (*FieldAddImpulse)(void* engine, const MyeFieldImpulse* impulse)` — 1 = 積んだ、0 = null / キューが満杯 / C# レーン
- `int32_t (*FieldSample)(void* engine, MyeEntityId field, MyeVec3 pos, MyeFieldSample* out)` — 1 = 場の中、0 = 外 (out は 0 埋め)。C# から呼べる
- `MyeFieldImpulse` / `MyeFieldSample` は `Shared` の POD (`static_assert` でサイズ固定、`Interop.cs` に同じ並びでミラー)。C++ の糖衣 (`ScriptAPI.h`、既定値入りの初期化) と C# の糖衣 (`MyeScript`、読み取りだけ)
- 位置ミラーの照合は check_rules 規則 11

#### 互換

- 既存の `.rep` は再検証で PASS (場が無いのでハッシュ列不変)。新しい型は opt-in なので既存シーンの World 節のハッシュは不変
- 既存スナップショット blob は版不一致で拒否 (既存の規約どおり)

### 4.3 UI / ビジュアル

- 霧の媒質の式 (共通): `σ = fogDensity × ρ(posW) × p(y) × edgeFade`。`edgeFade` = 格子の縁 2 マスで 0 → 1 (箱の壁が見えない)。散乱は (環境光 + 局所ライト※) × σ_s に、場の分だけ `σ × fogColor × fogBrightness` (自己照明) を足し、`fogSunLight` の場はさらに平行光 × HG (影なし) を足す (§2 #14)。※局所ライトは、フロクセル (Volume) では常に、地面の霧では `fogLocalLights` の層だけ (最大 8 本、影なし。sub-15)
- 地面の霧 (GroundLayer、sub-03、§2 #13): 合成する 10 か所で `GroundFogComposite` が板の交差区間を固定段数でレイマーチし、`color × T + inscatter` を返す。順序は地面の霧 → 既存の解析フォグ・フロクセル。層が無い / 交わらないピクセルは何もしない。フロクセルは不要
- 立体の霧 (Volume、sub-07 / sub-08): フロクセルの注入で上の σ を足す (sub-02 で作った 2D の注入は sub-03 で外す)
- フロクセルのモード (sub-08、§2 #15。Volume の霧だけが対象): カメラ / グローバルで on = **フル** (従来 + 場の媒質)。off で `autoFroxel` の霧の Volume がある = **場だけ** (グローバルの媒質 0、解析フォグは全区間、ゴッドレイを降ろさない)。それ以外 = off (従来)。モードの出入りで INFO ログ 1 行
- 編集中 (Play 前・状態が無い場): 静止状態の定数 (`restDensity`) で描く。描画レーンは sim の状態を作らない
- 描画への受け渡し: Renderer 層は POD (`DisturbanceFieldUpload`) だけを受け取る (`AcousticVolumeUpload` と同じ。`AcousticVolumePass.h:17-27`)。2D = Texture2DArray `RGBA16F` (vx, vz, ρ, τ) + `RG16F` (R = 波紋 h、G = 列の基準高さ − baseY)、3D = 最大 4 本の Texture3D `RGBA16F` (vx, vy, vz, ρ)。通番が前回と同じなら転送しない。共有する定数とレイアウトは C++ と HLSL を一致させる (規則 9、CB の `static_assert`)
- 粒子 (sub-10): CPU の粒子は sim の状態を直接引く (ハッシュ対象の経路)、GPU の粒子は 2D / 3D テクスチャを引く。`fieldInfluence` × (場の流れ − 粒子の速度) を加速度に足す。**y 成分は Volume の中だけ寄せる** (2D 層の流れの y は常に 0 なので、寄せると上昇する煙の vy を 0 へ潰す)。ローカル空間 (`simulationSpace = 1`) のエミッタは場を読まない (座標がワールドでない。深度衝突と同じ扱い)。GPU は直近の描画が上げたテクスチャを読む (1 フレーム遅れ、見た目のレーン)。場が重なったときの選び方は CPU (entity index 順) と GPU (2D 層 → Volume の順) で違ってよい (sub-11 round 1 で決定)
- 水面 (sub-11): `water_surface.hlsl` の法線に波紋の勾配を足す (可能なら VS の変位も)。浮力 (`WaterWaveComponent` の CPU 側) は変えない
- 跡 (sub-12): `forward_terrain` / `deferred_terrain` の色を跡で暗く / 指定色へ寄せ、法線を跡の勾配で曲げる (凹みの変位は任意)。当たり判定は変えない
- 草 (sub-13): サーフェスシェーダ用の予約テクスチャ + `MyeFieldSample` を `MyEngineSurface.hlsli` に足し、`GrassField.surface.hlsl` の VS で流れの向きに倒す。デモに草束 (細い箱のエンティティを数百) を置く
- デバッグ描画 (sub-04): `drawDebug` の場で、数マスおきに速度の矢印、濃さを色の十字で debugLines に積む (出力レーン、再シム中は積まない。nav の線と同じ扱い)
- スクショでの検証は §5。最後の目視はユーザー

### 4.4 非機能

- 決定論: sim の計算は float、`+ − × ÷ sqrt floor min max` だけ。`std::exp / pow / log / sin / cos / tan` 等は Disturbance の sim ファイル (`src\Engine\Engine\Disturbance\*.cpp` のうち描画への詰め替えを除く) で使わない (check_rules 規則 18、sub-01)。順序決定に使うキーは entity index と積んだ順だけ。`_DEBUG` 分岐なし
- 並列: §2 #9。jobs あり / なしでハッシュ一致 (replay_verify `jobsab` にフィールドのデモを足す、sub-04)
- 性能: 1 フィールドの sim + ハッシュが Release・jobs なしで 1.67 ms/tick 以下 (§5 の 15 / 20)。Debug の selftest は既定層 60 秒の目安を壊さない (スイート `disturbance` は Debug 2 秒以下なら Fast、超えたら重い部分を Full へ)
- ローカライズ: Inspector の欄名は `MYE_JP`、新しい UI 文字列は `Tr()` と `LocalizationTable.inl` の両言語 (規則 10)
- レイヤー: sim は Engine 層、描画は Renderer 層 (POD だけ受け取る)。生の D3D 型を Renderer の外へ出さない (規則 16)
- 新しいファイルを足したら `tools\gen_project_files.ps1`

## 5. 受け入れ条件

**selftest と replay_verify の運用 (ユーザー指示 2026-11-08)**: `--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY` と `tools\replay_verify.bat` の実行は M96 全体で 2 回だけ (中間 = W2 の統合の後 (sub-07 を含む波。§6)、最後 = sub-17。サブの新設で最後が sub-14 → sub-15 → sub-16 → sub-17 と繰り下がった)。各サブの coder はビルドと check_rules (と、描画のサブのスクショなど、selftest と replay_verify 以外の検証) までを行い、selftest と replay_verify で確かめる受け入れ条件は「未実行 (sub-07 後 / sub-17 でまとめて実行)」と報告する。planner はそれを理由に REWORK にしない (検証が未実行で自己採点が 4 以上でも矛盾として扱わない)。まとめて実行して落ちた分は、司会が該当サブへ FIX_REQUEST で回す。selftest のコードを書くこと自体は各サブの受け入れ条件に残る。

共通 (全サブ): C1 Debug / Release の両構成でビルド成功し、**変更したファイルの警告 0**。`/p:MyeWarnAsError=true` は着手前からある `AnimatorControllerSelfTest.cpp` の警告 (C4456 / C4389) で落ちるので、その失敗は合否に数えない (別件、sub-01 round 1 で判明)。C2 `tools\check_rules.ps1` の error / warning が着手前から増えない。C3 関連スイートが両構成で FAIL 0。C4 `Editor.exe --selftest-full` と `tools\replay_verify.bat` が PASS。C3 / C4 は上の運用により、sub-07 の後と sub-17 でまとめて確認する。

| # | 条件 | 検証手段 | サブ |
|---|---|---|---|
| 1 | 場を置かないシーンの WorldHash 列・焼き込みハッシュが着手前と一致 | replay_verify の既存全ジョブ PASS、selftest の既存の焼き込み hash (CC2D / 庭 / tile / gridnav / twod 等) が不動、空の `DisturbanceState` で `HashWorld` が着手前の値と一致 | 01 |
| 2 | 源もインパルスも無い場は 600 tick 静止状態とビット一致 | selftest (全配列を memcmp) | 01 |
| 3 | 注入をやめた後、全マスの `|ρ − restDensity|` の最大値と跡の最大値が毎 tick 非増加し (流れの最大の速さは、投影が局所的に増やしうるので非増加を求めない。sub-04 からは「0.05 m/s 未満に消える」)、keep < 1 なら静止状態へ戻る。`trailKeep = 1` で跡が永続 | selftest | 01 (04 で移流込みで再表明) |
| 4 | スナップショット往復: 場 2 個 + キュー 3 件の状態を撮る → 新しいシーンへ戻す → 100 tick 毎 tick ハッシュ一致 | selftest (simsnapshot / disturbance) | 01 |
| 5 | 境界: 格子の角・外側への注入で格子外を書かない。完全に外の注入で状態不変。半径 0 は何もしない | selftest | 01 |
| 6 | 源の速度: CC / 非 kinematic Rigidbody / 位置の差分の 3 経路と、Constant の向き (エンティティの回転)。`fullSpeed` の速さ係数 | selftest | 01 |
| 7 | 波紋: 1 点の注入が 4 方向に同じ速さで広がり、`rippleSpeed` を上限超えに置いても 10000 tick 有限 (C が 0.7 に切られる) | selftest | 01 |
| 8 | 寿命: 寸法を変えると作り直す / 破棄でスロットが消え節が畳まれなくなる / 5 個目の場は sim されず WARN 1 回 / シーン切り替えと Play 終了で Clear | selftest | 01 |
| 9 | 決定論: 同じ場面を 2 ワールドで 600 tick 並走して毎 tick 一致、最終ハッシュの焼き込み値が Debug と Release で同じ | selftest | 01 |
| 10 | 霧の描画: `--field-demo` で、球が通った筋の霧が薄くなり、時間で戻る。通過前 / 直後 / 戻った後の 3 枚を撮り、筋の領域の平均輝度の差と、戻るまでのフレーム数を報告する。差は**同じフレーム内の「触っていない領域 − 筋の領域」**で測り (自動露出の画面全体の揺れを打ち消す)、ノイズは静止状態の 2 フレームの同じ指標とする。フロクセルのテンポラルの尾は理論値 (feedback 0.9 → 90% 収束 22 フレーム) と on / off の差指標で示せばよい (段差入力の専用プローブは作らない) | `Runtime.exe --field-demo --screenshot ... --shot-frame N` × 3 + `Editor.exe --img-diff`、数値を SELF_EVAL に | 02 |
| 11 | 編集中 (Play 前) も静止状態の霧が見え、Play で動き、Stop で静止に戻る | Editor の Scene View を `--screenshot` で撮る (グローバルのフロクセル on) | 02 |
| 12 | replay_verify のジョブ `field` (`--field-demo`) が Debug / Release で PASS。`tools\mcp\src\verify.rs` の JOBS と `run_parallel` の一覧に `field` | `tools\replay_verify.bat`、`cargo test` (tools\mcp) | 02 (以後のサブで維持) |
| 13 | 流れ: 投影後の発散の最大値が投影前の 10% 以下 (20 反復、源が通った直後)。源の後ろに渦 (速度の回転の符号が左右で逆) ができる | selftest + `drawDebug` のスクショ | 04 |
| 14 | jobs あり / なしでハッシュ一致 (selftest と replay_verify `jobsab` に `--field-demo`) | `SimParallelSelfTest` 相当 + replay_verify | 04 |
| 15 | 性能: 2D 128×128・全チャネル・20 反復で、sim + ハッシュが Release・jobs なし 1.67 ms/tick 以下 (jobs ありの値も報告)。並列の coder が同じ機械でビルドしているので、`--field-bench` で 10 tick の窓を 20 個以上測り、**最良の窓**で判定する (平均と最悪も報告)。ハッシュは `HashStateBytes` (ADR-030 決定 13 と同じ方式) で畳む。スナップショット 1 枚のバイト数を報告 | selftest の計測ログ (Release) | 04 |
| 16 | 風: 一様風 × `windScale` で霧が流れ、風上の縁から静止濃度が入る。`windScale = 0` で風の影響なし。`SampleWind` と空力の結果は不変 | selftest + replay_verify `physics` / `joints` PASS | 04 |
| 17 | ABI v35 = 202: 規則 11 PASS。C++ から `FieldAddImpulse` → 同じ tick (Update から) / 次の tick (LateUpdate から) の step で効く。`FieldSample` が内部の値と一致。C# から `FieldAddImpulse` は 0、`FieldSample` は値を返す | selftest (GameLogic のプローブと C# の一時プローブ。memory `abi-bump-verification` の手順) | 05 |
| 18 | インパルスの 3 形: Point は `dir` 向き、Radial は外向き (中心で 0)、Cone は円錐の外で 0 | selftest | 05 |
| 19 | アニメイベント `disturb`: 発火した tick の step で注入され、向きがコントローラのエンティティの回転に従う。JSON の各欄が読まれ、不正な `shape` はエラーで捨てる | selftest (animatorcontroller / disturbance) | 06 |
| 20 | 3D: Volume で煙が浮力で昇り、フロクセルに映る。32×16×32・20 反復で Release・**jobs あり** 1 個 1.67 ms/tick 以下、jobs なしの値は報告 (sub-07 round 1 で変更: 速度 3 成分と 7 点の Jacobi で 2D の約 1.8 倍が本質的に掛かり、jobs なし約 2.0 ms。実行時は jobs ありが既定)。Volume を 4 個置くと 4 個とも描かれ、フロクセルの注入パスの GPU 時間を 0 / 1 / 4 個で報告する | selftest + `--field-demo` のスクショ + GPU 時間 | 07 |
| 21 | 追従: 相手が `followMargin` セル動くと整数セルずれ、重なる部分の値が保たれ、入ってきたマスは静止状態。追従あり 600 tick の 2 ワールド一致と snapshot 往復 | selftest + デモのスクショ (プレイヤーの周りだけの霧) | 09 |
| 22 | 粒子: `fieldInfluence > 0` の CPU 粒子が流れに乗り、ハッシュ・snapshot・jobs A/B が一致。`fieldInfluence = 0` の既存デモ (`--particle-demo`) は不変。GPU 粒子も流れに乗る (スクショ) | selftest + replay_verify `jobsab` + スクショ | 11 |
| 23 | 水面の波紋: 歩くと波紋が広がる (スクショ 2 枚の差)。浮力の selftest と `WaterSurfaceSelfTest` 不変 | スクショ + selftest | 12 |
| 24 | 跡: 雪の地形に通った筋が残り、`trailKeep < 1` なら消える (スクショ)。地形の当たり判定の selftest 不変 | スクショ + selftest | 13 |
| 25 | 草: 球・プレイヤーの通り道の草が倒れ、戻る (スクショ 3 枚)。場が無い草のマテリアルは真っ直ぐ立つ | スクショ | 14 |
| 26 | 物理を押さない: `SampleWind` と空力 3 か所は差分なし。physics / joints / fracture の replay ジョブ PASS | `git diff` で該当関数を確認 + replay_verify | 17 (全サブで維持) |
| 27 | 文書: ADR-036、engine_spec の新しい節 (§ の番号は coder が既存に合わせる)、engine-feature-guide、test_checklists、dogfooding (該当があれば) | 差分の確認 | 17 |
| 28 | 全検証: `--selftest-full` Debug / Release、check_rules、replay_verify、三校 `C:\HAL\三校\tools\verify.bat` (既存の FAIL 2 件以外増えない) | コマンド | 17 |
| 29 | 静的規則 18: Disturbance の sim ファイルで CRT の超越関数を使うと error。規則自身の自己検査 (違反を含む一時入力で error になる) | `tools\check_rules.ps1` | 01 |
| 30 | ローカライズ: 新しい Inspector の欄と UI 文字列が日英とも埋まる (規則 10) | check_rules | 全サブ |
| 31 | フロクセルの自動有効 (場だけのモード): (a) Volume を視錐台の外に置いた場面 (解析フォグとゴッドレイ on) の絵が `autoFroxel = false` と差 0 (b) カメラの `froxelOn = false` のまま視界の Volume の霧が写る (c) `autoFroxel = false` でフロクセルが走らない (d) `--fog-demo` と、フロクセルを使わない既存デモ 1 本の絵が着手前と差 0 (e) モードの出入りで INFO が 1 行ずつ (f) Scene View (グローバル off) で Play 前の煙が見える (g) GroundLayer だけのシーンではフロクセルが走らない | `--img-diff`、スクショ、ログ | 08 |
| 32 | 太陽光の bool: `fogSunLight = false` の絵が切り替え前の同じ場面と差 0。true で太陽側を向いた霧が明るい (太陽の方向と逆の方向の平均輝度を報告)。場を視界外に置いて true にしても絵が変わらない (グローバルの媒質に足していない) | `--img-diff`、スクショ | 03 (GroundLayer) / 08 (Volume) |
| 33 | 起伏に沿う層: (a) 斜面の地形で、地形 + 0.5 × thickness の濃さが丘の上と平地で一致 (`followTerrain = false` なら丘の上は層の外) (b) `followTerrain = false` の場の sim の配列が変更前と 600 tick ビット一致 (c) 追従のずらしで残った列の基準高さはビット不変、入ってきた列だけ焼く (d) 2 ワールド一致・snapshot 往復 (e) 128² 列の焼き込みの ms を報告 (f) 丘を覆う霧のスクショ | selftest + スクショ | 10 |
| 34 | 地面の霧の専用描画: (a) sub-02 と同じカメラ・フレームで、霧の領域の行ごとの平均輝度の 2 階差分の最大値が sub-02 より小さい (横縞が消えた。画像を並べる) (b) フロクセル off で霧が写り、フロクセルのパスが走らない (c) 場の無い既存デモ 2 本の絵が `2687227` のビルドと差 0 (d) 霧の後ろの透明物と霧の中の不透明物が同じように霞む (e) 960×540 / 1920×1080 で層 1 / 4 枚の GPU 時間を報告 | スクショ、`--img-diff`、GpuTimer | 03 |
| 35 | 地面の霧の局所ライト: (a) `fogLocalLights = false` の絵が切り替え前と差 0 (b) true でランタンの周りの霧が明るい。層から離れたライトは絵を変えない (c) 9 本で 8 本だけ使われ WARN 1 回 (d) ライト 0 / 1 / 8 本の GPU 時間を報告 | `--img-diff`、スクショ、ログ、GpuTimer | 15 |
| 36 | Forward のスカイ無しの背景: (a) sub-03 round 2 と同じ地平線向きの場面で、Forward の背景に地面の霧が掛かる。Forward と Deferred を並べて背景の領域の平均輝度の差を報告 (目視で同じ霧) (b) **差 0 であるべき絵** (`--img-diff --tol 0`、着手前 = sub-15 のコミットのビルドと比較): Deferred の全部、スカイのある Forward の全部 (`--field-demo` はスカイが無いので (c) 側)、フロクセルも地面の霧も無い Forward の全部 (golden の `demo_forward` / `demo_forward_fxaa` / `demo_render_forward` / `acoustic_forward` など) (c) **変わってよい絵** = Forward かつスカイ無しかつフロクセルが走るシーン。coder がデモと golden を全部洗って一覧にし、各々について「パスを一時的に止めたビルドの絵が着手前とビット一致」(変化はこのパスだけが原因) と「差分の画素が背景 (深度 far) の画素に限られる」を示す。前後の画像を並べる (d) golden の更新: (c) の 2 点を満たした golden だけを `tools\shot_verify.bat --update` で撮り直し、差分画像を目視してからコミットに含める。(c) を満たさない golden の変化は不具合として直す (memory `golden-diff-triage`: 塗り潰す前に原因を断定する) (e) 霧の後ろの透明物が背景の霧の上に正しく重なる (f) パスの GPU 時間 | スクショ、`--img-diff`、`shot_verify.bat`、GpuTimer | 16 |

## 6. サブ分割

**並列化 (ユーザー指示 2026-11-08「coder 並列化して 4 つくらい」)**: sub-05〜17 を波 (wave) に分け、1 波あたり coder 最大 4 本 (メインの作業ツリーで動いている分を含む) で進める。並列のサブは各自の git worktree で実装し、司会が波ごとに master へ統合する。共有資源は付録 A の予約表、統合の手順と目視点は付録 B。selftest / replay_verify の**中間のまとめ実行は W2 の統合の後** (sub-07 を含む波。元の「sub-07 の後」と同じ意味)、最後は sub-17。

| 波 | 並列にするサブ | 依存 (波の前に master にあるべきもの) | 予約した資源 (付録 A) | 統合時の確認点 (付録 B) |
|---|---|---|---|---|
| W1 | sub-04 (実装中、メインの作業ツリー) / **sub-05** ABI / **sub-12** 水面の波紋 / **sub-15** 地面の霧の局所ライト | sub-03 (`9b16433`) + 計画ファイルのコミット (B.0) | 04: snapshot v71、DisturbanceFieldComponent の末尾。05: ABI v35 = 202 (唯一)。12: PS の固定スロット (t32 / t33 / b6 / b7 / s8) を読むだけ。15: `pad0` を `fogLocalLights` に (サイズ不変、snapshot 版なし)、新しい PS の b スロット 1 本、規則 9 のグループ「地面の霧のライト」 | 統合の順 04 → 05 → 15 → 12。Components.h の `pad0` の行 (15) と末尾の 2 欄 (04) が隣接。DemoContentField.cpp の呼び出し行 (04 / 05 / 12 / 15)。DisturbanceFieldSelfTest.cpp の末尾 (05 / 15)。`ground_fog.hlsli` (15 だけ) |
| W2 | **sub-06** アニメイベント / **sub-07** 3D / **sub-14** 草 / **sub-16** Forward の背景 | W1 の全部 | 06: `ClipEventKind` の末尾、TickRunner の配送の switch。07: snapshot v72、DisturbanceFieldComponent の末尾 (`buoyancy`)、DFLD のレイアウト (flowY)、froxel CS の t3〜t6 と b2、規則 9 のグループ「Volume」。14: VS の固定スロット (PS と同じ番号を VS にも張る口)、`MyEngineSurface.hlsli` の予約テクスチャ。16: golden PNG の撮り直し (唯一)、新しい PS 1 本 | 統合の順 07 → 06 → 14 → 16。RenderSystem.cpp (07 の CS 束縛 / 14 の VS 束縛 / 16 の条件) の 3 か所。TickRunner.cpp (06)。**統合の後に中間のまとめ実行** (B.3) |
| W3 | **sub-08** 自動有効と Volume の太陽光 / **sub-09** 追従 / **sub-11** 粒子 | W2 の全部 | 08: 合成箇所のシェーダ (10 本 + sub-16 の背景の PS)、RenderSystem のフロクセル判定とゴッドレイ。09: snapshot v73、DisturbanceFieldComponent の末尾 (`followTarget` / `followMargin`)。11: snapshot v74、ParticleEmitterComponent の末尾、粒子 CS の t3〜t7、規則 9 のグループ「粒子」 | 統合の順 09 → 11 → 08。SimSnapshot.h の版の行 (09 / 11 が両方上げる → 大きい方 v74 に揃え、コメントは両方残す)。`AcousticAudioSelfTest.cpp` の版の表明も v74 |
| W4 | **sub-10** 起伏に沿う層 / **sub-13** 跡 | W3 の全部 | 10: snapshot v75、DisturbanceFieldComponent の末尾 (`followTerrain`)、DFLD のレイアウト (columnBaseY)。13: snapshot v76、**TerrainComponent** の末尾 (跡の見た目の欄。場のコンポーネントは触らない、§2 #26)、地形のシェーダ | 統合の順 10 → 13。版の行を v76 に揃える |
| W5 | **sub-17** 仕上げ | W4 の全部 | 文書全部、最後のまとめ実行 | — |

**並列にしない組と理由**:
- sub-07 → sub-09 → sub-10 は直列 (W2 → W3 → W4)。3 本とも sim の核 (`DisturbanceStep` / `DisturbanceState` / DFLD のレイアウト) と DisturbanceFieldComponent の末尾を触る。git が競合を出さずに欄を別の struct へ混ぜる型の誤マージが起きやすい場所 (memory `parallel-milestone-execution`)。追従のずらしは 3D の配列を、起伏はずらしを前提にする
- sub-08 は sub-16 の後 (W3)。sub-08 は合成箇所のシェーダを全部触り、sub-16 が足す背景の PS もその 1 本になる
- sub-13 は W4 (sub-08 と別の波)。sub-08 と sub-13 はどちらも `forward_terrain.hlsl` を触る (08 = フロクセルの合成、13 = 跡)。同じファイルの近い場所で意味の違う変更が重なる
- sub-14 と sub-08 は別の波。どちらも `MyEngineSurface.hlsli` を触る
- 1 つの波で DisturbanceFieldComponent を変えるサブは 1 本だけ (W1 の 04 / 15 は `pad0` の改名と末尾の追加で行が分かれる。B.2 の目視点)
- sub-17 は最後に 1 本だけ

コミットの件名の記号 (M96e〜q) はサブ番号に付けたまま (統合の順と記号の順は一致しなくてよい)。下の表の依存は実際の依存 (波は上の表)。

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 2D 層の状態と 3 点セット | なし | 1〜9, 29, 30 | (済み) `M96a: かき乱しフィールドの核 (2D 層の状態・源・注入と復元・波紋、DFLD 節 snapshot v70、TypeId 96 / 97、規則 18)` |
| sub-02 | 霧の描画 (2D、フロクセルへの注入) と `--field-demo`、replay ジョブ `field` | sub-01 | 10, 11, 12, 30 | (済み `2687227`) `M96b: かき乱しフィールドの霧をフロクセルへ注入する (2D 層、霧の広場デモ、replay ジョブ field)` |
| sub-03 | 地面の霧の専用描画 (板のレイマーチ、自己照明 + 太陽光の bool) | sub-02 | 12, 30, 32 (2D), 34 | `M96c: 地面の霧を専用に描く (2D 層を板のレイマーチで合成、フロクセルから外す、太陽光の切り替え)` |
| sub-04 | 流れのソルバ (移流・投影・風・並列・デバッグ描画・計測) | sub-03 | 3, 13, 14, 15, 16, 30 | `M96d: かき乱しフィールドの流れ (半ラグランジュ移流・圧力投影・一様風・行並列、デバッグ線)` |
| sub-05 | スクリプトの口 (ABI v35 = 202: FieldAddImpulse / FieldSample) とデモの操作役 | sub-03 (W1) | 12, 17, 18, 30 | `M96e: かき乱しフィールドのスクリプト API (FieldAddImpulse / FieldSample、ABI v35 = 202)` |
| sub-06 | アニメイベント `disturb` (斬撃で霧を散らす) | sub-05 (W2) | 12, 19, 30 | `M96f: アニメイベント disturb (振りの瞬間にかき乱しフィールドへ注入する)` |
| sub-07 | 3D ボリューム (浮力・3D テクスチャ 4 本・フロクセル) | sub-04 (W2) | 12, 20, 30 | `M96g: かき乱しフィールドの 3D ボリューム (浮力、4 個までフロクセルへ注入)` |
| sub-08 | フロクセルの自動有効 (場だけのモード) と Volume の太陽光の bool | sub-07, sub-16 (W3) | 12, 30, 31, 32 (3D) | `M96h: Volume の霧でフロクセルを自動で回す (場だけのモード) と、太陽光の切り替え` |
| sub-09 | 追従モード (整数セルずらし) | sub-07 (W3) | 12, 21, 30 | `M96i: かき乱しフィールドの追従 (整数セル単位で格子をずらす)` |
| sub-10 | 地形の起伏に沿う 2D 層 (列ごとの基準高さ) | sub-09 (W4) | 12, 30, 33 | `M96j: かき乱しフィールドの 2D 層を地形の起伏に沿わせる (列ごとの基準高さ)` |
| sub-11 | 粒子 (CPU / GPU) | sub-07 (W3) | 12, 22, 30 | `M96k: パーティクルをかき乱しフィールドの流れに乗せる (CPU / GPU)` |
| sub-12 | 水面の波紋 | sub-03 (W1) | 12, 23, 30 | `M96l: 水面にかき乱しフィールドの波紋を描く` |
| sub-13 | 跡 (地形の雪・砂) | sub-03 (W4) | 12, 24, 30 | `M96m: 地形にかき乱しフィールドの跡を描く (雪・砂の筋)` |
| sub-14 | 草 (サーフェスシェーダの予約テクスチャ + デモの草束) | sub-03 (W2) | 12, 25, 30 | `M96n: 草をかき乱しフィールドで倒す (サーフェスシェーダの予約テクスチャ、デモの草束)` |
| sub-15 | 地面の霧を局所ライトで照らす (ON/OFF、最大 8 本、影なし) | sub-03 (W1) | 12, 30, 35 | `M96o: 地面の霧を局所ライトで照らす切り替え (最大 8 本、影なし)` |
| sub-16 | Forward のスカイ無しの背景に地面の霧とフロクセルを掛ける (背景だけの全画面パス、golden の更新) | sub-15 (W2) | 12, 30, 36 | `M96p: Forward でもスカイの無い背景に地面の霧とフロクセルを掛ける (背景だけの全画面パス)` |
| sub-17 | 仕上げ (ADR-036・文書・カタログ・全検証) | 全部 (W5) | 26, 27, 28, 30 | `M96q: かき乱しフィールドの文書 (ADR-036、engine_spec、機能ガイド、確認項目) と全検証` |

荷重のかかる未知の順: (1) 3 点セットと決定論 (sub-01、済み) → (2) フロクセルで霧として見えるか (sub-02、済み。横縞が出たので専用描画へ) → (3) 地面の霧を 10 か所の合成で既存の絵を 1 ビットも変えずに足せるか (sub-03) → (4) 流れのコスト (sub-04)。見た目の他の用途は sub-02 のアップロード経路に乗るだけなので後ろへ回した。

## 7. 未決事項・リスク

- R1 (sub-02、決着): 霧として見え、尾は理論 22 フレーム。深度スライスの横縞を planner は許容と裁定したが、ユーザーが専用描画を選んだ (2026-11-08) → sub-03 を新設 (§2 #13)。元の記述: フロクセルの解像度とテンポラルで、薄い層の割れ目が見えない / 尾を引きすぎる。判定は §5 の 10 の数値。駄目なら coder は SELF_EVAL の「不安・質問」で報告し、planner が「地面の層専用の描画」サブを足す (この仕様に無い描画を coder が足さない)
- R2 (sub-04 / sub-07): 性能が 1.67 ms を超える → 既定のセル数・反復回数を下げる (planner が判定)。それでも足りなければ `pressureIterations` の既定を下げて渦の強さを諦める
- R3: タイムトラベルの予算 64 MiB に対し、場を 4 個 + 3D で置くと 1 枚が 1 MiB を超え、遡れる時間が 60 秒より短くなる。予算は変えず、sub-04 の計測値を ADR-036 に書く
- R4: snapshot を上げるたびに `AcousticAudioSelfTest.cpp:129` の版の表明が落ちる (`2961ba3` の前例)。各サブで直す
- R5: 跡のマス 0.25 m では足跡の形までは出ず、通った筋になる (既定値の制限として文書化)
- R6 (sub-13): サーフェスシェーダがインスタンシング経路で使えるかは未確認。使えなければ草束は個別描画 (数百) で測り、重ければ数を減らす
- R7: アニメイベントの位置は前 tick の WorldMatrix から作る (1 tick 遅れ、`AnimatorController.h:259-261`)。斬撃の位置が 1 tick ぶん後ろにずれる。既存のイベント (sound / effect / noise) と同じ制限として受け入れる
- R8 (sub-09): 列の基準高さは焼いた時点の地形で決まり、地形を実行時に動かしても追わない (状態として決定的にするための割り切り)。焼き込みは作り直しの step と追従で入ってきた列だけ
- R9 (sub-03 / sub-08): 地面の霧と場だけのモードの合成の口は、どちらも同じ 10 本のシェーダにまたがる (`grep -rn gFroxelEnabled assets\shaders`)。フロクセル off とフルの絵を 1 ビットも変えないことを受け入れ 31 (a) / (d) で縛る
- R10 (sub-03 / sub-08): `fogSunLight` の散乱は影なし (木陰でも照る)。影は後回し (§3)
- R11 (sub-15): 地面の霧の局所ライトは最大 8 本・影なし (§2 #24)。影つきのビームは Volume で作る。8 本の既定は sub-15 の GPU 時間で planner が見直す
- R12 (sub-03): 地面の霧は面ごとに 1 回レイマーチするので、霧の後ろに透明物が重なると透明物のピクセルでも積分する (コストは面の数に比例)。段数の既定は sub-03 の計測で planner が判断する
- R13 (sub-03 → sub-16): スカイボックスの無い背景に地面の霧が掛かるのが Deferred だけ、という制限は、ユーザー指示 (2026-11-08) で取り消し。sub-16 で Forward にも掛ける (§2 #25)。フロクセルの背景も Forward で Deferred に揃える (ユーザー回答 2026-11-08、§2 #25)
- R14 (sub-04): 投影の Jacobi は幅 2 の 5 点 (中心差分の発散・勾配と一貫)。格子が 4 つの部分格子に分かれ、市松模様の圧力の成分が減衰しない。絵に格子状のムラが出たら、前進 / 後退差分 (発散の残りが多い) か、スタガード格子 (別の仕事) を検討する

### ユーザー回答 (2026-11-08、司会の AskUserQuestion)

1. 自動の源 (§2 #5) — 推奨どおり: 源のコンポーネントを付けた物だけ
2. 斬撃の速度 (§2 #6) — 推奨どおり: イベント定義の向き × 速さ
3. 霧への太陽光 (§2 #14) — **当てる / 当てないを bool で選べる** → 場ごとの `fogSunLight` (既定 false)、sub-03
4. フロクセルの有効化 (§2 #15) — **場を置いたら自動で有効、OFF にもできる** → 場ごとの `autoFroxel` (既定 true)、場だけのモード、sub-03
5. 3D の描画数 (§2 #16) — **4 個まで** → sub-07
6. 後回し候補 (§2 #12 / #23) — **起伏に沿う霧は今回入れる** (sub-09 を新設)。突風と視界を遮る機能は後回し
7. 確定確認 — 確定 (上の反映込み)

## 8. 変更履歴

- 2026-11-08 (ユーザー回答 3〜6、司会経由): §2 #12 / #14 / #15 / #16 / #23 を回答に合わせて書き換え。sub-03 (フロクセルの自動有効と太陽光の bool) と sub-09 (起伏に沿う層) を新設し、旧 sub-03〜12 を sub-04〜08 / sub-10〜14 へ繰り下げた (コミット件名の記号も M96d〜n へ)。受け入れ 20 を「3D 4 個を描く + GPU 時間」に変え、31 / 32 / 33 を追加。2D の 2 枚目のテクスチャを `R16F` から `RG16F` (G = 列の基準高さのずれ) へ。`fogSunLight` / `autoFroxel` は sub-01 の見た目の欄に含める (snapshot を余計に上げないため)
- 2026-11-08 (ユーザー指示、司会経由): selftest の実行を sub-07 の後と sub-14 の 2 回にまとめる運用を §5 冒頭に追加。各サブの selftest 系の受け入れ条件は「未実行」と報告してよく、VERDICT で REWORK の理由にしない
- 2026-11-08 (sub-01 round 1 の SELF_EVAL): §4.1 A の作り直しの条件に `cellSize` を追加。§4.1 D の `prevPos` は場が無い tick でも書くと明記。§5 C1 を「変更したファイルの警告 0」に (既存の警告で WarnAsError が落ちるため)。replay_verify は selftest のまとめ実行の対象外と明記 (司会の確認)。sub-01 の計測 (報告のみ) は sub-04 の受け入れ 15 の計測へ統合
- 2026-11-08 (ユーザー指示、司会経由): replay_verify も selftest と同じく sub-07 の後と sub-14 の 2 回にまとめる。§5 冒頭の運用と C1 / C4、sub-02〜14 の検証コマンドの注記を直した
- 2026-11-08 (sub-02 round 1 の SELF_EVAL): §5 の 10 の「差」と「ノイズ」を同じフレーム内の領域差で測ると明記し、テンポラルの尾は理論値と on / off の差で足りるとした。R1 を判定済みに (専用描画は足さない、横縞は許容の裁定でユーザーに確認中)
- 2026-11-08 (ユーザー回答、sub-02 の横縞): 地面の霧専用の描画を足す。§2 #13 / #14 / #15、§3、§4.3、§5 (31 / 32 の担当、34 を追加)、§6、§7 R1 / R9〜R12 を更新。sub-03 を「地面の霧の専用描画」として新設し、旧 sub-03 (フロクセルの自動有効と太陽光) は対象を Volume だけに絞って sub-07 の後の sub-08 へ移した (GroundLayer がフロクセルを使わなくなったので、自動有効は Volume にしか要らない)。旧 sub-08〜14 を sub-09〜15 へ繰り下げ、件名の記号を M96c〜o に。selftest / replay_verify の最後のまとめ実行は sub-15
- 2026-11-08 (ユーザー回答、地面の霧の局所ライト): 照らす / 照らさないを ON/OFF で切り替える。§2 #24 を追加、§3 / §4.2 / §4.3 / §5 (35 を追加) / §6 / §7 R11 を更新。sub-15 (局所ライト) を新設し、仕上げを sub-16 (M96p) へ繰り下げた。まとめ実行の最後は sub-16
- 2026-11-08 (sub-03 round 2): §7 R13 (スカイ無しの背景の Forward / Deferred の食い違い) を制限として追加
- 2026-11-08 (ユーザー指示「Forward でもスカイなしの時に霧を掛けて」): R13 の制限を取り消し、§2 #25・§5 の 36・§6 の sub-16 (背景だけの全画面パス) を追加。仕上げを sub-17 (M96q) へ繰り下げ、まとめ実行の最後も sub-17。sub-04 (実装中) 以前の番号は動かしていない
- 2026-11-08 (ユーザー回答、Forward のフロクセルの背景): Deferred に揃える。§2 #25 の描く条件を「地面の霧 または フロクセル」に、§5 の 36 を「差 0 の絵 / 変わってよい絵 / golden の更新手順」に分けた。sub-16 の件名を更新
- 2026-11-08 (ユーザー指示「coder 並列化して 4 つくらい」): §6 を波 W1〜W5 に分け、依存を実際の依存に直した。付録 A (予約表) と付録 B (統合の手順) を追加。中間のまとめ実行は W2 の統合の後。§2 #26 (跡の見た目の欄の置き場所) を追加
- 2026-11-08 (sub-04 round 1 の SELF_EVAL): §4.1 J に「作用素の一貫性」と「Radial のインパルスは発散源」を追加、§4.1 G に静止状態の流れ = 目標速度を明記、§5 の 3 から流れの単調性を外し、§5 の 15 の測り方 (最良の窓、HashStateBytes) を明記
- 2026-11-08 (sub-04 round 2): Jacobi は幅 2 の 5 点を採用 (前進 / 後退差分の組は 20 反復で発散が 22% 残り、受け入れ 13 を満たさない)。部分格子の分離による市松のムラは sub-17 の手動確認で見る (§7 R14)
- 2026-11-08 (ユーザー指示「寝るから推奨で進める方にシフトして」、司会経由): ここから先は M80 方式。`[ユーザーに聞ける]` の論点は planner が推奨で裁定して進め、§2 に採った案と却下した案を残す。回答待ちで作業を止めない (司会が Notion の質問表に転記し、後で回答をもらう)
- 2026-11-09 (sub-12 round 1): Gerstner の M79 サーフェスへの波紋は sub-12 の対象外とし、サーフェスシェーダの予約テクスチャを作る sub-14 に足した (受け入れ 23 の Gerstner の分)
- 2026-11-09 (sub-16 round 1): 受け入れ 36 (b) の「スカイのある `--field-demo`」は誤り (`--field-demo` にスカイは無い)。(c) の変わってよい絵へ移した。golden `flow_title` の FAIL は着手前 (bc957ea) から同じで M96 と無関係の既存の別件 (台帳の申し送り)
- 2026-11-09 (sub-07 round 1): 受け入れ 20 の性能の線を jobs ありへ (§2 #27)、§4.1 I の風を Volume では xyz と明記
- 2026-11-09 (sub-07 round 2、中間のまとめ実行で発覚): §4.1 C の波紋の注入を「加算」から「目標の高さへ寄せる」(有界) に変更。sub-12 のテストの前提ではなく、sub-01 の式の欠陥と判定
- 2026-11-09 (sub-07 round 3): §4.1 C の波紋の寄せに hPrev も含める (coder の実測による訂正)
- 2026-11-09 (sub-11 round 1): §4.3 の粒子に、y は Volume だけ寄せる・ローカル空間は読まない・GPU の 1 フレーム遅れと場の選び方の違いを明記
- 2026-11-09 (sub-09 round 1、司会の提案): 付録 A.2 の焼き込み値の扱いを「値を変えるサブが `--field-bench` で採ってそのサブで差し替える」に変更 (まとめ実行での切り分けを楽にするため)

## 付録 A. 並列の予約表 (W1〜W5)

各サブは**自分の行の資源だけ**を変える。表に無い共有資源を変えたくなったら、実装せずに「不安・質問」で返す (planner が予約を足す)。

### A.1 版と番号

| 資源 | 今 | 予約 |
|---|---|---|
| `kSimSnapshotVersion` (`SimSnapshot.h`) と `AcousticAudioSelfTest.cpp:129` の表明 | v70 (sub-01) | sub-04 = v71、sub-07 = v72、sub-09 = v73、sub-11 = v74、sub-10 = v75、sub-13 = v76。sub-15 は `pad0` を使うので上げない (詰め物が無いと分かったら実装せず返す)。版は一致しか見ないので、統合で飛び番になってよい。同じ波で 2 本が上げたら、統合で大きい方に揃え、版のコメントは両方残す |
| ABI (`MYE_API_VERSION` とスロット数) | v34 = 200 | sub-05 = v35 = 202 だけ。他のサブはスロットを足さない |
| ComponentTypeId | 96 / 97 使用 | 新しい型は予約しない (足したくなったら返す。次は 98) |
| DFLD 節のレイアウト | sub-01 | sub-07 (flowY) → sub-10 (columnBaseY) の順だけ。sub-04 / 09 / 11 / 13 / 15 は DFLD を変えない (sub-04 が変えたなら W1 の統合で確認する) |
| check_rules の規則番号 | 18 まで | 新しい規則番号は作らない。規則 9 のグループの追加は sub-07「Volume」/ sub-11「粒子」/ sub-15「地面の霧のライト」(別の波なので順序の衝突は無い) |
| replay ジョブ / MCP の JOBS | 24 (`field` を含む) | 増やさない (sub-04 が `jobsab` に `--field-demo` を足すのは既定どおり) |
| golden PNG (`tests\golden\`) | — | sub-16 だけが撮り直してよい (受け入れ 36 の条件つき)。他のサブで golden が動いたら不具合 |

### A.2 構造体・表の末尾 (誤マージが起きやすい所)

| 場所 | 触ってよいサブ (波) |
|---|---|
| `DisturbanceFieldComponent` の末尾 (`Components.h`) と反射登録 (`Components.cpp`) | W1: sub-04 (末尾に 2 欄) と sub-15 (`pad0` の改名だけ。末尾は触らない)。W2: sub-07。W3: sub-09。W4: sub-10 |
| `DisturbanceSourceComponent` | どのサブも触らない |
| `ParticleEmitterComponent` の末尾と登録 | W3: sub-11 |
| `TerrainComponent` の末尾と登録 | W4: sub-13 (跡の見た目の欄。§2 #26) |
| `DisturbanceSlot` / `DisturbanceState` / DFLD の Write・Read・ハッシュ | sub-04 (W1。移流の作業配列は状態にしない)、sub-07 (W2)、sub-09 (W3。ずらしは関数の追加で、スロットの欄は増やさない)、sub-10 (W4) |
| `ClipEventKind` / `ControllerClipEvent` の末尾 | W2: sub-06 |
| `EngineAPI.h` / `Interop.cs` / `ScriptAPI.h` | W1: sub-05 |
| 層の CB (`DisturbanceLayerCB` 336 B) / ビューの CB (`GroundFogViewCB` 128 B) | 誰も変えない。sub-15 はライトを**新しい CB** に載せる (b8 以降の空きを sub-15 が調べて決め、共有ヘッダに書く) |
| froxel CS の束縛 (今 t1 / t2 / s1 / b1) | W2: sub-07 が t3〜t6 (Texture3D × 4) と b2 (Volume の CB) |
| 粒子 CS の束縛 (今 t0〜t2) | W3: sub-11 が t3 (2D 配列) と t4〜t7 (3D × 4)。CB が要れば sub-11 が空きを調べて決める |
| PS の固定スロット (b6 / b7 / t32 / t33 / s8、sub-03) | 読むのは誰でもよい。**VS にも同じ番号で張る口**を足すのは W2 の sub-14 だけ (sub-12 の水面は PS の法線だけで、VS の変位はしない) |
| `LocalizationTable.inl` の末尾 | 誰でも足してよい (統合の目視点。両言語の対と `###` の識別子を確かめる) |
| `DisturbanceFieldSelfTest.cpp` | 各サブは**自分の名前の static 関数** (`RunDisturbance<機能>Tests`) をファイル末尾に足し、スイート関数の末尾に呼び出しを 1 行だけ足す。既存の関数の中に断言を混ぜない |
| selftest の焼き込み値 (`kBakedFinalHash` = 0x92713D47B7CE447A など) | **2026-11-09 に変更**: 値を変えるサブ (コンポーネントの欄・sim の式) は、selftest ではない `Editor.exe --field-bench` で Debug と Release の両方の値を採り、一致を確かめてから**そのサブで**差し替える (旧値 → 新値と理由を SELF_EVAL に書く。sub-04 / sub-07 の前例)。bench で採れない値だけ、従来どおりまとめ実行 (W2 の後、W5) で落ちたら、統合後の master で 1 本の修正コミット (司会が FIX_REQUEST) で、原因 (どのサブの、どの欄・どの計算か) を断定してから再採取する |
| `DemoContentField.cpp` | 各サブは**自分の関数** (`AddFieldDemo<機能>`) を足し、`BuildFieldShowcaseScene` の**末尾**に呼び出しを 1 行だけ足す。既存の関数の中身は変えない (sub-08 / sub-16 がカメラ・フロクセルの設定を変える分は除く)。呼び出しの順は統合の順 = 波の順 (後から足した物は末尾。memory `demo-entity-order-is-rng-stream`) |
| `build\Engine.vcxproj` / `.filters` | 競合したら手で直さず、統合後に `tools\gen_project_files.ps1` を回し直す |

## 付録 B. 統合の手順

- **B.0 (W1 の前に 1 回)**: 計画ファイル (`plans\m96-disturbance-field\` の spec / sub / 台帳) を、sub-04 のコードと分けて先に master へコミットする (件名例 `M96: 並列の予約表と波の計画`)。W1 の worktree はこのコミットから切る。sub-04 はそのコミットの上で、メインの作業ツリーのまま続ける
- **B.1 (各波)**: 波の中のサブは、それぞれ自分の worktree で VERDICT OK まで進める。統合は §6 の表の「統合の順」に、1 本ずつ master へマージする。マージごとにビルド (Debug / Release) と `tools\check_rules.ps1` を通す
- **B.2 (目視点)**: マージのたびに次を `git diff` で目で見る。競合が出なくても見る
  - A.2 の構造体の末尾の欄が正しい struct の中にあるか (`static inline ComponentTypeId sTypeId` の前か、別の struct へ入っていないか)。`static_assert(sizeof(...))` の値が欄の合計と合うか
  - 反射登録の表の欄が、正しいコンポーネントの登録の中にあるか
  - `SimSnapshot.h` の版の行とコメント、`AcousticAudioSelfTest.cpp` の版の表明が、波の最大値か
  - `DemoContentField.cpp` の `BuildFieldShowcaseScene` の末尾の呼び出しが、統合の順に並んでいるか
  - `DisturbanceFieldSelfTest.cpp` の新しい関数の括弧が閉じていて、スイート関数の末尾から呼ばれているか
  - `LocalizationTable.inl` の両言語の対
  - `tools\check_rules.ps1` 規則 9 のグループの追加が、別の規則の中へ入っていないか
- **B.3 (W2 の統合の後 = 中間のまとめ実行)**: master で `Editor.exe --selftest-full` (Debug / Release) と `tools\replay_verify.bat` を回す。落ちたら原因のサブを断定して FIX_REQUEST を回す (焼き込み値は A.2 の規則)
- **B.4 (W5)**: sub-17 が最後のまとめ実行を行う (受け入れ 28)
