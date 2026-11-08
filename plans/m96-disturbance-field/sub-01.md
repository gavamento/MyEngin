# sub-01: 2D 層の状態と 3 点セット (場と源の型、注入・復元・波紋、内部のインパルス、DFLD 節、規則 18)

- 依存: なし
- 状態: OK (commit 64f3b55)
- 往復: 2

## やること

spec §4.1 A〜E・H・I (移流と投影を除く)、§4.2 のコンポーネント 2 型 / Scene が持つ状態 / ハッシュ / スナップショット、§4.4 の規則 18。
このサブの終わりで「場を置く → 源 (CC / 剛体 / 位置の差分) が動く → 濃さ・跡・波紋・流れが変わる → 時間で戻る」が sim 状態として
ハッシュ・スナップショット・リプレイに乗り、場の無いシーンのハッシュ列は 1 ビットも動かない。見た目は無い (sub-02)。

1. `DisturbanceFieldComponent` (TypeId 96) と `DisturbanceSourceComponent` (TypeId 97) を `Components.h` の末尾に足し、`Components.cpp` の反射登録の末尾 (SpringFollow の後) に登録する。欄は spec §4.2 のうち sub-01 の分だけ (`pressureIterations` / `windScale` / `buoyancy` / `followTarget` / `followMargin` / `followTerrain` は**足さない** — 後のサブ)。見た目の欄 (`fogDensity` / `fogColor` / `fogBrightness` / `fogSunLight` / `autoFroxel` / `drawDebug`) は sub-02〜04 が使うが、まとめて今入れる (欄の追加で snapshot を余計に上げないため。sim は読まない)
2. `src\Engine\Engine\Disturbance\` に状態 (`DisturbanceState`) と step の関数群を置く。Scene が `Disturbance()` で持つ (spec §2 #2)。Play の開始 / 終了とシーン切り替えで Clear (Feel と同じ 3 か所)
3. step を `TickRunner.cpp` のフェーズ 4、Fracture (`:609-612`) の後・粒子 (`:614-617`) の前に、`stepSim` のときだけ回す。`MYE_PROFILE_SCOPE("disturbance")`。場が 1 つも無く、キューも空なら走査 1 回で抜ける
4. step の中身 (spec §4.1 B の 1, 3, 4, 5 の減衰と上限, 7 の復元, 8, 9, 10)。移流・投影は入れない
5. 内部 API: `DisturbanceQueueImpulse(Scene&, const FieldImpulse&)` (戻り値 bool。満杯で false + WARN) と `DisturbanceSampleAt(const DisturbanceState&, ..., pos, out)` (spec §4.1 G。sub-05 の ABI がこれを呼ぶ)。selftest はこれで注入・読み取りを行う
6. ハッシュ: `SimSources` の**末尾**に `disturbance`、`SimSourcesOf` で `&scene.Disturbance()` を渡す (引数は増やさない)。内容ゲート (スロット 0 かつキュー空なら畳まない)。`--hash-dump` に場ごとの小計
7. スナップショット: `'DFLD'` 節を TM2D 節の後・World 節の前に。復元は World 差し替えの後に丸ごと代入。`kSimSnapshotVersion` 69 → 70 と版コメント、`AcousticAudioSelfTest.cpp:129` の版の表明を 70 へ
8. selftest スイート `disturbance` (新規 `DisturbanceFieldSelfTest.cpp/.h`、`SelfTestSuites.cpp` に登録。Debug 2 秒以下なら Fast)
9. check_rules 規則 18: `src\Engine\Engine\Disturbance\` の sim ファイル (描画への詰め替えのファイルは除外できるよう許可リストを持つ) で `std::exp|exp\(|expf|pow|powf|log|sin|cos|tan|atan|asin|acos|sinh|cosh|tanh|cbrt|hypot` の呼び出しを error にする。規則自身の自己検査 (違反を含む一時ファイルで error) を既存の規則の自己検査の様式で
10. 新しいファイルを足したら `tools\gen_project_files.ps1`

## やらないこと (このサブでは)

- 移流・圧力投影・風・並列化 (sub-04)。step の各段は直列でよいが、**「前の配列を読み自分のマスだけ書く」形 (波紋は h / hPrev / 新しい h の 3 本)** で書いておき、sub-04 が行で割るだけにする
- 描画・アップロード・デモ (sub-02)、ABI (sub-05)、アニメイベント (sub-06)、3D (sub-07: `shape = Volume` の場は**状態を作らず sim しない** + WARN 1 回。spec §4.1 A のセルの並びは 3D を含めて今決めたものを使う)、追従 (sub-08)
- 物理側の変更 (`SampleWind`、空力)

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h` 末尾 (`:2618` の SpringFollow の後)。`static inline ComponentTypeId sTypeId`、`static_assert(sizeof == N)` (`Components.cpp:2048` の様式)
- `src\Engine\Core\Ecs\Components.cpp:2047-2058` の後に 2 型の登録。`MYE_JP("日本語名", MYE_FIELD_TIP(... "english tip"))`、範囲は `MYE_FIELD_RANGE`、`prevPos` / `prevValid` は `kFieldReadOnly`。hash 対象 (第 3 引数なし)。チャネルのビットは Inspector で分かる形 (既存にビットフラグの欄の型があればそれ、無ければ `UInt32` + 説明)
- `src\Engine\Engine\Scene\Scene.h:98-102, 258-259` (Feel / InputBuffer と並べて `DisturbanceState disturbance_`)
- `src\Editor\Scene\PlayModeController.cpp:26-27, 40-41`、`src\Engine\Engine\Loop\TickRunner.cpp:992-993` に Clear
- `src\Engine\Engine\Loop\TickRunner.cpp:609-617` に step。読むのはこの tick の Transform 確定後の WorldMatrix (`:582-585` の後なので確定済み)、CC の `velocity` (物理が書いた値、`PhysicsCharacters.cpp:500-503`)、Rigidbody の線速度 (欄名は coder が確認)、`PhysicsEnvironmentComponent` (sub-04 から)
- `src\Engine\Engine\Replay\WorldHasher.h:41-81` の `SimSources` 末尾、`WorldHasher.cpp:705-725` の畳み込み (Tilemap / Feel の節の書き方に合わせる)、`SimSnapshot.cpp:35-36` の `SimSourcesOf`
- `src\Engine\Engine\Replay\SimSnapshot.cpp:58` に `kDisturbanceMagic` ('DFLD')、`:633-680` (TM2D の Write / Read) の後に DFLD、`:687` / `:775` の並びに追加、復元の代入は `:790` 付近 (Feel と同じ場所)
- `src\Engine\Engine\Replay\SimSnapshot.h:179` 版 70 + コメント
- `src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp:129` 版の表明
- `src\Editor\...\SelfTestSuites.cpp:131-` の表 (名前 `disturbance`)
- `tools\check_rules.ps1` の末尾 (規則 17 `:1196` の後) に規則 18
- 既存の `SimSnapshotSelfTest.cpp:168-169` の「Scene の sim 状態を Clear する」箇所に Disturbance も (指紋が既存と変わらないことの確認に使う)

## 受け入れ条件 (このサブ)

spec §5 の 1〜9, 29, 30。

- 1: replay_verify の既存全ジョブ PASS。既存 selftest の焼き込みハッシュ (CC2D / 庭 / tile / gridnav / twod など) を**1 つも再採取しない**で PASS (再採取が要ったら、それは場の無いシーンのハッシュが動いた証拠 = 不合格)。空の `DisturbanceState` を配線した `HashWorld` と配線しない `HashWorld` が一致
- 2: 128×128・全チャネル (1|2|4) の場、源なし 600 tick で全配列が静止状態と memcmp 一致
- 3: 源を 30 tick 動かして止めた後、`max|ρ − restDensity|`・`max τ`・(流れの最大の速さ) が毎 tick 非増加、`densityKeep = 0.99` で 1200 tick 後に 1e-4 以下。`trailKeep = 1` で跡が 600 tick 不変
- 4: 場 2 個 (寸法違い) + キュー 3 件 (LateUpdate 相当で積んだ = step 後に積む) を撮る → 新しいシーンへ戻す → 100 tick 毎 tick ハッシュ一致。キューが戻った最初の step で消費されることも断言
- 5: 格子の角 (半径の半分が外)・完全に外・半径 0 の注入。外のマスへの書き込みが無いこと (配列の前後に番兵を置く、または Debug の範囲チェック) と、完全に外で状態不変
- 6: CC (moveInput で歩かせる)・非 kinematic Rigidbody (初速)・どちらも無いエンティティ (毎 tick LocalTransform を動かす) の 3 つで、注入された流れの向きが源の速度の向きと一致 (内積 > 0.9)。Constant の源をエンティティごと 90° 回すと流れも 90° 回る。`fullSpeed` 以上で係数 1、半分で 0.5
- 7: 波紋 1 点の注入から 20 tick 後、中心から ±x / ±z の 4 方向の h が一致 (ビット一致。対称な格子と中心のとき)。`rippleSpeed = 1000` でも 10000 tick 有限
- 8: `cellsX` を変えた tick に作り直され (全マス静止状態)、破棄した tick にスロットが消え節が畳まれない (場の無いシーンと同じハッシュ)。5 個目は sim されず WARN が 1 回だけ。シーン切り替えで Clear
- 9: 源 1 + インパルスの場面を 2 ワールドで 600 tick 並走 → 毎 tick 一致。最終ハッシュを焼き込み、Debug と Release の両方で同じ値
- 29: 規則 18 の自己検査
- 30: Inspector の欄名が日本語 (`MYE_JP`)、規則 10 PASS

## 検証コマンド

> selftest の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) を**実行しない**。ビルドと check_rules (と、selftest 以外の検証) までを行い、selftest で確かめる条件は「未実行 (sub-07 後 / sub-14 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release の両構成 (`/p:MyeWarnAsError=true`)
- 往復中: `$env:MYE_SELFTEST_ONLY='disturbance,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release 同じ
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- 計測 (報告のみ): sub-04 の受け入れ 15 の計測へ統合した (spec §8)。このサブでは不要

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1) の要約 (全文は司会への返信)
- 実装: Components.h/.cpp (TypeId 96/97)、Disturbance/{DisturbanceState.h/.cpp, DisturbanceStep.cpp, DisturbanceFieldSelfTest.h/.cpp}、Scene::Disturbance()、TickRunner (fracture の後・粒子の前、Clear 3 か所)、WorldHasher (SimSources 末尾 + 内容ゲート)、SimSnapshot DFLD 節 v70、check_rules 規則 18 + tools\check_rules_selftest.ps1
- 追加 [追加]: スイート 2 本 (disturbance = Fast 1.1 s、disturbancelong = Full 6 s)。cellSize も作り直し条件とハッシュに入れた。TilemapSelfTest の RunBareScene が空の DFLD 節も除く (焼き込み指紋は据え置き)
- 実行状況: Debug/Release ビルド OK、check_rules 0 error。selftest はユーザー指示 (sub-07 後にまとめて実行) の前に Debug で 1 回実行済み (disturbance/disturbancelong/既定層 105 本 0 失敗)。以後は未実行

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-08)。実装は 3 点セットの様式 (内容ゲート、DFLD 節を TM2D の後に、Clear 3 か所、SimSourcesOf の引数は増やさない) どおりで、仕様との差分 9 件は全て受け入れた (cellSize の作り直し、場が無い tick の prevPos、TilemapSelfTest の空の DFLD 節の除外、カタログ登録、SampleAt の World& など。spec §4.1 A / D に反映)。must 1 件: 受け入れ 1 の replay_verify.bat 未実行 (selftest のまとめ実行の対象外、司会の確認)。selftest (Release の焼き込み一致・--selftest-full) の未実行と計測の未実施は運用どおりで REWORK の理由にしない
- round 2: VERDICT OK (planner、2026-11-08)。replay_verify 全 23 ジョブ PASS (269.5 秒)、焼き込みの再採取なしで既存のハッシュ列が一致 = 受け入れ 1 を確認。受け入れ 9 の Release 側と --selftest-full は sub-07 後のまとめ実行。nit #2 は sub-14、#3 は sub-05 へ申し送った
